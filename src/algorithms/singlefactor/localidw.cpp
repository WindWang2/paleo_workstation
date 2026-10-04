// 层：数据
#include "localidw.h"

#include "../geostat/kriging.h"

#include "curvekernel.h"
#include "partition.h"
#include "support.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <utility>

// 层：数据
namespace paleo::singlefactor
{
namespace
{

constexpr int kBatch = 512;

struct DirTerm
{
  CurveKernel kernel;
  double reduction = 0;
  std::vector<double> wellGate;
  std::vector<std::array<double, 3>> wellTensor;
  bool anyWellGate = false;
};

struct SoftTerm
{
  CurveKernel kernel;
  double strength = 0;
  double radius = 1;
  double length = 0;
  std::vector<Point2> points;
  std::vector<double> station;
  std::vector<double> wellSides;
};

struct Engine
{
  std::vector<Sample> wells;
  std::vector<DirTerm> directions;
  std::vector<SoftTerm> soft;
  ClusterModel clusters;
  ResolvedParameters params;
  // 方向41：methodActual=="kriging" 时为非空（分量样本预建邻域索引）。
  // 空 = 纯 IDW 权重路径；病态格回落时仍用同一 IDW 权重公式。
  std::unique_ptr<geostat::KrigingSolver> solver;
};

// 变差函数参数 → geostat 模型（各向异性 ratio<1 或不设方位即各向同性）。
geostat::VariogramModel variogramModelOf( const ResolvedParameters &params )
{
  geostat::VariogramModel model;
  if ( params.variogramModel == "exponential" )
    model.type = geostat::VariogramModelType::Exponential;
  else if ( params.variogramModel == "gaussian" )
    model.type = geostat::VariogramModelType::Gaussian;
  else
    model.type = geostat::VariogramModelType::Spherical;
  model.nugget = params.nugget;
  model.sill = params.sill;
  model.range = params.range;
  model.anisotropyRatio = params.variogramAnisotropyRatio >= 1.0 ? params.variogramAnisotropyRatio : 1.0;
  model.azimuthDeg = params.variogramAzimuthDeg >= 0.0 ? params.variogramAzimuthDeg : 0.0;
  return model;
}

bool badSamples( const std::vector<Sample> &samples, std::string *message )
{
  for ( const Sample &sample : samples )
  {
    if ( !std::isfinite( sample.x ) || !std::isfinite( sample.y ) || !std::isfinite( sample.value ) )
    {
      if ( message )
        *message = "样本含非有限坐标或数值";
      return true;
    }
  }
  return false;
}

Point2 interpolateLine( const SoftTerm &soft, double station )
{
  const double s = std::clamp( station, 0.0, soft.length );
  if ( soft.points.empty() )
    return {};
  if ( soft.points.size() == 1 || !( soft.length > 0.0 ) )
    return soft.points.front();
  for ( std::size_t i = 1; i < soft.points.size(); ++i )
  {
    if ( soft.station[i] + 1e-15 < s && i + 1 < soft.points.size() )
      continue;
    const double seg = soft.station[i] - soft.station[i - 1];
    if ( !( seg > 1e-12 ) )
      return soft.points[i];
    const double t = std::clamp( ( s - soft.station[i - 1] ) / seg, 0.0, 1.0 );
    return Point2{ soft.points[i - 1].x + t * ( soft.points[i].x - soft.points[i - 1].x ),
                   soft.points[i - 1].y + t * ( soft.points[i].y - soft.points[i - 1].y ) };
  }
  return soft.points.back();
}

double sideOf( const SoftTerm &soft, Point2 point )
{
  double best = std::numeric_limits<double>::infinity();
  double station = 0;
  for ( std::size_t i = 1; i < soft.points.size(); ++i )
  {
    const Point2 a = soft.points[i - 1];
    const Point2 b = soft.points[i];
    const double vx = b.x - a.x;
    const double vy = b.y - a.y;
    const double len2 = vx * vx + vy * vy;
    if ( !( len2 > 1e-24 ) )
      continue;
    const double len = std::sqrt( len2 );
    const double t = std::clamp( ( ( point.x - a.x ) * vx + ( point.y - a.y ) * vy ) / len2, 0.0, 1.0 );
    const double dist = std::hypot( point.x - ( a.x + t * vx ), point.y - ( a.y + t * vy ) );
    if ( dist < best )
    {
      best = dist;
      station = soft.station[i - 1] + t * len;
    }
  }
  const Point2 foot = interpolateLine( soft, station );
  const double h = std::max( soft.radius * 0.1, 1e-9 );
  const Point2 a = interpolateLine( soft, std::max( 0.0, station - h ) );
  const Point2 b = interpolateLine( soft, std::min( soft.length, station + h ) );
  const double norm = std::max( std::hypot( b.x - a.x, b.y - a.y ), 1e-12 );
  const double tx = ( b.x - a.x ) / norm;
  const double ty = ( b.y - a.y ) / norm;
  const double signedCross = tx * ( point.y - foot.y ) - ty * ( point.x - foot.x );
  return std::tanh( signedCross / std::max( soft.radius * 0.25, 1e-9 ) );
}

Engine makeEngine( const std::vector<Sample> &wells, const ResolvedParameters &params )
{
  Engine engine;
  engine.wells = wells;
  engine.params = params;
  std::vector<Point2> wellXy;
  wellXy.reserve( wells.size() );
  for ( const Sample &sample : wells )
    wellXy.push_back( Point2{ sample.x, sample.y } );
  for ( const ResolvedDirection &direction : params.directions )
  {
    if ( !( direction.ratio > 1.0 ) || !( direction.influence > 0.0 ) || direction.points.size() < 2 )
      continue;
    const double core = std::min( std::max( direction.core, 0.0 ), direction.influence * 0.95 );
    DirTerm term{ CurveKernel( direction.points, direction.influence, core ),
                  1.0 - std::pow( direction.ratio, -2.0 ), {}, {}, false };
    term.kernel.evaluateInto( wellXy.data(), static_cast<int>( wellXy.size() ), term.wellGate, term.wellTensor );
    term.anyWellGate = std::any_of( term.wellGate.begin(), term.wellGate.end(), []( double g ) { return g > 0.0; } );
    engine.directions.push_back( std::move( term ) );
  }
  for ( const ResolvedSoft &boundary : params.soft )
  {
    if ( !( boundary.strength > 0.0 ) || !( boundary.radius > 0.0 ) || boundary.points.size() < 2 )
      continue;
    SoftTerm term{ CurveKernel( boundary.points, boundary.radius, boundary.radius * 0.25 ),
                   std::clamp( boundary.strength, 0.0, 0.8 ),
                   boundary.radius,
                   0,
                   boundary.points,
                   {},
                   {} };
    term.station.resize( term.points.size(), 0 );
    for ( std::size_t i = 1; i < term.points.size(); ++i )
    {
      const double seg = std::hypot( term.points[i].x - term.points[i - 1].x,
                                      term.points[i].y - term.points[i - 1].y );
      term.station[i] = term.station[i - 1] + ( seg > 1e-12 ? seg : 0 );
    }
    term.length = term.station.empty() ? 0 : term.station.back();
    term.wellSides.reserve( wellXy.size() );
    for ( const Point2 &xy : wellXy )
      term.wellSides.push_back( sideOf( term, xy ) );
    engine.soft.push_back( std::move( term ) );
  }
  engine.clusters = buildClusters( wells, params.clusterSpan );
  if ( params.methodActual == "kriging" && !wells.empty() )
  {
    std::vector<geostat::Sample> geostatSamples;
    geostatSamples.reserve( wells.size() );
    for ( const Sample &sample : wells )
      geostatSamples.push_back( geostat::Sample{ sample.x, sample.y, sample.value } );
    geostat::KrigingParams krigingParams;
    krigingParams.maxPoints = params.krigingMaxPoints;
    krigingParams.minPoints = params.krigingMinPoints;
    if ( params.searchRadius )
      krigingParams.searchRadius = *params.searchRadius;
    auto solver = std::make_unique<geostat::KrigingSolver>( geostatSamples, variogramModelOf( params ),
                                                            krigingParams );
    if ( solver->valid() )
      engine.solver = std::move( solver );
  }
  return engine;
}

void evaluateBatch( const Engine &engine, const std::vector<Point2> &queries, std::vector<double> &values,
                    std::vector<double> &influence, int *krigingCells = nullptr,
                    int *idwFallbackCells = nullptr )
{
  const int nQuery = static_cast<int>( queries.size() );
  const int nWell = static_cast<int>( engine.wells.size() );
  values.assign( static_cast<std::size_t>( nQuery ), std::numeric_limits<double>::quiet_NaN() );
  influence.assign( static_cast<std::size_t>( nQuery ), 0 );
  if ( nQuery == 0 || nWell == 0 )
    return;
  std::vector<std::vector<double>> dirGate( engine.directions.size() );
  std::vector<std::vector<std::array<double, 3>>> dirTensor( engine.directions.size() );
  for ( std::size_t d = 0; d < engine.directions.size(); ++d )
  {
    engine.directions[d].kernel.evaluateInto( queries.data(), nQuery, dirGate[d], dirTensor[d] );
  }
  std::vector<std::vector<double>> softGate( engine.soft.size() );
  std::vector<std::vector<double>> softSide( engine.soft.size() );
  for ( std::size_t s = 0; s < engine.soft.size(); ++s )
  {
    std::vector<std::array<double, 3>> unused;
    engine.soft[s].kernel.evaluateInto( queries.data(), nQuery, softGate[s], unused );
    softSide[s].resize( static_cast<std::size_t>( nQuery ) );
    for ( int q = 0; q < nQuery; ++q )
      softSide[s][static_cast<std::size_t>( q )] = sideOf( engine.soft[s], queries[static_cast<std::size_t>( q )] );
  }
  const std::vector<double> clusterW = engine.clusters.weights( queries, nWell );
  const double exponent = -engine.params.power / 2.0;
  const bool inverseSquare = engine.params.power == 2.0;
  const int minPoints = engine.params.minPoints;
  const int maxPoints = engine.params.maxPoints;
  const bool useSearch = engine.params.searchRadius.has_value();
  const double search2 = useSearch ? ( *engine.params.searchRadius ) * ( *engine.params.searchRadius ) : 0;
  std::vector<double> distance2( static_cast<std::size_t>( nWell ) );
  std::vector<double> weights( static_cast<std::size_t>( nWell ) );
  for ( int q = 0; q < nQuery; ++q )
  {
    const Point2 query = queries[static_cast<std::size_t>( q )];
    bool krigingUnavailable = false;
    if ( engine.solver )
    {
      const geostat::KrigingPointResult point = engine.solver->solveAt( query.x, query.y );
      if ( point.ok )
      {
        values[static_cast<std::size_t>( q )] = point.estimate;
        if ( krigingCells )
          ++( *krigingCells );
        continue;
      }
      // 方程奇异/病态（或半径闸不足）：落到下面的 IDW 权重。只有在确实给出
      // 有限 IDW 值时才算一次「回落」，两种都无值的格保持 NaN 不计数。
      krigingUnavailable = true;
    }
    double nearest = std::numeric_limits<double>::infinity();
    for ( int w = 0; w < nWell; ++w )
    {
      const double dx = query.x - engine.wells[static_cast<std::size_t>( w )].x;
      const double dy = query.y - engine.wells[static_cast<std::size_t>( w )].y;
      const double euclidean2 = dx * dx + dy * dy;
      double adjustment = 0;
      double total = 0;
      for ( std::size_t d = 0; d < engine.directions.size(); ++d )
      {
        const double activation = dirGate[d][static_cast<std::size_t>( q )] *
                                  engine.directions[d].wellGate[static_cast<std::size_t>( w )];
        const std::array<double, 3> tensor{
            0.5 * ( dirTensor[d][static_cast<std::size_t>( q )][0] +
                    engine.directions[d].wellTensor[static_cast<std::size_t>( w )][0] ),
            0.5 * ( dirTensor[d][static_cast<std::size_t>( q )][1] +
                    engine.directions[d].wellTensor[static_cast<std::size_t>( w )][1] ),
            0.5 * ( dirTensor[d][static_cast<std::size_t>( q )][2] +
                    engine.directions[d].wellTensor[static_cast<std::size_t>( w )][2] ) };
        adjustment += activation * engine.directions[d].reduction * tangentEnergy( dx, dy, tensor );
        total += activation;
        if ( engine.directions[d].anyWellGate )
          influence[static_cast<std::size_t>( q )] =
              std::max( influence[static_cast<std::size_t>( q )], dirGate[d][static_cast<std::size_t>( q )] );
      }
      distance2[static_cast<std::size_t>( w )] = std::max( euclidean2 - adjustment / std::max( total, 1.0 ), 0.0 );
      nearest = std::min( nearest, distance2[static_cast<std::size_t>( w )] );
    }
    if ( !std::isfinite( nearest ) )
      nearest = 1;
    nearest = std::max( nearest, 1e-20 );
    for ( int w = 0; w < nWell; ++w )
    {
      const double base = std::max( distance2[static_cast<std::size_t>( w )], 1e-20 ) / nearest;
      // distance2 已平方：默认 power=2 的指数为 -1，倒数与通用 pow 等价。
      // 保留相同 base 归一化/舍入顺序，其他（含非整数）幂仍走通用路径。
      weights[static_cast<std::size_t>( w )] = inverseSquare ? 1.0 / base : std::pow( base, exponent );
    }
    if ( !clusterW.empty() )
    {
      for ( int w = 0; w < nWell; ++w )
        weights[static_cast<std::size_t>( w )] *= clusterW[static_cast<std::size_t>( q ) * static_cast<std::size_t>( nWell ) +
                                                            static_cast<std::size_t>( w )];
    }
    if ( !engine.soft.empty() )
    {
      for ( int w = 0; w < nWell; ++w )
      {
        double penalty = 0;
        for ( std::size_t s = 0; s < engine.soft.size(); ++s )
        {
          const double opposite =
              ( 1.0 - softSide[s][static_cast<std::size_t>( q )] * engine.soft[s].wellSides[static_cast<std::size_t>( w )] ) *
              0.5;
          penalty = std::max( penalty, engine.soft[s].strength * softGate[s][static_cast<std::size_t>( q )] * opposite );
        }
        weights[static_cast<std::size_t>( w )] *= 1.0 - penalty;
      }
    }
    if ( useSearch && search2 > 0.0 )
    {
      for ( int w = 0; w < nWell; ++w )
      {
        const double dx = query.x - engine.wells[static_cast<std::size_t>( w )].x;
        const double dy = query.y - engine.wells[static_cast<std::size_t>( w )].y;
        const double euclidean2 = dx * dx + dy * dy;
        const double taper = std::max( 1.0 - euclidean2 / search2, 0.0 );
        weights[static_cast<std::size_t>( w )] *= taper * taper;
      }
    }
    if ( maxPoints > 0 && maxPoints < nWell )
    {
      std::vector<double> order = distance2;
      std::nth_element( order.begin(), order.begin() + maxPoints, order.end() );
      const double limit = std::max( order[static_cast<std::size_t>( maxPoints )], 1e-20 );
      for ( int w = 0; w < nWell; ++w )
      {
        const double taper = std::max( 1.0 - distance2[static_cast<std::size_t>( w )] / limit, 0.0 );
        weights[static_cast<std::size_t>( w )] *= taper * taper;
      }
    }
    double sum = 0;
    double valueSum = 0;
    int positive = 0;
    for ( int w = 0; w < nWell; ++w )
    {
      const double weight = weights[static_cast<std::size_t>( w )];
      if ( !( weight > 0.0 ) || !std::isfinite( weight ) )
        continue;
      ++positive;
      sum += weight;
      valueSum += weight * engine.wells[static_cast<std::size_t>( w )].value;
    }
    double value = std::numeric_limits<double>::quiet_NaN();
    if ( positive >= minPoints && sum != 0.0 && std::isfinite( sum ) && std::isfinite( valueSum ) )
      value = valueSum / std::max( sum, 1e-30 );
    double exactSum = 0;
    int exactCount = 0;
    for ( int w = 0; w < nWell; ++w )
    {
      const double dx = query.x - engine.wells[static_cast<std::size_t>( w )].x;
      const double dy = query.y - engine.wells[static_cast<std::size_t>( w )].y;
      if ( dx * dx + dy * dy <= 1e-20 )
      {
        exactSum += engine.wells[static_cast<std::size_t>( w )].value;
        ++exactCount;
      }
    }
    if ( exactCount > 0 )
      value = exactSum / static_cast<double>( exactCount );
    if ( krigingUnavailable && idwFallbackCells && std::isfinite( value ) )
      ++( *idwFallbackCells );
    values[static_cast<std::size_t>( q )] = value;
  }
}

void appendIssues( const PreparedInput &input, std::vector<std::string> &issues )
{
  issues.insert( issues.end(), input.ignored.begin(), input.ignored.end() );
  for ( const ConstraintLine &line : input.constraints )
  {
    if ( !line.enabled )
      issues.push_back( line.stableId + " disabled" );
    else if ( line.semantic == Semantic::ContourStop )
      issues.push_back( line.stableId + " contour_stop_does_not_change_analysis" );
    else if ( line.semantic == Semantic::CartographicDetour )
      issues.push_back( line.stableId + " cartographic_detour_not_analysis_constraint" );
    else if ( line.semantic == Semantic::Ignored )
      issues.push_back( line.stableId + " ignored" );
  }
}

bool stopped( const Control &control )
{
  return control.cancelled && control.cancelled();
}

void report( const Control &control, double value )
{
  if ( control.progress )
    control.progress( std::clamp( value, 0.0, 1.0 ) );
}

} // namespace

QueryResult evaluateAt( const PreparedInput &input, std::span<const Point2> queryPoints,
                        const ResolvedParameters &parameters, const Control &control )
{
  QueryResult result;
  result.resolved = parameters;
  appendIssues( input, result.issues );
  if ( stopped( control ) )
  {
    result.status = Status::Cancelled;
    result.message = "已取消";
    return result;
  }
  if ( !parameters.autosApplied )
  {
    result.status = Status::InvalidInput;
    result.message = "参数尚未解析";
    return result;
  }
  if ( !( parameters.power > 0.0 ) || !std::isfinite( parameters.power ) )
  {
    result.status = Status::InvalidInput;
    result.message = "power 必须为有限正数";
    return result;
  }
  std::string sampleError;
  if ( badSamples( input.samples, &sampleError ) )
  {
    result.status = Status::InvalidInput;
    result.message = sampleError;
    return result;
  }
  if ( input.samples.empty() )
  {
    result.status = Status::InvalidInput;
    result.message = "没有可用井点";
    return result;
  }
  for ( const Point2 &query : queryPoints )
  {
    if ( !std::isfinite( query.x ) || !std::isfinite( query.y ) )
    {
      result.status = Status::InvalidInput;
      result.message = "查询点含非有限坐标";
      return result;
    }
  }
  const Engine engine = makeEngine( input.samples, parameters );
  result.values.assign( queryPoints.size(), std::numeric_limits<double>::quiet_NaN() );
  result.influence.assign( queryPoints.size(), 0 );
  const int total = static_cast<int>( queryPoints.size() );
  int done = 0;
  for ( std::size_t start = 0; start < queryPoints.size(); start += static_cast<std::size_t>( kBatch ) )
  {
    if ( stopped( control ) )
    {
      result.status = Status::Cancelled;
      result.message = "已取消";
      result.values.clear();
      result.influence.clear();
      return result;
    }
    const std::size_t count = std::min( static_cast<std::size_t>( kBatch ), queryPoints.size() - start );
    std::vector<Point2> batch( queryPoints.begin() + static_cast<std::ptrdiff_t>( start ),
                               queryPoints.begin() + static_cast<std::ptrdiff_t>( start + count ) );
    std::vector<double> values;
    std::vector<double> influence;
    // 点查询面也给克里金回落计数：QueryResult 没有专门字段，落进 issues 如实说明
    //（否则带 kriging 标签的调用方拿到「部分克里金 + 部分 IDW」的混合值而无标记）。
    int krigingCells = 0;
    int idwFallbackCells = 0;
    evaluateBatch( engine, batch, values, influence, &krigingCells, &idwFallbackCells );
    result.krigingCells += krigingCells;
    result.idwFallbackCells += idwFallbackCells;
    for ( std::size_t i = 0; i < count; ++i )
    {
      result.values[start + i] = values[i];
      result.influence[start + i] = influence[i];
    }
    done += static_cast<int>( count );
    report( control, total ? static_cast<double>( done ) / static_cast<double>( total ) : 1 );
  }
  if ( result.idwFallbackCells > 0 )
  {
    result.issues.push_back( "kriging_point_fallback " + std::to_string( result.idwFallbackCells ) +
                             "（这些查询点没解出克里金值，用同参数 IDW 权重给出）" );
  }
  result.status = Status::Ok;
  return result;
}

SurfaceResult evaluateLocalIdw( const PreparedInput &input, const GridSpec &grid,
                                const ResolvedParameters &parameters, const Control &control )
{
  SurfaceResult result;
  result.resolved = parameters;
  appendIssues( input, result.issues );
  if ( stopped( control ) )
  {
    result.status = Status::Cancelled;
    result.message = "已取消";
    return result;
  }
  if ( !parameters.autosApplied )
  {
    result.status = Status::InvalidInput;
    result.message = "参数尚未解析";
    return result;
  }
  if ( input.domain.empty() )
  {
    result.status = Status::InvalidInput;
    result.message = "成图域为空";
    return result;
  }
  std::string budgetError;
  if ( !gridBudgetOk( grid, static_cast<int>( input.samples.size() ), &budgetError ) )
  {
    result.status = Status::BudgetExceeded;
    result.message = budgetError;
    return result;
  }
  std::string sampleError;
  if ( badSamples( input.samples, &sampleError ) )
  {
    result.status = Status::InvalidInput;
    result.message = sampleError;
    return result;
  }
  if ( input.samples.empty() )
  {
    result.status = Status::InvalidInput;
    result.message = "没有可用井点";
    return result;
  }
  const std::size_t cells = static_cast<std::size_t>( grid.cols ) * static_cast<std::size_t>( grid.rows );
  result.values.assign( cells, std::numeric_limits<double>::quiet_NaN() );
  result.marks.assign( cells, 0 );
  result.components.assign( cells, -1 );
  bool useBarriers = false;
  for ( const ConstraintLine &line : input.constraints )
  {
    if ( line.enabled && line.semantic == Semantic::HardBarrier && line.points.size() >= 2 )
      useBarriers = true;
  }
  BarrierGrid barriers;
  if ( useBarriers )
  {
    // 屏障模型分派：grid_connectivity_v1（默认，有限端可绕行）或
    // interpretation_partition_v1（自由端延界 + node-safe 洪泛，两侧不共享井）。
    barriers = parameters.hardBarrierModel == "interpretation_partition_v1"
                   ? labelInterpretationPartition( grid, input.constraints, input.samples,
                                                   parameters.tolerance, &control )
                   : labelHardBarriers( grid, input.constraints, input.samples,
                                        parameters.tolerance, &control );
    if ( barriers.barrierCells < 0 )
    {
      result.status = Status::Cancelled;
      result.message = "已取消";
      result.values.clear();
      result.marks.clear();
      result.components.clear();
      return result;
    }
    if ( !barriers.ambiguous.empty() )
    {
      result.status = Status::InvalidInput;
      result.message = "硬屏障上的井点归属不明确";
      result.ambiguous = barriers.ambiguous;
      result.values.clear();
      result.marks.clear();
      result.components.clear();
      return result;
    }
    result.components = barriers.component;
  }
  report( control, 0.05 );
  const int componentCount = useBarriers ? std::max( barriers.componentCount, 0 ) : 1;
  std::vector<std::vector<int>> wellsOf( static_cast<std::size_t>( componentCount ) );
  if ( !useBarriers )
  {
    wellsOf[0].resize( input.samples.size() );
    for ( int i = 0; i < static_cast<int>( input.samples.size() ); ++i )
      wellsOf[0][static_cast<std::size_t>( i )] = i;
  }
  else
  {
    for ( int i = 0; i < static_cast<int>( barriers.sampleComponent.size() ); ++i )
    {
      const int comp = barriers.sampleComponent[static_cast<std::size_t>( i )];
      if ( comp >= 0 && comp < componentCount )
        wellsOf[static_cast<std::size_t>( comp )].push_back( i );
    }
  }
  std::vector<Engine> engines;
  engines.reserve( static_cast<std::size_t>( componentCount ) );
  for ( int comp = 0; comp < componentCount; ++comp )
  {
    std::vector<Sample> subset;
    subset.reserve( wellsOf[static_cast<std::size_t>( comp )].size() );
    for ( int index : wellsOf[static_cast<std::size_t>( comp )] )
      subset.push_back( input.samples[static_cast<std::size_t>( index )] );
    engines.push_back( makeEngine( subset, parameters ) );
  }
  std::vector<std::vector<int>> cellsOf( static_cast<std::size_t>( componentCount ) );
  std::vector<int> regionCells( static_cast<std::size_t>( std::max( componentCount, 1 ) ), 0 );
  for ( int row = 0; row < grid.rows; ++row )
  {
    if ( stopped( control ) )
    {
      result.status = Status::Cancelled;
      result.message = "已取消";
      result.values.clear();
      result.marks.clear();
      result.components.clear();
      return result;
    }
    for ( int column = 0; column < grid.cols; ++column )
    {
      const std::size_t index = static_cast<std::size_t>( row ) * static_cast<std::size_t>( grid.cols ) +
                                static_cast<std::size_t>( column );
      const Point2 center = cellCenter( grid, column, row );
      if ( !pointInDomain( input.domain, center, parameters.tolerance ) )
      {
        result.components[index] = -1;
        continue;
      }
      if ( useBarriers && barriers.component[index] == -2 )
      {
        result.marks[index] = 3;
        continue;
      }
      const int comp = useBarriers ? barriers.component[index] : 0;
      if ( comp < 0 )
        continue;
      cellsOf[static_cast<std::size_t>( comp )].push_back( static_cast<int>( index ) );
      ++regionCells[static_cast<std::size_t>( comp )];
    }
  }
  const double cellArea = std::abs( grid.pixelWidth * grid.pixelHeight );
  for ( int comp = 0; comp < componentCount; ++comp )
  {
    if ( wellsOf[static_cast<std::size_t>( comp )].empty() && regionCells[static_cast<std::size_t>( comp )] > 0 )
    {
      UnsupportedRegion region;
      region.id = comp;
      region.area = cellArea * static_cast<double>( regionCells[static_cast<std::size_t>( comp )] );
      region.reason = "硬隔断无井闭合区";
      result.unsupported.push_back( region );
    }
  }
  int filledBatches = 0;
  int totalBatches = 0;
  for ( const std::vector<int> &indices : cellsOf )
    totalBatches += static_cast<int>( ( indices.size() + static_cast<std::size_t>( kBatch ) - 1 ) / static_cast<std::size_t>( kBatch ) );
  const double supportR = parameters.supportedRadius;
  const int supportMin = std::max( parameters.supportedMinPoints, 1 );
  const bool extrapolation = parameters.coverage == CoverageMode::DomainExtrapolation;
  for ( int comp = 0; comp < componentCount; ++comp )
  {
    const std::vector<int> &indices = cellsOf[static_cast<std::size_t>( comp )];
    const Engine &engine = engines[static_cast<std::size_t>( comp )];
    for ( std::size_t start = 0; start < indices.size(); start += static_cast<std::size_t>( kBatch ) )
    {
      if ( stopped( control ) )
      {
        result.status = Status::Cancelled;
        result.message = "已取消";
        result.values.clear();
        result.marks.clear();
        result.components.clear();
        return result;
      }
      const std::size_t count = std::min( static_cast<std::size_t>( kBatch ), indices.size() - start );
      std::vector<Point2> batch;
      batch.reserve( count );
      for ( std::size_t i = 0; i < count; ++i )
      {
        const int index = indices[start + i];
        const int column = index % grid.cols;
        const int row = index / grid.cols;
        batch.push_back( cellCenter( grid, column, row ) );
      }
      std::vector<double> values;
      std::vector<double> influence;
      evaluateBatch( engine, batch, values, influence, &result.krigingCells, &result.idwFallbackCells );
      for ( std::size_t i = 0; i < count; ++i )
      {
        const std::size_t index = static_cast<std::size_t>( indices[start + i] );
        const double value = values[i];
        result.values[index] = value;
        if ( !std::isfinite( value ) )
        {
          result.marks[index] = 0;
          continue;
        }
        if ( !extrapolation )
        {
          result.marks[index] = 1;
          continue;
        }
        int near = 0;
        bool exact = false;
        for ( const Sample &sample : engine.wells )
        {
          const double dx = batch[i].x - sample.x;
          const double dy = batch[i].y - sample.y;
          const double d2 = dx * dx + dy * dy;
          if ( d2 <= 1e-20 )
            exact = true;
          if ( supportR > 0.0 && d2 < supportR * supportR )
            ++near;
        }
        result.marks[index] = ( exact || near >= supportMin ) ? 1 : 2;
      }
      ++filledBatches;
      const double frac = totalBatches ? static_cast<double>( filledBatches ) / static_cast<double>( totalBatches ) : 1;
      report( control, 0.05 + 0.95 * frac );
    }
  }
  for ( std::uint8_t mark : result.marks )
  {
    if ( mark == 0 )
      ++result.nodataCells;
    else if ( mark == 1 )
      ++result.finiteCells;
    else if ( mark == 2 )
    {
      ++result.finiteCells;
      ++result.extrapolatedCells;
    }
    else if ( mark == 3 )
      ++result.barrierCells;
  }
  if ( result.unsupported.empty() )
    report( control, 1 );
  if ( !result.unsupported.empty() && parameters.requireFullCoverage )
  {
    result.status = Status::InvalidInput;
    result.message = "存在无井硬隔断区，已按 requireFullCoverage 拒绝";
    return result;
  }
  // 外推模式不得用全局均值填无井硬区：无样本连通区保持 NaN。
  for ( const UnsupportedRegion &region : result.unsupported )
  {
    for ( std::size_t index = 0; index < cells; ++index )
    {
      if ( result.components[index] == region.id && std::isfinite( result.values[index] ) )
      {
        result.status = Status::NumericalFailure;
        result.message = "无井硬隔断区出现了数值";
        return result;
      }
    }
  }
  result.status = Status::Ok;
  if ( totalBatches == 0 )
    report( control, 1 );
  return result;
}

} // namespace paleo::singlefactor
