// 层：数据
#include "support.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

// 层：数据
namespace paleo::singlefactor
{

double distanceToPolyline( Point2 point, const std::vector<Point2> &points )
{
  double best = std::numeric_limits<double>::infinity();
  for ( std::size_t i = 1; i < points.size(); ++i )
  {
    const Point2 a = points[i - 1];
    const Point2 b = points[i];
    const double vx = b.x - a.x;
    const double vy = b.y - a.y;
    const double len2 = vx * vx + vy * vy;
    double dist = 0;
    if ( !( len2 > 0.0 ) )
      dist = std::hypot( point.x - a.x, point.y - a.y );
    else
    {
      const double t = std::clamp( ( ( point.x - a.x ) * vx + ( point.y - a.y ) * vy ) / len2, 0.0, 1.0 );
      dist = std::hypot( point.x - ( a.x + t * vx ), point.y - ( a.y + t * vy ) );
    }
    best = std::min( best, dist );
  }
  if ( points.size() == 1 )
    best = std::hypot( point.x - points.front().x, point.y - points.front().y );
  return best;
}

namespace
{

constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;

double distToSeg( Point2 p, Point2 a, Point2 b )
{
  const double vx = b.x - a.x;
  const double vy = b.y - a.y;
  const double len2 = vx * vx + vy * vy;
  if ( !( len2 > 0.0 ) )
    return std::hypot( p.x - a.x, p.y - a.y );
  const double t = std::clamp( ( ( p.x - a.x ) * vx + ( p.y - a.y ) * vy ) / len2, 0.0, 1.0 );
  return std::hypot( p.x - ( a.x + t * vx ), p.y - ( a.y + t * vy ) );
}

double orient( Point2 a, Point2 b, Point2 c )
{
  return ( b.x - a.x ) * ( c.y - a.y ) - ( b.y - a.y ) * ( c.x - a.x );
}

enum class RingClass
{
  Out,
  Boundary,
  In
};

RingClass classifyRing( const Ring &ring, Point2 point, double tolerance )
{
  const std::vector<Point2> &v = ring.points;
  if ( v.size() < 2 )
    return RingClass::Out;
  const bool closed = v.front().x == v.back().x && v.front().y == v.back().y;
  const std::size_t n = closed ? v.size() - 1 : v.size();
  if ( n < 2 )
    return RingClass::Out;
  auto at = [&]( std::size_t i ) { return v[i]; };
  for ( std::size_t i = 0; i < n; ++i )
  {
    const Point2 b = ( i + 1 == n && !closed ) ? v[0] : v[i + 1];
    if ( distToSeg( point, at( i ), b ) <= tolerance )
      return RingClass::Boundary;
  }
  if ( n < 3 )
    return RingClass::Out;
  bool inside = false;
  for ( std::size_t i = 0; i < n; ++i )
  {
    const Point2 a = at( i );
    const Point2 b = ( i + 1 == n && !closed ) ? v[0] : v[i + 1];
    if ( ( a.y > point.y ) != ( b.y > point.y ) )
    {
      const double xint = ( b.x - a.x ) * ( point.y - a.y ) / ( b.y - a.y ) + a.x;
      if ( point.x < xint )
        inside = !inside;
    }
  }
  return inside ? RingClass::In : RingClass::Out;
}

double polylineLength( const std::vector<Point2> &points )
{
  double length = 0;
  for ( std::size_t i = 1; i < points.size(); ++i )
  {
    const double seg = std::hypot( points[i].x - points[i - 1].x, points[i].y - points[i - 1].y );
    if ( seg > 1e-12 )
      length += seg;
  }
  return length;
}

bool finitePoint( Point2 p )
{
  return std::isfinite( p.x ) && std::isfinite( p.y );
}

struct Bounds
{
  double xmin = 0;
  double ymin = 0;
  double xmax = 0;
  double ymax = 0;
  bool any = false;
  void add( double x, double y )
  {
    if ( !std::isfinite( x ) || !std::isfinite( y ) )
      return;
    if ( !any )
    {
      xmin = xmax = x;
      ymin = ymax = y;
      any = true;
      return;
    }
    xmin = std::min( xmin, x );
    xmax = std::max( xmax, x );
    ymin = std::min( ymin, y );
    ymax = std::max( ymax, y );
  }
  double span() const
  {
    if ( !any )
      return 0;
    return std::max( xmax - xmin, ymax - ymin );
  }
};

bool cellOf( const GridSpec &grid, double x, double y, int &column, int &row )
{
  if ( !( grid.pixelWidth > 0.0 ) || !( grid.pixelHeight < 0.0 ) || grid.cols <= 0 || grid.rows <= 0 )
    return false;
  const double fc = ( x - grid.originX ) / grid.pixelWidth;
  const double fr = ( y - grid.originY ) / grid.pixelHeight;
  if ( !std::isfinite( fc ) || !std::isfinite( fr ) )
    return false;
  column = static_cast<int>( std::floor( fc ) );
  row = static_cast<int>( std::floor( fr ) );
  return column >= 0 && column < grid.cols && row >= 0 && row < grid.rows;
}

double medianSorted( std::vector<double> values )
{
  if ( values.empty() )
    return 0;
  std::sort( values.begin(), values.end() );
  const std::size_t n = values.size();
  if ( n % 2 == 1 )
    return values[n / 2];
  return 0.5 * ( values[n / 2 - 1] + values[n / 2] );
}

std::vector<Point2> monotoneHull( std::vector<Point2> points )
{
  std::sort( points.begin(), points.end(), []( Point2 a, Point2 b ) {
    return a.x < b.x || ( a.x == b.x && a.y < b.y );
  } );
  points.erase( std::unique( points.begin(), points.end(),
                              []( Point2 a, Point2 b ) { return a.x == b.x && a.y == b.y; } ),
                points.end() );
  if ( points.size() <= 2 )
    return points;
  auto cross = []( Point2 o, Point2 a, Point2 b ) { return orient( o, a, b ); };
  std::vector<Point2> lower;
  for ( const Point2 &p : points )
  {
    while ( lower.size() >= 2 && cross( lower[lower.size() - 2], lower.back(), p ) <= 0.0 )
      lower.pop_back();
    lower.push_back( p );
  }
  std::vector<Point2> upper;
  for ( auto it = points.rbegin(); it != points.rend(); ++it )
  {
    while ( upper.size() >= 2 && cross( upper[upper.size() - 2], upper.back(), *it ) <= 0.0 )
      upper.pop_back();
    upper.push_back( *it );
  }
  lower.pop_back();
  upper.pop_back();
  lower.insert( lower.end(), upper.begin(), upper.end() );
  return lower;
}

double distanceToHull( Point2 point, const std::vector<Point2> &hull )
{
  if ( hull.empty() )
    return std::numeric_limits<double>::infinity();
  if ( hull.size() == 1 )
    return std::hypot( point.x - hull[0].x, point.y - hull[0].y );
  if ( hull.size() == 2 )
    return distToSeg( point, hull[0], hull[1] );
  double scale = 1;
  for ( const Point2 &p : hull )
    scale = std::max( scale, std::max( std::abs( p.x ), std::abs( p.y ) ) );
  const double edgeTol = 1e-12 * scale;
  double best = std::numeric_limits<double>::infinity();
  bool inside = false;
  for ( std::size_t i = 0; i < hull.size(); ++i )
  {
    const Point2 a = hull[i];
    const Point2 b = hull[( i + 1 ) % hull.size()];
    const double dist = distToSeg( point, a, b );
    best = std::min( best, dist );
    if ( dist <= edgeTol )
      return 0;
    if ( ( a.y > point.y ) != ( b.y > point.y ) )
    {
      const double xint = ( b.x - a.x ) * ( point.y - a.y ) / ( b.y - a.y ) + a.x;
      if ( point.x < xint )
        inside = !inside;
    }
  }
  return inside ? 0.0 : best;
}

bool cancelled( const Control *control )
{
  return control && control->cancelled && control->cancelled();
}

double minDistanceToLines( Point2 point, const std::vector<const ConstraintLine *> &lines )
{
  double best = std::numeric_limits<double>::infinity();
  for ( const ConstraintLine *line : lines )
  {
    const std::vector<Point2> &pts = line->points;
    for ( std::size_t i = 1; i < pts.size(); ++i )
    {
      if ( std::hypot( pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y ) <= 1e-12 )
        continue;
      best = std::min( best, distToSeg( point, pts[i - 1], pts[i] ) );
    }
  }
  return best;
}

} // namespace

double meanNearestSpacing( const std::vector<Point2> &points )
{
  if ( points.size() < 2 )
    return 0;
  for ( const Point2 &p : points )
  {
    if ( !finitePoint( p ) )
      return std::numeric_limits<double>::quiet_NaN();
  }
  // 与 estimate_mean_well_spacing 相同：保留重复行，重合点的最近距离为 0 并拉低均值。
  // n>400 不走 numpy Generator(0) 的 400 点抽样，改用全体均值。验收井数不超过 200。
  double sum = 0;
  int count = 0;
  for ( std::size_t i = 0; i < points.size(); ++i )
  {
    double nearest = std::numeric_limits<double>::infinity();
    for ( std::size_t j = 0; j < points.size(); ++j )
    {
      if ( i == j )
        continue;
      nearest = std::min( nearest, std::hypot( points[i].x - points[j].x, points[i].y - points[j].y ) );
    }
    if ( std::isfinite( nearest ) )
    {
      sum += nearest;
      ++count;
    }
  }
  return count ? sum / static_cast<double>( count ) : 0;
}

double medianNearestSpacing( const std::vector<Point2> &uniqueSorted )
{
  if ( uniqueSorted.size() < 2 )
    return 0;
  std::vector<double> nearest;
  nearest.reserve( uniqueSorted.size() );
  for ( std::size_t i = 0; i < uniqueSorted.size(); ++i )
  {
    double best = std::numeric_limits<double>::infinity();
    for ( std::size_t j = 0; j < uniqueSorted.size(); ++j )
    {
      if ( i == j )
        continue;
      best = std::min( best, std::hypot( uniqueSorted[i].x - uniqueSorted[j].x,
                                          uniqueSorted[i].y - uniqueSorted[j].y ) );
    }
    if ( std::isfinite( best ) )
      nearest.push_back( best );
  }
  return medianSorted( std::move( nearest ) );
}

bool pointInDomain( const std::vector<Polygon> &domain, Point2 point, double tolerance )
{
  if ( !finitePoint( point ) )
    return false;
  const double tol = std::max( tolerance, 0.0 );
  for ( const Polygon &poly : domain )
  {
    const RingClass exterior = classifyRing( poly.exterior, point, tol );
    if ( exterior == RingClass::Out )
      continue;
    bool hole = false;
    for ( const Ring &ring : poly.holes )
    {
      const RingClass cls = classifyRing( ring, point, tol );
      if ( cls == RingClass::In || cls == RingClass::Boundary )
      {
        hole = true;
        break;
      }
    }
    if ( !hole )
      return true;
  }
  return false;
}

Point2 cellCenter( const GridSpec &grid, int column, int row )
{
  return Point2{ grid.originX + ( static_cast<double>( column ) + 0.5 ) * grid.pixelWidth,
                 grid.originY + ( static_cast<double>( row ) + 0.5 ) * grid.pixelHeight };
}

bool gridBudgetOk( const GridSpec &grid, int sampleCount, std::string *error )
{
  auto fail = [&]( const char *text ) {
    if ( error )
      *error = text;
    return false;
  };
  if ( grid.cols <= 0 || grid.rows <= 0 )
    return fail( "网格行列必须为正" );
  if ( !std::isfinite( grid.originX ) || !std::isfinite( grid.originY ) ||
       !std::isfinite( grid.pixelWidth ) || !std::isfinite( grid.pixelHeight ) )
    return fail( "网格变换含非有限值" );
  if ( !( grid.pixelWidth > 0.0 ) || !( grid.pixelHeight < 0.0 ) )
    return fail( "首版栅格仅支持 north-up、pixel-is-area" );
  const auto cols = static_cast<long long>( grid.cols );
  const auto rows = static_cast<long long>( grid.rows );
  if ( cols > 0 && rows > std::numeric_limits<long long>::max() / cols )
    return fail( "网格像元数溢出" );
  const long long cells = cols * rows;
  if ( cells > 100000000LL )
    return fail( "网格像元数超过 1e8" );
  if ( sampleCount < 0 || sampleCount > 1000000 )
    return fail( "样本数超过预算" );
  const double bytes = static_cast<double>( cells ) * 64.0 + static_cast<double>( sampleCount ) * 128.0;
  if ( !( bytes <= 2.0 * kGiB ) )
    return fail( "估算工作集超过 2GiB" );
  return true;
}

BarrierGrid labelHardBarriers( const GridSpec &grid, const std::vector<ConstraintLine> &barriers,
                               const std::vector<Sample> &samples, double tolerance,
                               const Control *control )
{
  BarrierGrid out;
  std::string budgetError;
  if ( !gridBudgetOk( grid, static_cast<int>( samples.size() ), &budgetError ) )
  {
    out.componentCount = 0;
    return out;
  }
  const std::size_t cellCount = static_cast<std::size_t>( grid.cols ) * static_cast<std::size_t>( grid.rows );
  out.component.assign( cellCount, -1 );
  const double cell = std::min( std::abs( grid.pixelWidth ), std::abs( grid.pixelHeight ) );
  for ( const ConstraintLine &line : barriers )
  {
    if ( !line.enabled || line.semantic != Semantic::HardBarrier || line.points.size() < 2 )
      continue;
    for ( std::size_t i = 1; i < line.points.size(); ++i )
    {
      if ( cancelled( control ) )
      {
        out.component.clear();
        out.barrierCells = -1;
        return out;
      }
      const Point2 a = line.points[i - 1];
      const Point2 b = line.points[i];
      const double len = std::hypot( b.x - a.x, b.y - a.y );
      if ( len <= 1e-12 )
        continue;
      const int steps = std::max( 1, static_cast<int>( std::ceil( len / ( cell * 0.5 ) ) ) );
      for ( int s = 0; s <= steps; ++s )
      {
        const double t = static_cast<double>( s ) / static_cast<double>( steps );
        int column = 0;
        int row = 0;
        if ( cellOf( grid, a.x + ( b.x - a.x ) * t, a.y + ( b.y - a.y ) * t, column, row ) )
          out.component[static_cast<std::size_t>( row ) * static_cast<std::size_t>( grid.cols ) +
                        static_cast<std::size_t>( column )] = -2;
      }
    }
  }
  int next = 0;
  std::vector<std::size_t> queue;
  std::size_t visited = 0;
  for ( std::size_t start = 0; start < cellCount; ++start )
  {
    if ( out.component[start] != -1 )
      continue;
    if ( cancelled( control ) )
    {
      out.component.clear();
      out.barrierCells = -1;
      return out;
    }
    out.component[start] = next;
    queue.clear();
    queue.push_back( start );
    std::size_t head = 0;
    while ( head < queue.size() )
    {
      if ( ( ++visited & 1023U ) == 0 && cancelled( control ) )
      {
        out.component.clear();
        out.barrierCells = -1;
        return out;
      }
      const std::size_t cur = queue[head++];
      const int row = static_cast<int>( cur / static_cast<std::size_t>( grid.cols ) );
      const int column = static_cast<int>( cur % static_cast<std::size_t>( grid.cols ) );
      const int nr[4] = { row - 1, row + 1, row, row };
      const int nc[4] = { column, column, column - 1, column + 1 };
      for ( int k = 0; k < 4; ++k )
      {
        if ( nr[k] < 0 || nr[k] >= grid.rows || nc[k] < 0 || nc[k] >= grid.cols )
          continue;
        const std::size_t neighbor = static_cast<std::size_t>( nr[k] ) * static_cast<std::size_t>( grid.cols ) +
                                      static_cast<std::size_t>( nc[k] );
        if ( out.component[neighbor] == -1 )
        {
          out.component[neighbor] = next;
          queue.push_back( neighbor );
        }
      }
    }
    ++next;
  }
  out.componentCount = next;
  for ( int id : out.component )
  {
    if ( id == -2 )
      ++out.barrierCells;
  }
  out.sampleComponent.assign( samples.size(), -1 );
  const double tol = std::max( tolerance, 0.0 );
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
    int column = 0;
    int row = 0;
    const bool inside = cellOf( grid, sample.x, sample.y, column, row );
    int own = -3;
    if ( inside )
    {
      own = out.component[static_cast<std::size_t>( row ) * static_cast<std::size_t>( grid.cols ) +
                          static_cast<std::size_t>( column )];
    }
    double lineDist = std::numeric_limits<double>::infinity();
    for ( const ConstraintLine &line : barriers )
    {
      if ( !line.enabled || line.semantic != Semantic::HardBarrier )
        continue;
      for ( std::size_t i = 1; i < line.points.size(); ++i )
        lineDist = std::min( lineDist, distToSeg( Point2{ sample.x, sample.y }, line.points[i - 1], line.points[i] ) );
    }
    const bool onLine = lineDist <= tol;
    if ( !inside || own == -2 || onLine )
    {
      AmbiguousSample amb;
      amb.stableRowId = sample.stableRowId;
      amb.x = sample.x;
      amb.y = sample.y;
      if ( !inside )
        amb.reason = "井点不在输出网格内，有硬屏障时不能判定连通区";
      else if ( own == -2 )
        amb.reason = "井点落在硬屏障格，需明确归属";
      else
        amb.reason = "井点落在硬屏障线上，需明确归属";
      out.ambiguous.push_back( std::move( amb ) );
      continue;
    }
    out.sampleComponent[si] = own;
  }
  return out;
}

std::string resolveParameters( const PreparedInput &input, const GridSpec &grid, ResolvedParameters *params )
{
  if ( !params )
    return "缺少参数对象";
  params->autosApplied = false;
  params->directions.clear();
  params->soft.clear();
  params->spacingSubsampled = false;
  if ( !( params->power > 0.0 ) || !std::isfinite( params->power ) )
    return "power 必须为有限正数";
  const bool gridless = grid.cols <= 0 || grid.rows <= 0;
  double step = 0;
  if ( !gridless )
  {
    if ( !std::isfinite( grid.pixelWidth ) || !std::isfinite( grid.pixelHeight ) ||
         !std::isfinite( grid.originX ) || !std::isfinite( grid.originY ) )
      return "网格变换含非有限值";
    if ( !( grid.pixelWidth > 0.0 ) || !( grid.pixelHeight < 0.0 ) )
      return "首版栅格仅支持 north-up、pixel-is-area";
    step = std::min( std::abs( grid.pixelWidth ), std::abs( grid.pixelHeight ) );
  }
  Bounds box;
  for ( const Polygon &poly : input.domain )
    for ( const Point2 &p : poly.exterior.points )
      box.add( p.x, p.y );
  if ( !box.any && !gridless )
  {
    box.add( grid.originX, grid.originY );
    box.add( grid.originX + grid.cols * grid.pixelWidth, grid.originY + grid.rows * grid.pixelHeight );
  }
  std::vector<Point2> xy;
  xy.reserve( input.samples.size() );
  for ( const Sample &sample : input.samples )
  {
    if ( !std::isfinite( sample.x ) || !std::isfinite( sample.y ) || !std::isfinite( sample.value ) )
      return "样本含非有限坐标或数值";
    xy.push_back( Point2{ sample.x, sample.y } );
    if ( input.domain.empty() )
      box.add( sample.x, sample.y );
  }
  const double spacingMean = meanNearestSpacing( xy );
  if ( !std::isfinite( spacingMean ) )
    return "井距含非有限值";
  const double spacing = std::max( spacingMean, step );
  const double span = box.span();
  params->spacing = spacing;
  params->step = step;
  params->span = span;
  params->tolerance = std::max( std::max( step, span ) * 1e-9, 1e-12 );
  params->clusterSpan = params->wellClusterLocality ? span : 0;
  params->valueUnit = params->valueUnit.empty() ? input.valueUnit : params->valueUnit;
  const double requestedSearch = params->searchRadius.value_or( 0 );
  params->supportedMinPoints = std::max( params->minPoints, 1 );
  if ( params->coverage == CoverageMode::DomainExtrapolation )
  {
    params->searchRadius.reset();
    params->minPoints = 1;
    params->maxPoints = 0;
    params->supportedRadius = std::max( requestedSearch, spacing * 2.0 );
  }
  else
  {
    if ( !( requestedSearch > 0.0 ) && !( spacing > 0.0 ) )
      return "无可用距离尺度";
    params->searchRadius = std::max( requestedSearch, spacing * 2.0 );
    params->supportedRadius = *params->searchRadius;
    if ( params->minPoints < 1 )
      return "minPoints 必须 >= 1";
    if ( params->maxPoints < 0 )
      return "maxPoints 不能为负";
  }
  for ( const ConstraintLine &line : input.constraints )
  {
    if ( !line.enabled || line.semantic != Semantic::DirectionGuide )
      continue;
    if ( !std::isfinite( line.ratio ) || line.ratio < 1.0 || line.ratio > 100.0 )
      return "directionRatio 必须在 [1,100]";
    if ( !( line.ratio > 1.0 ) || line.points.size() < 2 )
      continue;
    const double length = polylineLength( line.points );
    double radius = line.influenceRadius > 0.0 ? line.influenceRadius
                                                : std::min( std::max( 2.0 * spacing, 0.2 * length ), 0.15 * span );
    if ( !( radius > 0.0 ) || !std::isfinite( radius ) )
      continue;
    double core = line.coreRadius > 0.0 ? line.coreRadius : radius * 0.3;
    if ( !std::isfinite( core ) )
      return "方向核半径含非有限值";
    core = std::min( std::max( core, 0.0 ), radius * 0.95 );
    ResolvedDirection resolved;
    resolved.id = line.stableId;
    resolved.ratio = line.ratio;
    resolved.influence = radius;
    resolved.core = core;
    resolved.points = line.points;
    params->directions.push_back( std::move( resolved ) );
  }
  for ( const ConstraintLine &line : input.constraints )
  {
    if ( !line.enabled || line.semantic != Semantic::InterpretiveBoundary )
      continue;
    if ( !std::isfinite( line.softStrength ) || line.softStrength < 0.0 || line.softStrength > 0.8 )
      return "softBoundaryStrength 必须在 [0,0.8]";
    if ( !( line.softStrength > 0.0 ) || line.points.size() < 2 )
      continue;
    double radius = line.softRadius > 0.0 ? line.softRadius : std::max( 4.0 * step, 0.04 * span );
    if ( !( radius > 0.0 ) || !std::isfinite( radius ) )
      return "软边界半径无可用距离尺度";
    ResolvedSoft resolved;
    resolved.id = line.stableId;
    resolved.radius = radius;
    resolved.strength = line.softStrength;
    resolved.points = line.points;
    params->soft.push_back( std::move( resolved ) );
  }
  params->hardBarrierModel = "grid_connectivity_v1";
  params->duplicatePolicy = "preserve_rows_exact_mean";
  params->autosApplied = true;
  return {};
}

ClusterModel buildClusters( const std::vector<Sample> &samples, double span )
{
  ClusterModel model;
  if ( !( span > 0.0 ) || samples.size() < 3 )
    return model;
  std::vector<Point2> unique;
  unique.reserve( samples.size() );
  for ( const Sample &sample : samples )
  {
    const Point2 p{ sample.x, sample.y };
    if ( std::none_of( unique.begin(), unique.end(), [&]( Point2 q ) { return q.x == p.x && q.y == p.y; } ) )
      unique.push_back( p );
  }
  std::sort( unique.begin(), unique.end(), []( Point2 a, Point2 b ) {
    return a.x < b.x || ( a.x == b.x && a.y < b.y );
  } );
  if ( unique.size() < 3 )
    return model;
  const double spacing = medianNearestSpacing( unique );
  const double link = std::min( 2.5 * spacing, 0.08 * span );
  model.radius = std::max( std::min( 3.0 * spacing, 0.12 * span ), span * 0.005 );
  const int n = static_cast<int>( unique.size() );
  std::vector<int> parent( static_cast<std::size_t>( n ) );
  for ( int i = 0; i < n; ++i )
    parent[static_cast<std::size_t>( i )] = i;
  auto find = [&]( auto &&self, int i ) -> int {
    if ( parent[static_cast<std::size_t>( i )] != i )
      parent[static_cast<std::size_t>( i )] = self( self, parent[static_cast<std::size_t>( i )] );
    return parent[static_cast<std::size_t>( i )];
  };
  if ( link > 0.0 )
  {
    for ( int i = 0; i < n; ++i )
    {
      for ( int j = i + 1; j < n; ++j )
      {
        const double dist = std::hypot( unique[static_cast<std::size_t>( i )].x - unique[static_cast<std::size_t>( j )].x,
                                         unique[static_cast<std::size_t>( i )].y - unique[static_cast<std::size_t>( j )].y );
        if ( dist <= link )
        {
          const int a = find( find, i );
          const int b = find( find, j );
          if ( a != b )
            parent[static_cast<std::size_t>( b )] = a;
        }
      }
    }
  }
  std::vector<int> label( static_cast<std::size_t>( n ) );
  std::vector<int> compact( static_cast<std::size_t>( n ), -1 );
  int groups = 0;
  for ( int i = 0; i < n; ++i )
  {
    const int root = find( find, i );
    if ( compact[static_cast<std::size_t>( root )] < 0 )
      compact[static_cast<std::size_t>( root )] = groups++;
    label[static_cast<std::size_t>( i )] = compact[static_cast<std::size_t>( root )];
  }
  std::vector<int> original( samples.size(), 0 );
  for ( std::size_t row = 0; row < samples.size(); ++row )
  {
    int best = 0;
    double bestDist = std::numeric_limits<double>::infinity();
    for ( int i = 0; i < n; ++i )
    {
      const double dist = std::hypot( samples[row].x - unique[static_cast<std::size_t>( i )].x,
                                       samples[row].y - unique[static_cast<std::size_t>( i )].y );
      if ( dist < bestDist )
      {
        bestDist = dist;
        best = i;
      }
    }
    original[row] = label[static_cast<std::size_t>( best )];
  }
  model.groups.resize( static_cast<std::size_t>( groups ) );
  model.hulls.resize( static_cast<std::size_t>( groups ) );
  for ( int g = 0; g < groups; ++g )
  {
    std::vector<Point2> members;
    for ( std::size_t row = 0; row < samples.size(); ++row )
    {
      if ( original[row] != g )
        continue;
      model.groups[static_cast<std::size_t>( g )].push_back( static_cast<int>( row ) );
      members.push_back( Point2{ samples[row].x, samples[row].y } );
    }
    model.hulls[static_cast<std::size_t>( g )] = monotoneHull( std::move( members ) );
  }
  return model;
}

std::vector<double> ClusterModel::weights( const std::vector<Point2> &queries, int sampleCount ) const
{
  const std::size_t rows = queries.size();
  const std::size_t cols = static_cast<std::size_t>( std::max( sampleCount, 0 ) );
  std::vector<double> result( rows * cols, 1.0 );
  if ( groups.empty() || !( radius > 0.0 ) || cols == 0 )
    return result;
  for ( std::size_t q = 0; q < rows; ++q )
  {
    for ( std::size_t g = 0; g < groups.size(); ++g )
    {
      const double dist = distanceToHull( queries[q], hulls[g] );
      const double t = std::clamp( dist / radius, 0.0, 1.0 );
      const double gate = std::pow( 1.0 - t, 4.0 ) * ( 1.0 + 4.0 * t );
      const double weight = 0.65 + 0.35 * gate;
      for ( int member : groups[g] )
      {
        if ( member < 0 || static_cast<std::size_t>( member ) >= cols )
          continue;
        result[q * cols + static_cast<std::size_t>( member )] = weight;
      }
    }
  }
  return result;
}

bool segmentsCross( Point2 a, Point2 b, Point2 c, Point2 d )
{
  const double d1 = orient( a, b, c );
  const double d2 = orient( a, b, d );
  const double d3 = orient( c, d, a );
  const double d4 = orient( c, d, b );
  return d1 * d2 < 0.0 && d3 * d4 < 0.0;
}

WorkField buildCartographicWork( const GridSpec &grid, const std::vector<double> &analysis,
                                 const std::vector<std::uint8_t> &valid,
                                 const std::vector<ConstraintLine> &stops,
                                 const std::vector<ContourPolyline> &contours,
                                 const std::vector<double> &levels, double transitionDistance )
{
  WorkField work;
  work.values = analysis;
  if ( analysis.empty() || static_cast<long long>( analysis.size() ) != static_cast<long long>( grid.cols ) * grid.rows )
  {
    work.status = Status::InvalidInput;
    work.message = "工作场网格与分析场不一致";
    work.values.clear();
    return work;
  }
  std::vector<const ConstraintLine *> selected;
  for ( const ConstraintLine &line : stops )
  {
    if ( !line.enabled || line.points.size() < 2 )
      continue;
    if ( line.semantic != Semantic::ContourStop && line.semantic != Semantic::CartographicDetour )
      continue;
    bool hit = false;
    for ( const ContourPolyline &poly : contours )
    {
      if ( hit )
        break;
      for ( std::size_t i = 1; i < poly.points.size() && !hit; ++i )
      {
        for ( std::size_t j = 1; j < line.points.size(); ++j )
        {
          if ( segmentsCross( poly.points[i - 1], poly.points[i], line.points[j - 1], line.points[j] ) )
          {
            hit = true;
            break;
          }
        }
      }
    }
    if ( hit )
      selected.push_back( &line );
  }
  if ( selected.empty() )
  {
    work.unchanged = true;
    work.message = "没有与原等值线相交的制图约束";
    return work;
  }
  for ( double level : levels )
  {
    if ( !std::isfinite( level ) )
    {
      work.status = Status::InvalidInput;
      work.message = "等值级别必须为有限数值";
      work.values = analysis;
      work.unchanged = true;
      return work;
    }
  }
  std::vector<double> ordered = levels;
  std::sort( ordered.begin(), ordered.end() );
  ordered.erase( std::unique( ordered.begin(), ordered.end() ), ordered.end() );
  if ( ordered.empty() )
  {
    work.unchanged = true;
    return work;
  }
  const double dx = std::abs( grid.pixelWidth );
  const double dy = std::abs( grid.pixelHeight );
  const double guard = 2.0 * std::hypot( dx, dy );
  double width = 0;
  for ( const ConstraintLine *line : selected )
    width = std::max( width, std::max( line->displayBuffer, line->cartographicBuffer ) );
  Bounds box;
  for ( int row = 0; row < grid.rows; ++row )
  {
    for ( int column = 0; column < grid.cols; ++column )
    {
      const Point2 c = cellCenter( grid, column, row );
      box.add( c.x, c.y );
    }
  }
  const double span = box.span();
  const double shoulder = transitionDistance > 0.0 ? transitionDistance
                                                    : std::max( width * 8.0, std::max( guard * 4.0, span * 0.025 ) );
  if ( !( shoulder > 0.0 ) || !std::isfinite( shoulder ) )
  {
    work.status = Status::NumericalFailure;
    work.message = "工作场过渡宽度无效";
    work.values = analysis;
    work.unchanged = true;
    return work;
  }
  auto isValid = [&]( std::size_t index ) {
    if ( !std::isfinite( analysis[index] ) )
      return false;
    if ( !valid.empty() && ( index >= valid.size() || valid[index] == 0 ) )
      return false;
    return true;
  };
  std::vector<double> finiteValues;
  std::vector<double> localValues;
  finiteValues.reserve( analysis.size() );
  for ( std::size_t i = 0; i < analysis.size(); ++i )
  {
    if ( !isValid( i ) )
      continue;
    finiteValues.push_back( analysis[i] );
    const int column = static_cast<int>( i % static_cast<std::size_t>( grid.cols ) );
    const int row = static_cast<int>( i / static_cast<std::size_t>( grid.cols ) );
    const double dist = minDistanceToLines( cellCenter( grid, column, row ), selected );
    if ( dist <= width + guard )
      localValues.push_back( analysis[i] );
  }
  if ( finiteValues.empty() )
  {
    work.unchanged = true;
    work.message = "分析场没有有限值";
    return work;
  }
  const double representative = medianSorted( localValues.empty() ? finiteValues : localValues );
  const double valueSpan = [&]() {
    const auto mm = std::minmax_element( finiteValues.begin(), finiteValues.end() );
    return *mm.second - *mm.first;
  }();
  const double levelSpan = ordered.back() - ordered.front();
  const double scale = std::max( valueSpan, std::max( levelSpan, 1e-9 ) );
  std::vector<double> gaps;
  gaps.push_back( ordered.front() - scale * 0.02 );
  for ( std::size_t i = 1; i < ordered.size(); ++i )
    gaps.push_back( 0.5 * ( ordered[i - 1] + ordered[i] ) );
  gaps.push_back( ordered.back() + scale * 0.02 );
  double core = gaps.front();
  double coreDist = std::abs( core - representative );
  for ( double gap : gaps )
  {
    const double d = std::abs( gap - representative );
    if ( d < coreDist )
    {
      coreDist = d;
      core = gap;
    }
  }
  int modified = 0;
  for ( std::size_t i = 0; i < analysis.size(); ++i )
  {
    if ( !isValid( i ) )
      continue;
    const int column = static_cast<int>( i % static_cast<std::size_t>( grid.cols ) );
    const int row = static_cast<int>( i / static_cast<std::size_t>( grid.cols ) );
    const double dist = minDistanceToLines( cellCenter( grid, column, row ), selected );
    const double t = std::clamp( ( dist - width - guard ) / shoulder, 0.0, 1.0 );
    if ( !( t < 1.0 ) )
      continue;
    const double blend = t * t * t * ( 10.0 - 15.0 * t + 6.0 * t * t );
    work.values[i] = core + blend * ( analysis[i] - core );
    ++modified;
  }
  work.coreValue = core;
  work.bufferHalfWidth = width;
  work.numericalGuard = guard;
  work.transitionDistance = shoulder;
  work.modifiedCells = modified;
  work.unchanged = modified == 0;
  for ( const ConstraintLine *line : selected )
    work.usedConstraintIds.push_back( line->stableId );
  return work;
}

} // namespace paleo::singlefactor
