// 层：功能
// workflow/geostatatomoperation.cpp — 见 geostatatomoperation.h。
#include "geostatatomoperation.h"

#include "geostatatominputs.h"
#include "nativecppadapter.h"
#include "nativeinputs_geostat.h"
#include "capabilitysourcepublication.h"
#include "constraintanalysisidentity.h"
#include "services/analysisunitcontext.h"
#include "services/geostaticapabilityspec.h"
#include "domain/stratigraphicunit.h"
#include "catalog/datacatalog.h"
#include "qgis/geostatsamplereader.h" // RasterCrs 由 WKT 在 worker 线程重建

#include "algorithms/geostat/types.h"
#include "algorithms/geostat/variogram.h"
// 重头直含（transitive 闸口径）：nativeinputs_geostat.h 扇出的核头在本 TU
// 直接消费，直接 include 边按工具口径不计出闸。
#include "algorithms/geostat/kriging.h"
#include "algorithms/geostat/sgs.h"
#include "algorithms/geostat/cokriging.h"
#include "algorithms/geostat/faultpath.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

#include <array>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

namespace paleo::geostat_atom
{
namespace
{

using paleo::capability::CapabilityExecutionAdapter;
using paleo::capability::CapabilityPreparation;
using paleo::capability::ExecuteRequest;
using paleo::capability::ExecuteResult;
using paleo::capability::FrozenOperation;
using paleo::capability::OperationInputValidator;
using paleo::capability::OperationLog;
using paleo::capability::OperationWorkContext;
using paleo::capability::OperationWorkResult;
using paleo::capability::OperationWorker;

// 适配器在 Status::Cancelled 时给出的唯一取消哨兵（algorithmadapters.cpp）。
constexpr char kAdapterCancelled[] = "已取消";

// 进发布成员谱系的计数诊断 / 进公开摘要的标量诊断（封闭集合；member_paths
// 等列表型诊断不外露）。
const QStringList &countDiagnostics()
{
  static const QStringList keys{ QStringLiteral( "finite_cells" ), QStringLiteral( "nodata_cells" ),
                                 QStringLiteral( "solver_failures" ),
                                 QStringLiteral( "merged_duplicates" ) };
  return keys;
}
const QStringList &scalarDiagnostics()
{
  static const QStringList keys{ QStringLiteral( "finite_cells" ), QStringLiteral( "nodata_cells" ),
                                 QStringLiteral( "solver_failures" ),
                                 QStringLiteral( "merged_duplicates" ),
                                 QStringLiteral( "sample_mean" ), QStringLiteral( "sample_std" ) };
  return keys;
}

paleo::geostat::VariogramModelType variogramTypeFor( const QString &name )
{
  if( name == QLatin1String( "exponential" ) )
    return paleo::geostat::VariogramModelType::Exponential;
  if( name == QLatin1String( "gaussian" ) )
    return paleo::geostat::VariogramModelType::Gaussian;
  return paleo::geostat::VariogramModelType::Spherical;
}

QString methodName( GeostatMethod method )
{
  return method == GeostatMethod::Sgs ? QStringLiteral( "sgs" ) : QStringLiteral( "kriging" );
}

QStringList outputPortsFor( GeostatMethod method )
{
  return method == GeostatMethod::Sgs ? QStringList{ QStringLiteral( "OUTPUT" ), QStringLiteral( "STD" ) }
                                      : QStringList{ QStringLiteral( "OUTPUT" ), QStringLiteral( "VARIANCE" ) };
}

// 成果种类：与 UI 单因素成图产物的 single_factor_raster 标记刻意区分——
// 无约束地统计 agent 成果不冒充成图产品，也不进入单元证据分类投影。
QString portKind( GeostatMethod method, const QString &portId )
{
  const bool sgs = method == GeostatMethod::Sgs;
  if( portId == QLatin1String( "OUTPUT" ) )
    return sgs ? QStringLiteral( "geostat_mean_raster" ) : QStringLiteral( "geostat_estimate_raster" );
  return sgs ? QStringLiteral( "geostat_std_raster" ) : QStringLiteral( "geostat_variance_raster" );
}

// 冻结单元身份（strat::toMap 的身份子集；displayName 等仅显示字段不入计划）。
QVariantMap unitIdentityMap( const paleo::strat::UnitSelection &unit )
{
  QVariantMap map;
  map.insert( QStringLiteral( "schemeId" ), unit.schemeId );
  map.insert( QStringLiteral( "revision" ), unit.revision );
  map.insert( QStringLiteral( "intervalId" ), unit.intervalId );
  map.insert( QStringLiteral( "subIntervalId" ), unit.subIntervalId );
  map.insert( QStringLiteral( "aggregate" ), false );
  return map;
}

// 发布成员谱系：单位/CRS/分析单元/来源摘要/有效参数全部来自冻结计划值。
QVariantMap publishExtra( const GeostatFrozenPlan &plan, const QString &portId )
{
  const QVariantMap &unit = plan.unit;
  QVariantMap extra;
  extra.insert( QStringLiteral( "unitSelection" ), unit );
  extra.insert( QStringLiteral( "subIntervalId" ), unit.value( QStringLiteral( "subIntervalId" ) ) );
  extra.insert( QStringLiteral( "intervalId" ), unit.value( QStringLiteral( "intervalId" ) ) );
  extra.insert( QStringLiteral( "schemeId" ), unit.value( QStringLiteral( "schemeId" ) ) );
  extra.insert( QStringLiteral( "schemeRevision" ), unit.value( QStringLiteral( "revision" ) ) );
  extra.insert( QStringLiteral( "sample_unit_association" ), plan.sample.association );
  extra.insert( QStringLiteral( "factor_id" ), plan.sample.factorId );
  extra.insert( QStringLiteral( "field" ), plan.sample.field );
  extra.insert( QStringLiteral( "value_unit" ), plan.sample.valueUnit );
  extra.insert( QStringLiteral( "method" ), methodName( plan.method ) );
  extra.insert( QStringLiteral( "method_actual" ), methodName( plan.method ) );
  extra.insert( QStringLiteral( "algorithm_id" ), algorithmIdForMethod( plan.method ) );
  extra.insert( QStringLiteral( "algorithm_contract_version" ), kGeostatAtomAlgorithmContractVersion );
  extra.insert( QStringLiteral( "kind" ), portKind( plan.method, portId ) );
  extra.insert( QStringLiteral( "cell_size" ), plan.grid.pixelWidth );
  extra.insert( QStringLiteral( "variogram" ), plan.variogram.toJson().toVariantMap() );
  extra.insert( QStringLiteral( "constraints_applied" ), QStringLiteral( "none" ) );
  extra.insert( QStringLiteral( "source_version_id" ), plan.sample.versionId );
  extra.insert( QStringLiteral( "source_sha256" ), plan.sample.sourceSha256 );
  extra.insert( QStringLiteral( "snapshot_sha256" ), plan.sample.snapshotSha256 );
  extra.insert( QStringLiteral( "crs_authid" ), plan.sample.crsAuthId );
  if( plan.method == GeostatMethod::Sgs )
  {
    extra.insert( QStringLiteral( "seed" ), QString::number( plan.seed ) );
    extra.insert( QStringLiteral( "realizations" ),
                  plan.effectiveParams.value( QStringLiteral( "REALIZATIONS" ) ) );
  }
  return extra;
}

QJsonObject planSummary( const GeostatFrozenPlan &plan )
{
  QJsonObject summary;
  summary.insert( "algorithm", algorithmIdForMethod( plan.method ) );
  summary.insert( "algorithm_contract_version", kGeostatAtomAlgorithmContractVersion );
  summary.insert( "method", methodName( plan.method ) );
  summary.insert( "sample_count", plan.sample.sampleCount );
  summary.insert( "skipped_rows", plan.sample.skippedRows );
  summary.insert( "source_version_id", plan.sample.versionId );
  summary.insert( "snapshot_sha256", plan.sample.snapshotSha256 );
  summary.insert( "factor_id", plan.sample.factorId );
  summary.insert( "field", plan.sample.field );
  summary.insert( "value_unit", plan.sample.valueUnit );
  summary.insert( "grid_cols", plan.grid.cols );
  summary.insert( "grid_rows", plan.grid.rows );
  summary.insert( "cell_size", plan.grid.pixelWidth );
  summary.insert( "subinterval_id", plan.unit.value( QStringLiteral( "subIntervalId" ) ).toString() );
  summary.insert( "constraints_applied", QStringLiteral( "none" ) );
  summary.insert( "peak_bytes", double( plan.estimate.peakBytes ) );
  if( plan.method == GeostatMethod::Sgs )
  {
    // 精确十进制字符串：内核 seed 是 qulonglong，经 JSON double 会丢精度。
    summary.insert( "seed", QString::number( plan.seed ) );
    summary.insert( "realizations",
                    plan.effectiveParams.value( QStringLiteral( "REALIZATIONS" ) ).toInt() );
  }
  return summary;
}

// 数值 worker：从冻结计划 + 样本集合跑既有核，写 attempt 私有目录。
// 闭包只持有纯值（冻结计划 / 样本 / CRS WKT）；QgsCRS 在执行线程由 WKT 重建。
OperationWorker geostatKernelWorker( GeostatFrozenPlan plan,
                                     std::shared_ptr<std::vector<paleo::geostat::Sample>> samples,
                                     QString crsWkt )
{
  return [plan, samples, crsWkt]( const OperationWorkContext &context ) {
    OperationWorkResult out;
    const auto fail = [&out]( const QString &code ) {
      out.ok = false;
      out.code = code;
      out.outputs = {};
      return out;
    };
    if( !samples || samples->empty() )
      return fail( QStringLiteral( "geostat_snapshot_unavailable" ) );
    if( context.cancelled && context.cancelled() )
      return fail( QStringLiteral( "cancelled" ) );

    paleo::geostat::GridSpec grid;
    grid.cols = plan.grid.cols;
    grid.rows = plan.grid.rows;
    grid.originX = plan.grid.originX;
    grid.originY = plan.grid.originY;
    grid.pixelWidth = plan.grid.pixelWidth;
    grid.pixelHeight = plan.grid.pixelHeight;
    grid.crs = crsWkt.toStdString();

    paleo::geostat::VariogramModel model;
    model.type = variogramTypeFor( plan.variogram.model );
    model.nugget = plan.variogram.nugget;
    model.sill = plan.variogram.sill; // 部分基台（与内核 VariogramModel 同义）
    model.range = plan.variogram.range;
    model.anisotropyRatio = plan.variogram.anisotropyRatio;
    model.azimuthDeg = plan.variogram.azimuthDeg;

    paleo::geostat::Control control;
    control.cancelled = context.cancelled;

    const std::array<double, 6> geoTransform{ plan.grid.originX, plan.grid.pixelWidth, 0.0,
                                             plan.grid.originY, 0.0, plan.grid.pixelHeight };
    const paleo::RasterCrs crs = paleo::geostat::geostatRasterCrsFromWkt( crsWkt );
    const QVariantMap &params = plan.effectiveParams;
    out.summary = planSummary( plan );

    QString kernelError;
    AlgorithmRunResult run;
    paleo::NativeCppAdapter native; // 既有类型化核入口（无状态）
    if( plan.method == GeostatMethod::Kriging )
    {
      paleo::OrdinaryKrigingInputs inputs;
      inputs.samples = *samples;
      inputs.grid = grid;
      inputs.model = model;
      inputs.params.maxPoints = params.value( QStringLiteral( "MAX_POINTS" ) ).toInt();
      inputs.params.minPoints = params.value( QStringLiteral( "MIN_POINTS" ) ).toInt();
      inputs.params.searchRadius = params.value( QStringLiteral( "SEARCH_RADIUS" ) ).toDouble();
      inputs.control = control;
      inputs.estimatePath = QDir( context.privateDir ).filePath( QStringLiteral( "OUTPUT.tif" ) );
      inputs.variancePath = QDir( context.privateDir ).filePath( QStringLiteral( "VARIANCE.tif" ) );
      inputs.geoTransform = geoTransform;
      inputs.crs = crs;
      inputs.canonicalCrsWkt = crsWkt;
      run = native.runOrdinaryKriging( inputs, &kernelError );
    }
    else
    {
      paleo::SgsInputs inputs;
      inputs.samples = *samples;
      inputs.grid = grid;
      inputs.model = model;
      inputs.params.nRealizations = params.value( QStringLiteral( "REALIZATIONS" ) ).toInt();
      inputs.params.seed = plan.seed;
      inputs.params.maxPoints = params.value( QStringLiteral( "MAX_POINTS" ) ).toInt();
      inputs.params.searchRadius = params.value( QStringLiteral( "SEARCH_RADIUS" ) ).toDouble();
      // 首版契约：无屏障、不在正态得分上重拟合、不持久化个体实现（仅 OUTPUT/STD）。
      inputs.params.barrier.reset();
      inputs.params.refitVariogramOnNormalScores = false;
      inputs.control = control;
      inputs.meanPath = QDir( context.privateDir ).filePath( QStringLiteral( "OUTPUT.tif" ) );
      inputs.stdPath = QDir( context.privateDir ).filePath( QStringLiteral( "STD.tif" ) );
      inputs.persistRealizations = false;
      inputs.memberPathPrefix = QString();
      inputs.geoTransform = geoTransform;
      inputs.crs = crs;
      inputs.canonicalCrsWkt = crsWkt;
      run = native.runSgs( inputs, &kernelError );
    }
    if( !kernelError.isEmpty() )
    {
      out.summary.insert( "kernel_error", kernelError.left( 1024 ) );
      const bool cancelled = kernelError == QLatin1String( kAdapterCancelled ) ||
                             ( context.cancelled && context.cancelled() );
      return fail( cancelled ? QStringLiteral( "cancelled" ) : QStringLiteral( "geostat_compute_failed" ) );
    }
    const QVariantMap &diagnostics = run.diagnostics;
    // 全域无有效值（如半径模式下邻点不足）不是成功：不把失败包装成空栅格。
    const int finiteCells = diagnostics.value( QStringLiteral( "finite_cells" ) ).toInt();
    if( finiteCells <= 0 )
    {
      out.summary.insert( "finite_cells", finiteCells );
      return fail( QStringLiteral( "geostat_no_finite_cells" ) );
    }
    // 端口契约：恰为固定端口集且每个成员是已落盘的常规文件；任何多余端口
    //（如 MEMBER_n）或缺失成员都不暴露部分成果。
    const QStringList expected = outputPortsFor( plan.method );
    QStringList actual;
    for( const auto &artifact : run.artifacts )
    {
      actual.append( artifact.portId );
      const QFileInfo file( artifact.absolutePath );
      if( !expected.contains( artifact.portId ) )
        return fail( QStringLiteral( "geostat_output_contract_violation" ) );
      if( !file.isFile() || file.isSymLink() || file.size() <= 0 )
        return fail( QStringLiteral( "geostat_output_incomplete" ) );
    }
    actual.sort();
    if( actual != expected )
      return fail( QStringLiteral( "geostat_output_incomplete" ) );
    for( const auto &artifact : run.artifacts )
    {
      AlgorithmOutputArtifact mapped;
      mapped.portId = artifact.portId;
      mapped.absolutePath = artifact.absolutePath;
      QVariantMap extra = publishExtra( plan, artifact.portId );
      for( const QString &key : countDiagnostics() )
        if( diagnostics.contains( key ) )
          extra.insert( key, diagnostics.value( key ) );
      mapped.extra = extra;
      out.outputs.artifacts.append( mapped );
    }
    // 公开摘要只补计划摘要没有的标量诊断；seed/realizations 以冻结计划的
    // 精确值为准（内核诊断的 seed 是 qulonglong，经 JSON double 会丢精度）。
    for( const QString &key : scalarDiagnostics() )
      if( diagnostics.contains( key ) && !out.summary.contains( key ) )
        out.summary.insert( key, QJsonValue::fromVariant( diagnostics.value( key ) ) );
    out.outputs.ok = true;
    out.ok = true;
    return out;
  };
}

// 惰性 worker：进入 worker 线程后才有界重读快照（缺失/摘要不符即如实失败）。
OperationWorker lazySnapshotKernelWorker( GeostatFrozenPlan plan )
{
  return [plan]( const OperationWorkContext &context ) {
    const GeostatRestoredSamples restored =
        restoreGeostatSampleSnapshot( context.projectDir, plan.sample );
    if( !restored.ok )
    {
      OperationWorkResult out;
      out.ok = false;
      out.code = restored.code;
      return out;
    }
    auto samples = std::make_shared<std::vector<paleo::geostat::Sample>>( restored.samples );
    return geostatKernelWorker( plan, std::move( samples ), restored.crsWkt )( context );
  };
}

// compute-only 继承：从持久 prepare 记录复用冻结计划（不再取当前样本）。
// parametersGiven：请求自带参数须与冻结计划的有效参数一致（省略 = 继承）。
FrozenOperation inheritPreparedPlan( const ExecuteRequest &request, const QJsonObject &intent,
                                     const OperationLog::Record &source,
                                     const GeostatDecodedRequest &decoded, bool parametersGiven,
                                     QString *error )
{
  const auto fail = [error]( const QString &code ) {
    if( error )
      *error = code;
    return FrozenOperation{};
  };
  if( source.schemaVersion != 2 || source.documentId != request.documentId ||
      source.callerScope != request.callerScope || source.capabilityId != request.capabilityId ||
      source.contractVersion != request.contractVersion )
    return fail( QStringLiteral( "source_operation_mismatch" ) );
  if( source.execution != QLatin1String( "prepared" ) || !source.preparedManifest.isEmpty() ||
      source.cancelRequested )
    return fail( QStringLiteral( "source_result_unavailable" ) );
  bool ok = false;
  const GeostatFrozenPlan plan =
      GeostatFrozenPlan::fromJson( source.frozenRequest.value( "plan" ).toObject(), &ok );
  if( !ok )
    return fail( QStringLiteral( "source_operation_mismatch" ) );
  bool methodKnown = false;
  const GeostatMethod method = methodForCapability( request.capabilityId, &methodKnown );
  if( !methodKnown || plan.method != method )
    return fail( QStringLiteral( "source_operation_mismatch" ) );
  if( const QString code = validateGeostatFrozenPlan( plan ); !code.isEmpty() )
    return fail( code );
  // 冻结计划是数值输入的唯一来源：请求自带的参数不得悄悄改写它。
  if( parametersGiven && QJsonObject::fromVariantMap( decoded.effectiveParams ) !=
                            QJsonObject::fromVariantMap( plan.effectiveParams ) )
    return fail( QStringLiteral( "geostat_source_parameters_conflict" ) );

  FrozenOperation frozen;
  frozen.capabilityId = request.capabilityId;
  frozen.contractVersion = request.contractVersion;
  frozen.callerScope = request.callerScope;
  frozen.clientRequestId = request.clientRequestId;
  frozen.intent = intent;
  frozen.frozenPlan = plan.toJson();
  frozen.phase = QStringLiteral( "compute-only" );
  frozen.sourceOperationId = source.operationId;
  frozen.outputSpec = AlgorithmSpecRegistry::specAtVersion( algorithmIdForMethod( method ),
                                                            kGeostatAtomAlgorithmContractVersion );
  if( !frozen.outputSpec.complete )
    return fail( QStringLiteral( "algorithm_not_available" ) );
  frozen.implementationVersion = source.implementationVersion;
  frozen.outputPorts = outputPortsFor( method );
  for( const auto &parent : source.stagePlan.value( "parents" ).toArray() )
    frozen.parentVersionIds.append( parent.toString() );
  frozen.worker = lazySnapshotKernelWorker( plan );
  frozen.inputValidator = [plan]( const OperationWorkContext & context ) {
    return verifyGeostatSnapshotFile( context.projectDir, plan.sample );
  };
  return frozen;
}

CapabilityPreparation capture( GeostatMethod method, const GeostatAtomProviders &providers,
                               const ExecuteRequest &request, const QJsonObject &intent,
                               QString *error )
{
  const auto fail = [error]( const QString &code ) {
    if( error )
      *error = code;
    return CapabilityPreparation{};
  };
  if( request.capabilityId != capabilityIdForMethod( method ) || request.contractVersion != 1 )
    return fail( QStringLiteral( "execution_adapter_unavailable" ) );
  if( request.documentId.isEmpty() || request.callerScope.isEmpty() ||
      request.clientRequestId.isEmpty() || request.projectDir.isEmpty() )
    return fail( QStringLiteral( "geostat_scope_required" ) );

  GeostatDecodedRequest decoded;
  if( const QString code = decodeGeostatAtomRequest( method, request.parameters, request.inputs,
                                                     request.publishIntent, &decoded );
      !code.isEmpty() )
    return fail( code );

  // compute-only 继承：复用持久 prepare 的冻结计划，不重新取样本。
  if( !decoded.sourceOperationId.isEmpty() )
  {
    OperationLog log( request.operationDir, request.projectDir, {} );
    const OperationLog::RecordLookup found = log.lookupRecordById( decoded.sourceOperationId );
    if( found.code != QLatin1String( "found" ) )
      return fail( QStringLiteral( "untrusted_result_ref" ) );
    bool planOk = false;
    const GeostatFrozenPlan inheritedPlan =
        GeostatFrozenPlan::fromJson( found.record.frozenRequest.value( "plan" ).toObject(), &planOk );
    if( !planOk )
      return fail( QStringLiteral( "source_operation_mismatch" ) );
    QString inheritError;
    const FrozenOperation inherited =
        inheritPreparedPlan( request, intent, found.record, decoded,
                             !request.parameters.isEmpty(), &inheritError );
    if( !inheritError.isEmpty() )
      return fail( inheritError );
    CapabilityPreparation preparation;
    preparation.worker = [inherited, inheritedPlan, projectDir = request.projectDir](
                             const std::function<bool()> &cancelled,
                             QString *workerError ) -> FrozenOperation {
      const auto fail = [workerError]( const QString &code ) {
        if( workerError )
          *workerError = code;
        return FrozenOperation{};
      };
      if( cancelled && cancelled() )
        return fail( QStringLiteral( "cancelled" ) );
      // 立项前在工作线程核对私有快照（整文件哈希不进 owner 回调）：缺失/被改
      // → 明确不可恢复，不建持久 compute 操作，也不回退到当前样本。
      if( const QString code = verifyGeostatSnapshotFile( projectDir, inheritedPlan.sample );
          !code.isEmpty() )
        return fail( code );
      if( workerError )
        workerError->clear();
      return inherited;
    };
    // owner 复核：绑定代次仍有效，且源记录仍属于同一文档/作用域。
    preparation.validate = [providers, sourceId = decoded.sourceOperationId,
                            documentId = request.documentId, scope = request.callerScope,
                            operationDir = request.operationDir, projectDir = request.projectDir]() {
      if( !providers.alive || !providers.alive() )
        return QStringLiteral( "stale_session" );
      OperationLog log( operationDir, projectDir, {} );
      const auto found = log.lookupRecordById( sourceId );
      if( found.code != QLatin1String( "found" ) || found.record.documentId != documentId ||
          found.record.callerScope != scope )
        return QStringLiteral( "source_operation_mismatch" );
      return QString();
    };
    if( error )
      error->clear();
    return preparation;
  }

  // 受信解析（owner）：单元实体核验 + 受管样本版本 + 单元关联核验。
  if( !providers.alive || !providers.alive() )
    return fail( QStringLiteral( "stale_session" ) );
  DataCatalog *catalog = providers.catalog ? providers.catalog() : nullptr;
  if( !catalog || !catalog->isOpen() )
    return fail( QStringLiteral( "document_unavailable" ) );

  const paleo::strat::UnitSelection requested = paleo::strat::fromMap( decoded.unitSelection );
  if( !requested.isValid() )
    return fail( QStringLiteral( "geostat_unit_incomplete" ) );
  paleo::strat::UnitSelection frozenUnit;
  if( !paleo::strat::freezeAnalysisUnit( request.projectDir, requested, frozenUnit, nullptr,
                                         /*allowAggregate=*/false ) )
    return fail( QStringLiteral( "geostat_unit_invalid" ) );

  const QString versionId =
      paleo::constraint_identity::resolveManagedVersionRef( catalog, decoded.sampleRef );
  if( versionId.isEmpty() )
    return fail( QStringLiteral( "geostat_sample_version_missing" ) );
  const CatalogVersion version = catalog->versionById( versionId );
  if( version.id != versionId || !version.managed )
    return fail( QStringLiteral( "geostat_sample_version_missing" ) );
  if( version.sha256.isEmpty() )
    return fail( QStringLiteral( "geostat_source_fingerprint_required" ) );
  const QString projectRoot = QFileInfo( catalog->projectDir() ).canonicalFilePath();
  const QString sourcePath =
      QFileInfo( DataCatalog::resolvedVersionPath( catalog->projectDir(), version ) )
          .canonicalFilePath();
  // 样本文件只能来自工程内的受管版本（不接受链接别名/越界路径）。
  if( projectRoot.isEmpty() || sourcePath.isEmpty() || !sourcePath.startsWith( projectRoot + '/' ) )
    return fail( QStringLiteral( "geostat_source_unavailable" ) );

  const QString manifestLayerKey = version.extra.value( QStringLiteral( "manifest_layer_id" ) ).toString();
  const QString layerKey = manifestLayerKey.isEmpty()
                               ? version.extra.value( QStringLiteral( "layer_id" ) ).toString()
                               : manifestLayerKey;
  // 样本↔单元的可验证关联（证据不足不盖 verified）：
  //   一、井点层键挂冻结单元父上界面——工程既有约定（UI 链 wellsLayerIdFor 同口径）；
  //   二、版本自带同一单元身份：scheme/interval/sub 三元组逐字一致（修订是几何
  //       版本，不改变单元身份；仅 subIntervalId 相同不足以证明关联）。
  const bool horizonMatched = layerKey.startsWith( QStringLiteral( "wells." ) ) &&
                              layerKey.mid( 6 ) == frozenUnit.topBoundary;
  const QVariantMap versionUnit = version.extra.value( QStringLiteral( "unitSelection" ) ).toMap();
  const bool unitMatched =
      versionUnit.value( QStringLiteral( "schemeId" ) ).toString() == frozenUnit.schemeId &&
      versionUnit.value( QStringLiteral( "intervalId" ) ).toString() == frozenUnit.intervalId &&
      versionUnit.value( QStringLiteral( "subIntervalId" ) ).toString() == frozenUnit.subIntervalId;
  if( !horizonMatched && !unitMatched )
    return fail( QStringLiteral( "geostat_sample_unit_unverified" ) );

  // 冻结计划（物化前字段；快照凭据、实际样本数与估算由 worker 补齐）。
  GeostatFrozenPlan plan;
  plan.method = method;
  plan.effectiveParams = decoded.effectiveParams;
  plan.variogram = decoded.variogram;
  plan.grid = decoded.grid;
  plan.seed = decoded.seed;
  plan.unit = unitIdentityMap( frozenUnit );
  plan.estimate = decoded.estimate;
  plan.sample.versionId = versionId;
  plan.sample.sourceSha256 = version.sha256;
  plan.sample.layerKey = layerKey;
  plan.sample.association = horizonMatched ? QStringLiteral( "layer_key" )
                                           : QStringLiteral( "version_unit" );
  plan.sample.factorId = decoded.factorId;
  plan.sample.field = decoded.factorField;
  plan.sample.valueUnit = decoded.factorUnit;

  // worker 闭包只携带值字段（python analysis 族同款）：不把 ExecuteRequest
  // 里的 catalog 指针/写锁 claim 带进 TaskService 线程。
  ExecuteRequest identity;
  identity.capabilityId = request.capabilityId;
  identity.contractVersion = request.contractVersion;
  identity.documentId = request.documentId;
  identity.callerScope = request.callerScope;
  identity.clientRequestId = request.clientRequestId;
  identity.publishIntent = request.publishIntent;
  identity.projectDir = request.projectDir;

  CapabilityPreparation preparation;
  preparation.worker = [plan, sourcePath, identity, intent](
                           const std::function<bool()> &cancelled,
                           QString *workerError ) mutable -> FrozenOperation {
    const auto fail = [workerError]( const QString &code ) {
      if( workerError )
        *workerError = code;
      return FrozenOperation{};
    };
    const GeostatMaterializedSamples materialized = materializeGeostatSampleSnapshot(
        identity.projectDir, sourcePath, plan.sample.versionId, plan.sample.sourceSha256,
        plan.sample.factorId, plan.sample.field, cancelled );
    if( !materialized.ok )
      return fail( materialized.code );
    GeostatFrozenPlan frozenPlan = plan;
    frozenPlan.sample.snapshotPath = materialized.snapshotPath;
    frozenPlan.sample.snapshotSha256 = materialized.snapshotSha256;
    frozenPlan.sample.sampleCount = materialized.sampleCount;
    frozenPlan.sample.skippedRows = materialized.skippedRows;
    frozenPlan.sample.crsAuthId = materialized.crsAuthId;
    frozenPlan.sample.crsWktSha256 = materialized.crsWktSha256;
    bool ok = false;
    // 物化后按实际样本数重算估算（准入上界 → 实际成本），再整体校验计划。
    frozenPlan.estimate =
        estimateGeostatResources( frozenPlan.method, frozenPlan.effectiveParams, frozenPlan.grid.cols,
                                  frozenPlan.grid.rows, frozenPlan.sample.sampleCount, &ok );
    if( !ok )
      return fail( QStringLiteral( "geostat_sample_plan_invalid" ) );
    if( const QString code = geostatEstimateAdmission( frozenPlan.estimate ); !code.isEmpty() )
      return fail( code );
    if( const QString code = validateGeostatFrozenPlan( frozenPlan ); !code.isEmpty() )
      return fail( code );

    FrozenOperation frozen;
    frozen.capabilityId = identity.capabilityId;
    frozen.contractVersion = identity.contractVersion;
    frozen.callerScope = identity.callerScope;
    frozen.clientRequestId = identity.clientRequestId;
    frozen.intent = intent;
    frozen.frozenPlan = frozenPlan.toJson();
    frozen.phase = identity.publishIntent.isEmpty() ? QStringLiteral( "compute-only" )
                                                    : identity.publishIntent;
    frozen.outputSpec = AlgorithmSpecRegistry::specAtVersion(
        algorithmIdForMethod( frozenPlan.method ), kGeostatAtomAlgorithmContractVersion );
    if( !frozen.outputSpec.complete )
      return fail( QStringLiteral( "algorithm_not_available" ) );
    frozen.implementationVersion = frozen.outputSpec.implementationVersion;
    frozen.outputPorts = outputPortsFor( frozenPlan.method );
    frozen.parentVersionIds = QStringList{ frozenPlan.sample.versionId };
    auto samples = std::make_shared<std::vector<paleo::geostat::Sample>>( materialized.samples );
    frozen.worker = geostatKernelWorker( frozenPlan, std::move( samples ), materialized.crsWkt );
    frozen.inputValidator = [frozenPlan]( const OperationWorkContext & context ) {
      return verifyGeostatSnapshotFile( context.projectDir, frozenPlan.sample );
    };
    if( workerError )
      workerError->clear();
    return frozen;
  };
  // owner 复核：物化期间绑定代次未失效、源版本仍存在。
  preparation.validate = [providers, capturedVersionId = versionId]() {
    if( !providers.alive || !providers.alive() )
      return QStringLiteral( "stale_session" );
    DataCatalog *catalog = providers.catalog ? providers.catalog() : nullptr;
    if( !catalog || !catalog->isOpen() )
      return QStringLiteral( "document_unavailable" );
    if( catalog->versionById( capturedVersionId ).id != capturedVersionId )
      return QStringLiteral( "input_version_missing" );
    return QString();
  };
  if( error )
    error->clear();
  return preparation;
}

// 持久记录 ↔ 计划的一致性（validateFrozen / restore 共用）。
QString checkGeostatRecord( const OperationLog::Record &record )
{
  bool methodKnown = false;
  const GeostatMethod method = methodForCapability( record.capabilityId, &methodKnown );
  if( !methodKnown || record.contractVersion != 1 )
    return QStringLiteral( "invalid_geostat_plan" );
  bool ok = false;
  const GeostatFrozenPlan plan =
      GeostatFrozenPlan::fromJson( record.frozenRequest.value( "plan" ).toObject(), &ok );
  if( !ok || plan.method != method )
    return QStringLiteral( "invalid_geostat_plan" );
  if( const QString code = validateGeostatFrozenPlan( plan ); !code.isEmpty() )
    return code;
  const AlgorithmSpec spec = AlgorithmSpecRegistry::specAtVersion( algorithmIdForMethod( method ),
                                                                   kGeostatAtomAlgorithmContractVersion );
  QStringList parents;
  for( const auto &parent : record.stagePlan.value( "parents" ).toArray() )
    parents.append( parent.toString() );
  if( !spec.complete || spec.implementationVersion != record.implementationVersion ||
      parents != QStringList{ plan.sample.versionId } )
    return QStringLiteral( "invalid_geostat_plan" );
  return QString();
}

bool planFromRecord( const OperationLog::Record &record, GeostatFrozenPlan *plan )
{
  bool methodKnown = false;
  const GeostatMethod method = methodForCapability( record.capabilityId, &methodKnown );
  bool ok = false;
  *plan = GeostatFrozenPlan::fromJson( record.frozenRequest.value( "plan" ).toObject(), &ok );
  return methodKnown && ok && plan->method == method;
}

} // namespace

CapabilityExecutionAdapter geostatAtomExecutionAdapter( const QString &capabilityId,
                                                        const GeostatAtomProviders &providers )
{
  CapabilityExecutionAdapter adapter;
  adapter.intent = []( const ExecuteRequest &request ) {
    return paleo::capability::capabilityIntent( request );
  };
  adapter.capture = [capabilityId, providers]( const ExecuteRequest &request,
                                               const QJsonObject &intent, QString *error ) {
    bool methodKnown = false;
    const GeostatMethod method = methodForCapability( capabilityId, &methodKnown );
    if( !methodKnown )
    {
      if( error )
        *error = QStringLiteral( "execution_adapter_unavailable" );
      return CapabilityPreparation{};
    }
    return capture( method, providers, request, intent, error );
  };
  adapter.validateFrozen = [providers]( const OperationLog::Record &record ) {
    if( !providers.alive || !providers.alive() )
      return QStringLiteral( "stale_session" );
    if( const QString code = checkGeostatRecord( record ); !code.isEmpty() )
      return code;
    // 源版本仍存在（与 Recovery::accept 的 input_version_missing 同口径）；
    // 缺失即如实拒绝，不臆造重取。
    DataCatalog *catalog = providers.catalog ? providers.catalog() : nullptr;
    if( catalog && catalog->isOpen() )
    {
      GeostatFrozenPlan plan;
      if( planFromRecord( record, &plan ) && catalog->versionById( plan.sample.versionId ).id != plan.sample.versionId )
        return QStringLiteral( "input_version_missing" );
    }
    return QString();
  };
  adapter.restoreWorker = []( const OperationLog::Record &record,
                              QString *error ) -> OperationWorker {
    GeostatFrozenPlan plan;
    if( !planFromRecord( record, &plan ) || !checkGeostatRecord( record ).isEmpty() )
    {
      if( error )
        *error = QStringLiteral( "invalid_geostat_plan" );
      return {};
    }
    if( error )
      error->clear();
    // 快照在 worker 线程有界重读；缺失即如实失败（geostat_snapshot_unavailable）。
    return lazySnapshotKernelWorker( plan );
  };
  adapter.restoreInputValidator = []( const OperationLog::Record &record,
                                      QString *error ) -> OperationInputValidator {
    GeostatFrozenPlan plan;
    if( !planFromRecord( record, &plan ) )
    {
      if( error )
        *error = QStringLiteral( "invalid_geostat_plan" );
      return {};
    }
    if( error )
      error->clear();
    // 发布只搬运已计算产物（outputsFromManifest 逐成员校验摘要），不依赖快照。
    if( record.frozenRequest.value( "phase" ).toString() == QLatin1String( "publish" ) )
      return []( const OperationWorkContext & ) { return QString(); };
    return [plan]( const OperationWorkContext & context ) {
      return verifyGeostatSnapshotFile( context.projectDir, plan.sample );
    };
  };
  return adapter;
}

QJsonObject geostatAtomToolResponse( QJsonObject payload, const ExecuteResult &result,
                                     const QString &projectDir, const QString &operationDir,
                                     const QString &documentId, const QString &callerScope )
{
  if( result.operationId.isEmpty() || projectDir.isEmpty() || documentId.isEmpty() ||
      callerScope.isEmpty() )
    return payload;
  OperationLog log( operationDir, projectDir, {} );
  const OperationLog::RecordLookup found = log.lookupRecordById( result.operationId );
  // Recovery 同款授权口径：仅同文档、同调用方作用域的 schema-v2 记录可读。
  if( found.code != QLatin1String( "found" ) || found.record.schemaVersion != 2 ||
      found.record.documentId != documentId || found.record.callerScope != callerScope )
    return payload;
  GeostatFrozenPlan plan;
  if( !planFromRecord( found.record, &plan ) )
    return payload;
  payload.insert( QStringLiteral( "frozenPlan" ), geostatPlanFacts( plan ) );
  return QJsonDocument::fromJson( paleo::capability::boundToolResponse( payload ) ).object();
}

} // namespace paleo::geostat_atom
