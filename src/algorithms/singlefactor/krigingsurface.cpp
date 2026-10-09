// 层：数据
#include "krigingsurface.h"

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
// 与 geostat 实验变差隔断档同一预算：超限不进测地场，回落欧氏口径。
constexpr int kBarrierSampleBudget = 512;

// 硬屏障折线展成测地墙。interpretation_partition_v1 把首尾沿端段外延封死；
// grid_connectivity_v1 保持有限端，样本可以绕过墙头。
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
  const auto offsetChain = [&]( double side ) {
    std::vector<Point2> chain( path.size() );
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
  polygon.exterior.points.reserve( left.size() );
  for ( const Point2 &point : left )
    polygon.exterior.points.push_back( geostat::Point2{ point.x, point.y } );
  return polygon;
}

geostat::VariogramBarriers barriersFor( const std::vector<geostat::Sample> &samples,
                                        const std::vector<ConstraintLine> &constraints,
                                        const ResolvedParameters &parameters, std::string *note )
{
  geostat::VariogramBarriers barriers;
  if ( !parameters.variogramBarrierAware )
    return barriers;
  double minX = std::numeric_limits<double>::infinity();
  double minY = minX;
  double maxX = -minX;
  double maxY = -minX;
  for ( const geostat::Sample &sample : samples )
  {
    minX = std::min( minX, sample.x );
    maxX = std::max( maxX, sample.x );
    minY = std::min( minY, sample.y );
    maxY = std::max( maxY, sample.y );
  }
  const double span = std::max( maxX - minX, maxY - minY );
  const bool sealEnds = parameters.hardBarrierModel == "interpretation_partition_v1";
  std::vector<geostat::BarrierPolygon> walls;
  for ( const ConstraintLine &line : constraints )
  {
    if ( !line.enabled || line.semantic != Semantic::HardBarrier || line.points.size() < 2 )
      continue;
    walls.push_back( wallPolygonOf( line.points, span / 64.0 + 1e-9, sealEnds, span * 1.5 + 1.0 ) );
  }
  if ( walls.empty() )
    return barriers;
  if ( static_cast<int>( samples.size() ) > kBarrierSampleBudget )
  {
    if ( note )
      *note = "variogram_barrier_aware_skipped_samples_over_budget（样本 " +
              std::to_string( samples.size() ) + " > " + std::to_string( kBarrierSampleBudget ) +
              "，回落欧氏滞后距口径）";
    return barriers;
  }
  barriers.enabled = true;
  barriers.polygons = std::move( walls );
  return barriers;
}

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
    for ( const ConstraintLine &line : constraints )
    {
      if ( line.enabled && line.semantic == Semantic::HardBarrier && line.points.size() >= 2 &&
           parameters.variogramBarrierAware )
      {
        result.variogramNote = "variogram_explicit_skips_barrier_aware_fit（显式参数不拟合，"
                               "硬屏障两侧样本仍进同一结构参数）";
        break;
      }
    }
    result.ok = true;
    return result;
  }

  const geostat::VariogramBarriers barriers = barriersFor( converted, constraints, parameters, &result.variogramNote );

  // ---- 自动拟合：滞后距取 max(像元, sqrt(面积/井数))，12 档（方向18 同口径） ----
  const double area = std::fabs( grid.pixelWidth * grid.pixelHeight ) *
                      static_cast<double>( std::max( grid.cols, 1 ) ) *
                      static_cast<double>( std::max( grid.rows, 1 ) );
  const double cellSize = std::fabs( grid.pixelWidth ) > 0 ? std::fabs( grid.pixelWidth ) : 1.0;
  const double lag = std::max( cellSize, std::sqrt( area / static_cast<double>( converted.size() ) ) );
  const bool hasAzimuth = parameters.variogramAzimuthDeg >= 0.0 && parameters.variogramAzimuthDeg <= 360.0;
  const bool explicitRatio = parameters.variogramAnisotropyRatio >= 1.0;

  geostat::VariogramModel model;
  model.type = type;
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
    const geostat::ExperimentalVariogram experimentalAcross =
        geostat::experimentalVariogram( converted, lag, kAutoLags, across, barriers );
    const geostat::VariogramFit fitAlong = geostat::fitVariogram( experimentalAlong, type );
    const geostat::VariogramFit fitAcross = geostat::fitVariogram( experimentalAcross, type );
    if ( fitAlong.status != geostat::Status::Ok || fitAcross.status != geostat::Status::Ok )
    {
      result.message = "方向变差函数拟合失败：" + fitAlong.message + " / " + fitAcross.message;
      return result;
    }
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
    result.barrierAware = experimentalAlong.barrierAware || experimentalAcross.barrierAware;
    result.unreachablePairs =
        std::max( experimentalAlong.unreachablePairs, experimentalAcross.unreachablePairs );
  }
  else
  {
    const geostat::ExperimentalVariogram experimental =
        geostat::experimentalVariogram( converted, lag, kAutoLags, {}, barriers );
    const geostat::VariogramFit fit = geostat::fitVariogram( experimental, type );
    if ( fit.status != geostat::Status::Ok )
    {
      result.message = "变差函数拟合失败：" + fit.message;
      return result;
    }
    model = fit.model;
    if ( explicitRatio )
      model.anisotropyRatio = parameters.variogramAnisotropyRatio;
    result.r2 = fit.r2;
    result.rmse = fit.rmse;
    result.usedLags = fit.usedLags;
    result.barrierAware = experimental.barrierAware;
    result.unreachablePairs = experimental.unreachablePairs;
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
  const VariogramResolution resolution =
      resolveVariogram( input.samples, grid, parameters, input.constraints );
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

  // 确保当输入含启用方向线或软边界但参数中尚未解析时，能正确进入克里金插值引擎
  for ( const ConstraintLine &line : input.constraints )
  {
    if ( !line.enabled )
      continue;
    if ( line.semantic == Semantic::DirectionGuide && line.ratio > 1.0 && line.points.size() >= 2 )
    {
      bool found = false;
      for ( const auto &d : kriging.directions )
      {
        if ( d.id == line.stableId )
        {
          found = true;
          break;
        }
      }
      if ( !found )
      {
        ResolvedDirection resolved;
        resolved.id = line.stableId;
        resolved.ratio = line.ratio;
        resolved.influence = line.influenceRadius > 0.0 ? line.influenceRadius : 50.0;
        resolved.core = line.coreRadius > 0.0 ? line.coreRadius : 0.0;
        resolved.points = line.points;
        kriging.directions.push_back( std::move( resolved ) );
      }
    }
    else if ( line.semantic == Semantic::InterpretiveBoundary && line.softStrength > 0.0 && line.points.size() >= 2 )
    {
      bool found = false;
      for ( const auto &s : kriging.soft )
      {
        if ( s.id == line.stableId )
        {
          found = true;
          break;
        }
      }
      if ( !found )
      {
        ResolvedSoft resolved;
        resolved.id = line.stableId;
        resolved.radius = line.softRadius > 0.0 ? line.softRadius : 25.0;
        resolved.strength = std::clamp( line.softStrength, 0.0, 0.8 );
        resolved.points = line.points;
        kriging.soft.push_back( std::move( resolved ) );
      }
    }
  }

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
  if ( resolution.barrierAware )
  {
    result.issues.push_back( "barrierAware:true unreachable_pairs=" +
                             std::to_string( resolution.unreachablePairs ) +
                             "（跨硬隔断样本对不进变差结构拟合）" );
  }
  else if ( !resolution.variogramNote.empty() )
  {
    result.issues.push_back( resolution.variogramNote );
  }
  result.issues.push_back( "kriging_dedupes_coincident_samples（重合井按精确均值合并）" );

  // 约束语义消费回执与不可耦合项诚实记录
  if ( !kriging.directions.empty() )
    result.issues.push_back( "direction_guide_applied:" + std::to_string( kriging.directions.size() ) );
  if ( !kriging.soft.empty() )
    result.issues.push_back( "soft_boundary_applied:" + std::to_string( kriging.soft.size() ) );
  if ( kriging.wellClusterLocality )
    result.issues.push_back( "well_cluster_locality_not_used_by_kriging（克里金方程组已通过样本协方差矩阵内置筛选/去聚类效应，不叠加几何去聚类权重）" );
  return result;
}

} // namespace paleo::singlefactor
