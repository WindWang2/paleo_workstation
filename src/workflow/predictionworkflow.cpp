// 层：功能
#include "workflows.h"
#include "workflows_internal.h"

#include "../ai/onnxpredictionservice.h" // ORT-free header；符号引用受 PALEO_HAVE_ORT 守卫
#include "../catalog/datacatalog.h"      // localGridCrsWkt — ONNX 栅格落在局部测网
#include "../metadata/paleoprojectstore.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprocessingservice.h"
#include "derivedassets.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QSet>

#include <gdal.h>

#include <qgsmaplayer.h>
#include <ogr_spatialref.h>
#include <cpl_conv.h>

#include <cmath>

// 预测工作流（①智能预测）。
// 方向20 轮4：按类边界从 workflows.cpp 析出。头文件契约不动。
// 注：ORT 辅助（onnxAreaGrid/resolveOnnxGrid/writeOnnxRaster）与本段同迁——
// 它们是 Prediction 独占（实测跨类使用 0 次），留在本文件即可。


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
  job->horizon = horizon;
  job->algorithmId = algorithmId;
  job->params = params;

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
    if ( onnx->loadModelMeta( model, nullptr, error ) != OnnxLoadStatus::Ok )
      return fail( ( error && !error->isEmpty() )
                       ? *error
                       : tr( "failed to load ONNX model '%1'" ).arg( model ) );

    // params-derived tensors; defaults keep toy-model runs trivial:
    // input {0}, shape {1}, inputName "x".
    const QVariantList inVals = params.value( QStringLiteral( "input" ) ).toList();
    if ( inVals.isEmpty() )
      job->onnxInput.append( 0.0f );
    else
      for ( const QVariant &v : inVals )
        job->onnxInput.append( v.toFloat() );

    const QVariantList shapeVals = params.value( QStringLiteral( "shape" ) ).toList();
    if ( shapeVals.isEmpty() )
      job->onnxShape.append( 1 );
    else
      for ( const QVariant &v : shapeVals )
        job->onnxShape.append( static_cast<int64_t>( v.toLongLong() ) );

    job->onnxInputName = params.value( QStringLiteral( "inputName" ) ).toString();
    if ( job->onnxInputName.isEmpty() )
      job->onnxInputName = QStringLiteral( "x" );

    // 网格几何（清单 horizon.D61* 声明）在 GUI 线程解析好，compute 直接用。
    const paleo::workflow_detail::OnnxAreaGrid grid = paleo::workflow_detail::onnxAreaGrid( layers );
    std::memcpy( job->onnxGt, grid.gt, sizeof( grid.gt ) );
    job->onnxProjection = grid.projection;
    job->onnxSourcePath = grid.sourcePath;
    job->onnxFromDecl = grid.fromDecl;

    // T26：预测栅格落 artifacts/derived + DERIVED 版本（父版本 = 提供 D61
    // geotransform 的时间栅格版本，可溯源时）。
    QString regErr;
    DerivedAssetRegistrar registrar = paleo::workflow_detail::derivedRegistrarOf( this, &regErr );
    if ( !registrar.isBound() )
      return fail( regErr );
    job->staging = registrar.stage(
        QStringLiteral( "onnx_prediction" ), tr( "%1 onnx %2 预测" ).arg( horizon, model ),
        QStringLiteral( "ONNX_%1_%2.tif" ).arg( horizon, model ), &regErr );
    if ( !job->staging.isValid() )
      return fail( regErr );
    job->sourceUri = QStringLiteral( "onnxworkflow/%1" ).arg( model );
    job->parentVersionIds = grid.fromDecl && !grid.sourcePath.isEmpty()
                                ? registrar.parentVersionIdsFor( QStringList{ grid.sourcePath } )
                                : QStringList();
    job->layerId = QStringLiteral( "pred.%1.onnx.%2" ).arg( horizon, model );
    job->layerGroup = QStringLiteral( "03_Predict" ); // 历史分组保持（m2(A)）
    job->layerTitle = tr( "%1 onnx %2 预测" ).arg( horizon, model );
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
      QStringLiteral( "prediction_raster" ), tr( "%1 %2 预测" ).arg( horizon, algorithmId ),
      QStringLiteral( "PREDICT_%1.tif" ).arg( horizon ), &regErr );
  if ( !job->staging.isValid() )
    return fail( regErr );
  job->params.insert( QStringLiteral( "OUTPUT" ), job->staging.absolutePath );

  // 父版本 = 预测输入图层（文件源可溯源时）——INPUT 层是 GUI 对象，其 source
  // 只在 GUI 线程读。
  QStringList parentPaths;
  if ( const QgsMapLayer *input = params.value( QStringLiteral( "INPUT" ) ).value<QgsMapLayer *>() )
  {
    const QString src = input->source().section( QLatin1Char( '|' ), 0, 0 );
    if ( paleo::workflow_detail::isFileBackedSource( src ) )
      parentPaths.append( src );
  }
  job->parentVersionIds = registrar.parentVersionIdsFor( parentPaths );
  job->sourceUri = QStringLiteral( "predictionworkflow/%1" ).arg( algorithmId );
  // m2(A)：稳定结果 id——同一 horizon+algorithmId 重跑复用同一 layerId
  // （manifest INSERT OR REPLACE upsert，清单不新增重复行）； tst_workflows
  // 既有断言 startsWith("predict.<h>.") 由稳定段继续满足。
  job->layerId = QStringLiteral( "predict.%1.%2" ).arg( horizon, stableResultSuffix( algorithmId ) );
  job->layerGroup = QStringLiteral( "01_Prediction" ); // 历史分组保持（m2(A)：新声明面才用 02_Prediction）
  job->layerTitle = tr( "%1 %2 预测" ).arg( horizon, algorithmId );
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

#if PALEO_HAVE_ORT
  if ( job->algorithmId.startsWith( QLatin1String( "onnx:" ) ) )
  {
    const QString model = job->algorithmId.mid( 5 );
    PaleoOnnxService *onnx = m_onnx.data();
    if ( !onnx )
      return fail( tr( "no ONNX service bound; cannot run '%1'" ).arg( job->algorithmId ) );

    // #144：绑定模型名推理，并发任务换活动模型不影响本次。
    const OnnxTensor tensor =
        onnx->runTensorOn( model, job->onnxInputName, job->onnxInput, job->onnxShape, &job->error );
    if ( tensor.values.isEmpty() )
      return fail( job->error.isEmpty()
                       ? tr( "ONNX model '%1' produced no output" ).arg( model )
                       : job->error );

    // 网格门禁（plan §1 trap）：输出挤成二维后必须是工区网格 411×641，否则
    // 不写栅格、不登记图层。geotransform 由 horizon.D61* 声明提供，缺省常量。
    paleo::workflow_detail::OnnxAreaGrid grid;
    std::memcpy( grid.gt, job->onnxGt, sizeof( grid.gt ) );
    grid.projection = job->onnxProjection;
    grid.sourcePath = job->onnxSourcePath;
    grid.fromDecl = job->onnxFromDecl;
    int rows = 0;
    int cols = 0;
    if ( !paleo::workflow_detail::resolveOnnxGrid( tensor.values, tensor.shape, rows, cols,
                                                   &job->error ) )
      return fail( job->error.isEmpty() ? tr( "cannot place ONNX output on a grid" ) : job->error );

    const QString outPath = paleo::workflow_detail::writeOnnxRaster(
        job->staging.absolutePath, tensor.values, rows, cols, grid, job->params, model,
        tensor.shape, &job->error );
    if ( outPath.isEmpty() )
      return fail( job->error.isEmpty() ? tr( "failed to write ONNX prediction raster" )
                                        : job->error );
    job->outputPath = outPath;
    job->commitExtra.insert( QStringLiteral( "model" ), model );
    job->commitExtra.insert( QStringLiteral( "rows" ), rows );
    job->commitExtra.insert( QStringLiteral( "cols" ), cols );
    job->commitExtra.insert( QStringLiteral( "geotransform_source" ),
                             grid.fromDecl ? QStringLiteral( "horizon.%1" ).arg(
                                                 AreaRules::active().targetHorizon )
                                           : QStringLiteral( "project_area" ) );
    job->ok = true;
    return true;
  }
#endif

  QgisProcessingService *proc = m_proc.data();
  if ( !proc )
    return fail( tr( "prediction workflow is not bound to services" ) );

  const QVariantMap results = proc->run( job->algorithmId, job->params, &job->error );
  if ( results.isEmpty() )
  {
    if ( job->error.isEmpty() )
      job->error = tr( "prediction algorithm '%1' produced no results" ).arg( job->algorithmId );
    return false;
  }

  job->outputPath = paleo::workflow_detail::outputPathOf( results );
  if ( job->outputPath.isEmpty() )
    return fail( tr( "prediction algorithm '%1' returned no output path" ).arg( job->algorithmId ) );
  job->ok = true;
  return true;
}

bool PredictionWorkflow::publishPredictionJob( const PredictionJob &job, QString *error )
{
  // publish 回 catalog 所属线程：commitExternal（addVersion 线程闸）、图层
  // 声明（GUI 对象）与 predictionDone 都在这里。
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
  QString commitErr;
  if ( !registrar.commitExternal( job.staging, job.outputPath, job.parentVersionIds,
                                  job.sourceUri, job.commitExtra, &commitErr ) )
    return fail( commitErr );

  LayerDeclaration decl;
  decl.layerId = job.layerId;
  decl.horizon = job.horizon;
  decl.type = QStringLiteral( "raster" );
  decl.source = job.staging.absolutePath;
  decl.group = job.layerGroup;
  decl.title = job.layerTitle;
  if ( !layers->declare( decl, error ) )
  {
    return fail( ( error && !error->isEmpty() )
                     ? *error
                     : tr( "failed to declare result layer '%1'" ).arg( decl.layerId ) );
  }

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
