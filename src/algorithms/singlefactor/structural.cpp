// 层：数据
#include "structural.h"

#include "geosutil.h"
#include "localidw.h"

#include <geos_c.h>

#include <QVariantList>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <unordered_set>

namespace paleo::singlefactor
{
namespace
{
// ---- 阻断语义（constraint_semantics.py）----
const std::unordered_set<std::string> kNonBlockingModes = {
    "none", "off", "no_block", "soft", "partial", "display_only", "0", "false" };

// fast_grid.py::rasterize_polygon_mask——向量化射线法，无 on-segment 判边。
// 返回行主序掩膜（行=gridY 升序，列=gridX 升序）。
void rasterizeRingMask( const std::vector<double> &gridX, const std::vector<double> &gridY,
                        const std::vector<Point2> &ring, std::vector<std::uint8_t> *mask )
{
  const std::size_t rows = gridY.size();
  const std::size_t cols = gridX.size();
  mask->assign( rows * cols, 0 );
  if ( ring.size() < 3 || cols == 0 || rows == 0 )
    return;
  std::size_t j = ring.size() - 1;
  for ( std::size_t i = 0; i < ring.size(); ++i )
  {
    const double xi = ring[i].x, yi = ring[i].y;
    const double xj = ring[j].x, yj = ring[j].y;
    const double denom = yj - yi;
    for ( std::size_t r = 0; r < rows; ++r )
    {
      const double py = gridY[r];
      if ( ( yi > py ) == ( yj > py ) )
        continue;
      const double effDenom = std::abs( denom ) > 1e-30 ? denom : 1e-30;
      const double xIntersect = ( xj - xi ) * ( py - yi ) / effDenom + xi;
      for ( std::size_t c = 0; c < cols; ++c )
      {
        if ( gridX[c] < xIntersect )
          ( *mask )[r * cols + c] ^= 1;
      }
    }
    j = i;
  }
}

// fast_grid.py::build_boundary_union_mask——外环减孔洞的并集。
void boundaryUnionMask( const std::vector<double> &gridX, const std::vector<double> &gridY,
                        const std::vector<Polygon> &boundaries,
                        std::vector<std::uint8_t> *mask )
{
  const std::size_t cells = gridY.size() * gridX.size();
  mask->assign( cells, 0 );
  std::vector<std::uint8_t> poly;
  std::vector<std::uint8_t> hole;
  for ( const Polygon &boundary : boundaries )
  {
    if ( boundary.exterior.points.size() < 3 )
      continue;
    rasterizeRingMask( gridX, gridY, boundary.exterior.points, &poly );
    for ( const Ring &h : boundary.holes )
    {
      if ( h.points.size() < 3 )
        continue;
      rasterizeRingMask( gridX, gridY, h.points, &hole );
      for ( std::size_t i = 0; i < cells; ++i )
        poly[i] = static_cast<std::uint8_t>( poly[i] && !hole[i] );
    }
    for ( std::size_t i = 0; i < cells; ++i )
      mask->at( i ) = static_cast<std::uint8_t>( mask->at( i ) || poly[i] );
  }
}

// direction_corridor.py::estimate_mean_well_spacing（n<=400 精确路径；
// n>400 的 numpy PCG64 抽样不可复刻——types.h 同约定，全体均值近似）。
double estimateMeanWellSpacing( const std::vector<Point2> &xy )
{
  const std::size_t n = xy.size();
  if ( n < 2 )
    return 0.0;
  double sum = 0;
  std::size_t count = 0;
  for ( std::size_t i = 0; i < n; ++i )
  {
    double nearest = std::numeric_limits<double>::infinity();
    for ( std::size_t j = 0; j < n; ++j )
    {
      if ( j == i )
        continue;
      nearest = std::min( nearest,
                          std::hypot( xy[j].x - xy[i].x, xy[j].y - xy[i].y ) );
    }
    if ( std::isfinite( nearest ) )
    {
      sum += nearest;
      ++count;
    }
  }
  return count ? sum / static_cast<double>( count ) : 0.0;
}

} // namespace

std::string normalizeBlockMode( const std::string &mode )
{
  // str(block_mode or "").strip().lower()
  std::string text = mode;
  const std::size_t first = text.find_first_not_of( " \t\r\n" );
  if ( first == std::string::npos )
    return std::string();
  text = text.substr( first, text.find_last_not_of( " \t\r\n" ) - first + 1 );
  for ( char &c : text )
  {
    if ( c >= 'A' && c <= 'Z' )
      c = static_cast<char>( c - 'A' + 'a' );
  }
  return text;
}

bool isFullBlockMode( const std::string &mode )
{
  return kNonBlockingModes.count( normalizeBlockMode( mode ) ) == 0;
}

bool isContourStopMode( const std::string &mode )
{
  const std::string normalized = normalizeBlockMode( mode );
  return isFullBlockMode( normalized ) || normalized == "display_only";
}

int resolvePerformanceGridResolution( double mapWidth, double mapHeight, int requested,
                                      int maxCells )
{
  requested = std::max( 40, std::min( 2000, requested ) );
  if ( maxCells <= 0 )
  {
    // 冻结口径：UI 性能档不在算法层取数，钉上游异常分支默认 200000。
    maxCells = 200000;
  }
  const double extent = std::max( mapWidth, std::max( mapHeight, 1.0 ) );
  if ( extent <= 0.0 )
    return requested;
  const double ratio =
      std::max( mapWidth, mapHeight ) / std::max( std::min( mapWidth, mapHeight ), 1e-9 );
  int cap;
  if ( ratio >= 1.5 )
  {
    const int major = static_cast<int>( std::sqrt( maxCells * ratio ) );
    const int minor = std::max( 1, static_cast<int>( maxCells / std::max( major, 1 ) ) );
    cap = std::max( major, minor );
  }
  else
  {
    cap = static_cast<int>( std::sqrt( static_cast<double>( maxCells ) ) );
  }
  return std::max( 40, std::min( requested, cap ) );
}

std::pair<double, bool> resolveBarrierBufferDistance( double requestedDistance,
                                                      double legacyCellBuffer,
                                                      double gridStep,
                                                      double mapWidth,
                                                      double mapHeight,
                                                      double searchRadius,
                                                      bool hasBarriers,
                                                      bool autoEnabled )
{
  if ( requestedDistance > 0.0 )
    return { requestedDistance, false };
  if ( legacyCellBuffer > 0.0 )
    return { legacyCellBuffer, false };
  if ( !hasBarriers || !autoEnabled )
    return { 0.0, false };

  const double mapExtent = std::max( mapWidth, std::max( mapHeight, gridStep ) );
  double adaptive = std::max(
      gridStep * 2.0,
      std::max( searchRadius * 0.03, mapExtent * 0.012 ) );
  adaptive = std::min( adaptive, std::min( mapExtent * 0.04, 400.0 ) );
  return { adaptive, true };
}

StructuralResult buildStructuralSurface( const std::vector<AcquiredWell> &wells,
                                         const std::vector<Polygon> &boundaries,
                                         const std::vector<StructuralBarrier> &barriers,
                                         const std::vector<StructuralDirection> &directions,
                                         const std::vector<Polygon> &interpolationAreas,
                                         const StructuralRequest &request,
                                         const Control &control )
{
  const auto cancel = [&control]() {
    return control.cancelled && control.cancelled();
  };
  const auto fail = []( Status status, const std::string &message ) {
    StructuralResult result;
    result.status = status;
    result.message = message;
    return result;
  };

  GeosContext geos;
  if ( !geos.handle )
    return fail( Status::NumericalFailure, "GEOS 初始化失败" );

  // hard/stops 语义划分（is_full_block_mode / is_contour_stop_mode）
  std::vector<const StructuralBarrier *> hard;
  std::vector<const StructuralBarrier *> stops;
  for ( const StructuralBarrier &barrier : barriers )
  {
    if ( !barrier.active )
      continue;
    if ( isFullBlockMode( barrier.blockMode ) )
      hard.push_back( &barrier );
    if ( isContourStopMode( barrier.blockMode ) )
      stops.push_back( &barrier );
  }

  // 成图域：边界并集（必要时与插值区相交）——上游 regions 恒为 [domain]
  GeomPtr domain = unionPolygons( geos.handle, boundaries );
  if ( !domain || GEOSisValid_r( geos.handle, domain.get() ) != 1 )
    return fail( Status::InvalidInput, "成图边界几何无效，请先修正自交或孔洞" );
  if ( !interpolationAreas.empty() )
  {
    GeomPtr areaUnion = unionPolygons( geos.handle, interpolationAreas );
    domain = makeGeom( geos.handle,
                       GEOSIntersection_r( geos.handle, domain.get(), areaUnion.get() ) );
    if ( !domain )
      return fail( Status::NumericalFailure, "GEOS 插值区相交失败" );
  }
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  if ( !GEOSGeom_getExtent_r( geos.handle, domain.get(), &x0, &y0, &x1, &y1 ) )
    return fail( Status::NumericalFailure, "GEOS 范围读取失败" );
  const double span = std::max( x1 - x0, y1 - y0 );
  const double step = span / std::max( 19, request.resolution - 1 );
  // numpy.linspace 等价：i*delta + start，末元素即终点
  const auto linspace = []( double start, double stop, int count ) {
    std::vector<double> axis( static_cast<std::size_t>( count ) );
    const double delta = ( stop - start ) / static_cast<double>( count - 1 );
    for ( int i = 0; i < count; ++i )
      axis[static_cast<std::size_t>( i )] = start + i * delta;
    return axis;
  };
  // Python round() = half-even → std::nearbyint
  const int nx = std::max(
      2, static_cast<int>( std::nearbyint( ( x1 - x0 ) / step ) ) + 1 );
  const int ny = std::max(
      2, static_cast<int>( std::nearbyint( ( y1 - y0 ) / step ) ) + 1 );
  const std::vector<double> xs = linspace( x0, x1, nx );
  const std::vector<double> ys = linspace( y0, y1, ny );
  const std::size_t cells = xs.size() * ys.size();

  StructuralResult result;
  result.xAxis = xs;
  result.yAxis = ys;
  result.grid.assign( cells, std::numeric_limits<double>::quiet_NaN() );
  result.regionIds.assign( cells, -1 );

  // 有效掩膜 = 边界并集射线掩膜 ∩ 插值区掩膜
  boundaryUnionMask( xs, ys, boundaries, &result.validMask );
  if ( !interpolationAreas.empty() )
  {
    std::vector<std::uint8_t> areaMask;
    boundaryUnionMask( xs, ys, interpolationAreas, &areaMask );
    for ( std::size_t i = 0; i < cells; ++i )
      result.validMask[i] = static_cast<std::uint8_t>( result.validMask[i] && areaMask[i] );
  }

  // _region_ids：GEOS intersectsXY（边界含）∩ 掩膜 → 区域 0
  const GEOSPreparedGeometry *prepared = GEOSPrepare_r( geos.handle, domain.get() );
  if ( !prepared )
    return fail( Status::NumericalFailure, "GEOS 域索引失败" );
  const auto intersectsDomain = [&]( double x, double y ) {
    return GEOSPreparedIntersectsXY_r( geos.handle, prepared, x, y ) == 1;
  };
  for ( std::size_t r = 0; r < ys.size(); ++r )
  {
    for ( std::size_t c = 0; c < xs.size(); ++c )
    {
      const std::size_t idx = r * xs.size() + c;
      if ( result.validMask[idx] && intersectsDomain( xs[c], ys[r] ) )
        result.regionIds[idx] = 0;
    }
  }

  result.wellRegionIds.reserve( wells.size() );
  std::vector<Sample> regionSamples;
  regionSamples.reserve( wells.size() );
  std::vector<Point2> wellXy;
  wellXy.reserve( wells.size() );
  for ( const AcquiredWell &well : wells )
  {
    const int id = intersectsDomain( well.x, well.y ) ? 0 : -1;
    result.wellRegionIds.push_back( id );
    wellXy.push_back( Point2{ well.x, well.y } );
    if ( id == 0 )
    {
      Sample sample;
      sample.wellId = well.wellId;
      sample.x = well.x;
      sample.y = well.y;
      sample.value = well.value;
      regionSamples.push_back( std::move( sample ) );
    }
  }

  const double spacing = std::max( estimateMeanWellSpacing( wellXy ), step );

  // 方向线：解析自动半径后按域裁剪，每片段独立核项（上游 L166-177）
  PreparedInput input;
  input.samples = regionSamples;
  ResolvedParameters params;
  params.autosApplied = true;
  params.power = request.power;
  params.clusterSpan = request.wellClusterLocality ? span : 0.0;
  params.spacing = spacing;
  params.step = step;
  params.span = span;
  params.searchRadius = request.extendTrendToBoundary
                            ? std::optional<double>()
                            : std::optional<double>(
                                  std::max( request.searchRadius, spacing * 2 ) );
  params.minPoints = request.extendTrendToBoundary ? 1 : request.minPoints;
  params.maxPoints = request.extendTrendToBoundary ? 0 : request.maxPoints;
  params.algorithmId = "paleo:paleo_structural_idw";
  result.span = span;
  result.spacing = spacing;
  result.searchRadius = params.searchRadius;
  result.resolvedMinPoints = params.minPoints;
  result.resolvedMaxPoints = params.maxPoints;

  if ( request.enableDirections )
  {
    for ( const StructuralDirection &direction : directions )
    {
      if ( !direction.active || direction.points.size() < 2 )
        continue;
      const double length = polylineLength( direction.points );
      const double radius =
          direction.influenceRadius > 0
              ? direction.influenceRadius
              : std::min( std::max( 2 * spacing, 0.2 * length ), 0.15 * span );
      const double core =
          direction.coreRadius > 0 ? direction.coreRadius : radius * 0.3;
      StructuralDirectionInfo info;
      info.lineId = direction.lineId;
      info.ratio = direction.ratio;
      info.influenceRadius = radius;
      info.coreRadius = core;
      info.active = direction.active;
      GeomPtr line = makeLineString( geos.handle, direction.points );
      GeomPtr clipped;
      if ( line )
        clipped = makeGeom( geos.handle, GEOSIntersection_r( geos.handle, line.get(),
                                                           domain.get() ) );
      if ( clipped )
        collectLineParts( geos.handle, clipped.get(), &info.pieces );
      for ( const std::vector<Point2> &piece : info.pieces )
      {
        if ( polylineLength( piece ) <= 1e-10 )
          continue;
        ResolvedDirection resolved;
        resolved.id = direction.lineId;
        resolved.ratio = direction.ratio;
        resolved.influence = radius;
        resolved.core = core;
        resolved.points = piece;
        params.directions.push_back( std::move( resolved ) );
      }
      result.directions.push_back( std::move( info ) );
    }
  }

  // 停线 → 解释性软边界（仅影响趋势权重，不改写数值域）；
  // Paleo interpretive_boundary 语义线同样入列（radius>0 显式值优先）。
  const double softRadiusAuto = std::max(
      step * 4.0,
      request.barrierShapeRadius > 0
          ? request.barrierShapeRadius
          : std::min( span * 0.08,
                      std::max( span * 0.04, request.barrierBufferDistance * 8.0 ) ) );
  const auto appendSoft = [&]( const std::vector<Point2> &points, const std::string &id,
                              double radius, double strength ) {
    StructuralSoftBoundary boundary;
    boundary.points = points;
    boundary.radius = radius;
    boundary.strength = strength;
    result.interpretiveBoundaries.push_back( boundary );
    ResolvedSoft soft;
    soft.id = id;
    soft.radius = radius;
    soft.strength = strength;
    soft.points = points;
    params.soft.push_back( std::move( soft ) );
  };
  if ( request.enableBarriers && request.interpretiveBoundaryStrength > 0 )
  {
    for ( const StructuralBarrier *stop : stops )
    {
      if ( stop->points.size() < 2 )
        continue;
      appendSoft( stop->points, stop->lineId, softRadiusAuto,
                  request.interpretiveBoundaryStrength );
    }
  }
  for ( const StructuralSoftBoundary &explicitSoft : request.explicitSoftBoundaries )
  {
    if ( explicitSoft.points.size() < 2 )
      continue;
    appendSoft( explicitSoft.points, "interpretive_boundary",
                explicitSoft.radius > 0 ? explicitSoft.radius : softRadiusAuto,
                explicitSoft.strength );
  }

  // 单区域求值：labels==0 的网格节点，样本=well_ids==0 的井
  std::vector<Point2> queries;
  queries.reserve( cells );
  std::vector<std::size_t> queryIndex;
  queryIndex.reserve( cells );
  for ( std::size_t i = 0; i < cells; ++i )
  {
    if ( result.regionIds[i] != 0 )
      continue;
    queryIndex.push_back( i );
    queries.push_back( Point2{ xs[i % xs.size()], ys[i / xs.size()] } );
  }
  std::vector<double> influence( cells, 0.0 );
  if ( !queries.empty() && !regionSamples.empty() && !cancel() )
  {
    QueryResult evaluated = evaluateAt( input, queries, params, control );
    if ( evaluated.status == Status::Ok )
    {
      for ( std::size_t i = 0; i < queries.size(); ++i )
      {
        result.grid[queryIndex[i]] = evaluated.values[i];
        influence[queryIndex[i]] = evaluated.influence[i];
      }
    }
    else if ( evaluated.status == Status::Cancelled )
    {
      return fail( Status::Cancelled, evaluated.message );
    }
  }
  GEOSPreparedGeom_destroy_r( geos.handle, prepared );

  if ( cancel() )
    return fail( Status::Cancelled, "已取消" );

  // extend 模式：有效域内任一目标格非有限 → 上游固定报错文案
  std::size_t unfilled = 0;
  for ( std::size_t i = 0; i < cells; ++i )
  {
    if ( result.validMask[i] && !std::isfinite( result.grid[i] ) )
      ++unfilled;
  }
  if ( request.extendTrendToBoundary && unfilled > 0 )
    return fail( Status::InvalidInput,
                 "有效成图范围内缺少可用井控，请补充数据或调整插值范围。" );

  const auto widthPair = resolveBarrierBufferDistance(
      std::max( 0.0, request.barrierBufferDistance ),
      std::max( 0.0, request.barrierBlankCells ) * step, step, x1 - x0, y1 - y0,
      spacing * 2, !stops.empty(), request.barrierBufferAuto );
  result.barrierBufferDistance = widthPair.first;
  result.barrierBufferAutoApplied = widthPair.second;

  // contour_stop_buffer（workflow.py L1218-1223）：请求值优先，自动=
  // max(缓冲宽, 0.5×最大像元距)
  const double dx = nx > 1 ? xs[1] - xs[0] : 0.0;
  const double dy = ny > 1 ? ys[1] - ys[0] : 0.0;
  result.contourStopBufferDistance =
      request.contourStopBufferDistance >= 0
          ? request.contourStopBufferDistance
          : std::max( result.barrierBufferDistance, 0.5 * std::max( dx, dy ) );

  std::size_t targetCount = 0;
  std::size_t covered = 0;
  for ( std::size_t i = 0; i < cells; ++i )
  {
    if ( !result.validMask[i] )
      continue;
    ++targetCount;
    if ( influence[i] > 1e-6 )
      ++covered;
  }
  const double coverage =
      static_cast<double>( covered ) /
          static_cast<double>( std::max<std::size_t>( 1, targetCount ) ) * 100.0;

  // 图例值域：井 + 有限网格（workflow.py L1114-1126）
  {
    std::vector<double> finite;
    for ( std::size_t i = 0; i < cells; ++i )
    {
      if ( std::isfinite( result.grid[i] ) )
        finite.push_back( result.grid[i] );
    }
    const ResolvedValueRange range =
        resolveValueRangeForWells( wells, request.valueMin, request.valueMax, finite );
    result.valueMin = range.min;
    result.valueMax = range.max;
    if ( result.valueMin && result.valueMax && *result.valueMax <= *result.valueMin )
      result.valueMax = *result.valueMin + 1.0;
    if ( !result.valueMin || !result.valueMax ||
         !std::isfinite( *result.valueMin ) || !std::isfinite( *result.valueMax ) )
    {
      if ( !finite.empty() )
      {
        const auto [lo, hi] = std::minmax_element( finite.begin(), finite.end() );
        result.valueMin = *lo;
        result.valueMax = *hi;
        if ( *result.valueMax <= *result.valueMin )
          result.valueMax = *result.valueMin + 1.0;
      }
      else
      {
        result.valueMin = 0.0;
        result.valueMax = 1.0;
      }
    }
  }

  QVariantMap diagnostics;
  diagnostics.insert( QStringLiteral( "interpolation_model" ),
                      QStringLiteral( "continuous_local_trend_idw" ) );
  diagnostics.insert( QStringLiteral( "barrier_distance_model" ),
                      QStringLiteral( "contour_only" ) );
  diagnostics.insert( QStringLiteral( "direction_model" ),
                      QStringLiteral( "continuous_tangent_kernels" ) );
  diagnostics.insert( QStringLiteral( "region_count" ), 1 );
  diagnostics.insert( QStringLiteral( "partition_complete" ), 0 );
  diagnostics.insert( QStringLiteral( "partition_mode" ), QStringLiteral( "local" ) );
  diagnostics.insert( QStringLiteral( "partition_version" ),
                      QStringLiteral( "2026-09-13-contour-only" ) );
  diagnostics.insert( QStringLiteral( "active_barrier_count" ),
                      static_cast<int>( hard.size() ) );
  diagnostics.insert( QStringLiteral( "active_direction_count" ),
                      static_cast<int>( result.directions.size() ) );
  diagnostics.insert( QStringLiteral( "direction_coverage_percent" ), coverage );
  diagnostics.insert( QStringLiteral( "global_anisotropy_ratio" ), 1.0 );
  diagnostics.insert( QStringLiteral( "global_anisotropy_angle" ), 0.0 );
  diagnostics.insert( QStringLiteral( "coverage_unfilled_cells" ),
                      static_cast<int>( unfilled ) );
  diagnostics.insert( QStringLiteral( "barrier_buffer_distance" ),
                      result.barrierBufferDistance );
  diagnostics.insert( QStringLiteral( "barrier_buffer_auto_applied" ),
                      result.barrierBufferAutoApplied ? 1 : 0 );
  diagnostics.insert( QStringLiteral( "barrier_shape_strength" ),
                      hard.empty() ? 0.0 : request.barrierShapeStrength );
  diagnostics.insert( QStringLiteral( "barrier_shape_radius" ),
                      request.barrierShapeRadius );
  QVariantList resolvedDirs;
  for ( const StructuralDirectionInfo &d : result.directions )
  {
    QVariantMap entry;
    entry.insert( QStringLiteral( "line_id" ), QString::fromStdString( d.lineId ) );
    entry.insert( QStringLiteral( "ratio" ), d.ratio );
    entry.insert( QStringLiteral( "influence_radius" ), d.influenceRadius );
    entry.insert( QStringLiteral( "core_radius" ), d.coreRadius );
    resolvedDirs.append( entry );
  }
  diagnostics.insert( QStringLiteral( "resolved_directions" ), resolvedDirs );
  diagnostics.insert( QStringLiteral( "方向线覆盖百分比" ), coverage );
  diagnostics.insert( QStringLiteral( "分割区域数" ), 1 );
  diagnostics.insert( QStringLiteral( "有效方向线数" ),
                      static_cast<int>( result.directions.size() ) );
  diagnostics.insert( QStringLiteral( "有效打断线数" ),
                      static_cast<int>( hard.size() ) );
  result.diagnostics = diagnostics;

  result.status = Status::Ok;
  return result;
}

} // namespace paleo::singlefactor
