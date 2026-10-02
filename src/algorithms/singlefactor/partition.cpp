// 层：数据
#include "partition.h"

#include "support.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>

// 层：数据
namespace paleo::singlefactor
{
namespace
{

double cross( double ax, double ay, double bx, double by )
{
  return ax * by - ay * bx;
}

bool pointOnSegment( Point2 point, Point2 a, Point2 b, double tol )
{
  const double dx = b.x - a.x;
  const double dy = b.y - a.y;
  const double len2 = dx * dx + dy * dy;
  double dist = 0;
  if ( len2 < 1e-24 )
  {
    dist = std::hypot( point.x - a.x, point.y - a.y );
  }
  else
  {
    const double t = std::clamp( ( ( point.x - a.x ) * dx + ( point.y - a.y ) * dy ) / len2, 0.0, 1.0 );
    dist = std::hypot( point.x - ( a.x + t * dx ), point.y - ( a.y + t * dy ) );
  }
  return dist <= tol;
}

double distToSegment( Point2 point, Point2 a, Point2 b )
{
  const double dx = b.x - a.x;
  const double dy = b.y - a.y;
  const double len2 = dx * dx + dy * dy;
  if ( len2 < 1e-24 )
    return std::hypot( point.x - a.x, point.y - a.y );
  const double t = std::clamp( ( ( point.x - a.x ) * dx + ( point.y - a.y ) * dy ) / len2, 0.0, 1.0 );
  return std::hypot( point.x - ( a.x + t * dx ), point.y - ( a.y + t * dy ) );
}

// 域边界环集合：外环 + 洞环（>=2 点）。
std::vector<const std::vector<Point2> *> boundaryRings( const std::vector<Polygon> &boundaries )
{
  std::vector<const std::vector<Point2> *> rings;
  for ( const Polygon &boundary : boundaries )
  {
    if ( boundary.exterior.points.size() >= 2 )
      rings.push_back( &boundary.exterior.points );
    for ( const Ring &hole : boundary.holes )
      if ( hole.points.size() >= 2 )
        rings.push_back( &hole.points );
  }
  return rings;
}

double pointToRingDistance( Point2 point, const std::vector<Point2> &ring )
{
  double best = std::numeric_limits<double>::infinity();
  for ( std::size_t i = 1; i < ring.size(); ++i )
    best = std::min( best, distToSegment( point, ring[i - 1], ring[i] ) );
  if ( ring.size() == 1 )
    best = std::hypot( point.x - ring.front().x, point.y - ring.front().y );
  return best;
}

bool endpointOnDomainBoundary( Point2 point, const std::vector<Polygon> &boundaries, double snapTol )
{
  for ( const auto *ring : boundaryRings( boundaries ) )
  {
    if ( pointToRingDistance( point, *ring ) <= snapTol )
      return true;
  }
  return false;
}

bool unitVector( double dx, double dy, double &ux, double &uy )
{
  const double length = std::hypot( dx, dy );
  if ( length <= 1e-12 )
    return false;
  ux = dx / length;
  uy = dy / length;
  return true;
}

std::optional<Point2> rayHitDomainBoundary( Point2 origin, Point2 direction,
    const std::vector<Polygon> &boundaries, double maxDist )
{
  const Point2 far{ origin.x + direction.x * maxDist, origin.y + direction.y * maxDist };
  double bestT = std::numeric_limits<double>::infinity();
  std::optional<Point2> best;
  const double fx = far.x - origin.x;
  const double fy = far.y - origin.y;
  const double span2 = fx * fx + fy * fy;
  if ( span2 <= 1e-24 )
    return std::nullopt;
  for ( const auto *ring : boundaryRings( boundaries ) )
  {
    for ( std::size_t i = 1; i < ring->size(); ++i )
    {
      const std::optional<Point2> hit =
          segmentIntersectionClosed( origin, far, ( *ring )[i - 1], ( *ring )[i] );
      if ( !hit )
        continue;
      const double t = ( ( hit->x - origin.x ) * fx + ( hit->y - origin.y ) * fy ) / span2;
      if ( t <= 1e-6 )
        continue;
      if ( t < bestT )
      {
        bestT = t;
        best = hit;
      }
    }
  }
  return best;
}

bool cancelled( const Control *control )
{
  return control && control->cancelled && control->cancelled();
}

std::vector<Point2> gridRectRing( const GridSpec &grid )
{
  const double x0 = grid.originX;
  const double x1 = grid.originX + grid.cols * grid.pixelWidth;
  const double y0 = grid.originY + grid.rows * grid.pixelHeight;
  const double y1 = grid.originY;
  return { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 }, { x0, y0 } };
}

} // namespace

bool closedSegmentsIntersect( Point2 a, Point2 b, Point2 c, Point2 d, double tol )
{
  const double rx = b.x - a.x;
  const double ry = b.y - a.y;
  const double sx = d.x - c.x;
  const double sy = d.y - c.y;
  const double denom = cross( rx, ry, sx, sy );
  const double qx = c.x - a.x;
  const double qy = c.y - a.y;
  if ( std::abs( denom ) <= tol )
  {
    if ( std::abs( cross( qx, qy, rx, ry ) ) > std::max( tol, 1e-12 ) )
      return false;
    const double rr = rx * rx + ry * ry;
    if ( rr <= tol )
      return std::hypot( c.x - a.x, c.y - a.y ) <= 1e-9;
    const double t0 = ( ( c.x - a.x ) * rx + ( c.y - a.y ) * ry ) / rr;
    const double t1 = ( ( d.x - a.x ) * rx + ( d.y - a.y ) * ry ) / rr;
    const double lo = std::min( t0, t1 );
    const double hi = std::max( t0, t1 );
    return hi >= -1e-9 && lo <= 1.0 + 1e-9;
  }
  const double t = cross( qx, qy, sx, sy ) / denom;
  const double u = cross( qx, qy, rx, ry ) / denom;
  return -1e-9 <= t && t <= 1.0 + 1e-9 && -1e-9 <= u && u <= 1.0 + 1e-9;
}

std::optional<Point2> segmentIntersectionClosed( Point2 a, Point2 b, Point2 c, Point2 d )
{
  const double rx = b.x - a.x;
  const double ry = b.y - a.y;
  const double sx = d.x - c.x;
  const double sy = d.y - c.y;
  const double denom = cross( rx, ry, sx, sy );
  if ( std::abs( denom ) <= 1e-12 )
    return std::nullopt;
  const double qx = c.x - a.x;
  const double qy = c.y - a.y;
  const double t = cross( qx, qy, sx, sy ) / denom;
  const double u = cross( qx, qy, rx, ry ) / denom;
  if ( t < 1e-9 || t > 1.0 + 1e-9 || u < -1e-9 || u > 1.0 + 1e-9 )
    return std::nullopt;
  return Point2{ a.x + t * rx, a.y + t * ry };
}

bool pointOnBarriers( Point2 point, std::span<const BarrierSpec> barriers, double tol )
{
  for ( const BarrierSpec &barrier : barriers )
  {
    for ( std::size_t i = 1; i < barrier.points.size(); ++i )
    {
      if ( pointOnSegment( point, barrier.points[i - 1], barrier.points[i], std::max( tol, 1e-9 ) ) )
        return true;
    }
  }
  return false;
}

bool partitionEdgeBlocked( Point2 a, Point2 b, std::span<const BarrierSpec> barriers, double onLineTol )
{
  if ( pointOnBarriers( a, barriers, onLineTol ) || pointOnBarriers( b, barriers, onLineTol ) )
    return true;
  for ( const BarrierSpec &barrier : barriers )
  {
    for ( std::size_t i = 1; i < barrier.points.size(); ++i )
    {
      if ( closedSegmentsIntersect( a, b, barrier.points[i - 1], barrier.points[i] ) )
        return true;
    }
  }
  return false;
}

ExtendResult extendBarriersToDomain( std::span<const BarrierSpec> barriers,
                                     const std::vector<Polygon> &boundaries,
                                     double snapTol, double maxDist )
{
  ExtendResult result;
  if ( barriers.empty() )
    return result;
  if ( boundaries.empty() )
  {
    result.extended.assign( barriers.begin(), barriers.end() );
    result.conflicts.push_back( "no_domain_boundary" );
    return result;
  }

  for ( const BarrierSpec &barrier : barriers )
  {
    if ( barrier.points.size() < 2 )
    {
      result.extended.push_back( barrier );
      continue;
    }
    std::vector<Point2> points = barrier.points;

    double sx = 0, sy = 0;
    if ( unitVector( points[0].x - points[1].x, points[0].y - points[1].y, sx, sy ) &&
         !endpointOnDomainBoundary( points[0], boundaries, snapTol ) )
    {
      const std::optional<Point2> hit = rayHitDomainBoundary( points[0], { sx, sy }, boundaries, maxDist );
      if ( !hit )
      {
        result.conflicts.push_back( barrier.lineId + ":start" );
      }
      else
      {
        points.insert( points.begin(), *hit );
        BarrierExtension ext;
        ext.barrierId = barrier.lineId;
        ext.end = "start";
        ext.from = *hit;
        ext.to = barrier.points.front();
        result.extensions.push_back( ext );
      }
    }

    double ex = 0, ey = 0;
    if ( unitVector( barrier.points.back().x - barrier.points[barrier.points.size() - 2].x,
                     barrier.points.back().y - barrier.points[barrier.points.size() - 2].y, ex, ey ) &&
         !endpointOnDomainBoundary( barrier.points.back(), boundaries, snapTol ) )
    {
      const std::optional<Point2> hit =
          rayHitDomainBoundary( barrier.points.back(), { ex, ey }, boundaries, maxDist );
      if ( !hit )
      {
        result.conflicts.push_back( barrier.lineId + ":end" );
      }
      else
      {
        points.push_back( *hit );
        BarrierExtension ext;
        ext.barrierId = barrier.lineId;
        ext.end = "end";
        ext.from = barrier.points.back();
        ext.to = *hit;
        result.extensions.push_back( ext );
      }
    }
    BarrierSpec extended;
    extended.lineId = barrier.lineId;
    extended.points = std::move( points );
    result.extended.push_back( std::move( extended ) );
  }
  return result;
}

std::vector<int> buildRegionLabelsNodeSafe( const GridSpec &grid,
    const std::vector<std::uint8_t> &domainMask, std::span<const BarrierSpec> barriers,
    const Control *control )
{
  const std::size_t cellCount = static_cast<std::size_t>( grid.cols ) * static_cast<std::size_t>( grid.rows );
  std::vector<int> labels( cellCount, kRegionOutside );
  if ( cellCount == 0 || domainMask.size() != cellCount )
    return labels;

  const double step = std::min( std::abs( grid.pixelWidth ), std::abs( grid.pixelHeight ) );
  const double onLineTol = std::max( step * 0.25, 1e-9 );

  int current = 0;
  std::deque<std::pair<int, int>> queue;
  for ( int seedRow = 0; seedRow < grid.rows; ++seedRow )
  {
    for ( int seedCol = 0; seedCol < grid.cols; ++seedCol )
    {
      const std::size_t seed = static_cast<std::size_t>( seedRow ) * grid.cols + seedCol;
      if ( !domainMask[seed] || labels[seed] >= 0 )
        continue;
      if ( cancelled( control ) )
        return labels;
      const Point2 center0 = cellCenter( grid, seedCol, seedRow );
      if ( !barriers.empty() && pointOnBarriers( center0, barriers, onLineTol ) )
        continue;
      labels[seed] = current;
      queue.clear();
      queue.emplace_back( seedRow, seedCol );
      while ( !queue.empty() )
      {
        if ( cancelled( control ) )
          return labels;
        const auto [row, col] = queue.front();
        queue.pop_front();
        const Point2 center = cellCenter( grid, col, row );
        const int nr[4] = { row + 1, row - 1, row, row };
        const int nc[4] = { col, col, col + 1, col - 1 };
        for ( int k = 0; k < 4; ++k )
        {
          if ( nr[k] < 0 || nr[k] >= grid.rows || nc[k] < 0 || nc[k] >= grid.cols )
            continue;
          const std::size_t neighbor =
              static_cast<std::size_t>( nr[k] ) * grid.cols + static_cast<std::size_t>( nc[k] );
          if ( !domainMask[neighbor] || labels[neighbor] >= 0 )
            continue;
          if ( !barriers.empty() )
          {
            const Point2 neighborCenter = cellCenter( grid, nc[k], nr[k] );
            if ( partitionEdgeBlocked( center, neighborCenter, barriers, onLineTol ) )
              continue;
          }
          labels[neighbor] = current;
          queue.emplace_back( nr[k], nc[k] );
        }
      }
      ++current;
    }
  }

  if ( !barriers.empty() && current > 0 )
  {
    for ( int row = 0; row < grid.rows; ++row )
    {
      for ( int col = 0; col < grid.cols; ++col )
      {
        const std::size_t idx = static_cast<std::size_t>( row ) * grid.cols + col;
        if ( !domainMask[idx] || labels[idx] >= 0 )
          continue;
        bool hasRegion[2] = { false, false };
        int seen[2] = { -1, -1 };
        int distinct = 0;
        const int nr[4] = { row + 1, row - 1, row, row };
        const int nc[4] = { col, col, col + 1, col - 1 };
        for ( int k = 0; k < 4; ++k )
        {
          if ( nr[k] < 0 || nr[k] >= grid.rows || nc[k] < 0 || nc[k] >= grid.cols )
            continue;
          const std::size_t neighbor =
              static_cast<std::size_t>( nr[k] ) * grid.cols + static_cast<std::size_t>( nc[k] );
          if ( labels[neighbor] < 0 )
            continue;
          int slot = -1;
          for ( int s = 0; s < distinct; ++s )
            if ( seen[s] == labels[neighbor] )
              slot = s;
          if ( slot < 0 && distinct < 2 )
          {
            seen[distinct] = labels[neighbor];
            hasRegion[distinct] = true;
            ++distinct;
          }
        }
        if ( distinct == 1 )
          labels[idx] = seen[0];
      }
    }
  }
  return labels;
}

std::vector<int> assignWellRegions( std::span<const Point2> wellXy, const GridSpec &grid,
                                    const std::vector<int> &regionIds, bool exclusive )
{
  const std::size_t count = wellXy.size();
  const int missing = exclusive ? kWellPending : kWellShared;
  std::vector<int> out( count, missing );
  const std::size_t rows = static_cast<std::size_t>( grid.rows );
  const std::size_t cols = static_cast<std::size_t>( grid.cols );
  if ( regionIds.size() != rows * cols || rows < 2 || cols < 2 )
    return out;

  const double x0 = cellCenter( grid, 0, 0 ).x;
  const double y0 = cellCenter( grid, 0, 0 ).y;
  const double dx = std::abs( grid.pixelWidth );
  const double dy = std::abs( grid.pixelHeight );
  const double xLast = cellCenter( grid, static_cast<int>( cols ) - 1, 0 ).x;
  const double yLast = cellCenter( grid, 0, static_cast<int>( rows ) - 1 ).y;

  for ( std::size_t index = 0; index < count; ++index )
  {
    const double wx = wellXy[index].x;
    const double wy = wellXy[index].y;
    if ( exclusive )
    {
      if ( wx < x0 - 0.51 * dx || wx > xLast + 0.51 * dx || wy < yLast - 0.51 * dy ||
           wy > y0 + 0.51 * dy )
        continue;
    }
    const int col = static_cast<int>( std::lround( ( wx - x0 ) / dx ) );
    const int row = static_cast<int>( std::lround( ( y0 - wy ) / dy ) );
    int found = missing;
    for ( int radius = 0; radius < 4; ++radius )
    {
      const int r0 = std::max( 0, row - radius );
      const int r1 = std::min( static_cast<int>( rows ) - 1, row + radius );
      const int c0 = std::max( 0, col - radius );
      const int c1 = std::min( static_cast<int>( cols ) - 1, col + radius );
      if ( r1 < r0 || c1 < c0 )
        continue;
      int bestLabel = -1;
      double bestDist = std::numeric_limits<double>::infinity();
      for ( int rr = r0; rr <= r1; ++rr )
      {
        for ( int cc = c0; cc <= c1; ++cc )
        {
          const int label = regionIds[static_cast<std::size_t>( rr ) * cols + cc];
          if ( label < 0 )
            continue;
          const Point2 center = cellCenter( grid, cc, rr );
          const double dist = ( center.x - wx ) * ( center.x - wx ) + ( center.y - wy ) * ( center.y - wy );
          if ( dist < bestDist )
          {
            bestDist = dist;
            bestLabel = label;
          }
        }
      }
      if ( bestLabel >= 0 )
      {
        found = bestLabel;
        break;
      }
    }
    out[index] = found;
  }
  return out;
}

bool wellAllowedForCell( int cellLabel, int wellLabel, bool exclusive )
{
  if ( exclusive )
    return cellLabel >= 0 && wellLabel == cellLabel;
  if ( wellLabel == kWellPending )
    return false;
  if ( cellLabel < 0 || wellLabel < 0 )
    return true;
  return wellLabel == cellLabel;
}

PartitionResult buildPartition( const GridSpec &grid, const std::vector<std::uint8_t> &domainMask,
    std::span<const BarrierSpec> barriers, std::span<const Point2> wellXy,
    const std::vector<Polygon> &boundaries, bool interpretation, const Control *control )
{
  PartitionResult result;
  result.mode = interpretation ? "interpretation" : "local";
  result.partitionBarriers.assign( barriers.begin(), barriers.end() );

  if ( interpretation && !barriers.empty() && !boundaries.empty() )
  {
    const double spanX = static_cast<double>( grid.cols ) * std::abs( grid.pixelWidth );
    const double spanY = static_cast<double>( grid.rows ) * std::abs( grid.pixelHeight );
    const double step = std::min( std::abs( grid.pixelWidth ), std::abs( grid.pixelHeight ) );
    ExtendResult extended = extendBarriersToDomain(
        barriers, boundaries, std::max( step * 0.6, 1e-9 ), std::hypot( spanX, spanY ) * 1.05 );
    result.partitionBarriers = std::move( extended.extended );
    result.extensions = std::move( extended.extensions );
    result.conflicts = std::move( extended.conflicts );
  }

  const std::size_t cellCount = static_cast<std::size_t>( grid.cols ) * static_cast<std::size_t>( grid.rows );
  if ( !barriers.empty() )
  {
    result.regionIds = buildRegionLabelsNodeSafe( grid, domainMask, result.partitionBarriers, control );
  }
  else
  {
    result.regionIds.assign( cellCount, kRegionOutside );
    for ( std::size_t i = 0; i < cellCount; ++i )
      if ( i < domainMask.size() && domainMask[i] )
        result.regionIds[i] = 0;
  }

  int maxLabel = kRegionOutside;
  for ( int label : result.regionIds )
    maxLabel = std::max( maxLabel, label );
  result.regionCount = maxLabel >= 0 ? maxLabel + 1 : 0;
  result.wellRegionIds = assignWellRegions( wellXy, grid, result.regionIds, interpretation );
  result.complete = interpretation && result.regionCount >= 2 && result.conflicts.empty();
  if ( interpretation && result.regionCount < 2 )
    result.conflicts.push_back( "partition_not_split" );
  return result;
}

BarrierGrid labelInterpretationPartition( const GridSpec &grid,
    const std::vector<ConstraintLine> &barriers, const std::vector<Sample> &samples,
    double tolerance, const Control *control )
{
  BarrierGrid out;
  std::string budgetError;
  if ( !gridBudgetOk( grid, static_cast<int>( samples.size() ), &budgetError ) )
    return out;
  const std::size_t cellCount =
      static_cast<std::size_t>( grid.cols ) * static_cast<std::size_t>( grid.rows );

  std::vector<BarrierSpec> hard;
  for ( const ConstraintLine &line : barriers )
  {
    if ( !line.enabled || line.semantic != Semantic::HardBarrier || line.points.size() < 2 )
      continue;
    BarrierSpec spec;
    spec.lineId = line.stableId;
    spec.points = line.points;
    hard.push_back( std::move( spec ) );
  }

  const std::vector<std::uint8_t> domain( cellCount, std::uint8_t{ 1 } );
  Polygon rect;
  rect.exterior.points = gridRectRing( grid );
  const std::vector<Polygon> boundaries{ rect };

  const PartitionResult partition = buildPartition( grid, domain, hard, {}, boundaries, true, control );
  if ( cancelled( control ) )
  {
    out.component.clear();
    out.barrierCells = -1;
    return out;
  }

  out.component.assign( cellCount, kRegionOutside );
  out.barrierCells = 0;
  for ( std::size_t i = 0; i < cellCount; ++i )
  {
    if ( partition.regionIds[i] >= 0 )
      out.component[i] = partition.regionIds[i];
    else
    {
      out.component[i] = -2;
      ++out.barrierCells;
    }
  }
  out.componentCount = partition.regionCount;

  out.sampleComponent.assign( samples.size(), -1 );
  for ( std::size_t si = 0; si < samples.size(); ++si )
  {
    const Sample &sample = samples[si];
    if ( sample.componentOverride >= 0 )
    {
      if ( sample.componentOverride < out.componentCount )
      {
        out.sampleComponent[si] = sample.componentOverride;
        continue;
      }
      AmbiguousSample amb;
      amb.stableRowId = sample.stableRowId;
      amb.x = sample.x;
      amb.y = sample.y;
      amb.reason = "指定连通区不存在";
      out.ambiguous.push_back( std::move( amb ) );
      continue;
    }
    const std::vector<Point2> single{ Point2{ sample.x, sample.y } };
    const std::vector<int> region =
        assignWellRegions( single, grid, partition.regionIds, true );
    const int own = region.front();
    if ( own < 0 )
    {
      AmbiguousSample amb;
      amb.stableRowId = sample.stableRowId;
      amb.x = sample.x;
      amb.y = sample.y;
      amb.reason = own == kWellPending && !partition.conflicts.empty()
                       ? "井点不在分区网格内，解释分区不能判定归属"
                       : "井点落在解释分区屏障上，需明确归属";
      out.ambiguous.push_back( std::move( amb ) );
      continue;
    }
    out.sampleComponent[si] = own;
  }
  (void )tolerance;
  return out;
}

} // namespace paleo::singlefactor
