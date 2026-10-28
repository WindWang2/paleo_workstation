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
  // PW-128：double 域判界再 cast——屏障多边形顶点可在样本域网格任意远处。
  const double fc = std::floor( ( x - grid.originX ) / grid.pixelWidth );
  if ( !( fc >= 0.0 ) )
    return 0;
  return static_cast<int>( std::min( fc, static_cast<double>( grid.cols - 1 ) ) );
}

int cellRow( const GridSpec &grid, double y )
{
  // pixelHeight < 0：y < originY 给出正行号；PW-128：double 域判界再 cast。
  const double fr = std::floor( ( y - grid.originY ) / grid.pixelHeight );
  if ( !( fr >= 0.0 ) )
    return 0;
  return static_cast<int>( std::min( fr, static_cast<double>( grid.rows - 1 ) ) );
}

// cellColumn/cellRow 的钳位只服务源点与包围盒；屏障采样点落在网格外时若也
// 钳到边缘格，会把域外的屏障顶点标成场内 barrier——这些格测地距离变 NaN、
// Dijkstra 路径被迫绕行，滞后距被静默偏大或判不可达（issue 评审 finding）。
// markCell 因此按 double 先判界，越界采样点直接丢弃（不落格）。
bool markCell( std::vector<std::uint8_t> &barrier, const GridSpec &grid, double x, double y )
{
  const double fc = std::floor( ( x - grid.originX ) / grid.pixelWidth );
  const double fr = std::floor( ( y - grid.originY ) / grid.pixelHeight );
  if ( !( fc >= 0.0 ) || !( fr >= 0.0 ) || fc > grid.cols - 1 || fr > grid.rows - 1 )
    return false;
  barrier[static_cast<std::size_t>( static_cast<int>( fr ) ) * grid.cols +
          static_cast<std::size_t>( static_cast<int>( fc ) )] = 1;
  return true;
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
  if ( ring.size() < 3 )
    return false; // 空环 ring.size()-1 下溢 → size_t(-1) 下标 UB（#417）
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
      // 斜墙角点密封（issue #292）：对角步两侧正交格任一被挡即禁行，否则
      // 路径从两个角接障碍格之间斜穿，绕障距离退化成近欧氏距离。
      if ( neighborDr[k] != 0 && neighborDc[k] != 0 &&
           ( barrier[static_cast<std::size_t>( row + neighborDr[k] ) * grid.cols + column] ||
             barrier[static_cast<std::size_t>( row ) * grid.cols + column + neighborDc[k]] ) )
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
