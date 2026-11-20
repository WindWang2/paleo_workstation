// 层：功能
#include "workflows.h"
#include "workflows_internal.h"

#if PALEO_HAVE_ORT
#include "algorithms/projectgeotiff.h"
#endif

#include "../ai/onnxpredictionservice.h" // ORT-free header；符号引用受 PALEO_HAVE_ORT 守卫
#include "../catalog/datacatalog.h"      // localGridCrsWkt — ONNX 栅格落在局部测网
#include "../domain/arearules.h"         // AreaRules::active().targetHorizon（本文件直接用）
#include "../metadata/paleoprojectstore.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprocessingservice.h"
#include "derivedassets.h"
#include "algorithmpublish.h"
#include "algorithmadapters.h"
#include "predictionpublication.h"
#include "services/analysisunitcontext.h"
#include "services/seismicunitinput.h"
#include "services/seismicdataaccess.h"
#include "qgis/unitraster.h"
#include "analysisresultsync.h"
#include <QTemporaryDir>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <memory>
#include <vector>

#include <gdal.h>

#include <qgscoordinatereferencesystem.h>
#include <qgsmaplayer.h>
#include <qgsrasterlayer.h>
#include <qgsvectorlayer.h>
#include <ogr_spatialref.h>
#include <cpl_conv.h>

#include <cmath>
#include <cstring>

// 预测工作流（①智能预测）。
// 方向20 轮4：按类边界从 workflows.cpp 析出。头文件契约不动。

#if PALEO_HAVE_ORT
// P4-W4：输出回读验证——「文件存在」不等于「可发布」。重开 GDAL 数据集
// 核对尺寸/地理变换/CRS 与计算记录一致，统计有限像元/缺测像元作为覆盖
// 摘要进 provenance（411×641 Float32 ≈ 1 MiB，worker 线程内代价可忽略）。
static bool verifyOnnxRasterOutput( const QString &path, int rows, int cols, const double gt[6],
                                    const QString &crsWkt, QVariantMap *summary, QString *error )
{
  GDALDatasetH ds = GDALOpen( path.toUtf8().constData(), GA_ReadOnly );
  if ( !ds )
  {
    paleo::workflow_detail::setError( error, QObject::tr( "预测输出无法回读（GDAL 打开失败）：%1" ).arg( path ) );
    return false;
  }
  bool ok = true;
  QString reason;
  if ( GDALGetRasterXSize( ds ) != cols || GDALGetRasterYSize( ds ) != rows )
  {
    ok = false;
    reason = QObject::tr( "回读尺寸 %1×%2 与计算记录 %3×%4 不符" )
                 .arg( GDALGetRasterXSize( ds ) ).arg( GDALGetRasterYSize( ds ) ).arg( cols ).arg( rows );
  }
  double readGt[6] = { 0, 0, 0, 0, 0, 0 };
  if ( ok && GDALGetGeoTransform( ds, readGt ) == CE_None )
    for ( int i = 0; i < 6; ++i )
      if ( std::abs( readGt[i] - gt[i] ) > 1e-9 )
      {
        ok = false;
        reason = QObject::tr( "回读 geotransform[%1]（%2）与计算记录（%3）不符" ).arg( i ).arg( readGt[i] ).arg( gt[i] );
        break;
      }
  if ( ok && !crsWkt.isEmpty() )
  {
    const char *proj = GDALGetProjectionRef( ds );
    if ( !proj || !*proj )
    {
      ok = false;
      reason = QObject::tr( "回读数据集缺少 CRS（计算记录要求 %1）" ).arg( crsWkt );
    }
  }
  int finiteCells = 0;
  int nodataCells = 0;
  if ( ok )
  {
    GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
    QVector<float> px( rows * cols );
    if ( GDALRasterIO( band, GF_Read, 0, 0, cols, rows, px.data(), cols, rows,
                       GDT_Float32, 0, 0 ) != CE_None )
    {
      ok = false;
      reason = QObject::tr( "回读像元失败（IO 错误）" );
    }
    else
    {
      int hasNodata = 0;
      const double nodata = GDALGetRasterNoDataValue( band, &hasNodata );
      for ( float v : px )
      {
        if ( std::isfinite( v ) )
          ++finiteCells;
        else if ( hasNodata && v == static_cast<float>( nodata ) )
          ++nodataCells;
        else
        {
          // 有限/nodata 之外的原值不应出现在已落盘栅格（writeOnnxRaster 已
          // 把非有限值规范为 nodata）——如实按覆盖摘要异常处理。
          ++nodataCells;
        }
      }
    }
  }
  GDALClose( ds );
  if ( !ok )
  {
    paleo::workflow_detail::setError( error, QObject::tr( "预测输出验证失败：%1（%2）" ).arg( reason, path ) );
    return false;
  }
  if ( summary )
  {
    summary->insert( QStringLiteral( "rows" ), rows );
    summary->insert( QStringLiteral( "cols" ), cols );
    summary->insert( QStringLiteral( "finite_cells" ), finiteCells );
    summary->insert( QStringLiteral( "nodata_cells" ), nodataCells );
  }
  return true;
}
#endif

#if PALEO_HAVE_ORT
// 把通过门禁的 411×641 结果写成 Float32 GeoTIFF，geotransform/SRS 用
// OnnxAreaGrid（D61 声明优先，否则 §3 常量 + 局部测网 CRS）。
QString paleo::workflow_detail::writeOnnxRaster( const QString &path, const QVector<float> &values,
                                                int rows, int cols, const OnnxAreaGrid &grid,
                                                const QVariantMap &params, const QString &model,
                                                const QVector<int64_t> &outShape, QString *error )
{
  GDALAllRegister();
  GDALDriverH drv = ProjectGeoTiff::driver();
  if ( !drv )
  {
    paleo::workflow_detail::setError( error, QObject::tr( "GTiff driver is not available" ) );
    return QString();
  }
  if ( QFile::exists( path ) )
    QFile::remove( path );
  GDALDatasetH ds = ProjectGeoTiff::create( path, cols, rows, GDT_Float32, drv );
  if ( !ds )
  {
    paleo::workflow_detail::setError( error, QObject::tr( "cannot create prediction raster %1" ).arg( path ) );
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
  QVector<qint64> dims;
  for ( qint64 d : outShape )
  {
    if ( d != 1 )
      dims.append( d );
  }
  const int pixels = rows * cols;
  QVector<float> writeBuffer;
  const float *dataPtr = values.constData();
  if ( dims.size() == 3 && dims[0] > 1 && dims[0] * pixels == static_cast<qint64>( values.size() ) )
  {
    const int C = static_cast<int>( dims[0] );
    writeBuffer.resize( pixels );
    for ( int p = 0; p < pixels; ++p )
    {
      bool finite = true;
      for ( int c = 0; c < C; ++c )
      {
        if ( !std::isfinite( values[c * pixels + p] ) )
        {
          finite = false;
          break;
        }
      }
      if ( !finite )
      {
        writeBuffer[p] = -9999.0f;
        continue;
      }
      int bestC = 0;
      float maxVal = values[p];
      for ( int c = 1; c < C; ++c )
      {
        const float v = values[c * pixels + p];
        if ( v > maxVal )
        {
          maxVal = v;
          bestC = c;
        }
      }
      writeBuffer[p] = static_cast<float>( bestC );
    }
    dataPtr = writeBuffer.constData();
  }
  else
  {
    bool hasNonFinite = false;
    for ( float v : values )
    {
      if ( !std::isfinite( v ) )
      {
        hasNonFinite = true;
        break;
      }
    }
    if ( hasNonFinite )
    {
      writeBuffer = values;
      for ( float &v : writeBuffer )
      {
        if ( !std::isfinite( v ) )
          v = -9999.0f;
      }
      dataPtr = writeBuffer.constData();
    }
  }

  GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
  GDALSetRasterNoDataValue( band, -9999.0 );
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
  const CPLErr writeError = GDALRasterIO( band, GF_Write, 0, 0, cols, rows,
                                        const_cast<float *>( dataPtr ), cols, rows, GDT_Float32, 0, 0 );
  const CPLErr closeError = GDALClose( ds );
  if ( writeError != CE_None || closeError != CE_None )
  {
    QFile::remove( path );
    paleo::workflow_detail::setError( error, QObject::tr( "failed to write prediction raster %1" ).arg( path ) );
    return QString();
  }
  return path;
}
#endif


// ---------------------------------------------------------------------------
// PredictionWorkflow — ①智能预测
// ---------------------------------------------------------------------------

PredictionWorkflow::PredictionWorkflow( QgisProcessingService *proc, QgisLayerService *layers, QObject *parent )
  : QObject( parent ), m_proc( proc ), m_layers( layers )
{
}

void PredictionWorkflow::setCatalog( DataCatalog *catalog, const QString &projectDir )
{
  ++m_projectGeneration;
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

bool PredictionWorkflow::preparePredictionJob( const QString &horizon, const QString &algorithmId,
                                               const QVariantMap &params, PredictionJob *job,
                                               QString *error )
{
  if ( !job )
  {
    paleo::workflow_detail::setError( error, tr( "缺少预测任务" ) );
    return false;
  }
  *job = PredictionJob();
  job->params=params;
  job->projectGeneration = m_projectGeneration;

  // P4-W2（#972 预测入口）：带 unitSelection 的预测先冻结有效分析单元——
  // 结果键从冻结单元产生，不再采信调用方 analysisLayerKey/horizon 字符串；
  // Boundary（如 T70）、伪造 S 编号、不支持的 aggregate 由 freezeAnalysisUnit
  // 的方案校验拒绝，显示名不参与身份。无单元的调用：onnx 引擎在下方分支
  // 前置拒绝（正式地质预测必须落在 S1–S4 分析单位上；无单元张量调用不经
  // 正式发布入口，低层 PaleoOnnxService::runTensor* 按原合同独立可用）；
  // Processing 引擎维持 legacy 可选单元合同（生产 UI 恒带单元；多输入算法
  // 的单元化语义属因子链，不在本任务改写）。
  QString resultKey;
  const QVariantMap requestedUnit = params.value( QStringLiteral( "unitSelection" ) ).toMap();
  if ( !requestedUnit.isEmpty() )
  {
    paleo::strat::UnitSelection frozen;
    QString unitError;
    if ( !paleo::strat::freezeAnalysisUnit( m_projectDir, paleo::strat::fromMap( requestedUnit ),
                                            frozen, &unitError ) )
    {
      paleo::workflow_detail::setError( error, unitError );
      emit predictionFailed( horizon, unitError );
      return false;
    }
    resultKey = paleo::strat::analysisLayerKey( frozen );
    const QString requestedKey = params.value( QStringLiteral( "analysisLayerKey" ) ).toString();
    if ( !requestedKey.isEmpty() && requestedKey != resultKey )
    {
      const QString msg = tr( "analysisLayerKey（%1）与冻结单元的结果键（%2）不一致——结果键由冻结单元产生，不允许绕过单元身份" )
                              .arg( requestedKey, resultKey );
      paleo::workflow_detail::setError( error, msg );
      emit predictionFailed( horizon, msg );
      return false;
    }
    job->params.insert( "unitSelection", paleo::strat::toMap( frozen ) );
    job->params.insert( "unitProject", m_projectDir );
    job->commitExtra.insert( "unitSelection", paleo::strat::toMap( frozen ) );
  }
  else
  {
    resultKey = params.value( QStringLiteral( "analysisLayerKey" ), horizon ).toString();
  }
  job->horizon = resultKey;
  job->commitExtra.insert(QStringLiteral("source_horizon"), resultKey);
  QVariantMap metadata = job->params;
  metadata.remove("unitProject");
  metadata.remove(QStringLiteral("INPUT"));
  metadata.remove(QStringLiteral("OUTPUT"));
  for (const auto &key : metadata.keys())
    if (metadata.value(key).value<QgsMapLayer *>()) metadata.remove(key);
  job->commitExtra.insert(QStringLiteral("algorithm_parameters"), metadata);
  job->algorithmId = algorithmId;

  // prepare 是 catalog 所属线程段（stage → addAsset 有线程闸）：登记、参数
  // 解析、父版本溯源全在这里做完，compute 在 worker 线程不碰 catalog。
  const auto fail = [this, &horizon, error]( const QString &msg ) {
    paleo::workflow_detail::setError( error, msg );
    emit predictionFailed( horizon, msg );
    return false;
  };

#if PALEO_HAVE_ORT
  // "onnx:<model>" dispatches to PaleoOnnxService; every other id falls
  // through to the Processing-registry path below untouched.
  if ( algorithmId.startsWith( QLatin1String( "onnx:" ) ) )
  {
    const QString model = algorithmId.mid( 5 );
    QgisLayerService *layers = m_layers.data();
    PaleoOnnxService *onnx = m_onnx.data();

    if ( !layers )
      return fail( tr( "prediction workflow is not bound to a layer service" ) );
    if ( !onnx )
      return fail( tr( "no ONNX service bound; cannot run '%1'" ).arg( algorithmId ) );
    if ( model.isEmpty() )
      return fail( tr( "onnx algorithm id '%1' carries no model name" ).arg( algorithmId ) );
    // P4-W2：正式 ONNX 地质预测必须冻结有效分析单元——结果键从单元产生、
    // 数值输入是单元 RMS。无地质单元的张量调用不能经本发布入口默认贴上
    // 工区网格并产出「某层预测」（低层 runTensor* 按原合同独立可用；历史
    // 无单元成果保持只读兼容，不迁移）。
    if ( !job->params.contains( QStringLiteral( "unitSelection" ) ) )
      return fail( tr( "正式 ONNX 预测需要先冻结分析单元（方案内具体子层段 S1–S4）；"
                       "未带单元的张量调用不产生工区预测成果" ) );

    // P4-W1：准入探测前置（与 compute/loadModelMeta、预测页同一诊断）——
    // 运行库不兼容在准备阶段就给出可行动结论，不进 worker 才失败。
    const paleo::onnx::OnnxRuntimeProbeResult admission = PaleoOnnxService::runtimeDiagnostics();
    if ( !admission.ok() )
      return fail( admission.publicCode() + QStringLiteral( ": " ) + admission.summaryText() );

    // P4-W3：模型身份冻结——注册表门控（#145/#737）先在这里把关（不可用/
    // 未登记的模型 prepare 即拒），可用的按注册表钉 SHA 冻结期望身份。
    // 轻查询（锁内拷贝，无文件 IO/哈希/会话构建，不占 GUI 线程）。
    if ( !onnx->availableModels().contains( model ) )
      return fail( tr( "模型 '%1' 在当前工程注册表中不可用（未登记/被门控/未钉哈希）" ).arg( model ) );
    const ModelRegistryEntry entry = onnx->registryEntryFor( model );
    if ( entry.status == ModelRegistryEntry::Status::Ok )
    {
      job->expectedModelSha256 = entry.sha256Pinned;
      QVariantMap modelIdentity;
      modelIdentity.insert( QStringLiteral( "name" ), entry.name );
      if ( !entry.version.isEmpty() )
        modelIdentity.insert( QStringLiteral( "registry_version" ), entry.version );
      modelIdentity.insert( QStringLiteral( "sha256_pinned" ), entry.sha256Pinned );
      job->commitExtra.insert( QStringLiteral( "model_registry" ), modelIdentity );
    }
    // 无 manifest 的开发降级：expectedModelSha256 留空（不伪造注册表身份），
    // compute 仍把实测 SHA 写进 provenance。

    job->executionEngine = QStringLiteral( "onnxruntime" );

    // 地震源解析在 prepare（GUI 线程）：单元 RMS 输入实际消费的地震体路径。
    {
      QString sourceError;
      const auto source = paleo::seismicProjectSource( m_projectDir,
                                                       params.value( "seismicPath" ).toString(),
                                                       &sourceError );
      if ( source.isEmpty() )
        return fail( sourceError );
      job->params.insert( "seismicPath", source );
    }
    // T26：预测栅格落 artifacts/derived + DERIVED 版本。
    QString regErr;
    DerivedAssetRegistrar registrar = paleo::workflow_detail::derivedRegistrarOf( this, &regErr );
    if ( !registrar.isBound() )
      return fail( regErr );
    job->staging = registrar.stage(
        QStringLiteral( "onnx_prediction" ), tr( "%1 onnx %2 预测" ).arg( resultKey, model ),
        QStringLiteral( "ONNX_%1_%2.tif" ).arg( resultKey, model ), &regErr );
    if ( !job->staging.isValid() )
      return fail( regErr );
    job->sourceUri = QStringLiteral( "onnxworkflow/%1" ).arg( model );
    // P4-W3（#1008）：精确父引用——单元路径的数值输入是地震源（RMS 由其
    // 派生），只登记按实际路径解析出的版本；路径不在 catalog 管理范围时
    // 不伪造父（缺父版本不猜测）。删除按实体名/displayName.contains(horizon)
    // 搜集 parents 的回退：T7/T70/T71、重名与重命名资产不再污染血缘。
    job->parentVersionIds = registrar.parentVersionIdsFor( { job->params.value( "seismicPath" ).toString() } );
    QVariantMap inputSources;
    inputSources.insert( QStringLiteral( "seismic_path" ), job->params.value( "seismicPath" ).toString() );
    inputSources.insert( QStringLiteral( "input_attribute" ), QStringLiteral( "layer_window_rms" ) );
    job->commitExtra.insert( QStringLiteral( "input_sources" ), inputSources );

    job->layerId = QStringLiteral( "pred.%1.onnx.%2" ).arg( resultKey, model );
    job->layerGroup = QStringLiteral( "03_Predict" ); // 历史分组保持（m2(A)）
    job->layerTitle = tr( "%1 onnx %2 预测" ).arg( resultKey, model );
    // P4-W5：冻结展示目标现状——publish 据此识别本任务运行期间 layerId
    // 声明是否已被另一有效运行更新（并发/重放不覆盖较新展示）。
    for ( const LayerDeclaration &d : layers->declared() )
      if ( d.layerId == job->layerId )
      {
        job->declaredSourceAtPrepare = d.source;
        break;
      }
    job->prepared = true;
    return true;
  }
#else
  if ( algorithmId.startsWith( QLatin1String( "onnx:" ) ) )
    return fail( tr( "cannot run '%1': this build lacks ONNX Runtime" ).arg( algorithmId ) );
#endif

  QgisProcessingService *proc = m_proc.data();
  QgisLayerService *layers = m_layers.data();
  if ( !proc || !layers )
    return fail( tr( "prediction workflow is not bound to services" ) );

  // T26：未钉 OUTPUT 的 Processing 结果会落进程临时池（退出即删）——预测产物
  // 同样先 stage 到 artifacts/derived，再作为 DERIVED 版本登记。
  QString regErr;
  DerivedAssetRegistrar registrar = paleo::workflow_detail::derivedRegistrarOf( this, &regErr );
  if ( !registrar.isBound() )
    return fail( regErr );
  job->staging = registrar.stage(
      QStringLiteral( "prediction_raster" ), tr( "%1 %2 预测" ).arg( resultKey, algorithmId ),
      QStringLiteral( "PREDICT_%1.tif" ).arg( resultKey ), &regErr );
  if ( !job->staging.isValid() )
    return fail( regErr );
  job->params.insert( QStringLiteral( "OUTPUT" ), job->staging.absolutePath );

  if(job->params.contains("unitSelection")){
    if(!job->params.contains("INPUT")||job->params.value("INPUT").isNull()){
      if(!proc->algorithmRequiresParameter(algorithmId,"INPUT"))return fail(tr("所选算法未声明地震层段输入"));
      const auto source=paleo::seismicProjectSource(m_projectDir,params.value("seismicPath").toString(),&regErr);
      if(source.isEmpty())return fail(regErr);
      job->params.insert("unitRasterInput",true);job->params.insert("seismicPath",source);
      job->parentVersionIds=registrar.parentVersionIdsFor({source});
    }else{
      const auto *input=job->params.value("INPUT").value<QgsMapLayer *>();
      const auto path=input?input->source().section('|',0,0):job->params.value("INPUT").toString();
      bool scoped=false;const auto selected=paleo::strat::fromMap(job->params.value("unitSelection").toMap());
      for(const auto &id:registrar.parentVersionIdsFor({path}))
        if(analysisResultUnitSelection(m_catalog,m_catalog->versionById(id))==selected)scoped=true;
      if(!scoped)return fail(tr("输入成果不属于所选地震层段，请先按该层段获取输入"));
    }
  }
  // P0-1: 自动解析层位输入栅格图层作为 INPUT（页面只收标量参数，图层输入由工作流上下文解析）
  if (!job->params.value("unitRasterInput").toBool() && proc->algorithmRequiresParameter(algorithmId, QStringLiteral("INPUT")) &&
      (!job->params.contains(QStringLiteral("INPUT")) || job->params.value(QStringLiteral("INPUT")).isNull()))
  {
    QgsMapLayer *inputLayer = nullptr;
    const QString target = horizon.trimmed().isEmpty() ? AreaRules::active().targetHorizon : horizon.trimmed();
    const QString prefix = QStringLiteral( "horizon.%1" ).arg( target );
    for ( const LayerDeclaration &d : layers->declared() )
    {
      if ( d.layerId.startsWith( prefix ) && d.type.compare( QStringLiteral( "raster" ), Qt::CaseInsensitive ) == 0 )
      {
        inputLayer = layers->instantiate( d.layerId );
        if ( inputLayer )
          break;
      }
    }
    if ( !inputLayer )
    {
      // 找不到目标层位的栅格就如实失败：旧实现回退取声明表里第一个任意
      // 类型 raster 图层，预测会静默跑在无关栅格上、又按目标层位登记
      // （#204）。确需兜底的调用方请显式传 INPUT，不要靠这里猜。
      return fail( tr( "找不到层位 %1 的输入栅格（horizon.%1*），请先导入或生成该层位栅格" )
                       .arg( target ) );
    }
    if ( inputLayer )
      job->params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( inputLayer ) );
  }

  // FC-001：INPUT 统一冻结为 URI/CRS 快照——compute 在 worker 线程自建独立
  // 层，主线程拥有的活 QgsMapLayer 不得跨线程传给 QGIS Processing（服务的
  // 成文契约，见 qgisprocessingservice.cpp #235-2 注）。页面自动解析与调用方
  // 直接传活层（旧测试入口）在这里一并归一。
  if ( const QgsMapLayer *input = job->params.value( QStringLiteral( "INPUT" ) ).value<QgsMapLayer *>() )
  {
    job->inputUri = input->source();
    job->inputProvider = input->providerType();
    job->inputCrsWkt = input->crs().toWkt();
    job->inputIsRaster = qobject_cast<const QgsRasterLayer *>( input ) != nullptr;
    job->params.remove( QStringLiteral( "INPUT" ) );
  }

  // 父版本 = 预测输入图层（文件源可溯源时）——source 从 prepare（GUI 线程）
  // 提取的冻结快照读，compute 不再碰 GUI 对象。
  QStringList parentPaths;
  {
    const QString src = job->inputUri.section( QLatin1Char( '|' ), 0, 0 );
    if ( !src.isEmpty() && paleo::workflow_detail::isFileBackedSource( src ) )
      parentPaths.append( src );
  }
  job->parentVersionIds.append(registrar.parentVersionIdsFor( parentPaths ));
  for (const auto &key : job->params.keys())
    if (const auto *layer = job->params.value(key).value<QgsMapLayer *>()) {
      job->extraInputLayers.insert(key, QVariantMap{{"uri", layer->source()}, {"provider", layer->providerType()},
          {"crs", layer->crs().toWkt()}, {"raster", qobject_cast<const QgsRasterLayer *>(layer) != nullptr}});
      job->parentVersionIds.append(registrar.parentVersionIdsFor({layer->source().section('|', 0, 0)}));
      job->params.remove(key);
    }
  job->parentVersionIds.removeDuplicates();
  job->sourceUri = QStringLiteral( "predictionworkflow/%1" ).arg( algorithmId );
  // m2(A)：稳定结果 id——同一 horizon+algorithmId 重跑复用同一 layerId
  // （manifest INSERT OR REPLACE upsert，清单不新增重复行）； tst_workflows
  // 既有断言 startsWith("predict.<h>.") 由稳定段继续满足。
  job->layerId = QStringLiteral( "predict.%1.%2" ).arg( resultKey, stableResultSuffix( algorithmId ) );
  job->layerGroup = QStringLiteral( "01_Prediction" ); // 历史分组保持（m2(A)：新声明面才用 02_Prediction）
  job->layerTitle = tr( "%1 %2 预测" ).arg( resultKey, algorithmId );
  // P4-W5：展示目标快照（同 ONNX 分支）——publish 拒绝覆盖运行期间被另一
  // 有效运行更新的声明。
  for ( const LayerDeclaration &d : layers->declared() )
    if ( d.layerId == job->layerId )
    {
      job->declaredSourceAtPrepare = d.source;
      break;
    }
  job->prepared = true;
  return true;
}

bool PredictionWorkflow::computePredictionJob( PredictionJob *job )
{
  // compute 在 worker 线程：只做推理/Processing 与栅格落盘——不碰 catalog
  // （线程闸）、不读 GUI 对象、不发信号。失败原因如实写 job.error，由调用
  // 侧（GUI 线程）上屏/发 predictionFailed。
  if ( !job || !job->prepared )
  {
    if ( job )
      job->error = tr( "预测任务未准备" );
    return false;
  }
  const auto fail = [job]( const QString &msg ) {
    job->error = msg;
    return false;
  };

  if ( job->isCancelled && job->isCancelled() )
    return fail( tr( "预测已取消" ) );

#if PALEO_HAVE_ORT
  if ( job->algorithmId.startsWith( QLatin1String( "onnx:" ) ) )
  {
    const QString model = job->algorithmId.mid( 5 );
    PaleoOnnxService *onnx = m_onnx.data();
    if ( !onnx )
      return fail( tr( "no ONNX service bound; cannot run '%1'" ).arg( job->algorithmId ) );

    // P0-3: 模型哈希、会话加载与 warmup 卸载至 worker 线程，决不阻塞主线程
    OnnxModelMeta meta;
    if ( onnx->loadModelMeta( model, &meta, &job->error ) != OnnxLoadStatus::Ok )
      return fail( job->error.isEmpty() ? tr( "加载 ONNX 模型 '%1' 失败" ).arg( model ) : job->error );
    // P4-W4：冻结契约复验——实际加载模型的指纹必须与 prepare 冻结的注册表
    // 钉哈希一致。注册表补钉/换文件/并发换活动模型在这里显式拒绝，不悄悄
    // 换模型推理（runPinnedTensor 在会话层再核一次，双保险）。
    if ( !job->expectedModelSha256.isEmpty() &&
         meta.sha256.compare( job->expectedModelSha256, Qt::CaseInsensitive ) != 0 )
      return fail( tr( "model_revision_changed: 模型 '%1' 与准备阶段冻结的身份不一致（冻结 %2，实际 %3）——请重新发起预测" )
                       .arg( model, job->expectedModelSha256, meta.sha256 ) );
    job->modelSha256 = meta.sha256;
    job->executionEngine = QStringLiteral( "onnxruntime" );

    if ( job->isCancelled && job->isCancelled() )
      return fail( tr( "预测已取消" ) );

    paleo::strat::SeismicUnitInput scopedInput;
    const bool scoped=job->params.contains("unitSelection");
    if(scoped){
      scopedInput=paleo::strat::prepareSeismicUnitInput(job->params.value("unitProject").toString(),
          paleo::strat::fromMap(job->params.value("unitSelection").toMap()),job->params.value("seismicPath").toString(),job->isCancelled);
      if(!scopedInput.error.isEmpty())return fail(scopedInput.error);
      if(meta.inputShape.size()!=4||meta.inputShape[0]>1||meta.inputShape[1]>1||
         (meta.inputShape[2]>0&&meta.inputShape[2]!=scopedInput.rows)||(meta.inputShape[3]>0&&meta.inputShape[3]!=scopedInput.columns))
        return fail(tr("模型输入与当前层段的 RMS 网格不匹配，需要 [1,1,%1,%2]").arg(scopedInput.rows).arg(scopedInput.columns));
      job->onnxInput=scopedInput.samples;
      for(auto &v:job->onnxInput)if(!std::isfinite(v))v=0;
      job->onnxShape={1,1,scopedInput.rows,scopedInput.columns};job->onnxInputName=meta.inputName;
      job->onnxGt[0]=scopedInput.extent.left();job->onnxGt[1]=scopedInput.extent.width()/scopedInput.columns;job->onnxGt[2]=0;
      job->onnxGt[3]=scopedInput.extent.bottom();job->onnxGt[4]=0;job->onnxGt[5]=-scopedInput.extent.height()/scopedInput.rows;
      job->params.insert("crs",scopedInput.crs);job->onnxProjection=scopedInput.crs;
      job->commitExtra.insert("input_attribute","layer_window_rms");job->commitExtra.insert("valid_cells",scopedInput.validCells);
    }
    // #144：绑定模型名推理，并发任务换活动模型不影响本次。P4-W4：冻结了
    // 注册表钉哈希的任务走 runPinnedTensor——会话指纹与期望不符（LRU 期间
    // 被换/文件被替换重建）在会话层再次拒绝。
    OnnxTensor tensor =
        job->expectedModelSha256.isEmpty()
            ? onnx->runTensorOn( model, job->onnxInputName, job->onnxInput, job->onnxShape, &job->error )
            : onnx->runPinnedTensor( model, job->expectedModelSha256, job->onnxInputName,
                                     job->onnxInput, job->onnxShape, &job->error );
    if ( tensor.values.isEmpty() )
      return fail( job->error.isEmpty()
                       ? tr( "ONNX model '%1' produced no output" ).arg( model )
                       : job->error );

    if ( job->isCancelled && job->isCancelled() )
      return fail( tr( "预测已取消" ) );

    // 网格门禁（plan §1 trap）：输出挤成二维后必须是工区网格 411×641，否则
    // 不写栅格、不登记图层。geotransform 由 horizon.D61* 声明提供，缺省常量。
    paleo::workflow_detail::OnnxAreaGrid grid;
    std::memcpy( grid.gt, job->onnxGt, sizeof( grid.gt ) );
    grid.projection = job->onnxProjection;
    grid.sourcePath = job->onnxSourcePath;
    grid.fromDecl = job->onnxFromDecl;
    int rows = 0;
    int cols = 0;
    if(scoped){
      if(tensor.shape.size()!=4||tensor.shape[0]!=1||tensor.shape[1]<1||tensor.shape[1]>256||
         tensor.shape[2]!=scopedInput.rows||tensor.shape[3]!=scopedInput.columns||
         tensor.values.size()!=tensor.shape[1]*scopedInput.rows*scopedInput.columns)
        return fail(tr("模型输出与所选层段网格不一致"));
      rows=scopedInput.rows;cols=scopedInput.columns;
      for(qsizetype i=0;i<tensor.values.size();++i)if(!std::isfinite(scopedInput.samples[i%scopedInput.samples.size()]))tensor.values[i]=NAN;
    }
    if ( !scoped&&!paleo::workflow_detail::resolveOnnxGrid( tensor.values, tensor.shape, rows, cols,
                                                   &job->error ) )
      return fail( job->error.isEmpty() ? tr( "cannot place ONNX output on a grid" ) : job->error );

    if ( job->isCancelled && job->isCancelled() )
      return fail( tr( "预测已取消" ) );

    const QString outPath = paleo::workflow_detail::writeOnnxRaster(
        job->staging.absolutePath, tensor.values, rows, cols, grid, job->params, model,
        tensor.shape, &job->error );
    if ( outPath.isEmpty() )
      return fail( job->error.isEmpty() ? tr( "failed to write ONNX prediction raster" )
                                        : job->error );
    // P4-W4：输出回读验证——尺寸/geotransform/CRS 与计算记录一致、覆盖摘要
    // 进 provenance；「文件存在」不视为可发布。
    {
      QVariantMap verified;
      if ( !verifyOnnxRasterOutput( outPath, rows, cols, grid.gt,
                                    job->params.value( QStringLiteral( "crs" ) ).toString(),
                                    &verified, &job->error ) )
        return fail( job->error );
      job->commitExtra.insert( QStringLiteral( "verified_output" ), verified );
    }
    job->outputPath = outPath;
    job->commitExtra.insert( QStringLiteral( "model" ), model );
    job->commitExtra.insert( QStringLiteral( "model_names" ), QStringList{ model } );
    job->commitExtra.insert( QStringLiteral( "execution_engine" ),
                             job->executionEngine.isEmpty() ? QStringLiteral( "onnxruntime" ) : job->executionEngine );
    if ( !job->modelSha256.isEmpty() )
      job->commitExtra.insert( QStringLiteral( "model_sha256" ), job->modelSha256 );
    job->commitExtra.insert( QStringLiteral( "rows" ), rows );
    job->commitExtra.insert( QStringLiteral( "cols" ), cols );
    job->commitExtra.insert( QStringLiteral( "geotransform_source" ),
                             scoped ? QStringLiteral( "unit_window_extent" )
                                    : ( grid.fromDecl ? QStringLiteral( "horizon.%1" ).arg( job->horizon )
                                                      : QStringLiteral( "project_area" ) ) );
    job->ok = true;
    return true;
  }
#endif

  QgisProcessingService *proc = m_proc.data();
  if ( !proc )
    return fail( tr( "prediction workflow is not bound to services" ) );

  if ( job->isCancelled && job->isCancelled() )
    return fail( tr( "预测已取消" ) );

  // FC-001：worker 自建独立输入层（URI 冻结快照回放）——同线程直调
  // runPrediction 时行为等价（layer 数据同源），跨线程时不再触碰主线程活层。
  QVariantMap runParams = job->params;
  std::vector<std::unique_ptr<QgsMapLayer>> extraInputs;
  for (auto it = job->extraInputLayers.cbegin(); it != job->extraInputLayers.cend(); ++it) {
    const auto ref = it.value().toMap();
    std::unique_ptr<QgsMapLayer> layer;
    if (ref.value("raster").toBool()) layer = std::make_unique<QgsRasterLayer>(ref.value("uri").toString(), it.key(), ref.value("provider").toString());
    else layer = std::make_unique<QgsVectorLayer>(ref.value("uri").toString(), it.key(), ref.value("provider").toString());
    if (!layer->isValid()) return fail(tr("无法读取预测输入图层：%1").arg(ref.value("uri").toString()));
    layer->setCrs(QgsCoordinateReferenceSystem::fromWkt(ref.value("crs").toString()));
    runParams.insert(it.key(), QVariant::fromValue(layer.get()));
    extraInputs.push_back(std::move(layer));
  }
  std::unique_ptr<QTemporaryDir> unitTemporary;
  paleo::strat::SeismicUnitInput scopedInput;
  if(job->params.value("unitRasterInput").toBool()){
    scopedInput=paleo::strat::prepareSeismicUnitInput(job->params.value("unitProject").toString(),
        paleo::strat::fromMap(job->params.value("unitSelection").toMap()),job->params.value("seismicPath").toString(),job->isCancelled);
    if(!scopedInput.error.isEmpty())return fail(scopedInput.error);
    unitTemporary=std::make_unique<QTemporaryDir>(QFileInfo(job->staging.absolutePath).absolutePath()+"/unit-input-XXXXXX");
    if(!unitTemporary->isValid())return fail(tr("不能创建层段预测临时输入"));
    job->inputUri=unitTemporary->filePath("rms.tif");job->inputProvider="gdal";job->inputIsRaster=true;job->inputCrsWkt=scopedInput.crs;
    if(!UnitRaster::writeInput(scopedInput,job->inputUri,&job->error))return false;
    job->commitExtra.insert("input_attribute","layer_window_rms");job->commitExtra.insert("valid_cells",scopedInput.validCells);
  }
  std::unique_ptr<QgsMapLayer> inputLayer;
  if ( !job->inputUri.isEmpty() )
  {
    if ( job->inputIsRaster )
      inputLayer = std::make_unique<QgsRasterLayer>( job->inputUri,
                                                     QStringLiteral( "prediction_input" ),
                                                     job->inputProvider );
    else
      inputLayer = std::make_unique<QgsVectorLayer>( job->inputUri,
                                                     QStringLiteral( "prediction_input" ),
                                                     job->inputProvider );
    if ( !inputLayer->isValid() )
      return fail( tr( "无法读取预测输入图层：%1" ).arg( job->inputUri ) );
    if ( !job->inputCrsWkt.isEmpty() )
      inputLayer->setCrs( QgsCoordinateReferenceSystem::fromWkt( job->inputCrsWkt ) );
    runParams.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( inputLayer.get() ) );
  }

  // algo-atom P2：预测 Processing 经 Adapter（含 geological_smoothing 等）
  paleo::ProcessingAlgorithmAdapter adapter( proc );
  const AlgorithmRunResult runResult =
      adapter.runById( job->algorithmId, runParams, &job->error );
  if ( !runResult.ok )
  {
    if ( job->error.isEmpty() )
      job->error = tr( "prediction algorithm '%1' produced no results" ).arg( job->algorithmId );
    return false;
  }
  const QVariantMap results = paleo::processingResultsFromRunResult( runResult );

  if ( job->isCancelled && job->isCancelled() )
    return fail( tr( "预测已取消" ) );

  job->outputPath = paleo::workflow_detail::outputPathOf( results );
  if ( job->outputPath.isEmpty() )
    return fail( tr( "prediction algorithm '%1' returned no output path" ).arg( job->algorithmId ) );
  if(job->params.contains("unitSelection")){
    if(QFileInfo(job->outputPath)!=QFileInfo(job->staging.absolutePath))return fail(tr("算法输出未写入本次任务的成果路径，结果未发布"));
    const auto input=job->inputUri.isEmpty()?job->params.value("INPUT").toString():job->inputUri;
    if(!UnitRaster::maskOutputLikeInput(input,job->outputPath,&job->error))return false;
  }
  job->ok = true;
  return true;
}

bool PredictionWorkflow::publishPredictionJob( PredictionJob &job, QString *error )
{
  // publish 回 catalog 所属线程：commitExternal（addVersion 线程闸）、图层
  // 声明（GUI 对象）与 predictionDone 都在这里。
  //
  // P4-W5/W6 编排：① 先查「可证明的提交事实」（重复回调/崩溃后重试据此
  // 幂等，查询不确定即阻断，不把查询失败当未提交）；② 未提交才落位并经
  // publishArtifacts 提交 Catalog——提交失败只清本任务未提交暂存，旧声明/
  // 旧文件一律不动（我们尚未声明）；③ 提交成功后才声明展示目标（冲突按
  // prepare 冻结的声明快照识别：运行期间被另一有效运行更新则不覆盖），
  // 声明失败如实报「成果已提交、展示未完成」，不删已提交文件、不重提交；
  // ④ 声明路径必须与 Catalog 记录的同一已验证版本一致才算成功。
  const auto fail = [this, &job, error]( const QString &msg ) {
    paleo::workflow_detail::setError( error, msg );
    emit predictionFailed( job.horizon, msg );
    return false;
  };
  if ( !job.prepared )
    return fail( tr( "预测任务未准备" ) );
  if ( !job.ok || job.outputPath.isEmpty() )
    return fail( job.error.isEmpty() ? tr( "预测未返回输出路径" ) : job.error );

  QgisLayerService *layers = m_layers.data();
  if ( !layers )
    return fail( tr( "prediction workflow is not bound to a layer service" ) );
  QString regErr;
  DerivedAssetRegistrar registrar = paleo::workflow_detail::derivedRegistrarOf( this, &regErr );
  if ( !registrar.isBound() )
    return fail( regErr );

  // ① 提交事实先行（P4-W6）：已提交的版本必须先于工程代次检查——切换工程
  // 不能把已发生的提交误判成可丢弃的暂存。
  paleo::workflow::PredictionCommitEvidence commit =
      paleo::workflow::queryPredictionCommit( m_catalog.data(), job.staging );
  if ( commit.fact == paleo::workflow::PredictionCommitFact::CommitStateUnknown )
    return fail( tr( "无法证明预测提交状态（%1）——已阻断以避免重复提交，请重试或检查工程数据后重试发布" )
                     .arg( commit.detail ) );
  if ( job.projectGeneration && job.projectGeneration != m_projectGeneration )
  {
    if ( commit.fact == paleo::workflow::PredictionCommitFact::Committed )
    {
      job.committedVersionId = commit.version.id;
      return fail( tr( "预测成果已提交（版本 %1），但工程已切换，展示未恢复——成果保留在原工程受管路径，可从成果目录查看" )
                       .arg( commit.version.id ) );
    }
    PaleoWorkflowDiscardPredictionStagingDir( job.staging.versionId, job.staging.absolutePath );
    return fail( tr( "工程已切换，预测结果未发布" ) );
  }

  const QString stagedPath = job.staging.absolutePath;
  if ( commit.fact == paleo::workflow::PredictionCommitFact::NotCommitted )
  {
    // ② 落位 + 提交（先提交后声明——旧序在提交失败时删暂存却留声明，稳定
    // layerId 指向已删除文件，#1007）。
    if ( job.outputPath != stagedPath )
    {
      QDir().mkpath( QFileInfo( stagedPath ).absolutePath() );
      if ( QFile::exists( stagedPath ) )
        QFile::remove( stagedPath );
      if ( !QFile::copy( job.outputPath, stagedPath ) )
        return fail( tr( "无法将预测输出复制到受管路径" ) );
    }
    // P4-W7：提交前取消检查——此时产物未提交，终止并只清本任务未提交暂存
    //（committedVersionId 仍为空，调用方守卫语义一致）。
    if ( job.isCancelled && job.isCancelled() )
    {
      PaleoWorkflowDiscardPredictionStagingDir( job.staging.versionId, job.staging.absolutePath );
      return fail( tr( "预测已取消（提交前终止，产物未发布）" ) );
    }

    const AlgorithmRunResult predResult = paleo::runResultFromOutputPath( stagedPath );
    QString commitErr;
    if ( !paleo::publishArtifacts( registrar, job.staging, predResult, job.parentVersionIds,
                                   job.sourceUri, job.commitExtra, &commitErr ) )
    {
      // 提交失败：产物未入 catalog——只有本任务未提交暂存可清理；旧声明
      // 未被触碰（声明在提交之后），无悬空引用。
      if ( QFile::exists( stagedPath ) )
        QFile::remove( stagedPath );
      return fail( commitErr );
    }
    job.committedVersionId = job.staging.versionId;
    // 提交后凭证以 catalog 为准（延迟建资产归并可能移动受管文件）。
    commit = paleo::workflow::queryPredictionCommit( m_catalog.data(), job.staging );
    if ( commit.fact != paleo::workflow::PredictionCommitFact::Committed )
      return fail( tr( "预测已提交但提交凭证核对失败（%1）——请勿重复执行，检查工程数据后重试发布以恢复展示" )
                       .arg( commit.detail ) );
  }
  else
  {
    job.committedVersionId = commit.version.id;
  }

  // 提交成功后的产物路径以 catalog 记录为准（归并/受管路径解析），不回读
  // staging 猜测。
  const QString committedPath = m_catalog->versionFilePath( commit.version );

  // P4-W7：提交之后发生取消——尊重已持久化的提交凭证，不把已发生的提交
  // 改写成全部取消，也不触发任何删除；展示步骤按未完成报告。
  if ( job.isCancelled && job.isCancelled() )
    return fail( tr( "预测已提交（版本 %1），取消发生在提交之后——成果保留，展示未完成，重试发布可恢复展示" )
                     .arg( commit.version.id ) );

  // ③ 展示声明（P4-W5 冲突检查）。判定基准是「prepare 冻结的声明快照 +
  // 声明源文件现状」：
  //   · 现声明已指向本次提交路径 → 幂等重放，直接进复核；
  //   · 现声明与 prepare 快照一致（含两者皆空）且旧源文件有效 → 正常重跑
  //     upsert；旧源已缺失 → 悬空修复，同样声明；
  //   · 现声明在 prepare 之后出现/被换成另一有效源 → 并发有效运行已更新
  //     展示目标：拒绝覆盖，本次成果留在 catalog 供查看。
  LayerDeclaration existing;
  for ( const LayerDeclaration &d : layers->declared() )
    if ( d.layerId == job.layerId )
    {
      existing = d;
      break;
    }
  const bool alreadyDisplayed =
      existing.layerId == job.layerId && QDir::cleanPath( existing.source ) == QDir::cleanPath( committedPath );
  if ( !alreadyDisplayed && existing.layerId == job.layerId )
  {
    const bool unchangedSincePrepare = existing.source == job.declaredSourceAtPrepare;
    const bool winnerAlive = QFile::exists( existing.source );
    const bool concurrentWinner = winnerAlive && !unchangedSincePrepare;
    if ( concurrentWinner )
    {
      // 拒绝覆盖较新展示；稳定 layerId 当前指向并发运行的有效版本，本任务
      // 成果在 catalog 可追溯。
      emit predictionDone( job.horizon, job.layerId );
      paleo::workflow_detail::setError( error, QString() );
      return true;
    }
    // 其余情形（正常重跑 / 悬空修复 / 首次声明）落到下方 declare。
  }

  LayerDeclaration decl;
  decl.layerId = job.layerId;
  decl.horizon = job.horizon;
  decl.type = QStringLiteral( "raster" );
  decl.source = committedPath;
  decl.group = job.layerGroup;
  decl.title = job.layerTitle;
  QString declareErr;
  if ( !layers->declare( decl, &declareErr ) )
  {
    // P4-W5：Catalog 已提交、展示未完成——如实报告并保留可恢复引用；不删
    // 已提交文件、不重提交新版本。重试（再次 publish）经提交事实只补展示。
    return fail( tr( "成果已提交（版本 %1），展示未完成：%2——重试发布可恢复展示，成果文件保留在 %3" )
                     .arg( commit.version.id,
                           declareErr.isEmpty() ? tr( "图层声明失败" ) : declareErr,
                           committedPath ) );
  }

  // ④ 声明与 Catalog 同一已验证版本（P4-W6 成功返回前复核）。
  QString declaredNow;
  for ( const LayerDeclaration &d : layers->declared() )
    if ( d.layerId == job.layerId )
    {
      declaredNow = d.source;
      break;
    }
  if ( QDir::cleanPath( declaredNow ) != QDir::cleanPath( committedPath ) )
    return fail( tr( "图层声明（%1）与已提交版本路径（%2）不一致——展示未完成，请重试发布" )
                     .arg( declaredNow, committedPath ) );

  // m2(A) 3b：算法无置信度输出 → 不声明伴生层（confidenceCompanionAvailable
  // 对当前算法栈恒 false，见 workflows.h 上的调查注释）。真实置信度通道接入
  // 后在此声明 confidence.<horizon>：group "02_Prediction"、type raster、
  // styleRef 指向置信度色标——禁止常量假栅格冒充。
  if ( confidenceCompanionAvailable( job.algorithmId ) )
  {
    // 当前恒不可达——留作真实置信度输出的声明接入点。
  }

  emit predictionDone( job.horizon, decl.layerId );
  return true;
}

bool PredictionWorkflow::runPrediction( const QString &horizon, const QString &algorithmId,
                                        const QVariantMap &params, QString *error )
{
  // 同步路径 = 三段同线程顺序执行（行为与拆分前一致；单测直调口径不动）。
  PredictionJob job;
  if ( !preparePredictionJob( horizon, algorithmId, params, &job, error ) )
    return false;
  if ( !computePredictionJob( &job ) )
  {
    paleo::workflow_detail::setError( error, job.error );
    emit predictionFailed( horizon, job.error );
    // P4-W7：compute 失败只清本任务未提交暂存（此点 committedVersionId 恒
    // 空——publish 尚未运行；守卫语义与调用方一致）。
    PaleoWorkflowDiscardPredictionStagingDir( job.staging.versionId, job.staging.absolutePath );
    return false;
  }
  return publishPredictionJob( job, error );
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
