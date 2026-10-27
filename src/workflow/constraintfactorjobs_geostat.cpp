// 层：功能
// 方向57：从 constraintfactorjobs.cpp 按函数族析出。方法定义逐字搬迁，
// 类契约仍在 workflows.h（公共 API 零改动）；族间共享辅助经
// constraintfactorjobs_internal.h。
#include "../algorithms/ensemblestats.h" // StatsRequest（方向47 集合统计派生）
#include "../algorithms/geostat/cokriging.h"
#include "../algorithms/geostat/crossvalidation.h" // P2 LOO（P4 抽出） // CoKrigingModel / ordinaryCoKriging
#include "../algorithms/geostat/kriging.h" // KrigingParams / KrigingResult（方向18）
#include "../algorithms/geostat/sgs.h"       // SgsParams / SgsResult（方向18）
#include "../algorithms/geostat/variogram.h" // 变差函数模型（方向18）
#include "../algorithms/rasterout.h" // PaleoRasterOut / createFloatRaster
#include "../algorithms/singlefactor/cartographicworkfile.h"
#include "../catalog/datacatalog.h"
#include "../domain/arearules.h"
#include "../domain/singlefactorrequest.h" // 制图工作场不进融合/分相
#include "../io/constraintstore.h"
#include "../metadata/paleoprojectstore.h"
#include "../qgis/factorcontour.h"
#include "../qgis/factorstylewriter.h"
#include "../qgis/qgiseditingservice.h" // 拓扑提交门（geometryCommitError）
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprocessingservice.h"
#include "../qgis/qgisstyleservice.h" // applyFaciesBoundaryStyle
#include "../services/jobrunner.h"
#include "../services/paleotaskservice.h"
#include "../services/singlefactordef.h"
#include "boundarysemantics.h" // 相界地质语义类型词表
#include "constraintfactorjobs_internal.h"
#include "constraintworkflow_internal.h"
#include "constraintuseplan.h"
#include "derivedassets.h"
#include "algorithmpublish.h"
#include "algorithmadapters.h"
#include "nativecppadapter.h"
#include "nativeinputs_geostat.h"
#include "mappingworkflow.h"
#include "realizationworkflow.h"
#include "workflows.h"
#include "constraintjobs.h" // #558：job 族完整定义（原内嵌 workflows.h）
#include "workflows_internal.h"
#include "../io/atomicwrite.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUuid>
#include <QVariantList>
#include <algorithm>
#include <array>
#include <cmath>
#include <cpl_conv.h>
#include <cstring>
#include <gdal.h>
#include <ogr_spatialref.h>
#include <qgscoordinatereferencesystem.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfeaturerequest.h>
#include <qgsfield.h>
#include <qgsgeometry.h>
#include <qgsmaplayer.h>
#include <qgsrasterlayer.h>
#include <qgsvectorfilewriter.h> // WS-C：参与井点 samples GPKG 写出
#include <qgsvectorlayer.h>
#include <qgsvectordataprovider.h>
#include <variant>

using namespace paleo::constraint_detail;

// ---- 方向18：克里金 / SGS 三段式（LocalDirectionJob 同构）----

namespace
{
// 克里金/SGS 的降级阈值：有效样本少于 8 口时变差函数欠定，自动降级
// 约束 IDW 并如实记 method_actual="idw"。降级是能力事实，不是性能裁剪
//（局部邻域克里金对大样本量无悬崖，见 docs/progress/geostat-methods.md）。
constexpr int kGeostatMinSamples = 8;
// 工作流层栅格预算：交互式成图上限 4M 格（核层另有 1e8 硬闸）。
constexpr qint64 kGeostatMaxWorkflowCells = 4'000'000;

paleo::geostat::VariogramModelType variogramTypeFor( const QString &name )
{
  if ( name == QLatin1String( "exponential" ) )
    return paleo::geostat::VariogramModelType::Exponential;
  if ( name == QLatin1String( "gaussian" ) )
    return paleo::geostat::VariogramModelType::Gaussian;
  return paleo::geostat::VariogramModelType::Spherical;
}

QString variogramTypeName( paleo::geostat::VariogramModelType type )
{
  switch ( type )
  {
    case paleo::geostat::VariogramModelType::Spherical:
      return QStringLiteral( "spherical" );
    case paleo::geostat::VariogramModelType::Exponential:
      return QStringLiteral( "exponential" );
    case paleo::geostat::VariogramModelType::Gaussian:
      return QStringLiteral( "gaussian" );
  }
  return QStringLiteral( "spherical" );
}

// NaN → nodata 的 float32 写出（mincurvature 同款 geoTransform/CRS 口径）。
bool writeFloatRaster( const QString &path, const std::vector<double> &values, int cols, int rows,
                       const double geoTransform[6], const QgsCoordinateReferenceSystem &crs,
                       const QString &canonicalCrsWkt = QString() )
{
  // PW-040：nodata 哨兵取在数据值域之外——真实值恰为 -9999 时不被当作无值。
  double minFinite = std::numeric_limits<double>::infinity();
  for ( double v : values )
    if ( std::isfinite( v ) )
      minFinite = std::min( minFinite, v );
  double sentinel = -9999.0;
  if ( std::isfinite( minFinite ) && minFinite <= sentinel )
    sentinel = std::floor( minFinite ) - 10000.0;
  const float sentinelF = static_cast<float>( sentinel );
  GDALDatasetH ds = PaleoRasterOut::createFloatRaster( path, cols, rows, geoTransform, crs, sentinel, canonicalCrsWkt );
  if ( !ds )
    return false;
  std::vector<float> row( static_cast<std::size_t>( cols ) );
  bool ok = true;
  for ( int r = 0; r < rows && ok; ++r )
  {
    for ( int c = 0; c < cols; ++c )
    {
      const double value = values[static_cast<std::size_t>( r ) * cols + c];
      row[static_cast<std::size_t>( c )] = std::isfinite( value ) ? static_cast<float>( value ) : sentinelF;
    }
    if ( GDALRasterIO( GDALGetRasterBand( ds, 1 ), GF_Write, 0, r, cols, 1,
                       row.data(), cols, 1, GDT_Float32, 0, 0 ) != CE_None )
      ok = false;
  }
  GDALClose( ds );
  return ok;
}

QVariantMap variogramExtra( const paleo::geostat::VariogramModel &model, double fitR2, double fitRmse )
{
  QVariantMap map;
  map.insert( QStringLiteral( "model_type" ), variogramTypeName( model.type ) );
  map.insert( QStringLiteral( "nugget" ), model.nugget );
  map.insert( QStringLiteral( "sill" ), model.sill );
  map.insert( QStringLiteral( "range" ), model.range );
  map.insert( QStringLiteral( "anisotropy_ratio" ), model.anisotropyRatio );
  map.insert( QStringLiteral( "azimuth" ), model.azimuthDeg );
  map.insert( QStringLiteral( "fit_r2" ), fitR2 );
  map.insert( QStringLiteral( "fit_rmse" ), fitRmse );
  return map;
}

// P2 留一交叉验证已移到 algorithms/geostat/crossvalidation（P4：供性能测试直接调用同一实现）。
using paleo::geostat::CvPrediction;
using paleo::geostat::leaveOneOut;

// 原子整写 + 校验 write()==size：旧实现无视 write() 结果恒返回 true，磁盘满时
// 半截 qc.json 会随临时目录一起发布。
bool writeJsonFile( const QString &path, const QVariantMap &payload )
{
  return paleo::writeJsonAtomic( path, QJsonDocument( QJsonObject::fromVariantMap( payload ) ),
                                 nullptr, QJsonDocument::Compact );
}

// ---- #554：computeGeostatJobWithService 按段拆分 ----
// 段序：采样 →（样本不足早退降级 IDW）→ 网格 → 变差函数 → QC 输入面 →
// 方法执行（kriging/cokriging/sgs 三选一）→ QC 落盘 → 暂存。各段可独立
// 调用；除「采样」（临时目录尚未建立）与「暂存」（成果已拷出，原路径失败
// 即不回收临时目录）外，段失败统一由编排层 cleanupTemp()——与原内联路径
// 的清理时机一一对应。函数体自原函数逐字搬迁，tr 显式限定
// ConstraintWorkflow::tr（与原静态成员内裸 tr 同一翻译上下文）。

// 建模段共享上下文：网格/变差函数/网格变换/进度控制/邻域参数（字段与原
// 局部变量同名）。
struct GeostatRunContext
{
  ConstraintWorkflow::GeostatJob *job = nullptr;
  QString tempDir;
  paleo::geostat::GridSpec grid;
  paleo::geostat::VariogramModel model;
  std::array<double, 6> geoTransform{};
  QgsCoordinateReferenceSystem crs;
  paleo::geostat::Control control;
  int maxPoints = 16;
  double searchRadius = 0.0;
  int minPoints = 4;
};

// 建模段产物——QC 落盘的原料（字段与原局部变量同名，初值同原声明）。
struct GeostatQcParts
{
  bool autoFit = true;
  double fitR2 = 0;
  double fitRmse = 0;
  QVariantMap modelProvenance; // P2：变差函数来源与被派生/钳制的量（不进参数指纹）
  QVariantList ignoredInputs;
  QVariantMap searchNode;
  QVariantMap counts;
  QVariantMap cvNode;
  QVariantMap cokrigingNode;
  QVariantMap sgsNode;
};

// 采样段：井点图层 → 有效样本 + 参与/跳过井清单 + 井层 CRS。
bool sampleGeostatWells( ConstraintWorkflow::GeostatJob *job,
                         const std::function<bool()> &cancelled,
                         std::vector<paleo::geostat::Sample> &samples, QVariantList &supportingWells,
                         QVariantList &skippedWells, QgsCoordinateReferenceSystem &crs )
{
  auto wells = std::make_unique<QgsVectorLayer>( job->wellUri, QStringLiteral( "wells" ), QStringLiteral( "ogr" ) );
  if ( !wells || !wells->isValid() )
  {
    job->error = ConstraintWorkflow::tr( "井点图层无法读取" );
    return false;
  }
  // P2（pr457-closure）：地统计距离/变差函数/网格间距全按米制口径——经纬度
  // 坐标下把角度当米会得出错误的空间结构与估值。与 local_direction /
  // structural 引擎同口径明确拒绝（提示先投影），不静默执行。
  if ( !wells->crs().isValid() )
  {
    job->error = ConstraintWorkflow::tr( "井点图层没有可识别的坐标系：距离/变差函数的单位无法核实，"
                                         "请先为井点层声明 CRS（本地工程网格或投影坐标系）" );
    return false;
  }
  if ( wells->crs().isGeographic() )
  {
    job->error = ConstraintWorkflow::tr( "井点图层坐标系为地理坐标（%1，单位是度）：地质统计需要米制投影坐标，"
                                         "请先把井位投影（例如本地工程网格或投影坐标系）" )
                     .arg( wells->crs().userFriendlyIdentifier() );
    return false;
  }
  const int fieldIndex = wells->fields().lookupField( job->field );
  if ( fieldIndex < 0 )
  {
    job->error = ConstraintWorkflow::tr( "井点图层没有字段 %1" ).arg( job->field );
    return false;
  }
  QgsAttributeList fields{fieldIndex};
  for (const auto &name : {"well_id", "id", "well_name", "name", "paleo_feature_uid"})
    if (const int index = wells->fields().indexOf(name); index >= 0 && !fields.contains(index)) fields << index;
  QgsFeature feature;
  auto it = wells->getFeatures(QgsFeatureRequest().setSubsetOfAttributes(fields));
  const auto attribute = [&](const char *preferred, const char *fallback) {
    const int first = wells->fields().indexOf(preferred), second = wells->fields().indexOf(fallback);
    return first >= 0 ? feature.attribute(first) : second >= 0 ? feature.attribute(second) : QVariant();
  };
  while (it.nextFeature(feature)) {
    if (cancelled && cancelled()) { job->error = ConstraintWorkflow::tr("已取消"); return false; }
    QVariantMap observation{{"well_id", attribute("well_id", "id")},
        {"name", attribute("well_name", "name")}, {"horizon", job->horizon},
        {"source_record_id", QString::number(feature.id())}, {"source_uri", job->wellUri},
        {"source_record_scope", "provider_fid_with_source_version"},
        {"source_version_ids", job->params.value("well_input_version_ids")}};
    if (wells->fields().indexOf("paleo_feature_uid") >= 0)
      observation.insert("source_feature_uid", feature.attribute("paleo_feature_uid"));
    QString reason;
    const auto geometry = feature.geometry(); QgsPointXY point;
    if (geometry.isNull() || geometry.isEmpty()) reason = ConstraintWorkflow::tr("缺少有效井点坐标");
    else {
      point = geometry.isMultipart() ? geometry.asMultiPoint().value(0) : geometry.asPoint();
      if (!std::isfinite(point.x()) || !std::isfinite(point.y())) reason = ConstraintWorkflow::tr("井点坐标不是有限数值");
    }
    const auto value = feature.attribute(fieldIndex); bool numeric = false;
    const double z = value.toDouble(&numeric);
    if (value.isNull() || !numeric || !std::isfinite(z))
      reason += (reason.isEmpty() ? QString() : QStringLiteral("; ")) + ConstraintWorkflow::tr("字段 %1 缺失或不是有限数值").arg(job->field);
    observation.insert("participating", reason.isEmpty());
    if (!reason.isEmpty()) {
      observation.insert("reason", reason); skippedWells << observation; continue;
    }
    observation.insert("value", z); observation.insert("x", point.x()); observation.insert("y", point.y());
    supportingWells << observation;
    samples.push_back(paleo::geostat::Sample{point.x(), point.y(), z});
  }
  crs = wells->sourceCrs();
  if ( samples.empty() )
  {
    job->error = ConstraintWorkflow::tr( "井点图层没有可用样本（字段 %1）" ).arg( job->field );
    return false;
  }
  return true;
}

// 降级段：样本不足阈值 → 约束 IDW，method_actual 如实标注（成功即任务完成，
// 编排层直接返回 true）。
bool runDegradedGeostatIdw( QgisProcessingService *proc, ConstraintWorkflow::GeostatJob *job,
                            const std::vector<paleo::geostat::Sample> &samples,
                            const QgsCoordinateReferenceSystem &crs, const QVariantList &supportingWells,
                            const QVariantList &skippedWells, const QVariantMap &qcParameters,
                            const std::function<bool()> &cancelled,
                            const std::function<void( double )> &progress )
{
  // Pass exactly the admitted observations to fallback too; typed NULL can
  // otherwise become numeric zero again inside a legacy algorithm.
  QgsVectorLayer admitted("Point", "admitted_geostat_samples", "memory");
  admitted.setCrs(crs);
  admitted.dataProvider()->addAttributes({QgsField(job->field, QMetaType::Type::Double)});
  admitted.updateFields();
  QgsFeatureList accepted;
  for (const auto &sample : samples) {
    QgsFeature point(admitted.fields()); point.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(sample.x, sample.y)));
    point.setAttribute(0, sample.value); accepted << point;
  }
  admitted.dataProvider()->addFeatures(accepted); admitted.updateExtents();
  QVariantMap runParams;
  runParams.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( static_cast<QgsMapLayer *>( &admitted ) ) );
  runParams.insert( QStringLiteral( "FIELD" ), job->field );
  runParams.insert( QStringLiteral( "CELL_SIZE" ), job->cellSize );
  runParams.insert( QStringLiteral( "OUTPUT" ), job->outputPath );
  QgisProcessingService::ProcessingHooks hooks;
  hooks.cancelled = cancelled;
  hooks.progress = progress;
  QString runErr;
  // algo-atom P2：降级 IDW 经 ProcessingAlgorithmAdapter
  paleo::ProcessingAlgorithmAdapter adapter( proc );
  const AlgorithmRunResult idwRun =
      adapter.runById( QStringLiteral( "paleo:paleo_constraint_idw" ), runParams, &runErr, hooks );
  const QVariantMap results = paleo::processingResultsFromRunResult( idwRun );
  if ( results.isEmpty() || !QFile::exists( job->outputPath ) )
  {
    job->outputPath.clear();
    job->error = runErr.isEmpty() ? ConstraintWorkflow::tr( "降级 IDW 未返回输出" ) : runErr;
    return false;
  }
  job->degraded = true;
  QVariantMap qc;
  QVariantMap parameters = qcParameters;
  parameters.insert( QStringLiteral( "method_actual" ), QStringLiteral( "idw" ) );
  parameters.insert( QStringLiteral( "degraded_reason" ),
                     QStringLiteral( "samples %1 < %2" ).arg( samples.size() ).arg( kGeostatMinSamples ) );
  qc.insert( QStringLiteral( "schema" ), QStringLiteral( "paleo_geostat_qc_v1" ) );
  qc.insert( QStringLiteral( "method" ), job->method );
  qc.insert( QStringLiteral( "method_actual" ), QStringLiteral( "idw" ) );
  qc.insert( QStringLiteral( "cross_validation" ),
             QVariantMap{ { QStringLiteral( "status" ), QStringLiteral( "not_computed" ) },
                          { QStringLiteral( "reason" ), QStringLiteral( "degraded_to_idw_insufficient_samples" ) } } );
  // 降级 IDW 不传 CONSTRAINTS：约束同样一条未用。
  qc.insert( QStringLiteral( "constraints_consumed" ), QVariantList() );
  qc.insert( QStringLiteral( "constraints_unused" ), job->constraintsUnused );
  qc.insert( QStringLiteral( "samples" ), static_cast<int>( samples.size() ) );
  qc.insert("supporting_wells", supportingWells); qc.insert("skipped_wells", skippedWells);
  qc.insert( QStringLiteral( "parameters" ), parameters );
  if ( !writeJsonFile( job->qcPath, qc ) )
  {
    job->error = ConstraintWorkflow::tr( "QC 写盘失败" );
    return false;
  }
  job->ok = true;
  return true;
}

// 网格段产物：样本包围盒 + 2 像元边距的网格规格（spanX/spanY 供变差函数
// 自动拟合取面积）。
struct GeostatSampleGrid
{
  paleo::geostat::GridSpec spec;
  double spanX = 0.0;
  double spanY = 0.0;
};

GeostatSampleGrid buildGeostatGrid( const std::vector<paleo::geostat::Sample> &samples, double cellSize,
                                    const QgsCoordinateReferenceSystem &crs )
{
  double minX = std::numeric_limits<double>::max();
  double minY = std::numeric_limits<double>::max();
  double maxX = std::numeric_limits<double>::lowest();
  double maxY = std::numeric_limits<double>::lowest();
  for ( const paleo::geostat::Sample &sample : samples )
  {
    minX = std::min( minX, sample.x );
    minY = std::min( minY, sample.y );
    maxX = std::max( maxX, sample.x );
    maxY = std::max( maxY, sample.y );
  }
  const double margin = 2.0 * cellSize;
  GeostatSampleGrid run;
  run.spanX = std::max( maxX - minX, cellSize );
  run.spanY = std::max( maxY - minY, cellSize );
  run.spec.pixelWidth = cellSize;
  run.spec.pixelHeight = -cellSize;
  // PW-128 同族防线：极小 cellSize 使 ceil 结果超出 int 表示域时，
  // static_cast<int> 是 UB，且溢出后的负积会绕过调用方 4M 预算闸——
  // 先在 double 域夹紧（1e5/边，平方 1e10 远超 4M 闸），让预算闸按
  // 真实超限如实拒绝。
  run.spec.cols = static_cast<int>( qBound( 1.0, std::ceil( ( run.spanX + 2 * margin ) / cellSize ), 1.0e5 ) );
  run.spec.rows = static_cast<int>( qBound( 1.0, std::ceil( ( run.spanY + 2 * margin ) / cellSize ), 1.0e5 ) );
  run.spec.originX = ( minX + maxX ) / 2.0 - run.spec.cols * cellSize / 2.0;
  run.spec.originY = ( minY + maxY ) / 2.0 + run.spec.rows * cellSize / 2.0;
  run.spec.crs = crs.toWkt().toStdString();
  return run;
}

// 变差函数段：显式参数或自动拟合（可选方向各向异性）+ 模型有效性校验。
bool fitGeostatVariogram( ConstraintWorkflow::GeostatJob *job,
                          const std::vector<paleo::geostat::Sample> &samples, double spanX, double spanY,
                          paleo::geostat::VariogramModel *model, GeostatQcParts *parts )
{
  const QString modelName = job->params.value( QStringLiteral( "variogramModel" ), QStringLiteral( "spherical" ) ).toString();
  const paleo::geostat::VariogramModelType modelType = variogramTypeFor( modelName );
  const double explicitNugget = job->params.value( QStringLiteral( "nugget" ), 0.0 ).toDouble();
  const double explicitSill = job->params.value( QStringLiteral( "sill" ), 0.0 ).toDouble();
  const double explicitRange = job->params.value( QStringLiteral( "range" ), 0.0 ).toDouble();
  const double azimuthParam = job->params.value( QStringLiteral( "azimuth" ), -1.0 ).toDouble();
  const bool hasAzimuth = azimuthParam >= 0.0 && azimuthParam <= 360.0;

  model->type = modelType;
  double &fitR2 = parts->fitR2;
  double &fitRmse = parts->fitRmse;
  bool &autoFit = parts->autoFit;
  QVariantMap &modelProvenance = parts->modelProvenance;
  if ( explicitRange > 0 && explicitSill > 0 )
  {
    const double anisotropyRequested =
        job->params.value( QStringLiteral( "anisotropyRatio" ), 1.0 ).toDouble();
    model->nugget = std::max( 0.0, explicitNugget );
    model->sill = explicitSill;
    model->range = explicitRange;
    model->anisotropyRatio = std::max( 1.0, anisotropyRequested );
    if ( hasAzimuth )
      model->azimuthDeg = azimuthParam;
    autoFit = false;
    // P2 诚实面：负块金→0、比率<1→1 是静默钳制，留痕（同 rho_clamped 与
    // auto-fit anisotropy_ratio_clamped 口径）——仅发生钳制时插入，正常值无新键。
    if ( model->nugget != explicitNugget )
    {
      modelProvenance.insert( QStringLiteral( "nugget_clamped" ), true );
      modelProvenance.insert( QStringLiteral( "nugget_effective" ), model->nugget );
    }
    if ( model->anisotropyRatio != anisotropyRequested )
    {
      modelProvenance.insert( QStringLiteral( "anisotropy_clamped" ), true );
      modelProvenance.insert( QStringLiteral( "anisotropy_effective" ), model->anisotropyRatio );
    }
  }
  else
  {
    const double area = spanX * spanY;
    const int nLags = 12;
    // PW-036：滞后距按半对角线分 nLags 份，保证实验变差函数覆盖到半个工区；
    // 原「sqrt(area/n)」随样本加密而缩短，密集数据的变程被窗口截断。
    // 采样间距只作下限（不让滞后小于平均点距 / 网格单元）。
    const double halfDiag = 0.5 * std::hypot( spanX, spanY );
    const double spacing = std::max( job->cellSize, std::sqrt( area / samples.size() ) );
    const double lag = std::max( spacing, halfDiag / nLags );
    modelProvenance.insert( QStringLiteral( "variogram_lag" ), lag );
    modelProvenance.insert( QStringLiteral( "variogram_nlags" ), nLags );
    if ( hasAzimuth )
    {
      paleo::geostat::VariogramDirection along;
      along.omnidirectional = false;
      along.azimuthDeg = azimuthParam;
      along.toleranceDeg = 22.5;
      paleo::geostat::VariogramDirection across = along;
      across.azimuthDeg = std::fmod( azimuthParam + 90.0, 360.0 );
      const paleo::geostat::VariogramFit fitAlong =
          paleo::geostat::fitVariogram( paleo::geostat::experimentalVariogram( samples, lag, nLags, along ), modelType );
      const paleo::geostat::VariogramFit fitAcross =
          paleo::geostat::fitVariogram( paleo::geostat::experimentalVariogram( samples, lag, nLags, across ), modelType );
      if ( fitAlong.status != paleo::geostat::Status::Ok || fitAcross.status != paleo::geostat::Status::Ok )
      {
        job->error = ConstraintWorkflow::tr( "方向变差函数拟合失败：%1 / %2" )
                         .arg( QString::fromStdString( fitAlong.message ), QString::fromStdString( fitAcross.message ) );
        return false;
      }
      const double rawRatio = fitAcross.model.range > 0 ? fitAlong.model.range / fitAcross.model.range : 1.0;
      // PW-037：横向变程更大（rawRatio<1）时不夹成各向同性——主轴转 90°，
      // 比值取倒数，块金/基台取主轴（横向）拟合；如实记录换轴。
      const bool swapAxes = rawRatio > 0.0 && rawRatio < 1.0;
      const paleo::geostat::VariogramFit &major = swapAxes ? fitAcross : fitAlong;
      *model = major.model;
      model->azimuthDeg = swapAxes ? std::fmod( azimuthParam + 90.0, 360.0 ) : azimuthParam;
      model->anisotropyRatio = std::clamp( swapAxes ? 1.0 / rawRatio : rawRatio, 1.0, 8.0 );
      fitR2 = major.r2;
      fitRmse = major.rmse;
      modelProvenance.insert( QStringLiteral( "anisotropy_axis_swapped" ), swapAxes );
      modelProvenance.insert( QStringLiteral( "azimuth_effective" ), model->azimuthDeg );
      // 块金/基台取主轴拟合；另一向只贡献变程比。两向都如实记录。
      modelProvenance.insert( QStringLiteral( "across_range" ), fitAcross.model.range );
      modelProvenance.insert( QStringLiteral( "across_fit_r2" ), fitAcross.r2 );
      modelProvenance.insert( QStringLiteral( "across_fit_rmse" ), fitAcross.rmse );
      modelProvenance.insert( QStringLiteral( "anisotropy_ratio_raw" ), rawRatio );
      modelProvenance.insert( QStringLiteral( "anisotropy_ratio_clamped" ),
                              ( swapAxes ? 1.0 / rawRatio : rawRatio ) != model->anisotropyRatio );
    }
    else
    {
      const paleo::geostat::VariogramFit fit =
          paleo::geostat::fitVariogram( paleo::geostat::experimentalVariogram( samples, lag, nLags ), modelType );
      if ( fit.status != paleo::geostat::Status::Ok )
      {
        job->error = ConstraintWorkflow::tr( "变差函数拟合失败：%1" ).arg( QString::fromStdString( fit.message ) );
        return false;
      }
      *model = fit.model;
      fitR2 = fit.r2;
      fitRmse = fit.rmse;
    }
  }
  if ( !( model->range > 0 ) || model->nugget < 0 || model->sill < 0 )
  {
    job->error = ConstraintWorkflow::tr( "变差函数模型参数无效" );
    return false;
  }
  if ( model->nugget + model->sill <= 0 )
  {
    // 恒定场：零基台 → 克氏方程组全零无解。如实拒绝，不产空栅格。
    job->error = ConstraintWorkflow::tr( "字段 %1 在井点上是常量（零基台），克里金无空间结构可解" ).arg( job->field );
    return false;
  }
  return true;
}

// QC 输入面段：给了但引擎没读/被覆盖的参数逐条留因 + 检索邻域节点。
void collectGeostatQcInputs( const ConstraintWorkflow::GeostatJob *job,
                             const paleo::geostat::VariogramModel &model, const GeostatRunContext &ctx,
                             GeostatQcParts *parts )
{
  const double explicitSill = job->params.value( QStringLiteral( "sill" ), 0.0 ).toDouble();
  const double explicitRange = job->params.value( QStringLiteral( "range" ), 0.0 ).toDouble();
  const double azimuthParam = job->params.value( QStringLiteral( "azimuth" ), -1.0 ).toDouble();
  const bool hasAzimuth = azimuthParam >= 0.0 && azimuthParam <= 360.0;
  const bool autoFit = parts->autoFit;
  const int maxPoints = ctx.maxPoints;
  const double searchRadius = ctx.searchRadius;
  const int minPoints = ctx.minPoints;
  QVariantList &ignoredInputs = parts->ignoredInputs;
  QVariantMap &modelProvenance = parts->modelProvenance;
  const auto ignore = [&]( const QString &key, const QString &reason ) {
    if ( job->params.contains( key ) )
      ignoredInputs << QVariantMap{ { QStringLiteral( "key" ), key }, { QStringLiteral( "reason" ), reason } };
  };
  for ( const QString &key : { QStringLiteral( "power" ), QStringLiteral( "coverage" ), QStringLiteral( "wellClusterLocality" ),
                               QStringLiteral( "anisotropyAngle" ), QStringLiteral( "krigingMaxPoints" ) } )
    ignore( key, QStringLiteral( "not_read_by_geostat_engine" ) );
  if ( autoFit )
  {
    const bool partial = ( explicitRange > 0 ) != ( explicitSill > 0 );
    const QString why = partial ? QStringLiteral( "explicit_model_requires_range_and_sill" )
                                : QStringLiteral( "auto_fit_estimates_model" );
    ignore( QStringLiteral( "nugget" ), why );
    ignore( QStringLiteral( "sill" ), why );
    ignore( QStringLiteral( "range" ), why );
    ignore( QStringLiteral( "anisotropyRatio" ), hasAzimuth ? QStringLiteral( "directional_auto_fit_derives_ratio" )
                                                            : QStringLiteral( "omnidirectional_auto_fit" ) );
  }
  else if ( !hasAzimuth && model.anisotropyRatio > 1.0 )
  {
    modelProvenance.insert( QStringLiteral( "azimuth_defaulted" ), model.azimuthDeg );
  }
  if ( job->params.contains( QStringLiteral( "azimuth" ) ) && !hasAzimuth )
    ignore( QStringLiteral( "azimuth" ), QStringLiteral( "outside_0_360" ) );
  if ( job->method != QLatin1String( "cokriging" ) )
    for ( const QString &key : { QStringLiteral( "covariateLayerId" ), QStringLiteral( "crossCorrelation" ),
                                 QStringLiteral( "cross_correlation" ), QStringLiteral( "covariateMaxSamples" ) } )
      ignore( key, QStringLiteral( "method_has_no_covariate" ) );
  if ( job->method != QLatin1String( "sgs" ) )
    for ( const QString &key : { QStringLiteral( "realizations" ), QStringLiteral( "seed" ), QStringLiteral( "persistRealizations" ) } )
      ignore( key, QStringLiteral( "deterministic_method" ) );
  if ( job->method != QLatin1String( "kriging" ) )
    ignore( QStringLiteral( "minPoints" ), QStringLiteral( "method_neighbourhood_has_no_min_points" ) );
  else if ( !( searchRadius > 0 ) )
    ignore( QStringLiteral( "minPoints" ), QStringLiteral( "used_only_with_search_radius" ) );
  modelProvenance.insert( QStringLiteral( "variogram_source" ),
                          !autoFit ? QStringLiteral( "explicit" )
                                   : hasAzimuth ? QStringLiteral( "auto_fit_directional" )
                                                : QStringLiteral( "auto_fit_omnidirectional" ) );
  QVariantMap &searchNode = parts->searchNode;
  searchNode.insert( QStringLiteral( "max_points_requested" ), maxPoints );
  // 各内核口径：克里金/协克里金 ≤0 = 全部样本；SGS 一律钳到 [1,64]。0 表示「全部」。
  searchNode.insert( QStringLiteral( "max_points_effective" ),
                     job->method == QLatin1String( "sgs" ) || maxPoints > 0 ? std::clamp( maxPoints, 1, 64 ) : 0 );
  searchNode.insert( QStringLiteral( "search_radius" ), searchRadius );
  if ( job->method == QLatin1String( "kriging" ) && searchRadius > 0 )
    searchNode.insert( QStringLiteral( "min_points" ), minPoints );
}

// 方法执行段（kriging）：普通克里金 + 方差栅格 + LOO 交叉验证。
bool runGeostatKriging( const GeostatRunContext &ctx, const std::vector<paleo::geostat::Sample> &samples,
                        GeostatQcParts *parts )
{
  ConstraintWorkflow::GeostatJob *job = ctx.job;
  const paleo::geostat::GridSpec &grid = ctx.grid;
  const paleo::geostat::VariogramModel &model = ctx.model;
  const paleo::geostat::Control &control = ctx.control;
  const QString &tempDir = ctx.tempDir;
  const double *const geoTransform = ctx.geoTransform.data();
  const QgsCoordinateReferenceSystem &crs = ctx.crs;
  const int maxPoints = ctx.maxPoints;
  const double searchRadius = ctx.searchRadius;
  const int minPoints = ctx.minPoints;
  QVariantMap &counts = parts->counts;
  QVariantMap &cvNode = parts->cvNode;
  const std::function<bool()> &cancelled = control.cancelled;
  paleo::geostat::KrigingParams krigingParams;
  krigingParams.maxPoints = maxPoints;
  krigingParams.searchRadius = searchRadius;
  if ( searchRadius > 0 )
    krigingParams.minPoints = minPoints;
  // algo-atom P2：ordinary_kriging 经 NativeCppAdapter
  job->supportPath = QDir( tempDir ).filePath( QStringLiteral( "variance.tif" ) );
  paleo::OrdinaryKrigingInputs okIn;
  okIn.samples = samples;
  okIn.grid = grid;
  okIn.model = model;
  okIn.params = krigingParams;
  okIn.control = control;
  okIn.estimatePath = job->outputPath;
  okIn.variancePath = job->supportPath;
  for ( int i = 0; i < 6; ++i )
    okIn.geoTransform[static_cast<std::size_t>( i )] = geoTransform[i];
  okIn.crs = crs;
  okIn.canonicalCrsWkt = DataCatalog::localGridCrsWkt();
  paleo::NativeCppAdapter native;
  QString nativeErr;
  const AlgorithmRunResult okRun = native.runOrdinaryKriging( okIn, &nativeErr );
  if ( !okRun.ok )
  {
    job->error = nativeErr.isEmpty() ? ConstraintWorkflow::tr( "克里金失败" ) : nativeErr;
    return false;
  }
  counts.insert( QStringLiteral( "finite" ), okRun.diagnostics.value( QStringLiteral( "finite_cells" ) ) );
  counts.insert( QStringLiteral( "nodata" ), okRun.diagnostics.value( QStringLiteral( "nodata_cells" ) ) );
  counts.insert( QStringLiteral( "solver_failures" ),
                 okRun.diagnostics.value( QStringLiteral( "solver_failures" ) ) );
  bool cvCancelled = false;
  cvNode = leaveOneOut(
      samples, QStringLiteral( "ordinary_kriging" ),
      [&]( double x, double y, const std::vector<paleo::geostat::Sample> &rest ) {
        const paleo::geostat::KrigingPointResult point = paleo::geostat::ordinaryKrigingAt( x, y, rest, model, krigingParams );
        return CvPrediction{ point.ok, point.estimate, point.variance };
      },
      cancelled, &cvCancelled );
  if ( cvCancelled )
  {
    job->error = ConstraintWorkflow::tr( "已取消" );
    return false;
  }
  return true;
}

// 方法执行段（cokriging）：协变量取样、ρ 估计/钳制、协克里金 + LOO。
bool runGeostatCokriging( const GeostatRunContext &ctx, const std::vector<paleo::geostat::Sample> &samples,
                          GeostatQcParts *parts, QVariantMap &qcParameters )
{
  ConstraintWorkflow::GeostatJob *job = ctx.job;
  const paleo::geostat::GridSpec &grid = ctx.grid;
  const paleo::geostat::VariogramModel &model = ctx.model;
  const paleo::geostat::Control &control = ctx.control;
  const QString &tempDir = ctx.tempDir;
  const double *const geoTransform = ctx.geoTransform.data();
  const QgsCoordinateReferenceSystem &crs = ctx.crs;
  const int maxPoints = ctx.maxPoints;
  const double searchRadius = ctx.searchRadius;
  QVariantMap &counts = parts->counts;
  QVariantMap &cvNode = parts->cvNode;
  QVariantMap &cokrigingNode = parts->cokrigingNode;
  const std::function<bool()> &cancelled = control.cancelled;
  const QString covariateLayerId = job->params.value( QStringLiteral( "covariateLayerId" ) ).toString();
  // The covariate URI was frozen in prepare; never instantiate a GUI layer
  // through the live layer service from the worker.
  auto ownedRaster = std::make_unique<QgsRasterLayer>(
      job->parentPaths.last(), QStringLiteral("covariate"));
  auto *covRaster = ownedRaster.get();
  if ( !covRaster || !covRaster->isValid() )
  {
    job->error = ConstraintWorkflow::tr( "协克里金方法需要有效的次级协变量图层 (covariateLayerId)" );
    return false;
  }

  std::vector<double> primaryZ;
  std::vector<double> secondaryZ;
  primaryZ.reserve( samples.size() );
  secondaryZ.reserve( samples.size() );
  for ( const paleo::geostat::Sample &s : samples )
  {
    bool ok = false;
    const double v2 = covRaster->dataProvider()->sample( QgsPointXY( s.x, s.y ), 1, &ok );
    if ( ok && std::isfinite( v2 ) )
    {
      primaryZ.push_back( s.value );
      secondaryZ.push_back( v2 );
    }
  }

  double rho = 0.0;
  QString rhoSource = QStringLiteral( "default_zero" );
  if ( job->params.contains( QStringLiteral( "crossCorrelation" ) ) )
  {
    rho = job->params.value( QStringLiteral( "crossCorrelation" ) ).toDouble();
    rhoSource = QStringLiteral( "explicit" );
  }
  else if ( job->params.contains( QStringLiteral( "cross_correlation" ) ) )
  {
    rho = job->params.value( QStringLiteral( "cross_correlation" ) ).toDouble();
    rhoSource = QStringLiteral( "explicit" );
  }
  else if ( primaryZ.size() >= 3 )
  {
    rhoSource = QStringLiteral( "estimated_colocated" );
    const std::size_t n = primaryZ.size();
    double sum1 = 0, sum2 = 0;
    for ( std::size_t i = 0; i < n; ++i )
    {
      sum1 += primaryZ[i];
      sum2 += secondaryZ[i];
    }
    const double mean1 = sum1 / static_cast<double>( n );
    const double mean2 = sum2 / static_cast<double>( n );
    double cov = 0, var1 = 0, var2 = 0;
    for ( std::size_t i = 0; i < n; ++i )
    {
      const double d1 = primaryZ[i] - mean1;
      const double d2 = secondaryZ[i] - mean2;
      cov += d1 * d2;
      var1 += d1 * d1;
      var2 += d2 * d2;
    }
    if ( var1 > 1e-12 && var2 > 1e-12 )
      rho = cov / std::sqrt( var1 * var2 );
    else
    {
      rho = 0.0;
      rhoSource = QStringLiteral( "estimated_degenerate_zero" );
    }
  }
  if ( !std::isfinite( rho ) )
  {
    job->error = ConstraintWorkflow::tr( "协克里金交叉相关系数无效" );
    return false;
  }
  const double rhoRequested = rho;
  rho = std::clamp( rho, -0.99, 0.99 );
  // ρ=0 时交叉协方差为零、次级权重恒为零：结果等同只用主变量的普通克里金。
  // 方法名不改（仍是 cokriging 方程组），但如实标注协变量未生效。
  cokrigingNode.insert( QStringLiteral( "rho_source" ), rhoSource );
  cokrigingNode.insert( QStringLiteral( "rho_requested" ), rhoRequested );
  cokrigingNode.insert( QStringLiteral( "rho_effective" ), rho );
  cokrigingNode.insert( QStringLiteral( "rho_clamped" ), rho != rhoRequested );
  cokrigingNode.insert( QStringLiteral( "colocated_samples" ), static_cast<int>( primaryZ.size() ) );
  cokrigingNode.insert( QStringLiteral( "covariate_effective" ), rho != 0.0 );
  cokrigingNode.insert( QStringLiteral( "secondary_model" ),
                        QStringLiteral( "primary_variogram_nugget_zero" ) );

  std::vector<paleo::geostat::Sample> secondarySamples;
  const int totalCells = grid.cols * grid.rows;
  const int maxSec = job->params.value( QStringLiteral( "covariateMaxSamples" ), 2048 ).toInt();
  const int step = std::max( 1, static_cast<int>( std::ceil( std::sqrt( static_cast<double>( totalCells ) / std::max( 64, maxSec ) ) ) ) );
  for ( int r = 0; r < grid.rows; r += step )
  {
    for ( int c = 0; c < grid.cols; c += step )
    {
      const double gx = grid.cellCenterX( c );
      const double gy = grid.cellCenterY( r );
      bool ok = false;
      const double gv = covRaster->dataProvider()->sample( QgsPointXY( gx, gy ), 1, &ok );
      if ( ok && std::isfinite( gv ) )
        secondarySamples.push_back( paleo::geostat::Sample{ gx, gy, gv } );
    }
  }
  if ( secondarySamples.empty() )
  {
    job->error = ConstraintWorkflow::tr( "协变量栅格在计算网格范围内无有效数据" );
    return false;
  }
  // PW-019：模型假设协变量处于主变量单位/方差（γ12=ρ·γ1、γ22 取主基台），
  // 原始协变量值必须先按配置点统计线性标定，否则估值随协变量单位缩放。
  bool covariateRescaled = false;
  secondarySamples = paleo::geostat::rescaleSecondaryToPrimary( secondarySamples, primaryZ, secondaryZ,
                                                                &covariateRescaled );
  cokrigingNode.insert( QStringLiteral( "secondary_rescaled_to_primary" ), covariateRescaled );

  paleo::geostat::CoKrigingModel coModel;
  coModel.primary = model;
  // #326：次级自变差必须与交叉项同单位。交叉项是 ρ·γ1（主变量单位，
  // cokriging.h crossSemivariance），所以 γ22 也要落在主变量基台上——
  // 即 scaled MM1 的两基台相等那种退化。原实现把 γ22 的拱高换成协变量
  // 井点原始方差，地震属性与孔隙度/厚度差几个数量级时，方程组里混了两
  // 种单位：Pearson ρ 不再是那个 ρ，次级块还会主导条件数。测得的协变量
  // 方差只进 QC 参数，不改模型。
  coModel.secondary = model;
  coModel.secondary.nugget = 0.0;
  // 纯块金主变差（sill=0，方差全在 nugget）时不能把 secondary.nugget 也归零：
  // 那会让 γ22≡0、次级块秩亏（LU 奇异→整幅 nodata 而 Status 仍 Ok，正是
  // C.5 总基台闸要拒的退化）。总基台守恒到 secondary.sill，单位口径不变
  // （γ22 仍落在主变量基台上，同 #326）。
  if (coModel.secondary.sill <= 0.0)
    coModel.secondary.sill = model.nugget + model.sill;
  if ( !secondaryZ.empty() )
  {
    double secMean = 0;
    for ( double val : secondaryZ )
      secMean += val;
    secMean /= secondaryZ.size();
    double secVar = 0;
    for ( double val : secondaryZ )
      secVar += ( val - secMean ) * ( val - secMean );
    secVar /= secondaryZ.size();
    job->params.insert( QStringLiteral( "covariate_sample_variance" ),
                        QString::number( secVar, 'g', 10 ) );
  }
  coModel.crossCorrelation = rho;

  // algo-atom P2：ordinary_cokriging 经 NativeCppAdapter
  job->supportPath = QDir( tempDir ).filePath( QStringLiteral( "variance.tif" ) );
  paleo::OrdinaryCoKrigingInputs coIn;
  coIn.primary = samples;
  coIn.secondary = secondarySamples;
  coIn.grid = grid;
  coIn.model = coModel;
  coIn.params.maxPrimary = maxPoints;
  coIn.params.maxSecondary = maxPoints;
  coIn.params.searchRadius = searchRadius;
  coIn.control = control;
  coIn.estimatePath = job->outputPath;
  coIn.variancePath = job->supportPath;
  for ( int i = 0; i < 6; ++i )
    coIn.geoTransform[static_cast<std::size_t>( i )] = geoTransform[i];
  coIn.crs = crs;
  coIn.canonicalCrsWkt = DataCatalog::localGridCrsWkt();
  paleo::NativeCppAdapter native;
  QString nativeErr;
  const AlgorithmRunResult coRun = native.runOrdinaryCoKriging( coIn, &nativeErr );
  if ( !coRun.ok )
  {
    job->error = nativeErr.isEmpty() ? ConstraintWorkflow::tr( "协克里金失败" ) : nativeErr;
    return false;
  }
  counts.insert( QStringLiteral( "finite" ), coRun.diagnostics.value( QStringLiteral( "finite_cells" ) ) );
  counts.insert( QStringLiteral( "nodata" ), coRun.diagnostics.value( QStringLiteral( "nodata_cells" ) ) );
  counts.insert( QStringLiteral( "solver_failures" ),
                 coRun.diagnostics.value( QStringLiteral( "solver_failures" ) ) );
  counts.insert( QStringLiteral( "secondary_samples" ), static_cast<int>( secondarySamples.size() ) );
  {
    paleo::geostat::CoKrigingParams coParams;
    coParams.maxPrimary = maxPoints;
    coParams.maxSecondary = maxPoints;
    coParams.searchRadius = searchRadius;
    bool cvCancelled = false;
    cvNode = leaveOneOut(
        samples, QStringLiteral( "ordinary_cokriging" ),
        [&]( double x, double y, const std::vector<paleo::geostat::Sample> &rest ) {
          const paleo::geostat::CoKrigingPointResult point =
              paleo::geostat::coKrigingAt( x, y, rest, secondarySamples, coModel, coParams );
          return CvPrediction{ point.ok, point.estimate, point.variance };
        },
        cancelled, &cvCancelled );
    if ( cvCancelled )
    {
      job->error = ConstraintWorkflow::tr( "已取消" );
      return false;
    }
  }
  qcParameters.insert( QStringLiteral( "cross_correlation" ), rho );
  qcParameters.insert( QStringLiteral( "covariate_layer_id" ), covariateLayerId );
  return true;
}

// 方法执行段（sgs）：顺序高斯模拟 + 均值/标准差栅格 + 成员栅格持久化。
bool runGeostatSgs( const GeostatRunContext &ctx, const std::vector<paleo::geostat::Sample> &samples,
                    GeostatQcParts *parts )
{
  ConstraintWorkflow::GeostatJob *job = ctx.job;
  const paleo::geostat::GridSpec &grid = ctx.grid;
  const paleo::geostat::VariogramModel &model = ctx.model;
  const paleo::geostat::Control &control = ctx.control;
  const QString &tempDir = ctx.tempDir;
  const double *const geoTransform = ctx.geoTransform.data();
  const QgsCoordinateReferenceSystem &crs = ctx.crs;
  const int maxPoints = ctx.maxPoints;
  const double searchRadius = ctx.searchRadius;
  QVariantMap &counts = parts->counts;
  QVariantMap &cvNode = parts->cvNode;
  QVariantMap &sgsNode = parts->sgsNode;
  // algo-atom P2：sgs 经 NativeCppAdapter（均值/标准差/成员写盘）
  paleo::geostat::SgsParams sgsParams;
  sgsParams.nRealizations = job->params.value( QStringLiteral( "realizations" ), 4 ).toInt();
  sgsParams.seed = static_cast<std::uint64_t>( job->params.value( QStringLiteral( "seed" ), 42 ).toLongLong() );
  sgsParams.maxPoints = maxPoints;
  sgsParams.searchRadius = searchRadius;
  job->supportPath = QDir( tempDir ).filePath( QStringLiteral( "std.tif" ) );
  paleo::SgsInputs sgsIn;
  sgsIn.samples = samples;
  sgsIn.grid = grid;
  sgsIn.model = model;
  sgsIn.params = sgsParams;
  sgsIn.control = control;
  sgsIn.meanPath = job->outputPath;
  sgsIn.stdPath = job->supportPath;
  sgsIn.persistRealizations =
      job->params.value( QStringLiteral( "persistRealizations" ), true ).toBool();
  sgsIn.memberPathPrefix = QDir( tempDir ).filePath( QStringLiteral( "member_r" ) );
  for ( int i = 0; i < 6; ++i )
    sgsIn.geoTransform[static_cast<std::size_t>( i )] = geoTransform[i];
  sgsIn.crs = crs;
  sgsIn.canonicalCrsWkt = DataCatalog::localGridCrsWkt();
  paleo::NativeCppAdapter native;
  QString nativeErr;
  const AlgorithmRunResult sgsRun = native.runSgs( sgsIn, &nativeErr );
  if ( !sgsRun.ok )
  {
    job->error = nativeErr.isEmpty() ? ConstraintWorkflow::tr( "SGS 失败" ) : nativeErr;
    return false;
  }
  job->memberPaths = sgsRun.diagnostics.value( QStringLiteral( "member_paths" ) ).toStringList();
  if ( sgsIn.persistRealizations )
    job->memberSeed = sgsParams.seed;
  counts.insert( QStringLiteral( "finite" ), sgsRun.diagnostics.value( QStringLiteral( "finite_cells" ) ) );
  counts.insert( QStringLiteral( "nodata" ), sgsRun.diagnostics.value( QStringLiteral( "nodata_cells" ) ) );
  counts.insert( QStringLiteral( "solver_failures" ),
                 sgsRun.diagnostics.value( QStringLiteral( "solver_failures" ) ) );
  sgsNode.insert( QStringLiteral( "realizations" ), sgsParams.nRealizations );
  sgsNode.insert( QStringLiteral( "seed" ), static_cast<qulonglong>( sgsParams.seed ) );
  sgsNode.insert( QStringLiteral( "sample_mean" ),
                  sgsRun.diagnostics.value( QStringLiteral( "sample_mean" ) ) );
  sgsNode.insert( QStringLiteral( "sample_std" ),
                  sgsRun.diagnostics.value( QStringLiteral( "sample_std" ) ) );
  // 随机模拟成果是实现集合的均值/标准差，不是最优估值：不做 LOO，也不冒充。
  cvNode.insert( QStringLiteral( "status" ), QStringLiteral( "not_computed" ) );
  cvNode.insert( QStringLiteral( "reason" ), QStringLiteral( "stochastic_simulation_use_realization_spread" ) );
  return true;
}

// QC 落盘段：组装 paleo_geostat_qc_v1 并写盘。
bool writeGeostatQc( const GeostatRunContext &ctx, const std::vector<paleo::geostat::Sample> &samples,
                     const QVariantList &supportingWells, const QVariantList &skippedWells,
                     const QVariantMap &qcParameters, const GeostatQcParts &parts )
{
  ConstraintWorkflow::GeostatJob *job = ctx.job;
  const paleo::geostat::GridSpec &grid = ctx.grid;
  const paleo::geostat::VariogramModel &model = ctx.model;
  const bool autoFit = parts.autoFit;
  const double fitR2 = parts.fitR2;
  const double fitRmse = parts.fitRmse;
  const QVariantMap &modelProvenance = parts.modelProvenance;
  const QVariantList &ignoredInputs = parts.ignoredInputs;
  const QVariantMap &searchNode = parts.searchNode;
  const QVariantMap &counts = parts.counts;
  const QVariantMap &cvNode = parts.cvNode;
  const QVariantMap &cokrigingNode = parts.cokrigingNode;
  const QVariantMap &sgsNode = parts.sgsNode;
  QVariantMap qc;
  qc.insert( QStringLiteral( "schema" ), QStringLiteral( "paleo_geostat_qc_v1" ) );
  qc.insert( QStringLiteral( "method" ), job->method );
  qc.insert( QStringLiteral( "method_actual" ), job->method );
  qc.insert( QStringLiteral( "constraints_consumed" ), QVariantList() );
  qc.insert( QStringLiteral( "constraints_unused" ), job->constraintsUnused );
  qc.insert( QStringLiteral( "samples" ), static_cast<int>( samples.size() ) );
  qc.insert("supporting_wells", supportingWells); qc.insert("skipped_wells", skippedWells);
  qc.insert( QStringLiteral( "auto_fit" ), autoFit );
  qc.insert( QStringLiteral( "model" ), variogramExtra( model, fitR2, fitRmse ) );
  QVariantMap gridNode;
  gridNode.insert( QStringLiteral( "cols" ), grid.cols );
  gridNode.insert( QStringLiteral( "rows" ), grid.rows );
  gridNode.insert( QStringLiteral( "cell_size" ), job->cellSize );
  qc.insert( QStringLiteral( "grid" ), gridNode );
  qc.insert( QStringLiteral( "counts" ), counts );
  if ( !sgsNode.isEmpty() )
    qc.insert( QStringLiteral( "sgs" ), sgsNode );
  qc.insert( QStringLiteral( "model_provenance" ), modelProvenance );
  qc.insert( QStringLiteral( "ignored_inputs" ), ignoredInputs );
  qc.insert( QStringLiteral( "search" ), searchNode );
  qc.insert( QStringLiteral( "cross_validation" ), cvNode );
  if ( !cokrigingNode.isEmpty() )
    qc.insert( QStringLiteral( "cokriging" ), cokrigingNode );
  qc.insert( QStringLiteral( "parameters" ), qcParameters );
  if ( !writeJsonFile( job->qcPath, qc ) )
  {
    job->error = ConstraintWorkflow::tr( "QC 写盘失败" );
    return false;
  }
  return true;
}

// 暂存段：计算成果拷入暂存位并落 SHA（失败不回收临时目录——与原路径一致，
// 成果可能已部分拷出，回收交给上层清理）。
bool stageGeostatOutputs( ConstraintWorkflow::GeostatJob *job,
                          const std::function<bool()> &cancelled )
{
  if (job->staging.isValid()) {
    if (cancelled && cancelled()) {
      job->error = ConstraintWorkflow::tr("已取消");
      return false;
    }
    const QString output = job->staging.absolutePath;
    const QString qc = fileStem(output) + ".qc.json";
    const QString support = job->supportPath.isEmpty()
                                ? QString()
                                : fileStem(output) + ".support.tif";
    if (!QFile::copy(job->outputPath, output) ||
        !QFile::copy(job->qcPath, qc) ||
        (!support.isEmpty() && !QFile::copy(job->supportPath, support))) {
      job->error = ConstraintWorkflow::tr("无法暂存计算成果");
      return false;
    }
    job->fingerprint =
        DerivedFileFingerprint::read(output, cancelled, &job->error);
    job->stagedQcSha = DataCatalog::sha256FileHex(qc, &job->error, cancelled);
    if (!support.isEmpty())
      job->stagedSupportSha =
          DataCatalog::sha256FileHex(support, &job->error, cancelled);
    if (job->fingerprint.sha256.isEmpty() || job->stagedQcSha.isEmpty() ||
        (!support.isEmpty() && job->stagedSupportSha.isEmpty()))
      return false;
  }
  return true;
}
} // namespace

bool ConstraintWorkflow::prepareGeostatJob( const QString &horizon, const QString &factorId,
                                            const QString &method, const QVariantMap &inputParams,
                                            GeostatJob *job, QString *error )
{
  QVariantMap params = inputParams;
  if ( !job )
  {
    paleo::workflow_detail::setError( error, tr( "缺少地质统计任务" ) );
    return false;
  }
  if ( method != QLatin1String( "kriging" ) && method != QLatin1String( "sgs" ) &&
       method != QLatin1String( "cokriging" ) )
  {
    paleo::workflow_detail::setError( error, tr( "未知地质统计方法：%1" ).arg( method ) );
    return false;
  }
  if (!prepareFactorInputs(horizon, factorId, params, error))
    return false;
  *job = GeostatJob();
  job->generation = ++m_publishGeneration;
  job->horizon = horizon;
  job->factorId = factorId;
  job->method = method;
  job->params = params;
  if ( ConstraintStore *store = constraintStore() )
  {
    paleo::singlefactor::ConstraintMethodContext useContext;
    useContext.methodId = method;
    useContext.horizon = horizon;
    const paleo::singlefactor::ConstraintUsePlan plan =
        paleo::singlefactor::planConstraintUse( store->load( horizon ), useContext );
    job->constraintsUnused = plan.summary.value( QStringLiteral( "unused" ) ).toList();
    job->constraintCapability = plan.capabilityNote;
  }

  QgisProcessingService *proc = m_proc.data();
  QgisLayerService *layers = m_layers.data();
  if ( !proc || !layers )
  {
    paleo::workflow_detail::setError( error, tr( "constraint workflow is not bound to services" ) );
    return false;
  }
  bool known = false;
  const SingleFactorDefinition def = SingleFactorRegistry::byId( factorId, &known );
  if ( !known )
  {
    paleo::workflow_detail::setError( error, tr( "未知单因素 id：%1" ).arg( factorId ) );
    return false;
  }
  job->field = params.value( QStringLiteral( "field" ), def.defaultParams.value( QStringLiteral( "field" ) ) ).toString();
  job->cellSize = params.value( QStringLiteral( "cellSize" ), def.defaultParams.value( QStringLiteral( "cellSize" ), 1.0 ) )
                      .toDouble();
  if ( job->field.isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "插值字段为空" ) );
    return false;
  }
  if ( !( job->cellSize > 0.0 ) )
  {
    paleo::workflow_detail::setError( error, tr( "像元大小必须是正数" ) );
    return false;
  }
  const QString overridePoints = params.value( QStringLiteral( "pointsLayerId" ) ).toString();
  const QString pointsId = overridePoints.isEmpty() ? wellsLayerIdFor( layers, horizon ) : overridePoints;
  if ( pointsId.isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "层位 %1 没有井点图层" ).arg( horizon ) );
    return false;
  }
  QVector<LayerDeclaration> declared;
  QString manifestErr;
  if ( !layers->tryDeclared( &declared, &manifestErr ) )
  {
    paleo::workflow_detail::setError( error, manifestErr.isEmpty() ? tr( "无法读取图层清单" ) : manifestErr );
    return false;
  }
  for ( const LayerDeclaration &d : declared )
  {
    if ( d.layerId == pointsId )
    {
      job->wellUri = d.source;
      break;
    }
  }
  if ( job->wellUri.isEmpty() )
  {
    paleo::workflow_detail::setError( error, tr( "井点图层 %1 没有数据源" ).arg( pointsId ) );
    return false;
  }
  job->parentPaths << job->wellUri.section( QLatin1Char( '|' ), 0, 0 );
  job->params.insert("well_input_version_ids", DerivedAssetRegistrar(m_catalog, m_projectDir).parentVersionIdsFor(job->parentPaths));
  if ( method == QLatin1String( "cokriging" ) )
  {
    const QString covariateLayerId = params.value( QStringLiteral( "covariateLayerId" ) ).toString();
    if ( covariateLayerId.trimmed().isEmpty() )
    {
      paleo::workflow_detail::setError( error, tr( "协克里金方法需要有效的次级协变量图层 (covariateLayerId)" ) );
      return false;
    }
    QString instErr;
    QgsMapLayer *covLayer = layers->instantiate( covariateLayerId, &instErr );
    auto *covRaster = qobject_cast<QgsRasterLayer *>( covLayer );
    if ( !covRaster || !covRaster->isValid() )
    {
      paleo::workflow_detail::setError( error, tr( "协克里金方法需要有效的次级协变量图层 (covariateLayerId)" ) );
      return false;
    }
    job->parentPaths << covRaster->source().section( QLatin1Char( '|' ), 0, 0 );
  }
  QString stageError;
  auto registrar =
      paleo::workflow_detail::derivedRegistrarOf(this, &stageError);
  if (m_factorTasks && registrar.isBound()) {
    bool known = false;
    const auto definition = SingleFactorRegistry::byId(factorId, &known);
    job->staging = registrar.stage(
        "single_factor_raster", tr("%1·%2").arg(definition.title, horizon),
        QStringLiteral("FACTOR_%1_%2.tif").arg(factorId, horizon), &stageError);
    if (!job->staging.isValid()) {
      paleo::workflow_detail::setError(error, stageError);
      return false;
    }
  }
  job->prepared = true;
  return true;
}

bool ConstraintWorkflow::computeGeostatJob(
    GeostatJob *job, const std::function<bool()> &cancelled,
    const std::function<void(double)> &progress) {
  return computeGeostatJobWithService(m_proc.data(), job, cancelled, progress);
}

bool ConstraintWorkflow::computeGeostatJobWithService(
    QgisProcessingService *proc, GeostatJob *job,
    const std::function<bool()> &cancelled,
    const std::function<void(double)> &progress) {
  if ( !job || !job->prepared )
  {
    if ( job )
      job->error = tr( "地质统计任务尚未准备" );
    return false;
  }
  if ( !proc )
  {
    job->error = tr( "constraint workflow is not bound to services" );
    return false;
  }
  if ( cancelled && cancelled() )
  {
    job->error = tr( "已取消" );
    return false;
  }

  // ---- 采样：井点图层 → 有效样本 + 参证/跳过清单 + CRS ----
  std::vector<paleo::geostat::Sample> samples;
  QVariantList supportingWells, skippedWells;
  QgsCoordinateReferenceSystem sampleCrs;
  if ( !sampleGeostatWells( job, cancelled, samples, supportingWells, skippedWells, sampleCrs ) )
    return false;

  const QString tempDir = QDir( QDir::tempPath() )
                              .filePath( QStringLiteral( "paleo-gs-%1" ).arg( QUuid::createUuid().toString( QUuid::Id128 ) ) );
  if ( !QDir().mkpath( tempDir ) )
  {
    job->error = tr( "无法创建临时计算目录" );
    return false;
  }
  const auto cleanupTemp = [tempDir]() { QDir( tempDir ).removeRecursively(); };
  job->outputPath = QDir( tempDir ).filePath( QStringLiteral( "factor.tif" ) );
  job->qcPath = QDir( tempDir ).filePath( QStringLiteral( "qc.json" ) );

  QVariantMap qcParameters = job->params;
  qcParameters.insert( QStringLiteral( "field" ), job->field );
  qcParameters.insert( QStringLiteral( "cell_size" ), job->cellSize );

  // ---- 降级：样本不足阈值 → 约束 IDW，method_actual 如实标注 ----
  if ( static_cast<int>( samples.size() ) < kGeostatMinSamples )
  {
    if ( !runDegradedGeostatIdw( proc, job, samples, sampleCrs, supportingWells, skippedWells,
                                 qcParameters, cancelled, progress ) )
    {
      cleanupTemp();
      return false;
    }
    return true;
  }

  // ---- 网格：样本包围盒 + 2 像元边距（超工作流预算即拒） ----
  const GeostatSampleGrid gridRun = buildGeostatGrid( samples, job->cellSize, sampleCrs );
  const paleo::geostat::GridSpec &grid = gridRun.spec;
  if ( static_cast<qint64>( grid.rows ) * grid.cols > kGeostatMaxWorkflowCells )
  {
    cleanupTemp();
    job->error = tr( "地质统计栅格 %1×%2 超过工作流预算 %3 格——像元过细或井域过大" )
                     .arg( grid.cols )
                     .arg( grid.rows )
                     .arg( static_cast<int>( kGeostatMaxWorkflowCells ) );
    return false;
  }

  // ---- 变差函数：显式参数或自动拟合（可选方向各向异性） ----
  paleo::geostat::VariogramModel model;
  GeostatQcParts parts;
  if ( !fitGeostatVariogram( job, samples, gridRun.spanX, gridRun.spanY, &model, &parts ) )
  {
    cleanupTemp();
    return false;
  }

  // ---- P2：输入取用诚实面 + 检索邻域节点（建模上下文就绪后收集） ----
  GeostatRunContext ctx;
  ctx.job = job;
  ctx.tempDir = tempDir;
  ctx.grid = grid;
  ctx.model = model;
  ctx.geoTransform = { grid.originX, grid.pixelWidth, 0.0, grid.originY, 0.0, grid.pixelHeight };
  ctx.crs = sampleCrs;
  ctx.control.cancelled = cancelled;
  ctx.control.progress = progress;
  ctx.maxPoints = job->params.value( QStringLiteral( "maxPoints" ), 16 ).toInt();
  ctx.searchRadius = std::max( 0.0, job->params.value( QStringLiteral( "searchRadius" ), 0.0 ).toDouble() );
  ctx.minPoints = job->params.value( QStringLiteral( "minPoints" ), 4 ).toInt();
  collectGeostatQcInputs( job, model, ctx, &parts );

  // ---- 方法执行：kriging / cokriging / sgs 三选一 ----
  bool ran = false;
  if ( job->method == QLatin1String( "kriging" ) )
    ran = runGeostatKriging( ctx, samples, &parts );
  else if ( job->method == QLatin1String( "cokriging" ) )
    ran = runGeostatCokriging( ctx, samples, &parts, qcParameters );
  else
    ran = runGeostatSgs( ctx, samples, &parts );
  if ( !ran )
  {
    cleanupTemp();
    return false;
  }

  // ---- QC 落盘 ----
  if ( !writeGeostatQc( ctx, samples, supportingWells, skippedWells, qcParameters, parts ) )
  {
    cleanupTemp();
    return false;
  }

  // ---- 暂存：成果拷入暂存位并落 SHA（失败不回收临时目录，与原路径一致） ----
  if ( !stageGeostatOutputs( job, cancelled ) )
    return false;
  job->ok = true;
  return true;
}


bool ConstraintWorkflow::publishGeostatJob( GeostatJob &job, QString *error )
{
  const auto discardTemp = [&job]() {
    if ( job.outputPath.contains( QStringLiteral( "paleo-gs-" ) ) )
      QDir( QFileInfo( job.outputPath ).absolutePath() ).removeRecursively();
  };
  if ( !job.ok || job.outputPath.isEmpty() )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, job.error.isEmpty() ? tr( "地质统计计算未返回输出路径" ) : job.error );
    return false;
  }
  if ( job.generation != m_publishGeneration )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, tr( "发布代次已变，丢弃这次地质统计成果" ) );
    return false;
  }
  bool known = false;
  const SingleFactorDefinition def = SingleFactorRegistry::byId( job.factorId, &known );
  if ( !known )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, tr( "未知单因素 id：%1" ).arg( job.factorId ) );
    return false;
  }
  QgisLayerService *layers = m_layers.data();
  if ( !layers )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, tr( "constraint workflow is not bound to a layer service" ) );
    return false;
  }
  QString regErr;
  DerivedAssetRegistrar registrar = paleo::workflow_detail::derivedRegistrarOf( this, &regErr );
  if ( !registrar.isBound() )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, regErr );
    return false;
  }
  const DerivedStaging st =
      job.staging.isValid()
          ? job.staging
          : registrar.stage(QStringLiteral("single_factor_raster"),
                            tr("%1·%2").arg(def.title, job.horizon),
                            QStringLiteral("FACTOR_%1_%2.tif")
                                .arg(job.factorId, job.horizon),
                            &regErr);
  if ( !st.isValid() )
  {
    discardTemp();
    paleo::workflow_detail::setError( error, regErr );
    return false;
  }
  const QString stagedSupport = job.supportPath.isEmpty() ? QString()
                                    : fileStem( st.absolutePath ) + QStringLiteral( ".support.tif" );
  const QString stagedQc = fileStem( st.absolutePath ) + QStringLiteral( ".qc.json" );
  auto discard = [&]() {
    removeIfPresent( st.absolutePath );
    removeIfPresent( stagedSupport );
    removeIfPresent( stagedQc );
    discardTemp();
  };
  if ( job.generation != m_publishGeneration )
  {
    discard();
    paleo::workflow_detail::setError( error, tr( "发布代次已变，丢弃这次地质统计成果" ) );
    return false;
  }
  if (!job.staging.isValid() && !QFile::copy(job.qcPath, stagedQc)) {
    discard();
    paleo::workflow_detail::setError( error, tr( "地质统计 QC 无法写入成果目录" ) );
    return false;
  }
  if (!job.staging.isValid() && !stagedSupport.isEmpty() &&
      !QFile::copy(job.supportPath, stagedSupport)) {
    discard();
    paleo::workflow_detail::setError( error, tr( "地质统计旁路文件无法写入成果目录" ) );
    return false;
  }
  QString jsonErr;
  const QVariantMap qc = readJsonObject( stagedQc, &jsonErr );
  QVariantMap hashParams = qc.value( QStringLiteral( "parameters" ) ).toMap();
  if ( hashParams.isEmpty() )
  {
    discard();
    paleo::workflow_detail::setError( error, jsonErr.isEmpty() ? tr( "地质统计成果缺少参数指纹输入" ) : jsonErr );
    return false;
  }
  QStringList parentIds = registrar.parentVersionIdsFor( job.parentPaths );
  parentIds.append( job.params.value( QStringLiteral( "parentVersionIds" ) ).toStringList() );
  parentIds.removeDuplicates();
  parentIds.sort();
  QVariantList parentList;
  for ( const QString &id : parentIds )
    parentList << id;
  hashParams.insert( QStringLiteral( "horizon" ), job.horizon );
  hashParams.insert( QStringLiteral( "factor_id" ), job.factorId );
  hashParams.insert( QStringLiteral( "parent_version_ids" ), parentList );
  const paleo::singlefactor::ParameterHash hash = paleo::singlefactor::parameterHash( hashParams );
  if ( !hash.ok )
  {
    discard();
    paleo::workflow_detail::setError( error, hash.error.isEmpty() ? tr( "参数指纹计算失败" ) : hash.error );
    return false;
  }
  QString shaErr;
  const QString qcSha = job.staging.isValid()
                            ? job.stagedQcSha
                            : DataCatalog::sha256FileHex(stagedQc, &shaErr);
  QString supportSha;
  if ( !stagedSupport.isEmpty() )
  {
    supportSha = job.staging.isValid()
                     ? job.stagedSupportSha
                     : DataCatalog::sha256FileHex(stagedSupport, &shaErr);
    if ( supportSha.isEmpty() )
    {
      discard();
      paleo::workflow_detail::setError( error, shaErr.isEmpty() ? tr( "旁路文件 sha256 计算失败" ) : shaErr );
      return false;
    }
  }
  if ( qcSha.isEmpty() )
  {
    discard();
    paleo::workflow_detail::setError( error, shaErr.isEmpty() ? tr( "QC sha256 计算失败" ) : shaErr );
    return false;
  }
  if ( job.generation != m_publishGeneration )
  {
    discard();
    paleo::workflow_detail::setError( error, tr( "发布代次已变，丢弃这次地质统计成果" ) );
    return false;
  }
  const QDir projectDir( registrar.projectDir() );
  const QVariantMap counts = qc.value( QStringLiteral( "counts" ) ).toMap();
  const QString methodActual = job.degraded ? QStringLiteral( "idw" ) : job.method;
  const QString algorithmId = job.degraded
                                  ? QStringLiteral( "paleo:paleo_constraint_idw" )
                                  : QStringLiteral( "paleo:geostat_%1" ).arg( job.method );
  QVariantMap extra;
  extra.insert( QStringLiteral( "mapping_product" ), true );
  extra.insert( QStringLiteral( "layer_id" ), QStringLiteral( "product." ) + st.versionId );
  extra.insert( QStringLiteral( "manifest_layer_id" ),
                QStringLiteral( "factor.%1.%2" ).arg( job.horizon, job.factorId ) );
  extra.insert( QStringLiteral( "layer_type" ), QStringLiteral( "raster" ) );
  extra.insert( QStringLiteral( "title" ), tr( "%1·%2" ).arg( def.title, job.horizon ) );
  extra.insert( QStringLiteral( "group" ), QStringLiteral( "04_SingleFactor" ) );
  extra.insert( QStringLiteral( "factor_id" ), job.factorId );
  extra.insert( QStringLiteral( "field" ), job.field );
  extra.insert( QStringLiteral( "cell_size" ), job.cellSize );
  extra.insert( QStringLiteral( "horizon" ), job.horizon );
  extra.insert( QStringLiteral( "kind" ), QStringLiteral( "single_factor_raster" ) );
  extra.insert( QStringLiteral( "value_source" ), QStringLiteral( "analysis" ) );
  extra.insert( QStringLiteral( "parameter_hash" ), hash.sha256 );
  extra.insert( QStringLiteral( "provenance_schema_version" ), 1 );
  extra.insert( QStringLiteral( "method" ), job.method );
  extra.insert( QStringLiteral( "method_actual" ), methodActual );
  extra.insert( QStringLiteral( "algorithm_id" ), algorithmId );
  extra.insert( QStringLiteral( "constraints_consumed" ), QVariantList() );
  extra.insert( QStringLiteral( "constraints_unused" ), job.constraintsUnused );
  if ( !job.constraintCapability.isEmpty() )
    extra.insert( QStringLiteral( "constraint_capability" ), job.constraintCapability );
  extra.insert( QStringLiteral( "samples" ), qc.value( QStringLiteral( "samples" ) ) );
  extra.insert("supporting_wells", qc.value("supporting_wells"));
  extra.insert("skipped_wells", qc.value("skipped_wells"));
  extra.insert("participation_scope", QStringLiteral("accepted_geostat_observations; neighbourhood_per_cell"));
  extra.insert( QStringLiteral( "variogram" ), qc.value( QStringLiteral( "model" ) ) );
  if ( qc.contains( QStringLiteral( "sgs" ) ) )
    extra.insert( QStringLiteral( "sgs" ), qc.value( QStringLiteral( "sgs" ) ) );
  // P2：结果血缘带上模型来源、被忽略输入、邻域、交叉验证与协克里金 ρ 来源。
  for ( const QString &key : { QStringLiteral( "model_provenance" ), QStringLiteral( "ignored_inputs" ),
                               QStringLiteral( "search" ), QStringLiteral( "cross_validation" ),
                               QStringLiteral( "cokriging" ) } )
    if ( qc.contains( key ) )
      extra.insert( key, qc.value( key ) );
  extra.insert( QStringLiteral( "finite_cells" ), counts.value( QStringLiteral( "finite" ) ) );
  extra.insert( QStringLiteral( "nodata_cells" ), counts.value( QStringLiteral( "nodata" ) ) );
  extra.insert( QStringLiteral( "solver_failures" ), counts.value( QStringLiteral( "solver_failures" ) ) );
  if ( qc.contains( QStringLiteral( "parameters" ) ) )
  {
    const QVariantMap qcP = qc.value( QStringLiteral( "parameters" ) ).toMap();
    if ( qcP.contains( QStringLiteral( "cross_correlation" ) ) )
      extra.insert( QStringLiteral( "cross_correlation" ), qcP.value( QStringLiteral( "cross_correlation" ) ) );
  }
  if ( !stagedSupport.isEmpty() )
  {
    extra.insert( QStringLiteral( "support_path" ), projectDir.relativeFilePath( stagedSupport ) );
    extra.insert( QStringLiteral( "support_sha256" ), supportSha );
  }
  extra.insert( QStringLiteral( "qc_path" ), projectDir.relativeFilePath( stagedQc ) );
  extra.insert( QStringLiteral( "qc_sha256" ), qcSha );
  inheritMockFlag( PaleoWorkflowDerivedCatalog( this ), parentIds, extra );
  insertStrategyId( job.params, extra ); // 方向67：策略包 id 进血缘（kriging 词表项）
  // algo-atom P1-extend：geostat 经 publishArtifacts
  const QString publishPath =
      job.staging.isValid() ? st.absolutePath : job.outputPath;
  const AlgorithmRunResult runResult = paleo::runResultFromOutputPath( publishPath );
  QString commitErr;
  if ( !paleo::publishArtifacts(
           registrar, st, runResult, parentIds, algorithmId, extra, &commitErr,
           job.staging.isValid() ? &job.fingerprint : nullptr ) )
  {
    discard();
    paleo::workflow_detail::setError( error, commitErr );
    return false;
  }
  job.catalogPublished = true; // 已只读入册——后续 declare/集合/统计失败不回删
  if ( !declareFactorResult( layers, job.horizon, job.factorId, def, st.absolutePath, registrar.projectDir(),
                             st.assetId, error ) )
  {
    discardTemp();
    return false;
  }
  // 方向 47：SGS 成员集合入库——成员栅格由计算段写进同一临时目录，
  // 这里收编为 realization_set 成员版本，并派生均值/总体标准差/P10/P90
  // 统计面（成员 ≥2 才派生——单成员集合如实无不确定性）。
  if ( !job.memberPaths.isEmpty() )
  {
    RealizationWorkflow realizationWf;
    realizationWf.bind( m_catalog.data(), m_projectDir, layers );
    QVector<RealizationWorkflow::MemberInput> memberInputs;
    memberInputs.reserve( job.memberPaths.size() );
    for ( const QString &path : job.memberPaths )
      memberInputs.push_back( { path, job.memberSeed } );
    QVariantMap setExtra;
    setExtra.insert( QStringLiteral( "method" ), job.method );
    setExtra.insert( QStringLiteral( "method_actual" ), methodActual );
    setExtra.insert( QStringLiteral( "algorithm_id" ), algorithmId );
    setExtra.insert( QStringLiteral( "factor_id" ), job.factorId );
    setExtra.insert( QStringLiteral( "field" ), job.field );
    setExtra.insert( QStringLiteral( "cell_size" ), job.cellSize );
    setExtra.insert( QStringLiteral( "parameter_hash" ), hash.sha256 );
    setExtra.insert( QStringLiteral( "variogram" ), qc.value( QStringLiteral( "model" ) ) );
    setExtra.insert( QStringLiteral( "group" ), QStringLiteral( "04_SingleFactor/Realizations" ) );
    QString setErr;
    const QString setId = realizationWf.publishSet(
        tr( "%1·%2" ).arg( def.title, job.horizon ), job.horizon, memberInputs,
        parentIds, setExtra, &setErr );
    if ( setId.isEmpty() )
    {
      discardTemp();
      // 单因素图+图层已登记成功——集合是增量产物，如实报告两半段状态。
      paleo::workflow_detail::setError( error,
          tr( "单因素图已登记；realization 集合入库失败：%1" ).arg( setErr ) );
      return false;
    }
    if ( job.memberPaths.size() >= 2 )
    {
      paleo::ensemble::StatsRequest want;
      want.mean = want.stddev = want.p10 = want.p90 = true;
      if ( !realizationWf.deriveStatistics( setId, want, &setErr ) )
      {
        discardTemp();
        paleo::workflow_detail::setError( error,
            tr( "realization 集合 %1 已入库；统计派生失败：%2" ).arg( setId, setErr ) );
        return false;
      }
    }
  }
  discardTemp();
  return true;
}

bool ConstraintWorkflow::generateGeostatFactor( const QString &horizon, const QString &factorId,
                                                const QString &method, const SingleFactorDefinition &def,
                                                const QVariantMap &params, QString *error )
{
  Q_UNUSED( def );
  GeostatJob job;
  if ( !prepareGeostatJob( horizon, factorId, method, params, &job, error ) )
    return false;
  if (m_factorTasks) {
    auto shared = std::make_shared<GeostatJob>(job);
    return runFactorWork(
        tr("%1 单因素计算").arg(horizon),
        [proc = m_proc.data(), shared](PaleoTask *task) {
          const bool ok = computeGeostatJobWithService(
              proc, shared.get(), [task] { return task->cancelRequested(); },
              [task](double p) {
                task->reportStage("build", qBound(0, qRound(p), 95));
              });
          return ok ? QString() : shared->error;
        },
        [this, shared](QString *error) {
          return publishGeostatJob(*shared, error);
        },
        [shared] {
          // 临时目录前缀是 paleo-gs-<uuid>（computeGeostatJob:1164 建），
          // 不是 paleo-sf-——判错前缀等于永不命中，不走 publish 的收尾
          // 路径会把整棵临时树（factor.tif + variance/std.tif + SGS 的
          // R 个 member_r*.tif，按 4M 格预算每件约 16MB）永久留在系统
          // temp（#588 B.10）。
          if (shared->outputPath.contains("paleo-gs-"))
            QDir(QFileInfo(shared->outputPath).absolutePath())
                .removeRecursively();
          if (shared->catalogPublished)
            return; // 已入库（只读在册）：删除即悬垂版本——临时树已由
                    // publish 内 discardTemp 收尾，这里不再动 staging。
          if (shared->staging.isValid()) {
            const auto path = shared->staging.absolutePath;
            for (const auto &file :
                 QStringList{path, fileStem(path) + ".qc.json",
                             fileStem(path) + ".support.tif"})
              QFile::remove(file);
          }
        },
        error);
  }
  if ( !computeGeostatJob( &job ) )
  {
    paleo::workflow_detail::setError( error, job.error );
    return false;
  }
  return publishGeostatJob( job, error );
}

