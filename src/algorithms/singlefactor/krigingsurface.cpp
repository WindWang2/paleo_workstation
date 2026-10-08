// 层：数据
#include "krigingsurface.h"

#include "../geostat/faultpath.h"
#include "../geostat/variogram.h"

#include "localidw.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

// 层：数据
namespace paleo::singlefactor
{
namespace
{

// 变差函数欠定阈值：与 workflow/constraintfactorjobs.cpp kGeostatMinSamples 同口径
//（方向18 的降级 metric）。少于 8 口时实验变差的分档与最小二乘都不稳。
constexpr int kMinKrigingSamples = 8;
constexpr int kAutoLags = 12;
constexpr double kAutoToleranceDeg = 22.5;
// geostat 隔断感知档的样本预算闸（experimentalVariogram 限 n ≤ 512）。
constexpr int kBarrierSampleBudget = 512;

geostat::VariogramModelType modelTypeOf( const std::string &name )
{
  if ( name == "exponential" )
    return geostat::VariogramModelType::Exponential;
  if ( name == "gaussian" )
    return geostat::VariogramModelType::Gaussian;
  return geostat::VariogramModelType::Spherical;
}

std::string modelNameOf( geostat::VariogramModelType type )
{
  switch ( type )
  {
    case geostat::VariogramModelType::Exponential:
      return "exponential";
    case geostat::VariogramModelType::Gaussian:
      return "gaussian";
    case geostat::VariogramModelType::Spherical:
      return "spherical";
  }
  return "spherical";
}

std::vector<geostat::Sample> toGeostatSamples( const std::vector<Sample> &samples )
{
  std::vector<geostat::Sample> converted;
  converted.reserve( samples.size() );
  for ( const Sample &sample : samples )
  {
    if ( std::isfinite( sample.x ) && std::isfinite( sample.y ) && std::isfinite( sample.value ) )
      converted.push_back( geostat::Sample{ sample.x, sample.y, sample.value } );
  }
  return converted;
}

std::string number( double value )
{
  return std::to_string( value );
}

// 方向84（D4）：硬屏障折线 → 测地变差的窄墙多边形。宽度取 2 像元（栅格化后
// 至少占格）；sealEnds=true（interpretation_partition_v1）时首尾沿端段方向外延
// 封死（两侧不连通），grid_connectivity_v1 保持有限端（可绕行）——与 localidw
// 屏障模型分派同语义。
geostat::BarrierPolygon wallPolygonOf( const std::vector<Point2> &points, double width,
                                       bool sealEnds, double extend )
{
  std::vector<Point2> path = points;
  if ( sealEnds && path.size() >= 2 )
  {
    const Point2 headDir{ path[0].x - path[1].x, path[0].y - path[1].y };
    const double headLen = std::max( std::hypot( headDir.x, headDir.y ), 1e-12 );
    path.insert( path.begin(),
                 Point2{ path[0].x + headDir.x / headLen * extend,
                         path[0].y + headDir.y / headLen * extend } );
    const std::size_t last = path.size() - 1;
    const Point2 tailDir{ path[last].x - path[last - 1].x, path[last].y - path[last - 1].y };
    const double tailLen = std::max( std::hypot( tailDir.x, tailDir.y ), 1e-12 );
    path.push_back( Point2{ path[last].x + tailDir.x / tailLen * extend,
                            path[last].y + tailDir.y / tailLen * extend } );
  }
  // 逐点法向（相邻段法向平均）偏移 ±width/2，左链 + 逆序右链拼 ring。
  const auto offsetChain = [&]( double side ) {
    std::vector<Point2> chain;
    chain.resize( path.size() );
    for ( std::size_t i = 0; i < path.size(); ++i )
    {
      double nx = 0;
      double ny = 0;
      if ( i > 0 )
      {
        nx += -( path[i].y - path[i - 1].y );
        ny += path[i].x - path[i - 1].x;
      }
      if ( i + 1 < path.size() )
      {
        nx += -( path[i + 1].y - path[i].y );
        ny += path[i + 1].x - path[i].x;
      }
      const double len = std::max( std::hypot( nx, ny ), 1e-12 );
      chain[i] = Point2{ path[i].x + side * nx / len * width * 0.5,
                         path[i].y + side * ny / len * width * 0.5 };
    }
    return chain;
  };
  geostat::BarrierPolygon polygon;
  std::vector<Point2> left = offsetChain( 1.0 );
  std::vector<Point2> right = offsetChain( -1.0 );
  left.insert( left.end(), right.rbegin(), right.rend() );
  // singlefactor::Point2 → geostat::Point2（ BarrierRing 的坐标类型）。
  polygon.exterior.points.reserve( left.size() );
  for ( const Point2 &point : left )
    polygon.exterior.points.push_back( geostat::Point2{ point.x, point.y } );
  return polygon;
}

} // namespace

VariogramResolution resolveVariogram( const std::vector<Sample> &samples, const GridSpec &grid,
                                      const ResolvedParameters &parameters,
                                      const std::vector<ConstraintLine> &constraints )
{
  VariogramResolution result;
  result.modelName = modelNameOf( modelTypeOf( parameters.variogramModel ) );
  result.sampleCount = static_cast<int>( samples.size() );
  const geostat::VariogramModelType type = modelTypeOf( parameters.variogramModel );
  const std::vector<geostat::Sample> converted = toGeostatSamples( samples );
  if ( converted.size() < 2 )
  {
    result.message = "有效样本不足 2 口，无法计算实验变差函数";
    return result;
  }

  // ---- 显式参数：range>0 时不再拟合（nugget/sill 由用户给定） ----
  if ( parameters.range > 0.0 )
  {
    if ( !( parameters.sill >= 0.0 ) || !( parameters.nugget >= 0.0 ) )
    {
      result.message = "显式变差参数非法：需要 range>0、sill>=0、nugget>=0";
      return result;
    }
    if ( !( parameters.nugget + parameters.sill > 0.0 ) )
    {
      // 显式分支没看过样本值：这里只能说参数零信号，不能说「井值无空间变化」。
      result.message = "显式变差参数零信号（块金+拱高=0）：克里金方程组退化";
      return result;
    }
    result.nugget = parameters.nugget;
    result.sill = parameters.sill;
    result.range = parameters.range;
    result.fitted = false;
    result.anisotropyRatio =
        parameters.variogramAnisotropyRatio >= 1.0 ? parameters.variogramAnisotropyRatio : 1.0;
    result.azimuthDeg = parameters.variogramAzimuthDeg >= 0.0 ? parameters.variogramAzimuthDeg : 0.0;
    // 显式参数不拟合实验变差，隔断感知档无从生效（如实记录，不静默跳过）。
    for ( const ConstraintLine &line : constraints )
    {
      if ( line.enabled && line.semantic == Semantic::HardBarrier && line.points.size() >= 2 )
      {
        result.variogramNote = "variogram_explicit_skips_barrier_aware_fit（显式参数不拟合，"
                               "硬屏障两侧样本仍进同一结构参数）";
        break;
      }
    }
    result.ok = true;
    return result;
  }

  // ---- 方向84（D4）：硬屏障存在且样本在预算内 → 隔断感知拟合（测地滞后距，
  // 跨隔断样本对不进结构估计）；超预算如实回落欧氏口径并记录，不静默。 ----
  geostat::VariogramBarriers barriers;
  const bool sealEnds = parameters.hardBarrierModel == "interpretation_partition_v1";
  std::vector<geostat::BarrierPolygon> walls;
  double minX = std::numeric_limits<double>::infinity();
  double minY = minX;
  double maxX = -minX;
  double maxY = -minX;
  for ( const geostat::Sample &sample : converted )
  {
    minX = std::min( minX, sample.x );
    maxX = std::max( maxX, sample.x );
    minY = std::min( minY, sample.y );
    maxY = std::max( maxY, sample.y );
  }
  const double span = std::max( maxX - minX, maxY - minY );
  for ( const ConstraintLine &line : constraints )
  {
    if ( !line.enabled || line.semantic != Semantic::HardBarrier || line.points.size() < 2 )
      continue;
    // 墙宽取样本范围/64：测地栅格默认 256 格（格宽 ≈ span/256），4×格宽保证
    // 栅格化后墙至少占格（tst_geostat_variogram_barrier 的 wall 比例口径）。
    walls.push_back( wallPolygonOf( line.points, span / 64.0 + 1e-9, sealEnds,
                                    span * 1.5 + 1.0 ) );
  }
  if ( !walls.empty() )
  {
    if ( static_cast<int>( converted.size() ) <= kBarrierSampleBudget )
    {
      barriers.enabled = true;
      barriers.polygons = std::move( walls );
    }
    else
    {
      result.variogramNote = "variogram_barrier_aware_skipped_samples_over_budget（样本 " +
                             std::to_string( converted.size() ) + " > " +
                             std::to_string( kBarrierSampleBudget ) + "，回落欧氏滞后距口径）";
    }
  }

  // ---- 自动拟合：滞后距取 max(像元, sqrt(面积/井数))，12 档（方向18 同口径） ----
  const double area = std::fabs( grid.pixelWidth * grid.pixelHeight ) *
                      static_cast<double>( std::max( grid.cols, 1 ) ) *
                      static_cast<double>( std::max( grid.rows, 1 ) );
  const double cellSize = std::fabs( grid.pixelWidth ) > 0 ? std::fabs( grid.pixelWidth ) : 1.0;
  const double lag = std::max( cellSize, std::sqrt( area / static_cast<double>( converted.size() ) ) );
  const bool hasAzimuth = parameters.variogramAzimuthDeg >= 0.0 && parameters.variogramAzimuthDeg <= 360.0;
  const bool explicitRatio = parameters.variogramAnisotropyRatio >= 1.0;

  geostat::VariogramModel model;
  if ( hasAzimuth )
  {
    geostat::VariogramDirection along;
    along.omnidirectional = false;
    along.azimuthDeg = parameters.variogramAzimuthDeg;
    along.toleranceDeg = kAutoToleranceDeg;
    geostat::VariogramDirection across = along;
    across.azimuthDeg = std::fmod( parameters.variogramAzimuthDeg + 90.0, 360.0 );
    const geostat::ExperimentalVariogram experimentalAlong =
        geostat::experimentalVariogram( converted, lag, kAutoLags, along, barriers );
    if ( experimentalAlong.status != geostat::Status::Ok )
    {
      result.message = "方向变差函数（沿方位）失败：" + experimentalAlong.message;
      return result;
    }
    const geostat::ExperimentalVariogram experimentalAcross =
        geostat::experimentalVariogram( converted, lag, kAutoLags, across, barriers );
    if ( experimentalAcross.status != geostat::Status::Ok )
    {
      result.message = "方向变差函数（垂直方位）失败：" + experimentalAcross.message;
      return result;
    }
    const geostat::VariogramFit fitAlong = geostat::fitVariogram( experimentalAlong, type );
    const geostat::VariogramFit fitAcross = geostat::fitVariogram( experimentalAcross, type );
    if ( fitAlong.status != geostat::Status::Ok || fitAcross.status != geostat::Status::Ok )
    {
      result.message = "方向变差函数拟合失败：" + fitAlong.message + " / " + fitAcross.message;
      return result;
    }
    result.barrierAware = experimentalAlong.barrierAware || experimentalAcross.barrierAware;
    result.unreachablePairs =
        std::max( experimentalAlong.unreachablePairs, experimentalAcross.unreachablePairs );
    model = fitAlong.model;
    model.azimuthDeg = parameters.variogramAzimuthDeg;
    model.anisotropyRatio = explicitRatio
                                ? parameters.variogramAnisotropyRatio
                                : ( fitAcross.model.range > 0
                                        ? std::clamp( fitAlong.model.range / fitAcross.model.range, 1.0, 8.0 )
                                        : 1.0 );
    result.r2 = fitAlong.r2;
    result.rmse = fitAlong.rmse;
    result.usedLags = fitAlong.usedLags;
  }
  else
  {
    const geostat::ExperimentalVariogram experimental =
        geostat::experimentalVariogram( converted, lag, kAutoLags, {}, barriers );
    if ( experimental.status != geostat::Status::Ok )
    {
      result.message = "实验变差函数失败：" + experimental.message;
      return result;
    }
    const geostat::VariogramFit fit = geostat::fitVariogram( experimental, type );
    if ( fit.status != geostat::Status::Ok )
    {
      result.message = "变差函数拟合失败：" + fit.message;
      return result;
    }
    result.barrierAware = experimental.barrierAware;
    result.unreachablePairs = experimental.unreachablePairs;
    model = fit.model;
    if ( explicitRatio )
      model.anisotropyRatio = parameters.variogramAnisotropyRatio;
    result.r2 = fit.r2;
    result.rmse = fit.rmse;
    result.usedLags = fit.usedLags;
  }
  if ( !( model.nugget + model.sill > 0.0 ) )
  {
    result.message = "变差函数零信号（块金+拱高=0）：井值无空间变化，克里金方程组退化";
    return result;
  }
  if ( !( model.range > 0.0 ) )
  {
    result.message = "变差函数拟合未给出正变程";
    return result;
  }
  result.nugget = model.nugget;
  result.sill = model.sill;
  result.range = model.range;
  result.azimuthDeg = model.anisotropyRatio > 1.0 ? model.azimuthDeg : 0.0;
  result.anisotropyRatio = model.anisotropyRatio;
  result.fitted = true;
  result.ok = true;
  return result;
}

SurfaceResult evaluateLocalKriging( const PreparedInput &input, const GridSpec &grid,
                                    const ResolvedParameters &parameters, const Control &control )
{
  // 整面回落：仍走同一插值面，只是把引擎如实切回 IDW 并留下原因。
  const auto fallbackToIdw = [&]( const std::string &reason ) {
    ResolvedParameters idw = parameters;
    idw.methodActual = "local_direction_idw";
    idw.algorithmId = "paleo:paleo_local_direction_idw";
    idw.semanticProfile = "paleo_local_idw_v1";
    idw.fallbackReason = reason;
    SurfaceResult result = evaluateLocalIdw( input, grid, idw, control );
    result.surfaceFallbacks = 1;
    result.issues.push_back( "kriging_fallback: " + reason );
    return result;
  };

  if ( static_cast<int>( input.samples.size() ) < kMinKrigingSamples )
  {
    return fallbackToIdw( "有效样本 " + std::to_string( input.samples.size() ) + " < " +
                          std::to_string( kMinKrigingSamples ) + "，变差函数欠定" );
  }
  const VariogramResolution resolution = resolveVariogram( input.samples, grid, parameters,
                                                           input.constraints );
  if ( !resolution.ok )
    return fallbackToIdw( resolution.message );

  ResolvedParameters kriging = parameters;
  kriging.variogramModel = resolution.modelName;
  kriging.nugget = resolution.nugget;
  kriging.sill = resolution.sill;
  kriging.range = resolution.range;
  kriging.variogramAzimuthDeg = resolution.azimuthDeg;
  kriging.variogramAnisotropyRatio = resolution.anisotropyRatio;
  kriging.variogramFitR2 = resolution.r2;
  kriging.variogramFitRmse = resolution.rmse;
  kriging.variogramUsedLags = resolution.usedLags;
  kriging.methodActual = "kriging";
  kriging.algorithmId = "paleo:paleo_local_direction_kriging";
  kriging.semanticProfile = "paleo_local_kriging_v1";
  kriging.fallbackReason.clear();
  // 克里金邻域内重合井按精确均值合并（geostat 求解器口径），与 IDW 的
  // preserve_rows 口径不同，如实改名进血缘。
  kriging.duplicatePolicy = "dedupe_exact_mean";

  SurfaceResult result = evaluateLocalIdw( input, grid, kriging, control );
  if ( result.status != Status::Ok )
    return result;

  // 一个格都没解出来：按整面回落口径处理，不挂克里金标签——不重跑 IDW，
  // 现有数值本来就是同参数 IDW 权重给的（idwFallbackCells 如实保留）。
  if ( result.krigingCells == 0 && result.finiteCells > 0 )
  {
    ResolvedParameters idw = parameters;
    idw.methodActual = "local_direction_idw";
    idw.algorithmId = "paleo:paleo_local_direction_idw";
    idw.semanticProfile = "paleo_local_idw_v1";
    idw.fallbackReason = "没有任何格解出克里金值（" + std::to_string( result.idwFallbackCells ) +
                         " 格因方程奇异/病态或半径邻域不足改用 IDW 权重）";
    result.resolved = idw;
    result.surfaceFallbacks = 1;
    result.issues.push_back( "kriging_fallback: " + idw.fallbackReason );
    return result;
  }

  if ( result.idwFallbackCells > 0 )
  {
    result.issues.push_back( "kriging_solver_fallback_cells " +
                             std::to_string( result.idwFallbackCells ) +
                             "（方程奇异/病态，或半径邻域不足；这些格用同参数 IDW 权重，"
                             "不冒充克里金值）" );
  }
  // 全部样本邻域（K=0）时每格是 O(n³) 求解：井数大时如实提示，不静默拖慢。
  if ( kriging.krigingMaxPoints <= 0 && input.samples.size() > 256 )
  {
    result.issues.push_back( "kriging_all_samples_neighbourhood " +
                             std::to_string( input.samples.size() ) +
                             "（K=0：每格 O(n³) 求解，建议设邻域点数上限）" );
  }
  result.issues.push_back( "variogram " + kriging.variogramModel + " nugget=" + number( kriging.nugget ) +
                           " sill=" + number( kriging.sill ) + " range=" + number( kriging.range ) +
                           " ratio=" + number( kriging.variogramAnisotropyRatio ) +
                           " azimuth=" + number( kriging.variogramAzimuthDeg ) +
                           ( resolution.fitted ? " fitted=auto r2=" : " fitted=explicit r2=" ) +
                           number( resolution.r2 ) + " lags=" + std::to_string( resolution.usedLags ) );
  // 方向84（D4）：隔断感知拟合回执——跨隔断样本对没进结构估计，如实写进血缘。
  if ( resolution.barrierAware )
  {
    result.issues.push_back( "variogram_barrier_aware geodesic_lags unreachable_pairs=" +
                             std::to_string( resolution.unreachablePairs ) +
                             "（跨硬隔断样本对不进变差结构拟合）" );
  }
  else if ( !resolution.variogramNote.empty() )
  {
    result.issues.push_back( resolution.variogramNote );
  }
  result.issues.push_back( "kriging_dedupes_coincident_samples（重合井按精确均值合并）" );

  // 方向84（D1）：方向线/软边界经局部张量度量改写克里金半方差（与 IDW 权重
  // 公式同源，见 localidw 的 krigingMetric）。被解析过滤的线（ratio/半径/点数
  // 不达标）连 params.directions/soft 都没进——两族下同样不生效，不单列克里金
  // 专属 issue（列了反而暗示克里金特别忽略它）。
  const auto idAmong = []( const std::string &id, const auto &resolvedList ) {
    for ( const auto &entry : resolvedList )
      if ( entry.id == id )
        return true;
    return false;
  };
  for ( const ConstraintLine &line : input.constraints )
  {
    if ( !line.enabled )
      continue;
    if ( line.semantic == Semantic::DirectionGuide && idAmong( line.stableId, kriging.directions ) )
      result.issues.push_back( line.stableId +
                               " direction_guide_consumed_by_kriging_metric（局部张量改写"
                               "克里金半方差，与 IDW 方向权重同源）" );
    else if ( line.semantic == Semantic::InterpretiveBoundary && idAmong( line.stableId, kriging.soft ) )
      result.issues.push_back( line.stableId +
                               " soft_boundary_consumed_by_kriging_metric（跨线点对距离放大"
                               "进半方差，与 IDW 软边界衰减等效——多边界取最大 penalty、"
                               "gate 用点对两端平均）" );
  }
  // 方向84（D2）：井群去簇不进克里金——克里金权重是无偏约束下的最小方差解，
  // 乘去簇因子后不再最优（不冒充）；局部性由邻域 K 与硬屏障分量隔离提供。
  if ( kriging.wellClusterLocality )
    result.issues.push_back( "well_cluster_locality_not_used_by_kriging（克里金权重为无偏约束下"
                             "的最小方差解，去簇乘子破坏最优性；局部性由邻域 K 与硬屏障分量"
                             "隔离提供）" );
  return result;
}

namespace
{

// 方向84（D3）：γ22——协变量样本自动拟合（模型类型随 γ11，滞后距同口径：
// max(像元, sqrt(面积/井数))，12 档全向）。失败如实报因，不静默退化。
bool fitSecondaryVariogram( const std::vector<Sample> &covariate, const GridSpec &grid,
                            geostat::VariogramModelType type, VariogramResolution *out,
                            std::string *message )
{
  const std::vector<geostat::Sample> converted = toGeostatSamples( covariate );
  if ( converted.size() < 2 )
  {
    *message = "协变量有效样本不足 2 口，无法拟合协变量变差函数";
    return false;
  }
  const double area = std::fabs( grid.pixelWidth * grid.pixelHeight ) *
                      static_cast<double>( std::max( grid.cols, 1 ) ) *
                      static_cast<double>( std::max( grid.rows, 1 ) );
  const double cellSize = std::fabs( grid.pixelWidth ) > 0 ? std::fabs( grid.pixelWidth ) : 1.0;
  const double lag = std::max( cellSize, std::sqrt( area / static_cast<double>( converted.size() ) ) );
  const geostat::VariogramFit fit =
      geostat::fitVariogram( geostat::experimentalVariogram( converted, lag, kAutoLags ), type );
  if ( fit.status != geostat::Status::Ok )
  {
    *message = "协变量变差函数拟合失败：" + fit.message;
    return false;
  }
  if ( !( fit.model.nugget + fit.model.sill > 0.0 ) || !( fit.model.range > 0.0 ) )
  {
    *message = "协变量变差函数零信号或无正变程";
    return false;
  }
  out->nugget = fit.model.nugget;
  out->sill = fit.model.sill;
  out->range = fit.model.range;
  out->r2 = fit.r2;
  out->usedLags = fit.usedLags;
  return true;
}

} // namespace

SurfaceResult evaluateLocalCokriging( const PreparedInput &input, const GridSpec &grid,
                                      const ResolvedParameters &parameters, const Control &control )
{
  // 协克里金请求不成立（缺协变量/拟合不出结构）= 如实拒绝，不回落冒充：
  // 回落产物里没有任何协变量信息，挂着协克里金标签就是冒充。
  const auto reject = [&]( const std::string &reason ) {
    SurfaceResult result;
    result.status = Status::InvalidInput;
    result.message = "协克里金请求拒绝：" + reason;
    ResolvedParameters rejected = parameters;
    rejected.methodActual = "local_direction_idw";
    rejected.algorithmId = "paleo:paleo_local_direction_idw";
    rejected.semanticProfile = "paleo_local_idw_v1";
    rejected.fallbackReason = result.message;
    result.resolved = rejected;
    return result;
  };

  // 协变量必须与样本逐口对齐（review H1）：非空但尺寸不齐 = 上游采样契约破裂，
  // 拒绝而不是静默丢协变量（那会让面退化为普通克里金仍挂协克里金标签）；
  // 空 = 未提供协变量，落到下面的有效样本闸（同口径拒绝）。
  if ( !input.covariate.empty() && input.covariate.size() != input.samples.size() )
    return reject( "协变量样本 " + std::to_string( input.covariate.size() ) + " 口与主变量 " +
                   std::to_string( input.samples.size() ) + " 口不对齐（井位采样契约破裂）" );
  std::size_t covariateFinite = 0;
  for ( const Sample &sample : input.covariate )
    covariateFinite += std::isfinite( sample.value ) ? 1 : 0;
  if ( covariateFinite < 2 )
    return reject( "协变量有效样本 " + std::to_string( covariateFinite ) +
                   " < 2（未提供协变量栅格，或井位采样全部 nodata）" );

  if ( static_cast<int>( input.samples.size() ) < kMinKrigingSamples )
    return reject( "主变量有效样本 " + std::to_string( input.samples.size() ) + " < " +
                   std::to_string( kMinKrigingSamples ) + "，变差函数欠定" );
  const VariogramResolution resolution = resolveVariogram( input.samples, grid, parameters,
                                                           input.constraints );
  if ( !resolution.ok )
    return reject( resolution.message );

  const geostat::VariogramModelType secondaryType = modelTypeOf( resolution.modelName );
  VariogramResolution secondary;
  std::string secondaryError;
  if ( !fitSecondaryVariogram( input.covariate, grid, secondaryType, &secondary, &secondaryError ) )
    return reject( secondaryError );

  ResolvedParameters cokriging = parameters;
  cokriging.variogramModel = resolution.modelName;
  cokriging.nugget = resolution.nugget;
  cokriging.sill = resolution.sill;
  cokriging.range = resolution.range;
  cokriging.variogramAzimuthDeg = resolution.azimuthDeg;
  cokriging.variogramAnisotropyRatio = resolution.anisotropyRatio;
  cokriging.variogramFitR2 = resolution.r2;
  cokriging.variogramFitRmse = resolution.rmse;
  cokriging.variogramUsedLags = resolution.usedLags;
  cokriging.crossCorrelation = std::clamp( parameters.crossCorrelation, -1.0, 1.0 );
  cokriging.secondaryNugget = secondary.nugget;
  cokriging.secondarySill = secondary.sill;
  cokriging.secondaryRange = secondary.range;
  cokriging.methodActual = "cokriging";
  cokriging.algorithmId = "paleo:paleo_local_direction_cokriging";
  cokriging.semanticProfile = "paleo_local_cokriging_v1";
  cokriging.fallbackReason.clear();
  cokriging.duplicatePolicy = "dedupe_exact_mean";

  SurfaceResult result = evaluateLocalIdw( input, grid, cokriging, control );
  if ( result.status != Status::Ok )
    return result;

  // 一个格都没解出：按整面回落口径处理（同克里金），不挂协克里金标签。
  if ( result.krigingCells == 0 && result.finiteCells > 0 )
  {
    ResolvedParameters idw = parameters;
    idw.methodActual = "local_direction_idw";
    idw.algorithmId = "paleo:paleo_local_direction_idw";
    idw.semanticProfile = "paleo_local_idw_v1";
    idw.fallbackReason = "没有任何格解出协克里金值（" + std::to_string( result.idwFallbackCells ) +
                         " 格因方程奇异/病态或半径邻域不足改用 IDW 权重）";
    result.resolved = idw;
    result.surfaceFallbacks = 1;
    result.issues.push_back( "cokriging_fallback: " + idw.fallbackReason );
    return result;
  }

  if ( result.idwFallbackCells > 0 )
  {
    result.issues.push_back( "cokriging_solver_fallback_cells " +
                             std::to_string( result.idwFallbackCells ) +
                             "（方程奇异/病态，或半径邻域不足，或该硬隔断分量内协变量"
                             "枯竭；这些格用同参数 IDW 权重，不冒充协克里金值）" );
  }
  // 全部样本邻域（K=0）时每格 O((n+m)³) 求解：与克里金同口径如实提示。
  if ( cokriging.krigingMaxPoints <= 0 && input.samples.size() > 256 )
  {
    result.issues.push_back( "cokriging_all_samples_neighbourhood " +
                             std::to_string( input.samples.size() ) +
                             "（K=0：每格 O((n+m)³) 求解，建议设邻域点数上限）" );
  }
  result.issues.push_back( "variogram " + cokriging.variogramModel + " nugget=" + number( cokriging.nugget ) +
                           " sill=" + number( cokriging.sill ) + " range=" + number( cokriging.range ) +
                           " ratio=" + number( cokriging.variogramAnisotropyRatio ) +
                           " azimuth=" + number( cokriging.variogramAzimuthDeg ) +
                           ( resolution.fitted ? " fitted=auto r2=" : " fitted=explicit r2=" ) +
                           number( resolution.r2 ) + " lags=" + std::to_string( resolution.usedLags ) );
  result.issues.push_back( "covariate_variogram nugget=" + number( cokriging.secondaryNugget ) +
                           " sill=" + number( cokriging.secondarySill ) +
                           " range=" + number( cokriging.secondaryRange ) +
                           " fitted=auto r2=" + number( secondary.r2 ) +
                           " lags=" + std::to_string( secondary.usedLags ) +
                           "（协变量样本 " + std::to_string( covariateFinite ) + " 口）" );
  result.issues.push_back( "cokriging_cross_model MM1 rho=" + number( cokriging.crossCorrelation ) +
                           "（γ12 = ρ·γ1；ρ=0 时方程组退化为普通克里金；半径模式下协变量"
                           "出范围的格省 μ2 行、按普通克里金解）" );
  if ( resolution.barrierAware )
  {
    result.issues.push_back( "variogram_barrier_aware geodesic_lags unreachable_pairs=" +
                             std::to_string( resolution.unreachablePairs ) +
                             "（跨硬隔断样本对不进变差结构拟合）" );
  }
  else if ( !resolution.variogramNote.empty() )
  {
    result.issues.push_back( resolution.variogramNote );
  }
  // 约束线 v1 不进协克里金半方差（CoKrigingSolver 无度量变换接口）：逐条如实
  // 记 issue，不静默；井群去簇同克里金口径（最小方差解不容去簇乘子）。
  const auto idAmong = []( const std::string &id, const auto &resolvedList ) {
    for ( const auto &entry : resolvedList )
      if ( entry.id == id )
        return true;
    return false;
  };
  for ( const ConstraintLine &line : input.constraints )
  {
    if ( !line.enabled )
      continue;
    if ( line.semantic == Semantic::DirectionGuide && idAmong( line.stableId, cokriging.directions ) )
      result.issues.push_back( line.stableId + " direction_guide_not_used_by_cokriging（v1 语义）" );
    else if ( line.semantic == Semantic::InterpretiveBoundary && idAmong( line.stableId, cokriging.soft ) )
      result.issues.push_back( line.stableId + " soft_boundary_not_used_by_cokriging（v1 语义）" );
  }
  if ( cokriging.wellClusterLocality )
    result.issues.push_back( "well_cluster_locality_not_used_by_cokriging（克里金族权重为无偏"
                             "约束下的最小方差解，去簇乘子破坏最优性）" );
  return result;
}

} // namespace paleo::singlefactor
