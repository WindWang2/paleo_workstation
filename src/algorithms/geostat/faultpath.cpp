// 层：数据
#include "faultpath.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <utility>

// 层：数据
namespace paleo::geostat
{

namespace
{

int cellColumn( const GridSpec &grid, double x )
{
  const int column = static_cast<int>( std::floor( ( x - grid.originX ) / grid.pixelWidth ) );
  return std::clamp( column, 0, grid.cols - 1 );
}

int cellRow( const GridSpec &grid, double y )
{
  // pixelHeight < 0：y < originY 给出正行号
  const int row = static_cast<int>( std::floor( ( y - grid.originY ) / grid.pixelHeight ) );
  return std::clamp( row, 0, grid.rows - 1 );
}

void markCell( std::vector<std::uint8_t> &barrier, const GridSpec &grid, double x, double y )
{
  const int column = cellColumn( grid, x );
  const int row = cellRow( grid, y );
  barrier[static_cast<std::size_t>( row ) * grid.cols + column] = 1;
}

// 边界栅格化：半像元步长采样（support.cpp 的 labelHardBarriers 同口径，
// 细墙不丢）。
void rasterizeRingBoundary( std::vector<std::uint8_t> &barrier, const GridSpec &grid,
                            const std::vector<Point2> &points )
{
  if ( points.size() < 2 )
    return;
  const double minCell = std::min( grid.pixelWidth, -grid.pixelHeight );
  for ( std::size_t i = 0; i < points.size(); ++i )
  {
    const Point2 &a = points[i];
    const Point2 &b = points[( i + 1 ) % points.size()];
    const double length = std::hypot( b.x - a.x, b.y - a.y );
    const int steps = std::max<int>( 1, static_cast<int>( std::ceil( length / ( 0.5 * minCell ) ) ) );
    for ( int step = 0; step <= steps; ++step )
    {
      const double t = static_cast<double>( step ) / steps;
      markCell( barrier, grid, a.x + t * ( b.x - a.x ), a.y + t * ( b.y - a.y ) );
    }
  }
}

bool pointInRing( double x, double y, const std::vector<Point2> &ring )
{
  bool inside = false;
  for ( std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++ )
  {
    const Point2 &a = ring[i];
    const Point2 &b = ring[j];
    if ( ( a.y > y ) != ( b.y > y ) )
    {
      const double crossX = a.x + ( y - a.y ) * ( b.x - a.x ) / ( b.y - a.y );
      if ( x < crossX )
        inside = !inside;
    }
  }
  return inside;
}

void ringBBox( const std::vector<Point2> &points, const GridSpec &grid,
               int *columnMin, int *rowMin, int *columnMax, int *rowMax )
{
  double minX = std::numeric_limits<double>::max();
  double minY = std::numeric_limits<double>::max();
  double maxX = std::numeric_limits<double>::lowest();
  double maxY = std::numeric_limits<double>::lowest();
  for ( const Point2 &point : points )
  {
    minX = std::min( minX, point.x );
    minY = std::min( minY, point.y );
    maxX = std::max( maxX, point.x );
    maxY = std::max( maxY, point.y );
  }
  *columnMin = std::max( 0, cellColumn( grid, minX ) );
  *rowMin = std::max( 0, cellRow( grid, maxY ) );
  *columnMax = std::min( grid.cols - 1, cellColumn( grid, maxX ) );
  *rowMax = std::min( grid.rows - 1, cellRow( grid, minY ) );
}

// 内部填充：格心 even-odd 判定（外环在内 + 任一 hole 挖空），
// 扫描限定在环包围盒内。
void fillPolygonInterior( std::vector<std::uint8_t> &barrier, const GridSpec &grid,
                          const BarrierPolygon &polygon )
{
  int columnMin = 0, rowMin = 0, columnMax = 0, rowMax = 0;
  ringBBox( polygon.exterior.points, grid, &columnMin, &rowMin, &columnMax, &rowMax );
  for ( int row = rowMin; row <= rowMax; ++row )
    for ( int column = columnMin; column <= columnMax; ++column )
    {
      const double x = grid.cellCenterX( column );
      const double y = grid.cellCenterY( row );
      if ( !pointInRing( x, y, polygon.exterior.points ) )
        continue;
      bool inHole = false;
      for ( const BarrierRing &hole : polygon.holes )
      {
        if ( pointInRing( x, y, hole.points ) )
        {
          inHole = true;
          break;
        }
      }
      if ( !inHole )
        barrier[static_cast<std::size_t>( row ) * grid.cols + column] = 1;
    }
}

} // namespace

FaultPathResult faultPathMetric( const GridSpec &grid,
                                 const std::vector<BarrierPolygon> &faultPolygons,
                                 double sourceX, double sourceY,
                                 const Control &control )
{
  FaultPathResult result;
  if ( !grid.isValid() )
  {
    result.message = "grid is not valid";
    return result;
  }
  const std::int64_t cells64 = static_cast<std::int64_t>( grid.rows ) * grid.cols;
  if ( cells64 > 100'000'000 )
  {
    result.message = "grid budget exceeded (cells > 1e8)";
    return result;
  }
  const std::size_t cells = static_cast<std::size_t>( cells64 );

  std::vector<std::uint8_t> barrier( cells, 0 );
  for ( const BarrierPolygon &polygon : faultPolygons )
  {
    rasterizeRingBoundary( barrier, grid, polygon.exterior.points );
    for ( const BarrierRing &hole : polygon.holes )
      rasterizeRingBoundary( barrier, grid, hole.points );
    fillPolygonInterior( barrier, grid, polygon );
  }

  // 源点：落格；屏障格则邻域螺旋搜自由格（≤5 格）
  int sourceColumn = cellColumn( grid, sourceX );
  int sourceRow = cellRow( grid, sourceY );
  bool found = !barrier[static_cast<std::size_t>( sourceRow ) * grid.cols + sourceColumn];
  for ( int ring = 1; !found && ring <= 5; ++ring )
  {
    for ( int dr = -ring; !found && dr <= ring; ++dr )
      for ( int dc = -ring; !found && dc <= ring; ++dc )
      {
        if ( std::max( std::abs( dr ), std::abs( dc ) ) != ring )
          continue;
        const int r = sourceRow + dr;
        const int c = sourceColumn + dc;
        if ( r < 0 || c < 0 || r >= grid.rows || c >= grid.cols )
          continue;
        if ( !barrier[static_cast<std::size_t>( r ) * grid.cols + c] )
        {
          sourceRow = r;
          sourceColumn = c;
          found = true;
        }
      }
  }
  if ( !found )
  {
    result.message = "source falls inside barrier (no free cell within 5 cells)";
    return result;
  }

  // 8 邻接 Dijkstra（各向异性边权）
  const double inf = std::numeric_limits<double>::infinity();
  std::vector<double> dist( cells, inf );
  using Entry = std::pair<double, std::uint32_t>;
  std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
  const std::size_t seed = static_cast<std::size_t>( sourceRow ) * grid.cols + sourceColumn;
  dist[seed] = 0;
  queue.push( { 0.0, static_cast<std::uint32_t>( seed ) } );
  const double stepX = grid.pixelWidth;
  const double stepY = -grid.pixelHeight;
  const double diagonal = std::hypot( stepX, stepY );
  const int neighborDc[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
  const int neighborDr[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
  const double neighborWeight[8] = { stepX, stepX, stepY, stepY, diagonal, diagonal, diagonal, diagonal };
  std::size_t settled = 0;
  std::size_t sinceCheck = 0;

  while ( !queue.empty() )
  {
    const Entry entry = queue.top();
    queue.pop();
    const std::size_t cell = entry.second;
    if ( entry.first > dist[cell] )
      continue; // 过期堆项
    ++settled;
    const int row = static_cast<int>( cell / static_cast<std::size_t>( grid.cols ) );
    const int column = static_cast<int>( cell % static_cast<std::size_t>( grid.cols ) );
    for ( int k = 0; k < 8; ++k )
    {
      const int nr = row + neighborDr[k];
      const int nc = column + neighborDc[k];
      if ( nr < 0 || nc < 0 || nr >= grid.rows || nc >= grid.cols )
        continue;
      const std::size_t next = static_cast<std::size_t>( nr ) * grid.cols + nc;
      if ( barrier[next] )
        continue;
      const double candidate = dist[cell] + neighborWeight[k];
      if ( candidate < dist[next] )
      {
        dist[next] = candidate;
        queue.push( { candidate, static_cast<std::uint32_t>( next ) } );
      }
    }
    ++sinceCheck;
    if ( sinceCheck >= 65536 )
    {
      sinceCheck = 0;
      if ( control.cancelled && control.cancelled() )
      {
        result.status = Status::Cancelled;
        result.message = "cancelled";
        return result;
      }
      if ( control.progress )
        control.progress( std::min( 1.0, static_cast<double>( settled ) / static_cast<double>( cells ) ) );
    }
  }

  result.distance.assign( cells, std::numeric_limits<double>::quiet_NaN() );
  for ( std::size_t cell2 = 0; cell2 < cells; ++cell2 )
  {
    if ( barrier[cell2] )
    {
      ++result.barrierCells;
      continue;
    }
    if ( std::isfinite( dist[cell2] ) )
    {
      result.distance[cell2] = dist[cell2];
      ++result.reachedCells;
    }
    else
    {
      ++result.unreachableCells;
    }
  }
  result.sourceColumn = sourceColumn;
  result.sourceRow = sourceRow;
  if ( control.progress )
    control.progress( 1.0 );
  result.status = Status::Ok;
  return result;
}

} // namespace paleo::geostat
