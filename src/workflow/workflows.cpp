// 层：功能
#include "workflows.h"

#include "../ai/onnxpredictionservice.h" // ORT-free header; symbol refs are PALEO_HAVE_ORT-guarded
#include "../catalog/datacatalog.h"      // localGridCrsWkt — ONNX 栅格落在局部测网
#include "../io/constraintstore.h"
#include "../domain/arearules.h"
#include "../domain/singlefactorrequest.h" // 制图工作场不进融合/分相
#include "../metadata/paleoprojectstore.h"
#include "../qgis/qgiseditingservice.h" // 拓扑提交门（geometryCommitError）
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprocessingservice.h"
// ---- m2(B): 单因素生成链（ConstraintWorkflow::generateFactor/generateContours）----
#include "../qgis/factorcontour.h"
#include "../qgis/factorstylewriter.h"
#include "../qgis/qgisstyleservice.h" // C2：applyFaciesBoundaryStyle（相界语义符号）
#include "../services/singlefactordef.h"
#include "boundarysemantics.h" // C2：相界地质语义类型词表
// ---- m2(B) end ----
#include "../services/projectdata.h"
#include "derivedassets.h"
#include "mappingworkflow.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QVariantList>

#include <qgscoordinatereferencesystem.h>
#include <qgsfield.h>
#include <qgsgeometry.h>
#include <qgsmaplayer.h>
#include <qgsvectorlayer.h> // ---- m2(C)：saveFaciesAttributes 的 edit buffer 回写 ------

#include <gdal.h>
#include <ogr_spatialref.h>
#include <cpl_conv.h>

#include <algorithm>
#include <cmath>
#include <cstring>

// ---------------------------------------------------------------------------
// workflows.h fixes the public shape of these classes and declares no data
// members. Service bindings and per-instance state therefore ride on dynamic
// QObject properties — QObject owns the storage, so nothing needs manual
// cleanup when a workflow is destroyed.
// ---------------------------------------------------------------------------

namespace
{
  const char kCatalogProp[]     = "paleo.wf.catalog";     // QObject* (DataCatalog) — T26 派生产物登记
  const char kProjectDirProp[]  = "paleo.wf.projectdir";  // QString — 受管 artifacts/ 根

  void setError( QString *error, const QString &text )
  {
    if ( error )
      *error = text;
  }

  // C4（wave/deepen-perf）：layerId↔assetId 关联的写侧——派生产物 commit 成功
  // 后把 catalog assetId 盖到已实例化的图层对象（paleoAssetId 自定义属性，
  // 随 .qgz 持久化；LayerPropertiesDialog 业务页 assetIdForLayer 读侧消费）。
  // 层未实例化时跳过（不为了盖章而提前 materialize——首跑由 catalog 版本
  // extra 的 manifest_layer_id 兜底，重跑同 layerId 幂等补章）。
  void stampLayerAssetLink( QgisLayerService *layers, const QString &layerId,
                            const QString &assetId )
  {
    if ( !layers || layerId.isEmpty() || assetId.isEmpty() )
      return;
    if ( QgsMapLayer *l = layers->layer( layerId ) )
      l->setCustomProperty( QStringLiteral( "paleoAssetId" ), assetId );
  }

  // T26：三个写出产物的 workflow 共享的登记通道（动态属性，见文件头注释）。
  DerivedAssetRegistrar derivedRegistrarOf( const QObject *wf, QString *error )
  {
    DataCatalog *catalog = PaleoWorkflowDerivedCatalog( wf );
    if ( !catalog )
    {
      setError( error, QObject::tr( "工作流未绑定数据目录（catalog）——派生产物无法登记到工程" ) );
      return DerivedAssetRegistrar();
    }
    return DerivedAssetRegistrar( catalog, PaleoWorkflowDerivedProjectDir( wf ) );
  }

  // Compact UTC timestamp — makes result layer ids unique per run.
  QString stamp()
  {
    return QDateTime::currentDateTimeUtc().toString( QStringLiteral( "yyyyMMdd-hhmmss-zzz" ) );
  }

  // OUTPUT is the conventional destination key; fall back to the first
  // string-valued result so algorithms with differently named destinations
  // still declare their product.
  QString outputPathOf( const QVariantMap &results )
  {
    QString out = results.value( QStringLiteral( "OUTPUT" ) ).toString();
    if ( out.isEmpty() )
    {
      for ( const QVariant &v : results )
      {
        if ( v.typeId() == QMetaType::QString && !v.toString().isEmpty() )
        {
          out = v.toString();
          break;
        }
      }
    }
    return out;
  }

  // A declared source counts as "file-backed" when its base (before any
  // "|layername=" etc. suffix) is a plain filesystem path. Memory pseudo-URIs
  // and remote/VSI sources are not QFile-checkable and are skipped.
  bool isFileBackedSource( const QString &source )
  {
    if ( source.isEmpty() )
      return false;
    const QString base = source.section( QLatin1Char( '|' ), 0, 0 );
    if ( base.isEmpty() )
      return false;
    if ( base.startsWith( QStringLiteral( "memory" ), Qt::CaseInsensitive ) )
      return false;
    if ( base.contains( QLatin1Char( '?' ) ) )          // memory provider URI ("Point?crs=...")
      return false;
    if ( base.contains( QStringLiteral( "://" ) ) )     // remote URI
      return false;
    if ( base.startsWith( QStringLiteral( "/vsi" ) ) )  // GDAL virtual filesystem
      return false;
    return true;
  }
#if PALEO_HAVE_ORT
  // project_area 工区网格（PROJECT_AREA_PLAN §3 + autoplan eng trap）：ONNX 结果
  // 落在标定层位的工程栅格上（默认 D61 411×641，北向上 geotransform
  // (0, 12793/640, 0, 16406, 0, -16406/410)）。清单里已声明的
  // horizon.<target>* 栅格优先提供 geotransform 与 SRS，读不到时用这组常量；
  // 行列数硬要求 = AreaRules::onnxGrid（工程配置），不是就失败。
  // 注：geotransform 常量仍是本工区兜底——新工区应声明供体栅格（清单
  // horizon.<target>*），否则 GT 会落在 project_area 的原点上。

  struct OnnxAreaGrid
  {
    // (xmin, dx, 0, ymax, 0, -dy) — 原点是左上角像元的外角。
    double gt[6] = { 0.0, 12793.0 / 640.0, 0.0, 16406.0, 0.0, -16406.0 / 410.0 };
    QString projection; // 声明的 D61 栅格自身 SRS（可读时优先于参数/常量）
    QString sourcePath; // 提供几何的 D61 栅格文件（DERIVED 父版本溯源用）
    bool fromDecl = false;
  };

  // 从图层清单取 horizon.D61* 栅格声明的网格几何；声明缺失或文件不可读时
  // 保持常量（常量本来就是同一套 D61 geotransform）。
  OnnxAreaGrid onnxAreaGrid( const QgisLayerService *layers )
  {
    OnnxAreaGrid grid;
    if ( !layers )
      return grid;
    const QString prefix = QStringLiteral( "horizon.%1" )
                               .arg( AreaRules::active().targetHorizon );
    QString source;
    for ( const LayerDeclaration &d : layers->declared() )
    {
      if ( !d.layerId.startsWith( prefix ) ||
           d.type.compare( QStringLiteral( "raster" ), Qt::CaseInsensitive ) != 0 )
        continue;
      source = d.source;
      break;
    }
    if ( source.isEmpty() )
      return grid;
    const QString path = source.section( QLatin1Char( '|' ), 0, 0 );
    if ( path.isEmpty() || !QFile::exists( path ) )
      return grid;
    GDALAllRegister();
    GDALDatasetH ds = GDALOpen( path.toUtf8().constData(), GA_ReadOnly );
    if ( !ds )
      return grid;
    double gt[6] = { 0, 0, 0, 0, 0, 0 };
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
    return grid;
  }

  // 挤成二维（去掉全部长度-1 维）后必须正好是工区网格 411×641，否则不写栅格、
  // 不登记图层——plan 钉死的文案，附实际行列数（标量/点结果同样拒绝）。
  bool resolveOnnxGrid( const QVector<float> &values, const QVector<int64_t> &outShape,
                        int &rows, int &cols, QString *error )
  {
    const qsizetype n = values.size();
    if ( n <= 0 )
    {
      setError( error, QObject::tr( "ONNX output is empty" ) );
      return false;
    }
    QVector<qint64> dims;
    for ( qint64 d : outShape )
    {
      if ( d != 1 )
        dims.append( d );
    }
    // 标量（shape [] 或全 1）按 1×1 报告实际行列数。
    QStringList parts;
    for ( qint64 d : dims )
      parts << QString::number( d );
    const QString actual = parts.isEmpty() && n == 1 ? QStringLiteral( "1×1" )
                                                     : parts.join( QStringLiteral( "×" ) );
    if ( dims.size() == 2 && dims[0] > 0 && dims[1] > 0 &&
         dims[0] * dims[1] == static_cast<qint64>( n ) )
    {
      rows = static_cast<int>( dims[0] );
      cols = static_cast<int>( dims[1] );
    }
    else
    {
      rows = ( n == 1 ) ? 1 : 0;
      cols = ( n == 1 ) ? 1 : 0;
    }
    const AreaRules::OnnxGrid want = AreaRules::active().onnxGrid;
    if ( rows != want.rows || cols != want.cols )
    {
      setError( error, QObject::tr( "结果不是 %1×%2，没有写入栅格（实际 %3）" )
                           .arg( want.rows )
                           .arg( want.cols )
                           .arg( actual.isEmpty() ? QString::number( n ) : actual ) );
      return false;
    }
    return true;
  }

  // 把通过门禁的 411×641 结果写成 Float32 GeoTIFF，geotransform/SRS 用
  // OnnxAreaGrid（D61 声明优先，否则 §3 常量 + 局部测网 CRS）。
  QString writeOnnxRaster( const QString &path, const QVector<float> &values, int rows, int cols,
                          const OnnxAreaGrid &grid, const QVariantMap &params,
                          const QString &model, const QVector<int64_t> &outShape,
                          QString *error )
  {
    GDALAllRegister();
    GDALDriverH drv = GDALGetDriverByName( "GTiff" );
    if ( !drv )
    {
      setError( error, QObject::tr( "GTiff driver is not available" ) );
      return QString();
    }
    if ( QFile::exists( path ) )
      QFile::remove( path );
    GDALDatasetH ds = GDALCreate( drv, path.toUtf8().constData(), cols, rows, 1, GDT_Float32, nullptr );
    if ( !ds )
    {
      setError( error, QObject::tr( "cannot create prediction raster %1" ).arg( path ) );
      return QString();
    }
    double gt[6];
    std::memcpy( gt, grid.gt, sizeof( gt ) );
    GDALSetGeoTransform( ds, gt );
    // SRS 优先级：显式参数 > D61 栅格自身投影 > 局部测网 CRS（与 horizonbinner
    // 一致；绝不把局部米写成经纬度）。
    const QString auth = params.value( QStringLiteral( "crs" ) ).toString();
    if ( !auth.isEmpty() )
    {
      const QgsCoordinateReferenceSystem crs( auth );
      if ( crs.isValid() )
      {
        const QByteArray wkt = crs.toWkt( Qgis::CrsWktVariant::Wkt1Gdal ).toUtf8();
        GDALSetProjection( ds, wkt.constData() );
      }
    }
    else if ( !grid.projection.isEmpty() )
    {
      GDALSetProjection( ds, grid.projection.toUtf8().constData() );
    }
    else
    {
      OGRSpatialReference srs;
      if ( srs.SetFromUserInput( DataCatalog::localGridCrsWkt().toUtf8().constData() ) == OGRERR_NONE )
      {
        char *wkt = nullptr;
        if ( srs.exportToWkt( &wkt ) == OGRERR_NONE && wkt )
          GDALSetProjection( ds, wkt );
        CPLFree( wkt );
      }
    }
    GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
    GDALSetRasterNoDataValue( band, -9999.0 );
    if ( GDALRasterIO( band, GF_Write, 0, 0, cols, rows, const_cast<float *>( values.constData() ),
                       cols, rows, GDT_Float32, 0, 0 ) != CE_None )
    {
      GDALClose( ds );
      QFile::remove( path );
      setError( error, QObject::tr( "failed to write prediction raster %1" ).arg( path ) );
      return QString();
    }

    QJsonObject prov;
    prov.insert( QStringLiteral( "model" ), model );
    prov.insert( QStringLiteral( "rows" ), rows );
    prov.insert( QStringLiteral( "cols" ), cols );
    prov.insert( QStringLiteral( "geotransform_source" ),
                 grid.fromDecl ? QStringLiteral( "horizon.%1" ).arg(
                                     AreaRules::active().targetHorizon )
                               : QStringLiteral( "project_area" ) );
    QJsonArray gtJson;
    for ( int i = 0; i < 6; ++i )
      gtJson.append( gt[i] );
    prov.insert( QStringLiteral( "geotransform" ), gtJson );
    QJsonArray shapeJson;
    for ( qint64 d : outShape )
      shapeJson.append( static_cast<double>( d ) );
    prov.insert( QStringLiteral( "output_shape" ), shapeJson );
    const QByteArray json = QJsonDocument( prov ).toJson( QJsonDocument::Compact );
    GDALSetMetadataItem( ds, "PALEO_MODEL", model.toUtf8().constData(), nullptr );
    GDALSetMetadataItem( ds, "PALEO_PROVENANCE", json.constData(), nullptr );
    GDALClose( ds );
    return path;
  }
#endif
} // namespace

// T26：三个写出产物的 workflow 共享的派生产物登记通道（workflows.h 声明）。
void PaleoWorkflowBindDerivedCatalog( QObject *workflow, DataCatalog *catalog,
                                      const QString &projectDir )
{
  if ( !workflow )
    return;
  if ( auto *pw = qobject_cast<PredictionWorkflow *>( workflow ) )
    pw->setCatalog( catalog, projectDir );
  else if ( auto *cw = qobject_cast<ConstraintWorkflow *>( workflow ) )
    cw->setCatalog( catalog, projectDir );
  else if ( auto *comp = qobject_cast<CompositionWorkflow *>( workflow ) )
    comp->setCatalog( catalog, projectDir );
  else
  {
    workflow->setProperty( kCatalogProp, QVariant::fromValue( static_cast<QObject *>( catalog ) ) );
    workflow->setProperty( kProjectDirProp, projectDir );
  }
}

DataCatalog *PaleoWorkflowDerivedCatalog( const QObject *workflow )
{
  if ( !workflow )
    return nullptr;
  if ( auto *pw = qobject_cast<const PredictionWorkflow *>( workflow ) )
    return pw->catalog();
  if ( auto *cw = qobject_cast<const ConstraintWorkflow *>( workflow ) )
    return cw->catalog();
  if ( auto *comp = qobject_cast<const CompositionWorkflow *>( workflow ) )
    return comp->catalog();
  return qobject_cast<DataCatalog *>( workflow->property( kCatalogProp ).value<QObject *>() );
}

QString PaleoWorkflowDerivedProjectDir( const QObject *workflow )
{
  if ( !workflow )
    return QString();
  if ( auto *pw = qobject_cast<const PredictionWorkflow *>( workflow ) )
    return pw->projectDir();
  if ( auto *cw = qobject_cast<const ConstraintWorkflow *>( workflow ) )
    return cw->projectDir();
  if ( auto *comp = qobject_cast<const CompositionWorkflow *>( workflow ) )
    return comp->projectDir();
  return workflow->property( kProjectDirProp ).toString();
}

// ---------------------------------------------------------------------------
// PredictionWorkflow — ①智能预测
// ---------------------------------------------------------------------------

PredictionWorkflow::PredictionWorkflow( QgisProcessingService *proc, QgisLayerService *layers, QObject *parent )
  : QObject( parent ), m_proc( proc ), m_layers( layers )
{
}

void PredictionWorkflow::setCatalog( DataCatalog *catalog, const QString &projectDir )
{
  m_catalog = catalog;
  m_projectDir = projectDir;
}

void PredictionWorkflow::setOnnxService( PaleoOnnxService *onnx )
{
#if PALEO_HAVE_ORT
  m_onnx = onnx;
#else
  Q_UNUSED( onnx );
#endif
}

#if PALEO_HAVE_ORT
PaleoOnnxService *PredictionWorkflow::onnxService() const
{
  return m_onnx.data();
}
#endif

DataCatalog *PredictionWorkflow::catalog() const
{
  return m_catalog.data();
}

QgisProcessingService *PredictionWorkflow::processingService() const
{
  return m_proc.data();
}

QgisLayerService *PredictionWorkflow::layerService() const
{
  return m_layers.data();
}

QStringList PredictionWorkflow::availableAlgorithms() const
{
  QStringList ids;
  if ( m_proc )
    ids = m_proc->paleoAlgorithmIds();
#if PALEO_HAVE_ORT
  if ( m_onnx )
    for ( const QString &model : m_onnx->availableModels() )
      ids << QStringLiteral( "onnx:%1" ).arg( model );
#endif
  return ids;
}

bool PredictionWorkflow::runPrediction( const QString &horizon, const QString &algorithmId,
                                        const QVariantMap &params, QString *error )
{
#if PALEO_HAVE_ORT
  // "onnx:<model>" dispatches to PaleoOnnxService; every other id falls
  // through to the Processing-registry path below untouched.
  if ( algorithmId.startsWith( QLatin1String( "onnx:" ) ) )
  {
    const QString model = algorithmId.mid( 5 );
    QgisLayerService *layers = m_layers.data();
    PaleoOnnxService *onnx = m_onnx.data();

    const auto fail = [this, &horizon, error]( const QString &msg ) {
      setError( error, msg );
      emit predictionFailed( horizon, msg );
      return false;
    };
    if ( !layers )
      return fail( tr( "prediction workflow is not bound to a layer service" ) );
    if ( !onnx )
      return fail( tr( "no ONNX service bound; cannot run '%1'" ).arg( algorithmId ) );
    if ( model.isEmpty() )
      return fail( tr( "onnx algorithm id '%1' carries no model name" ).arg( algorithmId ) );
    if ( !onnx->isModelLoaded( model ) && !onnx->loadModel( model, error ) )
      return fail( ( error && !error->isEmpty() )
                       ? *error
                       : tr( "failed to load ONNX model '%1'" ).arg( model ) );

    // params-derived tensors; defaults keep toy-model runs trivial:
    // input {0}, shape {1}, inputName "x".
    QVector<float> input;
    const QVariantList inVals = params.value( QStringLiteral( "input" ) ).toList();
    if ( inVals.isEmpty() )
      input.append( 0.0f );
    else
      for ( const QVariant &v : inVals )
        input.append( v.toFloat() );

    QVector<int64_t> shape;
    const QVariantList shapeVals = params.value( QStringLiteral( "shape" ) ).toList();
    if ( shapeVals.isEmpty() )
      shape.append( 1 );
    else
      for ( const QVariant &v : shapeVals )
        shape.append( static_cast<int64_t>( v.toLongLong() ) );

    QString inputName = params.value( QStringLiteral( "inputName" ) ).toString();
    if ( inputName.isEmpty() )
      inputName = QStringLiteral( "x" );

    const OnnxTensor tensor = onnx->runTensor( inputName, input, shape, error );
    if ( tensor.values.isEmpty() )
      return fail( ( error && !error->isEmpty() )
                       ? *error
                       : tr( "ONNX model '%1' produced no output" ).arg( model ) );

    // 网格门禁（plan §1 trap）：输出挤成二维后必须是工区网格 411×641，否则
    // 不写栅格、不登记图层。geotransform 由 horizon.D61* 声明提供，缺省常量。
    const OnnxAreaGrid grid = onnxAreaGrid( layers );
    int rows = 0;
    int cols = 0;
    if ( !resolveOnnxGrid( tensor.values, tensor.shape, rows, cols, error ) )
    {
      return fail( ( error && !error->isEmpty() )
                       ? *error
                       : tr( "cannot place ONNX output on a grid" ) );
    }
    // T26：预测栅格落 artifacts/derived + DERIVED 版本（父版本 = 提供 D61
    // geotransform 的时间栅格版本，可溯源时）。
    QString regErr;
    DerivedAssetRegistrar registrar = derivedRegistrarOf( this, &regErr );
    if ( !registrar.isBound() )
      return fail( regErr );
    const DerivedStaging st = registrar.stage(
        QStringLiteral( "onnx_prediction" ), tr( "%1 onnx %2 预测" ).arg( horizon, model ),
        QStringLiteral( "ONNX_%1_%2.tif" ).arg( horizon, model ), &regErr );
    if ( !st.isValid() )
      return fail( regErr );
    const QString outPath = writeOnnxRaster(
        st.absolutePath, tensor.values, rows, cols, grid, params, model, tensor.shape, error );
    if ( outPath.isEmpty() )
    {
      return fail( ( error && !error->isEmpty() )
                       ? *error
                       : tr( "failed to write ONNX prediction raster" ) );
    }
    QVariantMap onnxExtra;
    onnxExtra.insert( QStringLiteral( "model" ), model );
    onnxExtra.insert( QStringLiteral( "rows" ), rows );
    onnxExtra.insert( QStringLiteral( "cols" ), cols );
    onnxExtra.insert( QStringLiteral( "geotransform_source" ),
                      grid.fromDecl ? QStringLiteral( "horizon.%1" ).arg(
                                          AreaRules::active().targetHorizon )
                                    : QStringLiteral( "project_area" ) );
    const QStringList onnxParents = grid.fromDecl && !grid.sourcePath.isEmpty()
                                        ? registrar.parentVersionIdsFor( QStringList{ grid.sourcePath } )
                                        : QStringList();
    QString commitErr;
    if ( !registrar.commitExternal( st, outPath, onnxParents,
                                    QStringLiteral( "onnxworkflow/%1" ).arg( model ), onnxExtra,
                                    &commitErr ) )
      return fail( commitErr );

    LayerDeclaration decl;
    decl.layerId = QStringLiteral( "pred.%1.onnx.%2" ).arg( horizon, model );
    decl.horizon = horizon;
    decl.type = QStringLiteral( "raster" );
    decl.source = st.absolutePath;
    decl.group = QStringLiteral( "03_Predict" ); // 历史分组保持（m2(A)）
    decl.title = tr( "%1 onnx %2 预测" ).arg( horizon, model );
    if ( !layers->declare( decl, error ) )
      return fail( ( error && !error->isEmpty() )
                       ? *error
                       : tr( "failed to declare result layer '%1'" ).arg( decl.layerId ) );

    // m2(A) 3b：算法无置信度输出 → 不声明伴生层（见
    // confidenceCompanionAvailable 调查注释；接入真实置信度后在此声明
    // confidence.<horizon>，group "02_Prediction"，带色标 styleRef）。
    if ( confidenceCompanionAvailable( algorithmId ) )
    {
      // 当前恒不可达——留作真实置信度输出的声明接入点，禁止造假数据填充。
    }

    emit predictionDone( horizon, decl.layerId );
    return true;
  }
#else
  if ( algorithmId.startsWith( QLatin1String( "onnx:" ) ) )
  {
    const QString msg = tr( "cannot run '%1': this build lacks ONNX Runtime" ).arg( algorithmId );
    setError( error, msg );
    emit predictionFailed( horizon, msg );
    return false;
  }
#endif

  QgisProcessingService *proc = m_proc.data();
  QgisLayerService *layers = m_layers.data();
  if ( !proc || !layers )
  {
    const QString msg = tr( "prediction workflow is not bound to services" );
    setError( error, msg );
    emit predictionFailed( horizon, msg );
    return false;
  }

  // T26：未钉 OUTPUT 的 Processing 结果会落进程临时池（退出即删）——预测产物
  // 同样先 stage 到 artifacts/derived，再作为 DERIVED 版本登记。
  QString regErr;
  DerivedAssetRegistrar registrar = derivedRegistrarOf( this, &regErr );
  if ( !registrar.isBound() )
  {
    setError( error, regErr );
    emit predictionFailed( horizon, regErr );
    return false;
  }
  const DerivedStaging st = registrar.stage(
      QStringLiteral( "prediction_raster" ), tr( "%1 %2 预测" ).arg( horizon, algorithmId ),
      QStringLiteral( "PREDICT_%1.tif" ).arg( horizon ), &regErr );
  if ( !st.isValid() )
  {
    setError( error, regErr );
    emit predictionFailed( horizon, regErr );
    return false;
  }
  QVariantMap runParams = params;
  runParams.insert( QStringLiteral( "OUTPUT" ), st.absolutePath );

  const QVariantMap results = proc->run( algorithmId, runParams, error );
  if ( results.isEmpty() )
  {
    QString msg = tr( "prediction algorithm '%1' produced no results" ).arg( algorithmId );
    if ( error && !error->isEmpty() )
      msg = *error;
    else
      setError( error, msg );
    emit predictionFailed( horizon, msg );
    return false;
  }

  const QString outPath = outputPathOf( results );
  if ( outPath.isEmpty() )
  {
    const QString msg = tr( "prediction algorithm '%1' returned no output path" ).arg( algorithmId );
    setError( error, msg );
    emit predictionFailed( horizon, msg );
    return false;
  }
  // 父版本 = 预测输入图层（文件源可溯源时）。
  QStringList parentPaths;
  if ( const QgsMapLayer *input = params.value( QStringLiteral( "INPUT" ) ).value<QgsMapLayer *>() )
  {
    const QString src = input->source().section( QLatin1Char( '|' ), 0, 0 );
    if ( isFileBackedSource( src ) )
      parentPaths.append( src );
  }
  QString commitErr;
  if ( !registrar.commitExternal( st, outPath, registrar.parentVersionIdsFor( parentPaths ),
                                  QStringLiteral( "predictionworkflow/%1" ).arg( algorithmId ), {},
                                  &commitErr ) )
  {
    setError( error, commitErr );
    emit predictionFailed( horizon, commitErr );
    return false;
  }

  LayerDeclaration decl;
  // m2(A)：稳定结果 id——同一 horizon+algorithmId 重跑复用同一 layerId
  // （manifest INSERT OR REPLACE upsert，清单不新增重复行）； tst_workflows
  // 既有断言 startsWith("predict.<h>.") 由稳定段继续满足。
  decl.layerId = QStringLiteral( "predict.%1.%2" ).arg( horizon, stableResultSuffix( algorithmId ) );
  decl.horizon = horizon;
  decl.type = QStringLiteral( "raster" );
  decl.source = st.absolutePath;
  decl.group = QStringLiteral( "01_Prediction" ); // 历史分组保持（m2(A)：新声明面才用 02_Prediction）
  decl.title = tr( "%1 %2 预测" ).arg( horizon, algorithmId );
  if ( !layers->declare( decl, error ) )
  {
    emit predictionFailed( horizon,
        ( error && !error->isEmpty() )
            ? *error
            : tr( "failed to declare result layer '%1'" ).arg( decl.layerId ) );
    return false;
  }

  // m2(A) 3b：算法无置信度输出 → 不声明伴生层（confidenceCompanionAvailable
  // 对当前算法栈恒 false，见 workflows.h 上的调查注释）。真实置信度通道接入
  // 后在此声明 confidence.<horizon>：group "02_Prediction"、type raster、
  // styleRef 指向置信度色标——禁止常量假栅格冒充。
  if ( confidenceCompanionAvailable( algorithmId ) )
  {
    // 当前恒不可达——留作真实置信度输出的声明接入点。
  }

  emit predictionDone( horizon, decl.layerId );
  return true;
}

QString PredictionWorkflow::stableResultSuffix( const QString &algorithmId )
{
  // algorithmId 净化为 id 片段（"paleo:paleo_geological_smoothing" →
  // "paleo.paleo_geological_smoothing"）：不同 provider 同名算法不会互相
  // 挤占同一 layerId。
  QString s = algorithmId;
  s.replace( QLatin1Char( ':' ), QLatin1Char( '.' ) );
  return s;
}

bool PredictionWorkflow::confidenceCompanionAvailable( const QString &algorithmId )
{
  // 置信度调查结论（2026-09-27，m2(A) 3b）：onnxpredictionservice.cpp 的
  // runTensor 只读 session 首个输出 tensor（GetOutputCount() 仅作下限检查），
  // 没有第二输出/方差通道可取；paleo_* 五个算法（smoothing/IDW/fusion/
  // isopach/polygonize）都是确定性单输出栅格算法。→ 没有真实置信度数据可
  // 落盘，返回 false（调用侧据此跳过 confidence.<horizon> 声明）。
  Q_UNUSED( algorithmId );
  return false;
}

// ---------------------------------------------------------------------------
// ConstraintWorkflow — ②约束与单因素
// ---------------------------------------------------------------------------

ConstraintWorkflow::ConstraintWorkflow( QgisProcessingService *proc, QgisLayerService *layers, QObject *parent )
  : QObject( parent ), m_proc( proc ), m_layers( layers )
{
}

ConstraintWorkflow::~ConstraintWorkflow() = default;

void ConstraintWorkflow::setConstraintStore( ConstraintStore *store )
{
  m_externalConstraintStore = store;
}

void ConstraintWorkflow::setCatalog( DataCatalog *catalog, const QString &projectDir )
{
  m_catalog = catalog;
  m_projectDir = projectDir;
}

void ConstraintWorkflow::setStore( PaleoProjectStore *store )
{
  if ( m_projectStore != store )
  {
    m_projectStore = store;
    m_ownedConstraintStore.reset();
  }
}

DataCatalog *ConstraintWorkflow::catalog() const
{
  return m_catalog.data();
}

QgisProcessingService *ConstraintWorkflow::processingService() const
{
  return m_proc.data();
}

QgisLayerService *ConstraintWorkflow::layerService() const
{
  return m_layers.data();
}

PaleoProjectStore *ConstraintWorkflow::projectStore() const
{
  return m_projectStore.data();
}

ConstraintStore *ConstraintWorkflow::constraintStore() const
{
  if ( m_externalConstraintStore )
    return m_externalConstraintStore;

  if ( m_projectStore && !m_projectStore->gpkgPath().isEmpty() )
  {
    if ( m_ownedConstraintStore && m_ownedConstraintStore->gpkgPath() == m_projectStore->gpkgPath() )
      return m_ownedConstraintStore.get();

    m_ownedConstraintStore = std::make_unique<ConstraintStore>( m_projectStore->gpkgPath(), m_projectStore.data() );
    return m_ownedConstraintStore.get();
  }

  m_ownedConstraintStore.reset();
  return nullptr;
}

bool ConstraintWorkflow::addConstraint( const QString &horizon, const QString &wkt,
                                        const QString &type, int faciesCode, QString *error,
                                        QString *constraintIdOut )
{
  QgisLayerService *layers = m_layers.data();
  if ( !layers )
  {
    setError( error, tr( "constraint workflow is not bound to a layer service" ) );
    return false;
  }
  if ( wkt.trimmed().isEmpty() )
  {
    setError( error, tr( "constraint geometry WKT is empty" ) );
    return false;
  }
  // 拓扑提交门（QGIS_NATIVE_ADOPTION）：约束几何进 IDW 掩膜前经原生验证
  // ——自相交多边形会让掩膜语义失真，如实拒收不静默修形。
  if ( const QString geomErr = QgisEditingService::geometryCommitError(
           QgsGeometry::fromWkt( wkt ), tr( "constraint geometry" ) );
       !geomErr.isEmpty() )
  {
    setError( error, geomErr );
    return false;
  }

  // In-memory constraint record (member-equivalent state via property).
  Constraint c;
  const int seq = ++m_inMemorySeq;
  c.id = QStringLiteral( "c-%1" ).arg( seq );
  c.type = type;
  c.wkt = wkt;
  c.targetFaciesCode = faciesCode;

  ConstraintStore *cs = constraintStore();
  if ( cs )
  {
    if ( !cs->append( horizon, c.id, wkt, type, faciesCode, error ) )
      return false;

    QVariantMap rec = c.toMap();
    rec.insert( QStringLiteral( "horizon" ), horizon );
    rec.insert( QStringLiteral( "facies_code" ), faciesCode );
    m_inMemoryConstraints.append( rec );

    LayerDeclaration decl;
    decl.layerId = QStringLiteral( "constraints.%1" ).arg( horizon );
    decl.horizon = horizon;
    decl.type = QStringLiteral( "vector" );
    decl.source = QStringLiteral( "%1|layername=constraints|subset=horizon='%2'" )
                      .arg( cs->gpkgPath(), horizon );
    decl.group = QStringLiteral( "02_Constraints" );
    if ( !layers->declare( decl, error ) )
    {
      cs->remove( c.id );
      return false;
    }

    if ( constraintIdOut )
      *constraintIdOut = c.id;
    emit constraintAdded( c.id );
    return true;
  }

  // Fallback when no store is configured (preserves memory-layer compatibility)
  QVariantMap rec = c.toMap();
  rec.insert( QStringLiteral( "horizon" ), horizon );
  m_inMemoryConstraints.append( rec );

  QStringList wkts;
  for ( const QVariantMap &m : m_inMemoryConstraints )
  {
    if ( m.value( QStringLiteral( "horizon" ) ).toString() == horizon )
      wkts << m.value( QStringLiteral( "wkt" ) ).toString();
  }

  LayerDeclaration decl;
  decl.layerId = QStringLiteral( "constraints.%1" ).arg( horizon );
  decl.horizon = horizon;
  decl.type = QStringLiteral( "vector" );
  decl.source = QStringLiteral( "memory|%1" ).arg( wkts.join( QLatin1Char( '|' ) ) );
  decl.group = QStringLiteral( "02_Constraints" );
  if ( !layers->declare( decl, error ) )
  {
    m_inMemoryConstraints.removeLast();
    return false;
  }

  if ( constraintIdOut )
    *constraintIdOut = c.id;
  emit constraintAdded( c.id );
  return true;
}

QVector<QVariantMap> ConstraintWorkflow::loadConstraints( const QString &horizon )
{
  ConstraintStore *cs = constraintStore();
  if ( !cs )
  {
    QVector<QVariantMap> res;
    for ( const QVariantMap &m : m_inMemoryConstraints )
    {
      if ( horizon.isEmpty() || m.value( QStringLiteral( "horizon" ) ).toString() == horizon )
        res.append( m );
    }
    return res;
  }

  QVector<QVariantMap> loaded = cs->load( horizon );

  if ( horizon.isEmpty() )
  {
    m_inMemoryConstraints.clear();
  }
  else
  {
    for ( int i = m_inMemoryConstraints.size() - 1; i >= 0; --i )
    {
      if ( m_inMemoryConstraints.at( i ).value( QStringLiteral( "horizon" ) ).toString() == horizon )
        m_inMemoryConstraints.removeAt( i );
    }
  }

  int maxSeq = m_inMemorySeq;
  for ( const QVariantMap &rec : loaded )
  {
    m_inMemoryConstraints.append( rec );
    const QString id = rec.value( QStringLiteral( "id" ) ).toString();
    if ( id.startsWith( QStringLiteral( "c-" ) ) )
    {
      bool ok = false;
      int num = id.mid( 2 ).toInt( &ok );
      if ( ok && num > maxSeq )
        maxSeq = num;
    }
  }
  m_inMemorySeq = maxSeq;

  QgisLayerService *layers = m_layers.data();
  if ( layers && !loaded.isEmpty() )
  {
    if ( !horizon.isEmpty() )
    {
      LayerDeclaration decl;
      decl.layerId = QStringLiteral( "constraints.%1" ).arg( horizon );
      decl.horizon = horizon;
      decl.type = QStringLiteral( "vector" );
      decl.source = QStringLiteral( "%1|layername=constraints|subset=horizon='%2'" )
                        .arg( cs->gpkgPath(), horizon );
      decl.group = QStringLiteral( "02_Constraints" );
      layers->declare( decl );
    }
    else
    {
      QSet<QString> horizons;
      for ( const QVariantMap &rec : loaded )
      {
        const QString h = rec.value( QStringLiteral( "horizon" ) ).toString();
        if ( !h.isEmpty() )
          horizons.insert( h );
      }
      for ( const QString &h : horizons )
      {
        LayerDeclaration decl;
        decl.layerId = QStringLiteral( "constraints.%1" ).arg( h );
        decl.horizon = h;
        decl.type = QStringLiteral( "vector" );
        decl.source = QStringLiteral( "%1|layername=constraints|subset=horizon='%2'" )
                          .arg( cs->gpkgPath(), h );
        decl.group = QStringLiteral( "02_Constraints" );
        layers->declare( decl );
      }
    }
  }

  return loaded;
}

bool ConstraintWorkflow::runConstraintIDW( const QString &horizon, const QString &pointsLayerId,
                                          const QString &field, double cellSize, QString *error )
{
  QgisProcessingService *proc = m_proc.data();
  QgisLayerService *layers = m_layers.data();
  if ( !proc || !layers )
  {
    setError( error, tr( "constraint workflow is not bound to services" ) );
    return false;
  }
  if ( pointsLayerId.isEmpty() )
  {
    setError( error, tr( "没有井点图层" ) );
    return false;
  }
  if ( field.isEmpty() )
  {
    setError( error, tr( "插值字段为空" ) );
    return false;
  }
  if ( !( cellSize > 0.0 ) )
  {
    setError( error, tr( "像元大小必须是正数" ) );
    return false;
  }

  // INPUT is a QgsProcessingParameterFeatureSource: it accepts a QgsMapLayer*
  // variant, so resolve the declared points layer through the layer service.
  QgsMapLayer *points = layers->instantiate( pointsLayerId, error );
  if ( !points )
    return false;

  // T26：IDW 单因素栅格落 artifacts/derived + DERIVED 版本（父版本 = 井点图层
  // 与约束图层的文件源版本）。
  QString regErr;
  DerivedAssetRegistrar registrar = derivedRegistrarOf( this, &regErr );
  if ( !registrar.isBound() )
  {
    setError( error, regErr );
    return false;
  }
  const DerivedStaging st = registrar.stage(
      QStringLiteral( "constraint_idw_raster" ), tr( "%1 约束 IDW" ).arg( horizon ),
      QStringLiteral( "IDW_%1.tif" ).arg( horizon ), &regErr );
  if ( !st.isValid() )
  {
    setError( error, regErr );
    return false;
  }
  QStringList parentPaths{ points->source().section( QLatin1Char( '|' ), 0, 0 ) };

  QVariantMap params;
  params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( points ) );
  params.insert( QStringLiteral( "FIELD" ), field );
  params.insert( QStringLiteral( "CELL_SIZE" ), cellSize );
  params.insert( QStringLiteral( "OUTPUT" ), st.absolutePath );

  QVector<LayerDeclaration> declared;
  QString manifestErr;
  if ( !layers->tryDeclared( &declared, &manifestErr ) )
  {
    setError( error, manifestErr.isEmpty() ? tr( "无法读取图层清单" ) : manifestErr );
    return false;
  }
  const QString constraintLayerId = QStringLiteral( "constraints.%1" ).arg( horizon );
  const bool hasConstraints = std::any_of(
      declared.cbegin(), declared.cend(),
      [&constraintLayerId]( const LayerDeclaration &d ) { return d.layerId == constraintLayerId; } );
  if ( hasConstraints )
  {
    QString constraintErr;
    QgsMapLayer *constraints = layers->instantiate( constraintLayerId, &constraintErr );
    if ( !constraints )
    {
      setError( error, constraintErr.isEmpty()
                           ? tr( "无法加载约束图层 %1" ).arg( constraintLayerId )
                           : constraintErr );
      return false;
    }
    params.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( constraints ) );
    parentPaths.append( constraints->source().section( QLatin1Char( '|' ), 0, 0 ) );
  }

  const QVariantMap results = proc->run( QStringLiteral( "paleo:paleo_constraint_idw" ), params, error );
  if ( results.isEmpty() )
    return false;

  const QString outPath = outputPathOf( results );
  if ( outPath.isEmpty() )
  {
    setError( error, tr( "constraint IDW returned no output path" ) );
    return false;
  }
  QVariantMap idwExtra;
  idwExtra.insert( QStringLiteral( "field" ), field );
  idwExtra.insert( QStringLiteral( "cell_size" ), cellSize );
  idwExtra.insert( QStringLiteral( "constrained" ), hasConstraints );
  QString commitErr;
  if ( !registrar.commitExternal( st, outPath, registrar.parentVersionIdsFor( parentPaths ),
                                  QStringLiteral( "paleo:paleo_constraint_idw" ), idwExtra,
                                  &commitErr ) )
  {
    setError( error, commitErr );
    return false;
  }

  LayerDeclaration decl;
  decl.layerId = QStringLiteral( "factor.%1.idw" ).arg( horizon );
  decl.horizon = horizon;
  decl.type = QStringLiteral( "raster" );
  decl.source = st.absolutePath;
  decl.group = QStringLiteral( "04_SingleFactor" );
  if ( !layers->declare( decl, error ) )
    return false;

  stampLayerAssetLink( layers, decl.layerId, st.assetId ); // C4：已实例化层补盖资产关联
  emit factorDone( horizon, decl.layerId );
  return true;
}

// ---- m2(B) 单因素图页：generateFactor / generateContours ---------------------
namespace
{
  // 原壳（paleomainwindow runIdwRequested 接线）里的井点图层查找，挪进
  // workflow 侧：vector 声明、layerId 以 "wells" 开头，优先当前层位，其次
  // 层位无关（horizon-agnostic）声明。
  QString wellsLayerIdFor( QgisLayerService *layers, const QString &horizon )
  {
    if ( !layers )
      return QString();
    QVector<LayerDeclaration> declared;
    if ( !layers->tryDeclared( &declared ) )
      return QString();
    QString agnostic;
    for ( const LayerDeclaration &d : declared )
    {
      if ( d.type.compare( QStringLiteral( "vector" ), Qt::CaseInsensitive ) != 0 )
        continue;
      if ( !d.layerId.startsWith( QStringLiteral( "wells" ) ) )
        continue;
      if ( !horizon.isEmpty() && d.horizon == horizon )
        return d.layerId;
      if ( agnostic.isEmpty() && d.horizon.isEmpty() )
        agnostic = d.layerId;
    }
    return agnostic;
  }
} // namespace

bool ConstraintWorkflow::generateFactor( const QString &horizon, const QString &factorId,
                                         const QVariantMap &params, QString *error )
{
  QgisProcessingService *proc = m_proc.data();
  QgisLayerService *layers = m_layers.data();
  if ( !proc || !layers )
  {
    setError( error, tr( "constraint workflow is not bound to services" ) );
    return false;
  }
  bool known = false;
  const SingleFactorDefinition def = SingleFactorRegistry::byId( factorId, &known );
  if ( !known )
  {
    setError( error, tr( "未知单因素 id：%1" ).arg( factorId ) );
    return false;
  }

  // 主线6 + C5（wave/deepen-perf）：按引擎分派参数整形。welldist 距离变换
  // 引擎已按冻结契约实现（src/algorithms/distancetransform.cpp）——注册面
  // 缺失时仍显式拒绝（不静默降级成 IDW，那会产出语义错误的栅格）；confidence
  // 的前置在 onnxpredictionservice（只读首个输出张量，无置信度通道），维持
  // 冻结拒绝。契约全文见 docs/progress/mapping.md。
  if ( def.processingAlgId == SingleFactorContracts::welldistEngineId() )
    return generateDistanceFactor( horizon, factorId, def, params, error );
  if ( def.processingAlgId == SingleFactorContracts::confidenceEngineId() )
  {
    setError( error, tr( "单因素 %1 的引擎 %2 尚未接入（前置在 ONNX 置信度"
                         "通道，当前会话只读首个输出张量；见 docs/progress/mapping.md）" )
                         .arg( factorId, def.processingAlgId ) );
    return false;
  }
  if ( def.processingAlgId == QStringLiteral( "paleo:paleo_isopach" ) )
    return generateIsopachFactor( horizon, factorId, def, params, error );
  const QString field = params.value( QStringLiteral( "field" ),
                                      def.defaultParams.value( QStringLiteral( "field" ) ) )
                            .toString();
  const double cellSize = params.value( QStringLiteral( "cellSize" ),
                                        def.defaultParams.value( QStringLiteral( "cellSize" ), 1.0 ) )
                              .toDouble();
  if ( field.isEmpty() )
  {
    setError( error, tr( "插值字段为空" ) );
    return false;
  }
  if ( !( cellSize > 0.0 ) )
  {
    setError( error, tr( "像元大小必须是正数" ) );
    return false;
  }

  const QString overridePoints = params.value( QStringLiteral( "pointsLayerId" ) ).toString();
  const QString pointsId = overridePoints.isEmpty() ? wellsLayerIdFor( layers, horizon )
                                                    : overridePoints;
  if ( pointsId.isEmpty() )
  {
    setError( error, tr( "层位 %1 没有井点图层" ).arg( horizon ) );
    return false;
  }

  // INPUT is a QgsProcessingParameterFeatureSource: resolve the declared points
  // layer through the layer service (same contract as runConstraintIDW).
  QgsMapLayer *points = layers->instantiate( pointsId, error );
  if ( !points )
    return false;

  QString regErr;
  DerivedAssetRegistrar registrar = derivedRegistrarOf( this, &regErr );
  if ( !registrar.isBound() )
  {
    setError( error, regErr );
    return false;
  }
  const DerivedStaging st = registrar.stage(
      QStringLiteral( "single_factor_raster" ), tr( "%1·%2" ).arg( def.title, horizon ),
      QStringLiteral( "FACTOR_%1_%2.tif" ).arg( factorId, horizon ), &regErr );
  if ( !st.isValid() )
  {
    setError( error, regErr );
    return false;
  }
  QStringList parentPaths{ points->source().section( QLatin1Char( '|' ), 0, 0 ) };

  QVariantMap runParams;
  runParams.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( points ) );
  runParams.insert( QStringLiteral( "FIELD" ), field );
  runParams.insert( QStringLiteral( "CELL_SIZE" ), cellSize );
  runParams.insert( QStringLiteral( "OUTPUT" ), st.absolutePath );

  // 约束线屏障随行（同 runConstraintIDW：清单里有该层位的约束图层就带上）。
  QVector<LayerDeclaration> declared;
  QString manifestErr;
  if ( !layers->tryDeclared( &declared, &manifestErr ) )
  {
    setError( error, manifestErr.isEmpty() ? tr( "无法读取图层清单" ) : manifestErr );
    return false;
  }
  const QString constraintLayerId = QStringLiteral( "constraints.%1" ).arg( horizon );
  const bool hasConstraints = std::any_of(
      declared.cbegin(), declared.cend(),
      [&constraintLayerId]( const LayerDeclaration &d ) { return d.layerId == constraintLayerId; } );
  std::unique_ptr<QgsVectorLayer> frozenConstraints;
  if ( hasConstraints )
  {
    QString constraintErr;
    QgsMapLayer *constraints = layers->instantiate( constraintLayerId, &constraintErr );
    const auto frozenPath=property(("paleo.constraint.snapshot."+horizon).toUtf8().constData()).toString();
    if(!frozenPath.isEmpty()) {
      frozenConstraints=std::make_unique<QgsVectorLayer>(frozenPath+"|layername=features","constraints","ogr");
      if(!frozenConstraints->isValid()){setError(error,tr("约束快照无法读取"));return false;}
      constraints=frozenConstraints.get();
    }
    if ( !constraints )
    {
      setError( error, constraintErr.isEmpty()
                           ? tr( "无法加载约束图层 %1" ).arg( constraintLayerId )
                           : constraintErr );
      return false;
    }
    runParams.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( constraints ) );
    parentPaths.append( constraints->source().section( QLatin1Char( '|' ), 0, 0 ) );
  }

  const QVariantMap results = proc->run( def.processingAlgId, runParams, error );
  if ( results.isEmpty() )
    return false;
  const QString outPath = outputPathOf( results );
  if ( outPath.isEmpty() )
  {
    setError( error, tr( "单因素生成未返回输出路径" ) );
    return false;
  }

  QVariantMap extra;
  extra.insert("mapping_product",true);extra.insert("layer_id","product."+st.versionId);
  extra.insert("manifest_layer_id",QStringLiteral("factor.%1.%2").arg(horizon,factorId)); // C4：catalog 侧权威反链（清单 layerId）
  extra.insert("layer_type","raster");extra.insert("title",tr("%1·%2").arg(def.title,horizon));extra.insert("group","04_SingleFactor");
  extra.insert( QStringLiteral( "factor_id" ), factorId );
  extra.insert( QStringLiteral( "field" ), field );
  extra.insert( QStringLiteral( "cell_size" ), cellSize );
  extra.insert( QStringLiteral( "constrained" ), hasConstraints );
  extra.insert(QStringLiteral("horizon"), horizon);
  extra.insert(QStringLiteral("kind"), QStringLiteral("single_factor_raster"));
  parentPaths.append(property(("paleo.constraint.snapshot."+horizon).toUtf8().constData()).toString());
  QStringList parentIds = registrar.parentVersionIdsFor(parentPaths);
  parentIds.append(params.value("parentVersionIds").toStringList());
  parentIds.removeDuplicates();
  if(auto *catalog=PaleoWorkflowDerivedCatalog(this))for(const auto &id:parentIds)
    if(catalog->versionById(id).extra.value("mock").toBool())extra.insert("mock",true);
  QString commitErr;
  if ( !registrar.commitExternal( st, outPath, parentIds,
                                  def.processingAlgId, extra, &commitErr ) )
  {
    setError( error, commitErr );
    return false;
  }

  // 色带样式落盘 <projectDir>/styles/factor_<factorId>.qml（best-effort）与
  // 声明/盖章/通知收尾见 declareFactorResult（三引擎共用）。
  return declareFactorResult( layers, horizon, factorId, def, outPath,
                              registrar.projectDir(), st.assetId, error );
}

bool ConstraintWorkflow::generateIsopachFactor( const QString &horizon, const QString &factorId,
                                                const SingleFactorDefinition &def,
                                                const QVariantMap &params, QString *error )
{
  QgisProcessingService *proc = m_proc.data();
  QgisLayerService *layers = m_layers.data();
  if ( !proc || !layers )
  {
    setError( error, tr( "constraint workflow is not bound to services" ) );
    return false;
  }

  // 等厚引擎的输入是两个已声明的构造面栅格（页面 topLayerId/baseLayerId）。
  const QString topId = params.value( QStringLiteral( "topLayerId" ),
                                      def.defaultParams.value( QStringLiteral( "topLayerId" ) ) )
                            .toString();
  const QString baseId = params.value( QStringLiteral( "baseLayerId" ),
                                       def.defaultParams.value( QStringLiteral( "baseLayerId" ) ) )
                             .toString();
  if ( topId.isEmpty() || baseId.isEmpty() )
  {
    setError( error, tr( "等厚引擎需要顶/底构造面图层（topLayerId/baseLayerId）" ) );
    return false;
  }

  QgsMapLayer *top = layers->instantiate( topId, error );
  if ( !top )
    return false;
  QgsMapLayer *base = layers->instantiate( baseId, error );
  if ( !base )
    return false;
  const auto isRaster = []( QgsMapLayer *l ) {
    return l->type() == Qgis::LayerType::Raster;
  };
  if ( !isRaster( top ) || !isRaster( base ) )
  {
    setError( error, tr( "顶/底输入必须是栅格图层：%1 / %2" ).arg( topId, baseId ) );
    return false;
  }

  QString regErr;
  DerivedAssetRegistrar registrar = derivedRegistrarOf( this, &regErr );
  if ( !registrar.isBound() )
  {
    setError( error, regErr );
    return false;
  }
  const DerivedStaging st = registrar.stage(
      QStringLiteral( "single_factor_raster" ), tr( "%1·%2" ).arg( def.title, horizon ),
      QStringLiteral( "FACTOR_%1_%2.tif" ).arg( factorId, horizon ), &regErr );
  if ( !st.isValid() )
  {
    setError( error, regErr );
    return false;
  }

  QVariantMap runParams;
  runParams.insert( QStringLiteral( "INPUT_TOP" ), QVariant::fromValue( top ) );
  runParams.insert( QStringLiteral( "INPUT_BASE" ), QVariant::fromValue( base ) );
  // 倒置层序（底高于顶）多为重叠/误拾取——默认折 nodata（注册表默认同值）。
  runParams.insert( QStringLiteral( "NEGATIVE_TO_NODATA" ),
                    params.value( QStringLiteral( "negativeToNodata" ),
                                  def.defaultParams.value( QStringLiteral( "negativeToNodata" ), true ) ) );
  runParams.insert( QStringLiteral( "OUTPUT" ), st.absolutePath );

  const QVariantMap results = proc->run( def.processingAlgId, runParams, error );
  if ( results.isEmpty() )
    return false;
  const QString outPath = outputPathOf( results );
  if ( outPath.isEmpty() )
  {
    setError( error, tr( "等厚生成未返回输出路径" ) );
    return false;
  }

  const QStringList parentPaths{
      top->source().section( QLatin1Char( '|' ), 0, 0 ),
      base->source().section( QLatin1Char( '|' ), 0, 0 ) };
  QVariantMap extra;
  extra.insert( QStringLiteral( "factor_id" ), factorId );
  extra.insert( QStringLiteral( "engine" ), def.processingAlgId );
  extra.insert( QStringLiteral( "top_layer_id" ), topId );
  extra.insert( QStringLiteral( "base_layer_id" ), baseId );
  QString commitErr;
  if ( !registrar.commitExternal( st, outPath, registrar.parentVersionIdsFor( parentPaths ),
                                  def.processingAlgId, extra, &commitErr ) )
  {
    setError( error, commitErr );
    return false;
  }

  return declareFactorResult( layers, horizon, factorId, def, outPath,
                              registrar.projectDir(), st.assetId, error );
}

bool ConstraintWorkflow::generateDistanceFactor( const QString &horizon, const QString &factorId,
                                                 const SingleFactorDefinition &def,
                                                 const QVariantMap &params, QString *error )
{
  QgisProcessingService *proc = m_proc.data();
  QgisLayerService *layers = m_layers.data();
  if ( !proc || !layers )
  {
    setError( error, tr( "constraint workflow is not bound to services" ) );
    return false;
  }

  // 引擎按冻结契约 id 解析；注册面缺失 → 显式拒绝（不静默降级 IDW）。
  if ( !proc->algorithmIds().contains( def.processingAlgId ) )
  {
    setError( error, tr( "单因素 %1 的引擎 %2 尚未注册（参数契约已冻结；"
                         "见 docs/progress/mapping.md）" )
                         .arg( factorId, def.processingAlgId ) );
    return false;
  }

  const double cellSize = params.value( QStringLiteral( "cellSize" ),
                                        def.defaultParams.value( QStringLiteral( "cellSize" ), 1.0 ) )
                              .toDouble();
  if ( !( cellSize > 0.0 ) )
  {
    setError( error, tr( "像元大小必须是正数" ) );
    return false;
  }

  const QString overridePoints = params.value( QStringLiteral( "pointsLayerId" ) ).toString();
  const QString pointsId = overridePoints.isEmpty() ? wellsLayerIdFor( layers, horizon )
                                                    : overridePoints;
  if ( pointsId.isEmpty() )
  {
    setError( error, tr( "层位 %1 没有井点图层" ).arg( horizon ) );
    return false;
  }
  QgsMapLayer *points = layers->instantiate( pointsId, error );
  if ( !points )
    return false;

  QString regErr;
  DerivedAssetRegistrar registrar = derivedRegistrarOf( this, &regErr );
  if ( !registrar.isBound() )
  {
    setError( error, regErr );
    return false;
  }
  const DerivedStaging st = registrar.stage(
      QStringLiteral( "single_factor_raster" ), tr( "%1·%2" ).arg( def.title, horizon ),
      QStringLiteral( "FACTOR_%1_%2.tif" ).arg( factorId, horizon ), &regErr );
  if ( !st.isValid() )
  {
    setError( error, regErr );
    return false;
  }
  QStringList parentPaths{ points->source().section( QLatin1Char( '|' ), 0, 0 ) };

  QVariantMap runParams;
  runParams.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( points ) );
  runParams.insert( QStringLiteral( "CELL_SIZE" ), cellSize );
  runParams.insert( QStringLiteral( "OUTPUT" ), st.absolutePath );

  // 约束图层随行（绕障语义在算法侧按 type 分拣：仅 break_line 阻断）。
  QVector<LayerDeclaration> declared;
  QString manifestErr;
  if ( !layers->tryDeclared( &declared, &manifestErr ) )
  {
    setError( error, manifestErr.isEmpty() ? tr( "无法读取图层清单" ) : manifestErr );
    return false;
  }
  const QString constraintLayerId = QStringLiteral( "constraints.%1" ).arg( horizon );
  const bool hasConstraints = std::any_of(
      declared.cbegin(), declared.cend(),
      [&constraintLayerId]( const LayerDeclaration &d ) { return d.layerId == constraintLayerId; } );
  if ( hasConstraints )
  {
    QString constraintErr;
    QgsMapLayer *constraints = layers->instantiate( constraintLayerId, &constraintErr );
    if ( !constraints )
    {
      setError( error, constraintErr.isEmpty()
                           ? tr( "无法加载约束图层 %1" ).arg( constraintLayerId )
                           : constraintErr );
      return false;
    }
    runParams.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( constraints ) );
    parentPaths.append( constraints->source().section( QLatin1Char( '|' ), 0, 0 ) );
  }

  const QVariantMap results = proc->run( def.processingAlgId, runParams, error );
  if ( results.isEmpty() )
    return false;
  const QString outPath = outputPathOf( results );
  if ( outPath.isEmpty() )
  {
    setError( error, tr( "单因素生成未返回输出路径" ) );
    return false;
  }

  QVariantMap extra;
  extra.insert( QStringLiteral( "factor_id" ), factorId );
  extra.insert( QStringLiteral( "engine" ), def.processingAlgId );
  extra.insert( QStringLiteral( "cell_size" ), cellSize );
  extra.insert( QStringLiteral( "constrained" ), hasConstraints );
  extra.insert( QStringLiteral( "horizon" ), horizon );
  extra.insert( QStringLiteral( "kind" ), QStringLiteral( "single_factor_raster" ) );
  extra.insert( QStringLiteral( "manifest_layer_id" ),
                QStringLiteral( "factor.%1.%2" ).arg( horizon, factorId ) ); // C4 反链
  extra.insert( QStringLiteral( "points_layer_id" ), pointsId );
  QString commitErr;
  if ( !registrar.commitExternal( st, outPath, registrar.parentVersionIdsFor( parentPaths ),
                                  def.processingAlgId, extra, &commitErr ) )
  {
    setError( error, commitErr );
    return false;
  }

  // 色带样式落盘（welldist 绿→灰预设已备；best-effort）与收尾同 IDW 路径
  // （declareFactorResult，三引擎共用）。
  return declareFactorResult( layers, horizon, factorId, def, outPath,
                              registrar.projectDir(), st.assetId, error );
}
// 三个单因素引擎（IDW / paleo_isopach / paleo_distance_transform）共用同一份
// 收尾：样式 best-effort 落盘（写失败不拦栅格成果）+ factor.<horizon>.<factorId>
// 栅格声明 + C4 资产关联补盖 + factorGenerated。声明失败不发成功信号。
bool ConstraintWorkflow::declareFactorResult( QgisLayerService *layers,
                                              const QString &horizon, const QString &factorId,
                                              const SingleFactorDefinition &def,
                                              const QString &outPath, const QString &projectDir,
                                              const QString &assetId, QString *error )
{
  if ( !projectDir.isEmpty() )
  {
    QString styleErr; // 降级不打断生成链
    FactorStyleWriter::writeStyleQml( factorId, outPath,
                                      QDir( projectDir ).filePath( QStringLiteral( "styles" ) ),
                                      &styleErr );
  }

  LayerDeclaration decl;
  decl.layerId = QStringLiteral( "factor.%1.%2" ).arg( horizon, factorId );
  decl.horizon = horizon;
  decl.type = QStringLiteral( "raster" );
  decl.source = outPath;
  decl.group = QStringLiteral( "04_SingleFactor" );
  decl.styleRef = def.styleRef;
  decl.title = tr( "%1·%2" ).arg( def.title, horizon );
  if ( !layers->declare( decl, error ) )
    return false;

  stampLayerAssetLink( layers, decl.layerId, assetId ); // C4：已实例化层补盖资产关联
  emit factorGenerated( horizon, factorId, decl.layerId );
  return true;
}

bool ConstraintWorkflow::generateContours( const QString &horizon, const QString &factorLayerId,
                                           double interval, QString *error )
{
  QgisLayerService *layers = m_layers.data();
  if ( !layers )
  {
    setError( error, tr( "constraint workflow is not bound to a layer service" ) );
    return false;
  }
  if ( factorLayerId.isEmpty() )
  {
    setError( error, tr( "缺少单因素图层 id" ) );
    return false;
  }
  if ( !( interval > 0.0 ) )
  {
    setError( error, tr( "等值线间距必须是正数" ) );
    return false;
  }

  // 输入必须是已声明的单因素栅格（等值线是对栅格的 §12 LineString 派生）。
  QVector<LayerDeclaration> declared;
  QString readErr;
  if ( !layers->tryDeclared( &declared, &readErr ) )
  {
    setError( error, readErr.isEmpty() ? tr( "无法读取图层清单" ) : readErr );
    return false;
  }
  const LayerDeclaration *factorDecl = nullptr;
  for ( const LayerDeclaration &d : declared )
  {
    if ( d.layerId == factorLayerId )
    {
      factorDecl = &d;
      break;
    }
  }
  if ( !factorDecl )
  {
    setError( error, tr( "图层 %1 未在清单声明" ).arg( factorLayerId ) );
    return false;
  }
  if ( factorDecl->type.compare( QStringLiteral( "raster" ), Qt::CaseInsensitive ) != 0 )
  {
    setError( error, tr( "等值线输入必须是栅格图层：%1" ).arg( factorLayerId ) );
    return false;
  }
  const QString rasterPath = factorDecl->source.section( QLatin1Char( '|' ), 0, 0 );

  // layerId "factor.<horizon>.<factorId>" → 尾段 factorId（命名失败安全退化）。
  QString factorId = factorLayerId;
  const QString factorPrefix = QStringLiteral( "factor.%1." ).arg( horizon );
  if ( factorLayerId.startsWith( factorPrefix ) )
    factorId = factorLayerId.mid( factorPrefix.size() );

  QString regErr;
  DerivedAssetRegistrar registrar = derivedRegistrarOf( this, &regErr );
  if ( !registrar.isBound() )
  {
    setError( error, regErr );
    return false;
  }
  const DerivedStaging st = registrar.stage(
      QStringLiteral( "contour_lines" ),
      tr( "等值线·%1" ).arg( SingleFactorRegistry::titleFor( factorId ) ),
      QStringLiteral( "CONTOURS_%1_%2.gpkg" ).arg( factorId, horizon ), &regErr );
  if ( !st.isValid() )
  {
    setError( error, regErr );
    return false;
  }

  QString contourErr;
  if ( !FactorContourService::generateContours( rasterPath, st.absolutePath, interval, &contourErr ) )
  {
    setError( error, contourErr );
    return false;
  }

  QVariantMap extra;
  extra.insert("mapping_product",true);extra.insert("layer_id","product."+st.versionId);extra.insert("horizon",horizon);extra.insert("kind","contour_lines");
  extra.insert("layer_type","vector");extra.insert("source_suffix","|layername=contours");extra.insert("title",tr("%1 等值线").arg(horizon));extra.insert("group","04_SingleFactor/Contours");
  extra.insert( QStringLiteral( "interval" ), interval );
  extra.insert( QStringLiteral( "factor_layer_id" ), factorLayerId );
  if(auto *catalog=PaleoWorkflowDerivedCatalog(this))
    for(const auto &id:registrar.parentVersionIdsFor({rasterPath}))
      if(catalog->versionById(id).extra.value("mock").toBool())extra.insert("mock",true);
  QString commitErr;
  if ( !registrar.commitExternal( st, st.absolutePath,
                                  registrar.parentVersionIdsFor( { rasterPath } ),
                                  QStringLiteral( "gdal_contour_c_api" ), extra, &commitErr ) )
  {
    setError( error, commitErr );
    return false;
  }

  LayerDeclaration decl;
  decl.layerId = QStringLiteral( "contours.%1.%2" ).arg( horizon, factorId );
  decl.horizon = horizon;
  decl.type = QStringLiteral( "vector" );
  decl.source = QStringLiteral( "%1|layername=contours" ).arg( st.absolutePath );
  decl.group = QStringLiteral( "04_SingleFactor/Contours" );
  decl.title = tr( "等值线·%1" ).arg( SingleFactorRegistry::titleFor( factorId ) );
  if ( !layers->declare( decl, error ) )
    return false;

  stampLayerAssetLink( layers, decl.layerId, st.assetId ); // C4：已实例化层补盖资产关联
  emit contoursGenerated( horizon, factorLayerId, decl.layerId );
  return true;
}
// ---- m2(B) end ----------------------------------------------------------------

// ---------------------------------------------------------------------------
// CompositionWorkflow — ③综合编图
// ---------------------------------------------------------------------------

CompositionWorkflow::CompositionWorkflow( QgisProcessingService *proc, QgisLayerService *layers, QObject *parent )
  : QObject( parent ), m_proc( proc ), m_layers( layers )
{
}

void CompositionWorkflow::setCatalog( DataCatalog *catalog, const QString &projectDir )
{
  m_catalog = catalog;
  m_projectDir = projectDir;
}

DataCatalog *CompositionWorkflow::catalog() const
{
  return m_catalog.data();
}

QgisProcessingService *CompositionWorkflow::processingService() const
{
  return m_proc.data();
}

QgisLayerService *CompositionWorkflow::layerService() const
{
  return m_layers.data();
}

bool CompositionWorkflow::fuseFactors( const QString &horizon, const QStringList &factorLayerIds,
                                       QString *error )
{
  QgisProcessingService *proc = m_proc.data();
  QgisLayerService *layers = m_layers.data();
  if ( !proc || !layers )
  {
    setError( error, tr( "composition workflow is not bound to services" ) );
    return false;
  }
  if ( factorLayerIds.isEmpty() )
  {
    setError( error, tr( "no factor layers supplied for fusion" ) );
    return false;
  }

  QVector<LayerDeclaration> decls;
  QString readErr;
  if ( !layers->tryDeclared( &decls, &readErr ) )
  {
    setError( error, readErr.isEmpty() ? tr( "无法读取图层清单" ) : readErr );
    return false;
  }

  QVariantList inputs;
  inputs.reserve( factorLayerIds.size() );
  QStringList parentPaths;
  for ( const QString &layerId : factorLayerIds )
  {
    const LayerDeclaration *decl = nullptr;
    for ( const LayerDeclaration &d : decls )
    {
      if ( d.layerId == layerId )
      {
        decl = &d;
        break;
      }
    }
    if ( !decl )
    {
      setError( error, tr( "图层 %1 未在清单声明" ).arg( layerId ) );
      return false;
    }
    if ( paleo::singlefactor::isCartographicProductLayer( layerId, decl->group ) )
    {
      setError( error, tr( "解释性制图工作场不能参与连续融合、分相或厚度统计" ) );
      return false;
    }
    QgsMapLayer *layer = layers->instantiate( layerId, error );
    if ( !layer )
      return false;
    inputs.append( QVariant::fromValue( layer ) );
    parentPaths.append( layer->source().section( QLatin1Char( '|' ), 0, 0 ) );
  }

  // T26：融合栅格落 artifacts/derived + DERIVED 版本（父版本 = 各单因素栅格）。
  QString regErr;
  DerivedAssetRegistrar registrar = derivedRegistrarOf( this, &regErr );
  if ( !registrar.isBound() )
  {
    setError( error, regErr );
    return false;
  }
  const DerivedStaging st = registrar.stage(
      QStringLiteral( "facies_fusion_raster" ), tr( "%1 相融合" ).arg( horizon ),
      QStringLiteral( "FUSION_%1.tif" ).arg( horizon ), &regErr );
  if ( !st.isValid() )
  {
    setError( error, regErr );
    return false;
  }

  QVariantMap params;
  params.insert( QStringLiteral( "INPUTS" ), inputs );
  params.insert( QStringLiteral( "OUTPUT" ), st.absolutePath );

  const QVariantMap results = proc->run( QStringLiteral( "paleo:paleo_facies_fusion" ), params, error );
  if ( results.isEmpty() )
    return false;

  const QString outPath = outputPathOf( results );
  if ( outPath.isEmpty() )
  {
    setError( error, tr( "facies fusion returned no output path" ) );
    return false;
  }
  QVariantMap fusionExtra;
  fusionExtra.insert( QStringLiteral( "factors" ), factorLayerIds );
  QString commitErr;
  if ( !registrar.commitExternal( st, outPath, registrar.parentVersionIdsFor( parentPaths ),
                                  QStringLiteral( "paleo:paleo_facies_fusion" ), fusionExtra,
                                  &commitErr ) )
  {
    setError( error, commitErr );
    return false;
  }

  LayerDeclaration decl;
  decl.layerId = QStringLiteral( "composite.%1" ).arg( horizon );
  decl.horizon = horizon;
  decl.type = QStringLiteral( "raster" );
  decl.source = st.absolutePath;
  decl.group = QStringLiteral( "03_Composite" );
  if ( !layers->declare( decl, error ) )
    return false;

  stampLayerAssetLink( layers, decl.layerId, st.assetId ); // C4：已实例化层补盖资产关联
  emit compositionDone( horizon, decl.layerId );
  return true;
}

bool CompositionWorkflow::deriveFaciesPolygons( const QString &horizon, const QString &rasterLayerId,
                                               const QVariantMap &params, QString *error )
{
  const auto fail = [this, &horizon, error]( const QString &msg ) {
    setError( error, msg );
    emit faciesPolygonsFailed( horizon, msg );
    return false;
  };

  QgisProcessingService *proc = m_proc.data();
  QgisLayerService *layers = m_layers.data();
  if ( !proc || !layers )
    return fail( tr( "composition workflow is not bound to services" ) );
  if ( rasterLayerId.isEmpty() )
    return fail( tr( "no raster layer supplied for facies polygons" ) );

  QVector<LayerDeclaration> declared;
  QString manifestErr;
  if ( !layers->tryDeclared( &declared, &manifestErr ) )
    return fail( manifestErr.isEmpty() ? tr( "无法读取图层清单" ) : manifestErr );

  QString declHorizon;
  QString declGroup;
  bool found = false;
  for ( const LayerDeclaration &d : declared )
  {
    if ( d.layerId == rasterLayerId )
    {
      declHorizon = d.horizon;
      declGroup = d.group;
      found = true;
      break;
    }
  }
  if ( !found )
    return fail( tr( "raster layer '%1' is not declared" ).arg( rasterLayerId ) );
  if ( paleo::singlefactor::isCartographicProductLayer( rasterLayerId, declGroup ) )
    return fail( tr( "解释性制图工作场不能参与分相或转面" ) );

  const QString h = !declHorizon.isEmpty() ? declHorizon : horizon;
  if ( h.isEmpty() )
    return fail( tr( "cannot derive facies polygons without a horizon" ) );

  QgsMapLayer *raster = layers->instantiate( rasterLayerId, error );
  if ( !raster )
  {
    return fail( ( error && !error->isEmpty() )
                     ? *error
                     : tr( "failed to instantiate raster '%1'" ).arg( rasterLayerId ) );
  }

  // T26：相多边形 gpkg 落 artifacts/derived + DERIVED 版本（父版本 = 被多边形
  // 化的栅格与约束图层的文件源版本）。
  QString regErr;
  DerivedAssetRegistrar registrar = derivedRegistrarOf( this, &regErr );
  if ( !registrar.isBound() )
    return fail( regErr );
  const DerivedStaging st = registrar.stage(
      QStringLiteral( "facies_polygons" ), tr( "%1 相多边形" ).arg( h ),
      QStringLiteral( "FACIES_%1.gpkg" ).arg( h ), &regErr );
  if ( !st.isValid() )
    return fail( regErr );
  QStringList parentPaths{ raster->source().section( QLatin1Char( '|' ), 0, 0 ) };

  QVariantMap alg;
  alg.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( raster ) );
  alg.insert( QStringLiteral( "OUTPUT" ), st.absolutePath );
  const QStringList forwarded = {
      QStringLiteral( "MIN_AREA" ), QStringLiteral( "SIMPLIFY" ),
      QStringLiteral( "SNAP_TOLERANCE" ), QStringLiteral( "ANGLE_TOLERANCE" ) };
  for ( const QString &key : forwarded )
  {
    if ( params.contains( key ) )
      alg.insert( key, params.value( key ) );
  }
  const QString constraintId = params.value( QStringLiteral( "CONSTRAINT_LAYER" ) ).toString();
  if ( !constraintId.isEmpty() )
  {
    QgsMapLayer *constraints = layers->instantiate( constraintId, error );
    if ( !constraints )
    {
      return fail( ( error && !error->isEmpty() )
                       ? *error
                       : tr( "failed to instantiate constraints '%1'" ).arg( constraintId ) );
    }
    alg.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( constraints ) );
    parentPaths.append( constraints->source().section( QLatin1Char( '|' ), 0, 0 ) );
  }

  const QVariantMap results = proc->run( QStringLiteral( "paleo:paleo_facies_polygonize" ), alg, error );
  if ( results.isEmpty() )
  {
    return fail( ( error && !error->isEmpty() )
                     ? *error
                     : tr( "facies polygonize produced no results" ) );
  }
  const QString outPath = outputPathOf( results );
  if ( outPath.isEmpty() )
    return fail( tr( "facies polygonize returned no output path" ) );
  QVariantMap faciesExtra;
  faciesExtra.insert( QStringLiteral( "raster_layer" ), rasterLayerId );
  faciesExtra.insert( QStringLiteral( "constrained" ), !constraintId.isEmpty() );
  QString commitErr;
  if ( !registrar.commitExternal( st, outPath, registrar.parentVersionIdsFor( parentPaths ),
                                  QStringLiteral( "paleo:paleo_facies_polygonize" ), faciesExtra,
                                  &commitErr ) )
    return fail( commitErr );

  LayerDeclaration decl;
  decl.layerId = QStringLiteral( "facies.%1" ).arg( h );
  decl.horizon = h;
  decl.type = QStringLiteral( "vector" );
  decl.source = QStringLiteral( "%1|layername=facies_polygons" ).arg( st.absolutePath );
  decl.group = QStringLiteral( "05_PaleoMap" );
  if ( !layers->declare( decl, error ) )
  {
    return fail( ( error && !error->isEmpty() )
                     ? *error
                     : tr( "failed to declare facies layer '%1'" ).arg( decl.layerId ) );
  }

  stampLayerAssetLink( layers, decl.layerId, st.assetId ); // C4：已实例化层补盖资产关联
  emit faciesPolygonsReady( h, decl.layerId );
  return true;
}

// ---- m2(C)：相属性回写 -------------------------------------------------------
QString CompositionWorkflow::prepareFaciesForEditing( const QString &layerId, QString *error )
{
  const auto fail = [error]( const QString &msg ) {
    setError( error, msg );
    return QString();
  };

  QgisLayerService *layers = m_layers.data();
  if ( !layers )
    return fail( tr( "composition workflow is not bound to services" ) );
  if ( layerId.isEmpty() )
    return fail( tr( "no facies layer supplied" ) );

  QVector<LayerDeclaration> declared;
  QString manifestErr;
  if ( !layers->tryDeclared( &declared, &manifestErr ) )
    return fail( manifestErr.isEmpty() ? tr( "无法读取图层清单" ) : manifestErr );
  LayerDeclaration decl;
  bool found = false;
  for ( const LayerDeclaration &d : declared )
  {
    if ( d.layerId == layerId )
    {
      decl = d;
      found = true;
      break;
    }
  }
  if ( !found )
    return fail( tr( "layer '%1' is not declared" ).arg( layerId ) );

  const QString srcPath = decl.source.section( QLatin1Char( '|' ), 0, 0 );
  const QString srcSuffix = decl.source.contains( QLatin1Char( '|' ) )
                                ? decl.source.section( QLatin1Char( '|' ), 1 )
                                : QString();
  if ( !QFileInfo( srcPath ).isFile() )
    return layerId; // 非文件源（memory 等）——交给常规编辑路径
  const QFile::Permissions perms = QFile::permissions( srcPath );
  if ( perms & ( QFile::WriteOwner | QFile::WriteUser | QFile::WriteGroup | QFile::WriteOther ) )
    return layerId; // 已可写（不是 T26 只读派生件）——原样

  const QString projectDir = PaleoWorkflowDerivedProjectDir( this );
  if ( projectDir.isEmpty() )
    return fail( tr( "无法定位工程目录来铺相界工作副本（%1）" ).arg( layerId ) );
  QString safeId = layerId;
  safeId.replace( QLatin1Char( '/' ), QLatin1Char( '_' ) )
      .replace( QLatin1Char( '\\' ), QLatin1Char( '_' ) );
  const QString workDir =
      QDir( projectDir ).filePath( QStringLiteral( "artifacts/layers/facies" ) );
  const QString workPath = QDir( workDir ).filePath( QStringLiteral( "%1.gpkg" ).arg( safeId ) );
  if ( !QDir().mkpath( workDir ) )
    return fail( tr( "cannot create %1" ).arg( workDir ) );
  if ( QFileInfo::exists( workPath ) && !QFile::remove( workPath ) )
    return fail( tr( "cannot replace stale working copy %1" ).arg( workPath ) );
  if ( !QFile::copy( srcPath, workPath ) )
    return fail( tr( "cannot copy %1 → %2" ).arg( srcPath, workPath ) );
  QFile::setPermissions( workPath,
                         QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                             QFileDevice::ReadUser | QFileDevice::WriteUser |
                             QFileDevice::ReadGroup | QFileDevice::ReadOther );

  // manifest 同 id 重指（layerId 不变——页面/导出/引用方无感）；只读原件
  // 留在 artifacts/derived，catalog 版本 sha 不动。
  LayerDeclaration work = decl;
  work.source = srcSuffix.isEmpty() ? workPath
                                    : QStringLiteral( "%1|%2" ).arg( workPath, srcSuffix );
  QString declErr;
  if ( !layers->declare( work, &declErr ) )
    return fail( declErr );

  // 实例缓存里可能压着只读旧层：按层位整组释放（manifest 是权威，图层按
  // 需重实例化；实例由 QgsProject 持有，removeMapLayer 即析构）。
  if ( layers->isInstantiated( layerId ) )
    layers->releaseHorizon( decl.horizon );
  return layerId;
}

bool CompositionWorkflow::saveFaciesAttributes( const QString &layerId, const QVariantMap &attrs,
                                                QString *error )
{
  const auto fail = [error]( const QString &msg ) {
    setError( error, msg );
    return false;
  };

  QgisLayerService *layers = m_layers.data();
  if ( !layers )
    return fail( tr( "composition workflow is not bound to services" ) );
  if ( layerId.isEmpty() )
    return fail( tr( "no facies layer supplied" ) );

  QgsMapLayer *l = layers->instantiate( layerId, error );
  auto *vl = qobject_cast<QgsVectorLayer *>( l );
  if ( !vl )
    return fail( ( error && !error->isEmpty() )
                     ? *error
                     : tr( "layer '%1' is not a vector layer" ).arg( layerId ) );

  // 页面拿不到选中要素 id：以图层当前选中集为准（空集 → 拒绝并说明）。
  QgsFeatureIds selected = vl->selectedFeatureIds();
  if ( selected.isEmpty() )
    return fail( tr( "先在地图上选中要改相属性的要素（%1）" ).arg( layerId ) );

  // 只读派生件兜底：startEditing 失败且源只读 → 铺工作副本重指后重试一次
  // （fid 跨实例稳定，选中集随行迁移）。
  if ( !vl->isEditable() && !vl->startEditing() )
  {
    QString prepErr;
    const QString prepared = prepareFaciesForEditing( layerId, &prepErr );
    if ( prepared.isEmpty() )
      return fail( tr( "cannot start editing on '%1'（%2）" ).arg( layerId, prepErr ) );
    vl = qobject_cast<QgsVectorLayer *>( layers->instantiate( layerId, error ) );
    if ( !vl )
      return fail( ( error && !error->isEmpty() )
                       ? *error
                       : tr( "failed to re-instantiate '%1' after preparing edit copy" )
                             .arg( layerId ) );
    vl->select( selected );
    if ( !vl->startEditing() )
      return fail( tr( "cannot start editing on '%1'" ).arg( layerId ) );
  }

  // 字段词表：facies_code 是 polygonize 算法产物（int）；facies_type/comment
  // 为编辑面字段，缺则先补建（补建也在编辑会话内，各自成 undo 步）。
  // boundary_kind 是相界地质语义类型（C2，词表 BoundarySemantics——值域
  // 校验在页面下拉侧，这里按词面落库，未知词留给地质评审口径）。
  struct FieldSpec
  {
    const char *key;
    const char *name;
    QVariant::Type type;
  };
  const FieldSpec wanted[] = {
    { "facies_code", "facies_code", QVariant::Int },
    { "facies_type", "facies_type", QVariant::String },
    { "comment", "comment", QVariant::String },
    { "boundary_kind", "boundary_kind", QVariant::String },
  };

  // 只写 attrs 携带的字段。
  QVector<QPair<int, QVariant>> writes; // resolved field index + value
  for ( const FieldSpec &f : wanted )
  {
    if ( !attrs.contains( QLatin1String( f.key ) ) )
      continue;
    QVariant value = attrs.value( QLatin1String( f.key ) );
    if ( value.type() == QVariant::String && f.type == QVariant::Int )
    {
      // 页面校验过整数，这里兜底：非整数串如实拒绝，不静默落 0。
      bool ok = false;
      const int code = value.toInt( &ok );
      if ( !ok )
        return fail( tr( "相代码须是整数：%1" ).arg( value.toString() ) );
      value = code;
    }
    int idx = vl->fields().lookupField( QLatin1String( f.name ) );
    if ( idx < 0 )
    {
      if ( !vl->addAttribute( QgsField( QLatin1String( f.name ), f.type ) ) )
        return fail( tr( "cannot add field '%1' to %2" ).arg( QLatin1String( f.name ), layerId ) );
      idx = vl->fields().lookupField( QLatin1String( f.name ) );
      if ( idx < 0 )
        return fail( tr( "field '%1' not visible after add" ).arg( QLatin1String( f.name ) ) );
    }
    writes.append( qMakePair( idx, value ) );
  }

  vl->beginEditCommand( tr( "编辑相属性" ) );
  for ( const auto &w : writes )
    for ( const QgsFeatureId fid : selected )
      vl->changeAttributeValue( fid, w.first, w.second );
  vl->endEditCommand();

  // C2：boundary_kind 变更即时反映到图面（相界语义符号映射——渲染器按
  // boundary_kind 分类描边；无该字段的层为无操作）。
  if ( attrs.contains( QLatin1String( "boundary_kind" ) ) )
    QgisStyleService::applyFaciesBoundaryStyle( vl );
  vl->triggerRepaint();
  return true;
}
// ---- m2(C) end --------------------------------------------------------------

// ---------------------------------------------------------------------------
// ValidationWorkflow — ④验证
// ---------------------------------------------------------------------------

ValidationWorkflow::ValidationWorkflow( QgisLayerService *layers, PaleoProjectStore *store, QObject *parent )
  : QObject( parent ), m_layers( layers ), m_store( store )
{
}

void ValidationWorkflow::setProjectData( ProjectDataFacade *projectData )
{
  m_projectData = projectData;
}

ProjectDataFacade *ValidationWorkflow::projectData() const
{
  return m_projectData.data();
}

void ValidationWorkflow::setResidualThresholdMs( double thresholdMs )
{
  m_residualThresholdMs = thresholdMs;
}

QVariantList ValidationWorkflow::lastResidualRows() const
{
  return m_residualRows;
}

QgisLayerService *ValidationWorkflow::layerService() const
{
  return m_layers.data();
}

PaleoProjectStore *ValidationWorkflow::projectStore() const
{
  return m_store.data();
}

QList<ValidationIssue> ValidationWorkflow::validate()
{
  QList<ValidationIssue> issues;
  QgisLayerService *layers = m_layers.data();
  PaleoProjectStore *store = m_store.data();
  QVector<LayerDeclaration> decls;
  if ( layers )
  {
    QString manifestErr;
    if ( !layers->tryDeclared( &decls, &manifestErr ) )
    {
      // Manifest unreadable is a finding, not a clean bill — otherwise a
      // corrupt store validates as "no issues".
      ValidationIssue v;
      v.severity = ValidationIssue::Error;
      v.code = QStringLiteral( "MANIFEST_READ_FAILED" );
      v.message = manifestErr.isEmpty() ? tr( "无法读取图层清单" ) : manifestErr;
      issues.append( v );
    }
  }

  // (a) declared raster/vector layers whose file source is missing on disk.
  for ( const LayerDeclaration &d : decls )
  {
    const bool fileType = d.type.compare( QStringLiteral( "raster" ), Qt::CaseInsensitive ) == 0 ||
                          d.type.compare( QStringLiteral( "vector" ), Qt::CaseInsensitive ) == 0;
    if ( !fileType || !isFileBackedSource( d.source ) )
      continue;
    const QString base = d.source.section( QLatin1Char( '|' ), 0, 0 );
    if ( !QFile::exists( base ) )
    {
      ValidationIssue v;
      v.severity = ValidationIssue::Error;
      v.code = QStringLiteral( "SRC_MISSING" );
      v.message = tr( "已声明图层 %1 的源文件在磁盘上不存在：%2" ).arg( d.layerId, base );
      v.layerId = d.layerId;
      v.horizon = d.horizon;
      issues.append( v );
    }
  }

  // (b) duplicate horizon names. The manifest's horizons() set is DISTINCT, so
  // an exact duplicate cannot occur; what slips through is a collision after
  // normalization (case / stray whitespace) — e.g. "T1" vs "t1".
  {
    QSet<QString> raw;
    for ( const LayerDeclaration &d : decls )
      if ( !d.horizon.isEmpty() )
        raw.insert( d.horizon );

    QHash<QString, QStringList> byNormalized;
    for ( const QString &h : raw )
      byNormalized[h.trimmed().toLower()].append( h );

    for ( auto it = byNormalized.constBegin(); it != byNormalized.constEnd(); ++it )
    {
      if ( it.value().size() < 2 )
        continue;
      QStringList names = it.value();
      names.sort();
      ValidationIssue v;
      v.severity = ValidationIssue::Warning;
      v.code = QStringLiteral( "DUP_HORIZON" );
      v.message = tr( "层位名冲突（归一化后相同）：%1" ).arg( names.join( QStringLiteral( " / " ) ) );
      v.horizon = names.first();
      issues.append( v );
    }
  }

  // (c) busy-layer conflicts — any declared layer still held by a running task.
  if ( store )
  {
    for ( const LayerDeclaration &d : decls )
    {
      QString reason;
      if ( store->layerBusy( d.layerId, &reason ) )
      {
        ValidationIssue v;
        v.severity = ValidationIssue::Info;
        v.code = QStringLiteral( "BUSY" );
        v.message = tr( "图层 %1 正忙：%2" ).arg( d.layerId, reason );
        v.layerId = d.layerId;
        v.horizon = d.horizon;
        issues.append( v );
      }
    }
  }

  // (d) 井上标定层位时间残差（autoplan §5C）— 只评 targetHorizon（工程参数，
  // 本工区 D61）。每口井一行（验证页残差表渲染源，存 paleo.wf.residualRows）；
  // |r|>阈值 → TIME_RESIDUAL 问题。
  // 默认阈值 10 ms；非数值行（无分层/TD 原因/不在测网内/落在空道）不成问题。
  {
    const QString tgtHorizon = AreaRules::active().targetHorizon;
    QVariantList rowMaps;
    if ( auto *pd = m_projectData.data() )
    {
      const double prop = m_residualThresholdMs;
      const double threshold = prop > 0.0 ? prop : 10.0;
      const QList<TimeResidualRow> rows =
          computeTimeResiduals( pd, tgtHorizon, threshold );
      // 残差行所属栅格图层（问题行/残差行的地图缩放目标与联动 layerId）。
      const HorizonRasterInfo rasterInfo =
          pd->horizonRasterDecl( tgtHorizon );
      const QString rasterLayerId = rasterInfo.layerId;
      // T25：有井但一行残差都没有 → 栅格没声明或打不开，残差检查其实没跑成；
      // 发 RASTER_MISSING 让问题表/摘要行说清原因，不和「还没计算」混为一谈。
      // 声明在而文件缺/打不开时复述底座原因（lastError），不写「还没有」。
      if ( rows.isEmpty() && !pd->wells().isEmpty() )
      {
        ValidationIssue v;
        v.severity = ValidationIssue::Warning;
        v.code = QStringLiteral( "RASTER_MISSING" );
        v.message = rasterLayerId.isEmpty()
                        ? tr( "层位 %1 还没有时间栅格" ).arg( tgtHorizon )
                        : ( pd->lastError().isEmpty()
                                ? tr( "层位 %1 的时间栅格不可用" ).arg( tgtHorizon )
                                : pd->lastError() );
        v.horizon = tgtHorizon;
        v.layerId = rasterLayerId;
        issues.append( v );
      }
      for ( const TimeResidualRow &row : rows )
      {
        QVariantMap m;
        m.insert( QStringLiteral( "well_id" ), row.wellId );
        m.insert( QStringLiteral( "well_name" ), row.wellName );
        m.insert( QStringLiteral( "horizon" ), tgtHorizon );
        m.insert( QStringLiteral( "layer_id" ), rasterLayerId );
        m.insert( QStringLiteral( "threshold_ms" ), threshold );
        m.insert( QStringLiteral( "reason" ), row.reason );
        if ( std::isfinite( row.x ) )
          m.insert( QStringLiteral( "x" ), row.x );
        if ( std::isfinite( row.y ) )
          m.insert( QStringLiteral( "y" ), row.y );
        if ( std::isfinite( row.timeMs ) )
          m.insert( QStringLiteral( "time_ms" ), row.timeMs );
        if ( std::isfinite( row.rasterMs ) )
          m.insert( QStringLiteral( "raster_ms" ), row.rasterMs );
        if ( std::isfinite( row.residualMs ) )
          m.insert( QStringLiteral( "residual_ms" ), row.residualMs );
        m.insert( QStringLiteral( "inline" ), row.inlineNo );
        const char *status = row.status == TimeResidualRow::Status::Pass       ? "pass"
                             : row.status == TimeResidualRow::Status::Exceeds  ? "exceed"
                             : row.status == TimeResidualRow::Status::Warn     ? "warn"
                                                                               : "na";
        m.insert( QStringLiteral( "status" ), QString::fromLatin1( status ) );
        rowMaps.append( m );

        if ( row.status != TimeResidualRow::Status::Exceeds )
          continue;
        ValidationIssue v;
        v.severity = ValidationIssue::Warning;
        v.code = QStringLiteral( "TIME_RESIDUAL" );
        QString inlineText;
        if ( row.inlineNo >= 0 )
          inlineText = tr( "，目标测线 %1" ).arg( row.inlineNo );
        v.message = tr( "井 %1 %2 时间残差 %3ms（井 %4ms vs 栅格 %5ms%6）" )
                        .arg( row.wellName, tgtHorizon )
                        .arg( row.residualMs, 0, 'f', 1 )
                        .arg( row.timeMs, 0, 'f', 1 )
                        .arg( row.rasterMs, 0, 'f', 1 )
                        .arg( inlineText );
        v.horizon = tgtHorizon;
        v.wellId = row.wellId;
        if ( std::isfinite( row.x ) && std::isfinite( row.y ) )
          v.wktLocation = QStringLiteral( "POINT(%1 %2)" ).arg( row.x ).arg( row.y );
        v.details.insert( QStringLiteral( "well_name" ), row.wellName );
        v.details.insert( QStringLiteral( "well_x" ), row.x );
        v.details.insert( QStringLiteral( "well_y" ), row.y );
        v.details.insert( QStringLiteral( "inline" ), row.inlineNo );
        v.details.insert( QStringLiteral( "time_ms" ), row.timeMs );
        v.details.insert( QStringLiteral( "raster_ms" ), row.rasterMs );
        v.details.insert( QStringLiteral( "residual_ms" ), row.residualMs );
        v.layerId = rasterLayerId;
        issues.append( v );
      }
    }
    m_residualRows = rowMaps;
  }

  emit validationDone( issues.size() );
  return issues;
}
