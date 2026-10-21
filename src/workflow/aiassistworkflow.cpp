// 层：功能
#include "aiassistworkflow.h"

#include <memory>
#include <mutex>

#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>

#include <cmath>
#include <cstring>
#include <limits>

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

// horizon.<target>* 声明里第一个 raster 的数据源路径（无则空串）。
// resolveAssistGrid（产品几何）与 horizonGridFetch（聊天取数）共用同一解析，
// 保证「写产品用的网格」与「读数用的网格」是同一张栅格。
QString horizonRasterPath( const QgisLayerService *layers )
{
  if ( !layers )
    return QString();
  const QString prefix = QStringLiteral( "horizon.%1" )
                           .arg( AreaRules::active().targetHorizon );
  for ( const LayerDeclaration &d : layers->declared() )
  {
    if ( d.layerId.startsWith( prefix ) &&
         d.type.compare( QStringLiteral( "raster" ), Qt::CaseInsensitive ) == 0 )
      return d.source.section( QLatin1Char( '|' ), 0, 0 );
  }
  return QString();
}

// horizon.<target>* 声明的栅格提供 GT/SRS（同 workflows.cpp onnxAreaGrid
// 语义；这里给任意行列的 tile 产品用，没有 D61 411×641 硬门）。
AssistGrid resolveAssistGrid( const QgisLayerService *layers, int rows, int cols )
{
  AssistGrid grid;
  grid.gt[3] = double( rows ); // 北向上 GT：y 原点 = 行数（左上外角）
  const QString path = horizonRasterPath( layers );
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

QString aiSafeSegment( const QString &s )
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

// 推理入口收口：非 ORT 构建（CMakeLists.txt 的 PALEO_HAVE_ORT=FALSE 分支）的
// paleo_ai 不编 ai/tileinference.cpp 与 ai/horizonsuggest.cpp，本 TU 若直接
// 引用两者的符号，链接 paleo_workflow 的所有可执行目标都会失败。四个调用点
// 统一经这两个包装；非 ORT 构建诚实报不可用（口径同 ai/chat/domaintools.cpp
// 对这两个工具的 Disabled 说明），不伪造结果。
bool inferTiles( PaleoOnnxService *onnx, const TileInferenceRequest &req,
                 TileInferenceResult *result, const std::function<void( int, int )> &progress,
                 const std::function<bool()> &cancelled, QString *error )
{
#if PALEO_HAVE_ORT
  return runTileInference( onnx, req, result, progress, cancelled, error );
#else
  Q_UNUSED( onnx );
  Q_UNUSED( req );
  Q_UNUSED( result );
  Q_UNUSED( progress );
  Q_UNUSED( cancelled );
  if ( error )
    *error = QObject::tr( "本构建未编译 ONNX Runtime，AI 相分类不可用" );
  return false;
#endif
}

bool suggestTrack( PaleoOnnxService *onnx, const QString &model,
                   const QVector<TrackingSeed> &seeds, int windowSamples, int radius,
                   const TraceWindowFetcher &fetch,
                   const std::function<bool()> &cancelled,
                   QVector<TrackingSuggestion> *out, QString *error )
{
#if PALEO_HAVE_ORT
  return suggestHorizonTracking( onnx, model, seeds, windowSamples, radius, fetch,
                                 cancelled, out, error );
#else
  Q_UNUSED( onnx );
  Q_UNUSED( model );
  Q_UNUSED( seeds );
  Q_UNUSED( windowSamples );
  Q_UNUSED( radius );
  Q_UNUSED( fetch );
  Q_UNUSED( cancelled );
  Q_UNUSED( out );
  if ( error )
    *error = QObject::tr( "本构建未编译 ONNX Runtime，AI 层位追踪建议不可用" );
  return false;
#endif
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
  if ( !inferTiles( m_onnx, req, &result, {}, nullptr, &runErr ) )
    return fail( runErr );

  if ( !writeTileProducts( horizon, model, result, tileRows, tileCols, halo,
                           result.modelSha256, lowConfidenceThreshold, error ) )
    return fail( error && !error->isEmpty() ? *error : tr( "产品写盘失败" ) );

  emit tileClassificationDone( horizon, m_lastProductLayerIds );
  return true;
}

bool AiAssistWorkflow::writeTileProducts( const QString &horizon, const QString &model,
                                          const TileInferenceResult &result, int tileRows,
                                          int tileCols, int halo, const QString &modelSha256,
                                          double lowConfidenceThreshold, QString *error )
{
  m_lastStats = QJsonObject(); // 失败不留旧摘要（诚实：stats 只在成功后有效）
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
  // 可复现性：同模型名不同切片参数（tile 形状/halo）产出不同栅格；模型文件
  // 字节指纹把「模型名」钉到具体字节（注册表钉哈希口径）。
  provCommon.insert( QStringLiteral( "model_sha256" ), modelSha256 );
  provCommon.insert( QStringLiteral( "rows" ), rows );
  provCommon.insert( QStringLiteral( "cols" ), cols );
  provCommon.insert( QStringLiteral( "tile_rows" ), tileRows );
  provCommon.insert( QStringLiteral( "tile_cols" ), tileCols );
  provCommon.insert( QStringLiteral( "halo" ), halo );
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
  facies.fileName = QStringLiteral( "AIFACIES_%1_%2.tif" ).arg( aiSafeSegment( horizon ), aiSafeSegment( model ) );
  facies.bytes = result.grid.argmax; // 255 已是无数据
  products.append( facies );

  Product masked;
  masked.layerId = facies.layerId + QStringLiteral( ".masked" );
  masked.assetType = QStringLiteral( "aifacies_masked" );
  masked.title = tr( "%1 AI 相分类 %2（低置信掩膜）" ).arg( horizon, model );
  masked.fileName = QStringLiteral( "AIFACIES_%1_%2_MASKED.tif" )
                      .arg( aiSafeSegment( horizon ), aiSafeSegment( model ) );
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
                    .arg( aiSafeSegment( horizon ), aiSafeSegment( model ) );
  conf.floats = QVector<float>( n, -9999.0f );
  for ( qsizetype i = 0; i < n; ++i )
  {
    if ( result.grid.argmax[i] != 255 )
      conf.floats[i] = result.grid.confidence[i];
  }
  conf.isFloat = true;
  products.append( conf );

  QStringList layerIds;
  // #588 L4.2：三段产物原先逐段 return false——已 commit 的外部资产与已声明
  // 图层留在盘上，「失败」被上报而部分产物实际留存。改为一次 BatchSave 事务：
  // 任一段失败即整批回滚（内层 commit 的 transaction.abort() 冒泡到本批），
  // 已登记的版本、已落盘的受管字节与已下的图层声明一并回收（顺序同
  // MappingWorkbench::removeProduct：先下架声明、再删受管文件）。
  DataCatalog::BatchSave transaction(m_catalog);
  struct Committed
  {
    QString layerId;
    QString path;
  };
  QVector<Committed> committed;
  QStringList declaredIds;
  const auto discardCommitted = [&]() {
    if ( !declaredIds.isEmpty() )
      m_layers->removeDeclarations( declaredIds );
    for ( const Committed &c : committed )
    {
      // 受管字节在 commit 后被置只读，Windows 删只读文件会失败——先恢复可写。
      QFile::setPermissions( c.path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                         QFileDevice::ReadUser );
      QFile::remove( c.path );
    }
    committed.clear();
    declaredIds.clear();
    layerIds.clear();
  };
  for ( const Product &p : products )
  {
    QString stageErr;
    const DerivedStaging st = registrar.stage( p.assetType, p.title, p.fileName, &stageErr );
    if ( !st.isValid() )
    {
      *error = stageErr;
      discardCommitted();
      transaction.abort();
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
    {
      QFile::remove( st.absolutePath );
      discardCommitted();
      transaction.abort();
      return false;
    }
    const QStringList parents = !grid.sourcePath.isEmpty()
                                  ? registrar.parentVersionIdsFor( QStringList { grid.sourcePath } )
                                  : QStringList();
    QString commitErr;
    if ( !registrar.commitExternal( st, st.absolutePath, parents,
                                    QStringLiteral( "aiassist/%1" ).arg( model ), prov.toVariantMap(),
                                    &commitErr ) )
    {
      *error = commitErr;
      QFile::remove( st.absolutePath );
      discardCommitted();
      transaction.abort();
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
    {
      discardCommitted();
      transaction.abort();
      return false;
    }
    committed.append( Committed { p.layerId, st.absolutePath } );
    declaredIds.append( p.layerId );
    layerIds.append( p.layerId );
  }
  QString flushErr;
  if ( !transaction.flush( &flushErr ) )
  {
    // 整批落盘失败：catalog 内存已由 BatchSave 回滚到批次前，受管字节与
    // 图层声明同步回收——不留下「盘上有、库里无」的孤儿产物。
    *error = tr( "AI 相分类产品登记落盘失败：%1" ).arg( flushErr );
    discardCommitted();
    return false;
  }
  m_lastProductLayerIds = layerIds;

  // 统计摘要（方向61）：给聊天工具结果回灌用——类直方图/无数据数/有效均值
  // 置信都从推理结果直接算，避免为摘要回读整幅栅格。
  QJsonObject histogram;
  qsizetype nodata = 0;
  double confidenceSum = 0.0;
  qsizetype valid = 0;
  for ( qsizetype i = 0; i < n; ++i )
  {
    const int code = result.grid.argmax[i];
    if ( code == 255 )
    {
      ++nodata;
      continue;
    }
    histogram.insert( QString::number( code ), histogram.value( QString::number( code ) ).toInt() + 1 );
    confidenceSum += double( result.grid.confidence[i] );
    ++valid;
  }
  QJsonObject stats;
  stats.insert( QStringLiteral( "rows" ), rows );
  stats.insert( QStringLiteral( "columns" ), cols );
  stats.insert( QStringLiteral( "classes" ), result.grid.classes );
  stats.insert( QStringLiteral( "tiles_done" ), result.tilesDone );
  stats.insert( QStringLiteral( "inference_ms" ), double( result.inferenceMs ) );
  stats.insert( QStringLiteral( "classified_pixels" ), qint64( valid ) );
  stats.insert( QStringLiteral( "nodata_pixels" ), qint64( nodata ) );
  stats.insert( QStringLiteral( "mean_confidence" ), valid > 0 ? confidenceSum / valid : 0.0 );
  stats.insert( QStringLiteral( "class_histogram" ), histogram );
  stats.insert( QStringLiteral( "layer_ids" ), QJsonArray::fromStringList( layerIds ) );
  m_lastStats = stats;
  return true;
}

AiAssistWorkflow::GridFetch AiAssistWorkflow::horizonGridFetch( QgisLayerService *layers )
{
  // 返回可拷贝的取数闭包（classifyTiles/startClassification/聊天执行器共用
  // 契约：NaN = 缺）。池线程调用——声明解析在调用时做，数据集按调用开关。
  //
  // 生命周期：闭包在池线程执行，绝不能触碰 QgisLayerService（主线程对象，可能
  // 已析构）。在调用线程（主线程）解析路径快照，并以 layers 为 context 订阅声明
  // 变更信号在主线程刷新快照——service 析构时连接自动断开，池线程只读加锁快照。
  struct PathSnapshot
  {
    std::mutex mutex;
    QString path;
  };
  auto snapshot = std::make_shared<PathSnapshot>();
  snapshot->path = horizonRasterPath( layers );
  if ( layers )
  {
    auto refresh = [snapshot, guard = QPointer<QgisLayerService>( layers )]( const QString & ) {
      const QString path = horizonRasterPath( guard.data() );
      std::lock_guard<std::mutex> lock( snapshot->mutex );
      snapshot->path = path;
    };
    QObject::connect( layers, &QgisLayerService::layerDeclared, layers, refresh );
    QObject::connect( layers, &QgisLayerService::horizonReleased, layers, refresh );
  }
  return [snapshot]( int row0, int col0, int rows, int cols, QVector<float> &out,
                     QString &err ) -> bool {
    QString path;
    {
      std::lock_guard<std::mutex> lock( snapshot->mutex );
      path = snapshot->path;
    }
    if ( path.isEmpty() || !QFile::exists( path ) )
    {
      err = QObject::tr( "未找到活动层位的栅格声明（horizon.%1）——先导入/生成层位栅格" )
              .arg( AreaRules::active().targetHorizon );
      return false;
    }
    GDALAllRegister();
    GDALDatasetH ds = GDALOpen( path.toUtf8().constData(), GA_ReadOnly );
    if ( !ds )
    {
      err = QObject::tr( "无法打开层位栅格 %1" ).arg( path );
      return false;
    }
    GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
    out.resize( rows * cols );
    const CPLErr rc = GDALRasterIO( band, GF_Read, col0, row0, cols, rows,
                                    out.data(), cols, rows, GDT_Float32, 0, 0 );
    if ( rc != CE_None )
    {
      GDALClose( ds );
      err = QObject::tr( "读取层位栅格窗口失败（%1，行 %2 列 %3）" ).arg( path ).arg( row0 ).arg( col0 );
      return false;
    }
    int hasNodata = 0;
    const float nodata = float( GDALGetRasterNoDataValue( band, &hasNodata ) );
    for ( float &v : out )
    {
      if ( !std::isfinite( v ) || ( hasNodata && v == nodata ) )
        v = std::numeric_limits<float>::quiet_NaN();
    }
    GDALClose( ds );
    return true;
  };
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
  // worker 只捕服务 QPointer 不捕 this：工作流若在任务运行中被析构，
  // 池线程解引用 this->m_onnx 是悬垂读。#402：裸指针改 QPointer——
  // 任务排空失败（~PaleoTaskService 明示可孤立 pool）时服务先亡，
  // 入口判活即诚实失败，不再对悬垂服务跑 Ort::Session::Run。
  const QPointer<PaleoOnnxService> onnx = m_onnx;
  const QString title = tr( "%1 AI 相分类 %2" ).arg( horizon, model );
  PaleoTask *task = m_tasks->start(
    title,
    [onnx, model, gridRows, gridCols, tileRows, tileCols, halo, fetch, result]( PaleoTask *t ) -> QString {
      if ( !onnx )
        return QStringLiteral( "ONNX 服务已销毁" );
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
      const bool ok = inferTiles(
        onnx, req, result,
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
  // finished 挂 task 上下文（而非 this）：工作流先亡时收尾仍执行、result
  // 仍被回收；触碰工作流的分支用 self 守卫。
  connect( task, &PaleoTask::finished, task, [self, task, horizon, model, result,
                                              tileRows, tileCols, halo,
                                              lowConfidenceThreshold]() {
    // result 在收尾后才删——写产品仍要读它（先删后用=UAF）。
    if ( task->state() == PaleoTask::State::Succeeded )
    {
      QString err;
      const bool ok =
          self && self->writeTileProducts( horizon, model, *result, tileRows, tileCols, halo,
                                           result->modelSha256,
                                           lowConfidenceThreshold, &err );
      if ( ok )
      {
        emit self->tileClassificationDone( horizon, self->m_lastProductLayerIds );
      }
      else
      {
        // 推理成功但产品落盘/登记失败（含 workflow 已亡、无人写产品的情形）：
        // 任务不能停留在 Succeeded——任务页、聊天工具回灌（toolrunner 按
        // state() 分派）与 dock 卡片必须看到失败，不允许「推理 ok=产品 ok」
        // 的冒充（PaleoTask::failPublication 即为此语义设计：Succeeded→Failed
        // 翻转，观察者读到失败原因）。
        task->failPublication( err );
        if ( self )
          emit self->tileClassificationFailed( horizon, err );
      }
    }
    else if ( task->state() == PaleoTask::State::Failed && self )
    {
      emit self->tileClassificationFailed( horizon, task->errorText() );
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
  // 同步入口无任务上下文，不支持取消（保持旧契约）；异步路径经任务取消位。
  if ( !suggestTrack( m_onnx, model, seeds, windowSamples, radius, fetch, {}, &out, error ) )
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

PaleoTask *AiAssistWorkflow::startSuggestion( const QString &horizon, const QString &model,
                                              const QVector<TrackingSeed> &seeds, int windowSamples,
                                              int radius, const TraceWindowFetcher &fetch,
                                              QString *error )
{
  if ( error )
    error->clear();
  if ( !m_tasks )
  {
    if ( error )
      *error = tr( "异步层位建议需要任务服务（setTaskService）" );
    return nullptr;
  }
  if ( !m_onnx )
  {
    if ( error )
      *error = tr( "AI 辅助未绑定 ONNX 服务" );
    return nullptr;
  }
  // 同 startClassification 范式：池线程只跑纯引擎（onnx 服务线程安全），
  // 建议入队/信号回主线程。取消经任务取消位逐道检查（引擎协作取消）。
  auto *result = new QVector<TrackingSuggestion>;
  QPointer<AiAssistWorkflow> self( this );
  // #402：同 startClassification——worker 捕服务 QPointer 判活，不捕 this。
  const QPointer<PaleoOnnxService> onnx = m_onnx;
  const QString title = tr( "%1 AI 层位建议 %2" ).arg( horizon, model );
  PaleoTask *task = m_tasks->start(
    title,
    [onnx, model, seeds, windowSamples, radius, fetch, result]( PaleoTask *t ) -> QString {
      if ( !onnx )
        return QStringLiteral( "ONNX 服务已销毁" );
      QString err;
      QVector<TrackingSuggestion> out;
      if ( !suggestTrack( onnx, model, seeds, windowSamples, radius, fetch,
                          [t]() { return t->cancelRequested(); }, &out, &err ) )
        return err;
      *result = out;
      return QString();
    },
    QString(), PaleoTask::Priority::Normal, true );
  // finished 挂 task 上下文：工作流先亡时 result 仍被回收（同上）。
  connect( task, &PaleoTask::finished, task, [self, task, horizon, result]() {
    if ( task->state() == PaleoTask::State::Succeeded && self )
    {
      const QVector<TrackingSuggestion> out = *result;
      QVector<SuggestionState> states;
      states.reserve( out.size() );
      for ( const TrackingSuggestion &s : out )
        states.append( SuggestionState { s, s.isSeed } );
      self->m_suggestions.insert( horizon, states );
      emit self->suggestionsReady( horizon, int( out.size() ) );
    }
    delete result;
  } );
  return task;
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
  // error 默认 nullptr（头文件）：所有失败路径经 fail 判空写入。
  const auto fail = [error]( const QString &msg ) {
    if ( error )
      *error = msg;
    return false;
  };
  DerivedAssetRegistrar registrar( m_catalog, m_projectDir );
  if ( !registrar.isBound() )
  {
    return fail( tr( "AI 辅助未绑定 catalog 登记通道（T26）" ) );
  }
  const QVector<TrackingSuggestion> accepted = acceptedSuggestions( horizon );
  if ( accepted.isEmpty() )
  {
    return fail( tr( "层位 %1 没有已接受的建议可提交" ).arg( horizon ) );
  }

  QString stageErr;
  const DerivedStaging st = registrar.stage(
    QStringLiteral( "aitrack" ), tr( "%1 AI 追踪拾取" ).arg( horizon ),
    QStringLiteral( "AITRACK_%1.json" ).arg( aiSafeSegment( horizon ) ), &stageErr );
  if ( !st.isValid() )
  {
    return fail( stageErr );
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
    return fail( tr( "无法写入 %1: %2" ).arg( st.absolutePath, f.errorString() ) );
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
    return fail( commitErr );
  }
  emit acceptedCommitted( horizon, st.relativePath );
  return true;
}
