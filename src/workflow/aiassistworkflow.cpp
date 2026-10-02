// 层：功能
#include "aiassistworkflow.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>

#include <cstring>

#include <gdal.h>
#include <cpl_conv.h>
#include <ogr_spatialref.h>

#include "../ai/onnxpredictionservice.h"
#include "../ai/tileinference.h"
#include "../catalog/datacatalog.h"
#include "../domain/arearules.h"
#include "../metadata/layermanifest.h"
#include "../qgis/qgislayerservice.h"
#include "../services/paleotaskservice.h"
#include "derivedassets.h"

// workflow/ — AI 辅助解释编排实现。
// · tile 分类：推理核心（ai/tileinference）+ 本文件的产品收尾（GTiff 写
//   盘 + DerivedAssetRegistrar + 声明）。geotransform 解析同 onnx 预测
//   先例：horizon.<target>* 声明栅格优先，缺省像素网格（1 单位/像元）。
// · 追踪建议：引擎结果整组缓存待裁决；commitAccepted 才碰 catalog。

namespace
{
struct AssistGrid
{
  double gt[6] = { 0, 1, 0, 0, 0, -1 }; // 像素网格兜底（origin=左上外角）
  QString projection;
  QString sourcePath;
  bool fromDecl = false;
};

// horizon.<target>* 声明的栅格提供 GT/SRS（同 workflows.cpp onnxAreaGrid
// 语义；这里给任意行列的 tile 产品用，没有 D61 411×641 硬门）。
AssistGrid resolveAssistGrid( const QgisLayerService *layers, int rows, int cols )
{
  AssistGrid grid;
  grid.gt[3] = double( rows ); // 北向上 GT：y 原点 = 行数（左上外角）
  if ( layers )
  {
    const QString prefix = QStringLiteral( "horizon.%1" )
                             .arg( AreaRules::active().targetHorizon );
    QString source;
    for ( const LayerDeclaration &d : layers->declared() )
    {
      if ( d.layerId.startsWith( prefix ) &&
           d.type.compare( QStringLiteral( "raster" ), Qt::CaseInsensitive ) == 0 )
      {
        source = d.source;
        break;
      }
    }
    const QString path = source.section( QLatin1Char( '|' ), 0, 0 );
    if ( !path.isEmpty() && QFile::exists( path ) )
    {
      GDALAllRegister();
      GDALDatasetH ds = GDALOpen( path.toUtf8().constData(), GA_ReadOnly );
      if ( ds )
      {
        double gt[6];
        if ( GDALGetGeoTransform( ds, gt ) == CE_None )
        {
          std::memcpy( grid.gt, gt, sizeof( gt ) );
          grid.fromDecl = true;
          grid.sourcePath = path;
        }
        const char *proj = GDALGetProjectionRef( ds );
        if ( proj && *proj )
          grid.projection = QString::fromUtf8( proj );
        GDALClose( ds );
      }
    }
  }
  Q_UNUSED( cols );
  return grid;
}

void applyGridSrs( GDALDatasetH ds, const AssistGrid &grid )
{
  if ( !grid.projection.isEmpty() )
  {
    GDALSetProjection( ds, grid.projection.toUtf8().constData() );
    return;
  }
  OGRSpatialReference srs;
  if ( srs.SetFromUserInput( DataCatalog::localGridCrsWkt().toUtf8().constData() ) == OGRERR_NONE )
  {
    char *wkt = nullptr;
    if ( srs.exportToWkt( &wkt ) == OGRERR_NONE && wkt )
      GDALSetProjection( ds, wkt );
    CPLFree( wkt );
  }
}

// 通用单波段 GeoTIFF 写盘（Byte=相/掩膜；Float32=置信度）。
bool writeBandRaster( const QString &path, int rows, int cols, GDALDataType dt,
                      double nodata, const void *data, const AssistGrid &grid,
                      const QByteArray &provenanceJson, QString *error )
{
  GDALAllRegister();
  GDALDriverH drv = GDALGetDriverByName( "GTiff" );
  if ( !drv )
  {
    *error = QObject::tr( "GTiff driver is not available" );
    return false;
  }
  if ( QFile::exists( path ) )
    QFile::remove( path );
  GDALDatasetH ds = GDALCreate( drv, path.toUtf8().constData(), cols, rows, 1, dt, nullptr );
  if ( !ds )
  {
    *error = QObject::tr( "cannot create raster %1" ).arg( path );
    return false;
  }
  GDALSetGeoTransform( ds, const_cast<double *>( grid.gt ) );
  applyGridSrs( ds, grid );
  GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
  GDALSetRasterNoDataValue( band, nodata );
  if ( GDALRasterIO( band, GF_Write, 0, 0, cols, rows, const_cast<void *>( data ), cols, rows,
                     dt, 0, 0 ) != CE_None )
  {
    GDALClose( ds );
    QFile::remove( path );
    *error = QObject::tr( "failed to write raster %1" ).arg( path );
    return false;
  }
  if ( !provenanceJson.isEmpty() )
  {
    GDALSetMetadataItem( ds, "PALEO_PROVENANCE", provenanceJson.constData(), nullptr );
  }
  GDALClose( ds );
  return true;
}

QString safeSegment( const QString &s )
{
  QString out;
  for ( const QChar &c : s )
  {
    if ( c.isLetterOrNumber() || c == QLatin1Char( '_' ) || c == QLatin1Char( '-' ) )
      out += c;
    else
      out += QLatin1Char( '_' );
  }
  return out.toUpper();
}
} // namespace

AiAssistWorkflow::AiAssistWorkflow( QgisLayerService *layers, QObject *parent )
  : QObject( parent )
  , m_layers( layers )
{
}

void AiAssistWorkflow::setOnnxService( PaleoOnnxService *onnx )
{
  m_onnx = onnx;
}

void AiAssistWorkflow::setCatalog( DataCatalog *catalog, const QString &projectDir )
{
  m_catalog = catalog;
  m_projectDir = projectDir;
}

void AiAssistWorkflow::setTaskService( PaleoTaskService *tasks )
{
  m_tasks = tasks;
}

bool AiAssistWorkflow::classifyTiles( const QString &horizon, const QString &model,
                                      int gridRows, int gridCols, int tileRows, int tileCols,
                                      int halo, const GridFetch &fetch,
                                      double lowConfidenceThreshold, QString *error )
{
  if ( error )
    error->clear();
  const auto fail = [this, &horizon, error]( const QString &msg ) {
    if ( error )
      *error = msg;
    emit tileClassificationFailed( horizon, msg );
    return false;
  };
  if ( !m_onnx )
    return fail( tr( "AI 辅助未绑定 ONNX 服务" ) );
  if ( !m_layers )
    return fail( tr( "AI 辅助未绑定图层服务" ) );
  if ( lowConfidenceThreshold < 0.0 || lowConfidenceThreshold > 1.0 )
    return fail( tr( "低置信阈值须在 [0,1]，实得 %1" ).arg( lowConfidenceThreshold ) );

  TileInferenceRequest req;
  req.model = model;
  req.gridRows = gridRows;
  req.gridCols = gridCols;
  req.tileRows = tileRows;
  req.tileCols = tileCols;
  req.halo = halo;
  req.fetch = fetch;

  TileInferenceResult result;
  QString runErr;
  if ( !runTileInference( m_onnx, req, &result, {}, nullptr, &runErr ) )
    return fail( runErr );

  if ( !writeTileProducts( horizon, model, result, lowConfidenceThreshold, error ) )
    return fail( error && !error->isEmpty() ? *error : tr( "产品写盘失败" ) );

  emit tileClassificationDone( horizon, m_lastProductLayerIds );
  return true;
}

bool AiAssistWorkflow::writeTileProducts( const QString &horizon, const QString &model,
                                          const TileInferenceResult &result,
                                          double lowConfidenceThreshold, QString *error )
{
  QString regErr;
  DerivedAssetRegistrar registrar( m_catalog, m_projectDir );
  if ( !registrar.isBound() )
  {
    *error = tr( "AI 辅助未绑定 catalog 登记通道（T26）" );
    return false;
  }
  const int rows = result.grid.rows;
  const int cols = result.grid.cols;
  const qsizetype n = qsizetype( rows ) * cols;
  const AssistGrid grid = resolveAssistGrid( m_layers, rows, cols );

  QJsonObject provCommon;
  provCommon.insert( QStringLiteral( "model" ), model );
  provCommon.insert( QStringLiteral( "rows" ), rows );
  provCommon.insert( QStringLiteral( "cols" ), cols );
  provCommon.insert( QStringLiteral( "classes" ), result.grid.classes );
  provCommon.insert( QStringLiteral( "tiles" ), result.tilesDone );
  provCommon.insert( QStringLiteral( "low_confidence_threshold" ), lowConfidenceThreshold );
  provCommon.insert( QStringLiteral( "inference_ms" ), double( result.inferenceMs ) );

  struct Product
  {
    QString layerId, assetType, title, fileName;
    QVector<quint8> bytes;
    QVector<float> floats;
    bool isFloat = false;
  };
  QVector<Product> products;

  Product facies;
  facies.layerId = QStringLiteral( "aifacies.%1.%2" ).arg( horizon, model );
  facies.assetType = QStringLiteral( "aifacies" );
  facies.title = tr( "%1 AI 相分类 %2" ).arg( horizon, model );
  facies.fileName = QStringLiteral( "AIFACIES_%1_%2.tif" ).arg( safeSegment( horizon ), safeSegment( model ) );
  facies.bytes = result.grid.argmax; // 255 已是无数据
  products.append( facies );

  Product masked;
  masked.layerId = facies.layerId + QStringLiteral( ".masked" );
  masked.assetType = QStringLiteral( "aifacies_masked" );
  masked.title = tr( "%1 AI 相分类 %2（低置信掩膜）" ).arg( horizon, model );
  masked.fileName = QStringLiteral( "AIFACIES_%1_%2_MASKED.tif" )
                      .arg( safeSegment( horizon ), safeSegment( model ) );
  masked.bytes = QVector<quint8>( n, 255 );
  for ( qsizetype i = 0; i < n; ++i )
  {
    if ( result.grid.argmax[i] == 255 )
      continue; // 原无数据保持掩膜
    if ( double( result.grid.confidence[i] ) >= lowConfidenceThreshold )
      masked.bytes[i] = result.grid.argmax[i];
  }
  products.append( masked );

  Product conf;
  conf.layerId = QStringLiteral( "confidence.%1.%2" ).arg( horizon, model );
  conf.assetType = QStringLiteral( "confidence" );
  conf.title = tr( "%1 置信度 %2" ).arg( horizon, model );
  conf.fileName = QStringLiteral( "CONFIDENCE_%1_%2.tif" )
                    .arg( safeSegment( horizon ), safeSegment( model ) );
  conf.floats = QVector<float>( n, -9999.0f );
  for ( qsizetype i = 0; i < n; ++i )
  {
    if ( result.grid.argmax[i] != 255 )
      conf.floats[i] = result.grid.confidence[i];
  }
  conf.isFloat = true;
  products.append( conf );

  QStringList layerIds;
  for ( const Product &p : products )
  {
    QString stageErr;
    const DerivedStaging st = registrar.stage( p.assetType, p.title, p.fileName, &stageErr );
    if ( !st.isValid() )
    {
      *error = stageErr;
      return false;
    }
    QJsonObject prov = provCommon;
    prov.insert( QStringLiteral( "layer_id" ), p.layerId );
    const QByteArray provJson = QJsonDocument( prov ).toJson( QJsonDocument::Compact );
    const bool written = p.isFloat
                           ? writeBandRaster( st.absolutePath, rows, cols, GDT_Float32, -9999.0,
                                              p.floats.constData(), grid, provJson, error )
                           : writeBandRaster( st.absolutePath, rows, cols, GDT_Byte, 255.0,
                                                              p.bytes.constData(), grid, provJson, error );
    if ( !written )
      return false;
    const QStringList parents = !grid.sourcePath.isEmpty()
                                  ? registrar.parentVersionIdsFor( QStringList { grid.sourcePath } )
                                  : QStringList();
    QString commitErr;
    if ( !registrar.commitExternal( st, st.absolutePath, parents,
                                    QStringLiteral( "aiassist/%1" ).arg( model ), prov.toVariantMap(),
                                    &commitErr ) )
    {
      *error = commitErr;
      return false;
    }
    LayerDeclaration decl;
    decl.layerId = p.layerId;
    decl.horizon = horizon;
    decl.type = QStringLiteral( "raster" );
    decl.source = st.absolutePath;
    decl.group = QStringLiteral( "03_Predict" );
    decl.title = p.title;
    if ( !m_layers->declare( decl, error ) )
      return false;
    layerIds.append( p.layerId );
  }
  m_lastProductLayerIds = layerIds;
  return true;
}

PaleoTask *AiAssistWorkflow::startClassification( const QString &horizon, const QString &model,
                                                  int gridRows, int gridCols, int tileRows,
                                                  int tileCols, int halo, const GridFetch &fetch,
                                                  double lowConfidenceThreshold, QString *error )
{
  if ( error )
    error->clear();
  if ( !m_tasks )
  {
    if ( error )
      *error = tr( "异步分类需要任务服务（setTaskService）" );
    return nullptr;
  }
  if ( !m_onnx )
  {
    if ( error )
      *error = tr( "AI 辅助未绑定 ONNX 服务" );
    return nullptr;
  }

  // 任务线程只做推理（onnx 服务线程安全）；产品收尾回主线程（图层/登记
  // 非线程安全）。结果经共享指针带出，取消/失败不带产品。
  auto *result = new TileInferenceResult;
  QPointer<AiAssistWorkflow> self( this );
  const QString title = tr( "%1 AI 相分类 %2" ).arg( horizon, model );
  PaleoTask *task = m_tasks->start(
    title,
    [this, model, gridRows, gridCols, tileRows, tileCols, halo, fetch, result]( PaleoTask *t ) -> QString {
      TileInferenceRequest req;
      req.model = model;
      req.gridRows = gridRows;
      req.gridCols = gridCols;
      req.tileRows = tileRows;
      req.tileCols = tileCols;
      req.halo = halo;
      req.fetch = fetch;
      QString err;
      int lastPct = -1;
      const bool ok = runTileInference(
        m_onnx, req, result,
        [ t, &lastPct ]( int done, int total ) {
          const int pct = total > 0 ? done * 100 / total : 0;
          if ( pct != lastPct )
          {
            lastPct = pct;
            t->reportStage( QStringLiteral( "infer" ), pct );
          }
        },
        [t]() { return t->cancelRequested(); }, &err );
      return ok ? QString() : err;
    },
    QString(), PaleoTask::Priority::Normal, true );
  connect( task, &PaleoTask::finished, this, [self, this, task, horizon, model, result,
                                              lowConfidenceThreshold]() {
    // result 在收尾后才删——写产品仍要读它（先删后用=UAF）。
    if ( task->state() == PaleoTask::State::Succeeded )
    {
      QString err;
      if ( self && writeTileProducts( horizon, model, *result, lowConfidenceThreshold, &err ) )
        emit tileClassificationDone( horizon, m_lastProductLayerIds );
      else if ( self )
        emit tileClassificationFailed( horizon, err );
    }
    else if ( task->state() == PaleoTask::State::Failed )
    {
      emit tileClassificationFailed( horizon, task->errorText() );
    }
    // Cancelled：无产品、无信号（任务页已呈现取消态）。
    delete result;
  } );
  return task;
}

bool AiAssistWorkflow::suggestTracking( const QString &horizon, const QString &model,
                                        const QVector<TrackingSeed> &seeds, int windowSamples,
                                        int radius, const TraceWindowFetcher &fetch,
                                        QVector<TrackingSuggestion> *suggestions, QString *error )
{
  if ( error )
    error->clear();
  QVector<TrackingSuggestion> out;
  if ( !suggestHorizonTracking( m_onnx, model, seeds, windowSamples, radius, fetch, &out, error ) )
    return false;

  QVector<SuggestionState> states;
  states.reserve( out.size() );
  for ( const TrackingSuggestion &s : out )
    states.append( SuggestionState { s, s.isSeed } ); // 种子=解释员锚点，默认已接受
  m_suggestions.insert( horizon, states );
  if ( suggestions )
    *suggestions = out;
  emit suggestionsReady( horizon, int( out.size() ) );
  return true;
}

bool AiAssistWorkflow::acceptSuggestion( const QString &horizon, int inlineNo, int xlineNo )
{
  auto it = m_suggestions.find( horizon );
  if ( it == m_suggestions.end() )
    return false;
  for ( SuggestionState &st : it.value() )
  {
    if ( st.suggestion.inlineNo == inlineNo && st.suggestion.xlineNo == xlineNo )
    {
      st.accepted = true;
      return true;
    }
  }
  return false;
}

bool AiAssistWorkflow::vetoSuggestion( const QString &horizon, int inlineNo, int xlineNo )
{
  auto it = m_suggestions.find( horizon );
  if ( it == m_suggestions.end() )
    return false;
  QVector<SuggestionState> &list = it.value();
  for ( int i = 0; i < list.size(); ++i )
  {
    if ( list[i].suggestion.inlineNo == inlineNo && list[i].suggestion.xlineNo == xlineNo )
    {
      if ( list[i].suggestion.isSeed )
        return false; // 种子是解释员给的锚点，不可否决成「不存在」
      // 否决 = 从待裁决集中移除（TrackingSuggestion 无值相等语义，按位删）。
      list.remove( i );
      return true;
    }
  }
  return false;
}

int AiAssistWorkflow::pendingCount( const QString &horizon ) const
{
  const auto it = m_suggestions.constFind( horizon );
  if ( it == m_suggestions.constEnd() )
    return 0;
  int n = 0;
  for ( const SuggestionState &st : it.value() )
    if ( !st.suggestion.isSeed && st.accepted == false )
      ++n;
  return n;
}

int AiAssistWorkflow::acceptedCount( const QString &horizon ) const
{
  const auto it = m_suggestions.constFind( horizon );
  if ( it == m_suggestions.constEnd() )
    return 0;
  int n = 0;
  for ( const SuggestionState &st : it.value() )
    if ( st.accepted )
      ++n;
  return n;
}

QVector<TrackingSuggestion> AiAssistWorkflow::acceptedSuggestions( const QString &horizon ) const
{
  QVector<TrackingSuggestion> out;
  const auto it = m_suggestions.constFind( horizon );
  if ( it == m_suggestions.constEnd() )
    return out;
  for ( const SuggestionState &st : it.value() )
    if ( st.accepted )
      out.append( st.suggestion );
  return out;
}

bool AiAssistWorkflow::commitAccepted( const QString &horizon, QString *error )
{
  if ( error )
    error->clear();
  DerivedAssetRegistrar registrar( m_catalog, m_projectDir );
  if ( !registrar.isBound() )
  {
    *error = tr( "AI 辅助未绑定 catalog 登记通道（T26）" );
    return false;
  }
  const QVector<TrackingSuggestion> accepted = acceptedSuggestions( horizon );
  if ( accepted.isEmpty() )
  {
    *error = tr( "层位 %1 没有已接受的建议可提交" ).arg( horizon );
    return false;
  }

  QString stageErr;
  const DerivedStaging st = registrar.stage(
    QStringLiteral( "aitrack" ), tr( "%1 AI 追踪拾取" ).arg( horizon ),
    QStringLiteral( "AITRACK_%1.json" ).arg( safeSegment( horizon ) ), &stageErr );
  if ( !st.isValid() )
  {
    *error = stageErr;
    return false;
  }

  QJsonObject doc;
  doc.insert( QStringLiteral( "horizon" ), horizon );
  doc.insert( QStringLiteral( "kind" ), QStringLiteral( "ai_tracking_picks" ) );
  doc.insert( QStringLiteral( "count" ), accepted.size() );
  QJsonArray picks;
  for ( const TrackingSuggestion &s : accepted )
  {
    QJsonObject p;
    p.insert( QStringLiteral( "inline" ), s.inlineNo );
    p.insert( QStringLiteral( "xline" ), s.xlineNo );
    p.insert( QStringLiteral( "sample" ), s.sampleIndex );
    p.insert( QStringLiteral( "score" ), double( s.score ) );
    p.insert( QStringLiteral( "confidence" ), double( s.confidence ) );
    p.insert( QStringLiteral( "seed" ), s.isSeed );
    picks.append( p );
  }
  doc.insert( QStringLiteral( "picks" ), picks );
  QFile f( st.absolutePath );
  if ( !f.open( QIODevice::WriteOnly ) )
  {
    *error = tr( "无法写入 %1: %2" ).arg( st.absolutePath, f.errorString() );
    return false;
  }
  f.write( QJsonDocument( doc ).toJson( QJsonDocument::Indented ) );
  f.close();

  QVariantMap extra;
  extra.insert( QStringLiteral( "horizon" ), horizon );
  extra.insert( QStringLiteral( "picks" ), accepted.size() );
  QString commitErr;
  if ( !registrar.commitExternal( st, st.absolutePath, QStringList(),
                                  QStringLiteral( "aiassist/aitrack" ), extra, &commitErr ) )
  {
    *error = commitErr;
    return false;
  }
  emit acceptedCommitted( horizon, st.relativePath );
  return true;
}
