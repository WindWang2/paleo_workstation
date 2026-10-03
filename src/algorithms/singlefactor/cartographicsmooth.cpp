// 层：数据
#include "cartographicsmooth.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

// 逐项移植 constrained_engine.py L7038-7697 的折线辅助：
// 同名、同参、同序。_point_to_segment_distance 取 L7351 的有效定义
// （文件尾部重复定义覆盖 L4093 的旧实现）。

namespace paleo::singlefactor
{
namespace
{

double cross2( double ax, double ay, double bx, double by )
{
  return ax * by - ay * bx;
}

double contourCloseTolerance( double gridStep )
{
  return std::max( gridStep * 0.35, 1e-6 );
}

struct SegmentProjection
{
  double t;
  Point2 closest;
};

std::optional<SegmentProjection> projectPointToSegment( Point2 pt, Point2 a, Point2 b )
{
  const double dx = b.x - a.x;
  const double dy = b.y - a.y;
  const double lengthSq = dx * dx + dy * dy;
  if ( lengthSq <= 1e-24 )
    return SegmentProjection{ 0.0, Point2{ a.x, a.y } };
  const double t =
      ( ( pt.x - a.x ) * dx + ( pt.y - a.y ) * dy ) / lengthSq;
  const double clamped = std::max( 0.0, std::min( 1.0, t ) );
  return SegmentProjection{ clamped,
                            Point2{ a.x + clamped * dx, a.y + clamped * dy } };
}

double pointToSegmentDistance( Point2 pt, Point2 a, Point2 b )
{
  const auto projection = projectPointToSegment( pt, a, b );
  if ( !projection )
    return std::numeric_limits<double>::infinity();
  return std::hypot( pt.x - projection->closest.x, pt.y - projection->closest.y );
}

// _segment_intersection_point：严格横向相交（endpoint_tolerance=1e-7 默认）。
std::optional<Point2> segmentIntersectionPoint( Point2 a, Point2 b, Point2 c, Point2 d,
                                                double endpointTolerance = 1e-7 )
{
  const double rx = b.x - a.x, ry = b.y - a.y;
  const double sx = d.x - c.x, sy = d.y - c.y;
  const double denom = cross2( rx, ry, sx, sy );
  const double qpx = c.x - a.x, qpy = c.y - a.y;
  if ( std::abs( denom ) <= 1e-12 )
    return std::nullopt;
  const double t = cross2( qpx, qpy, sx, sy ) / denom;
  const double u = cross2( qpx, qpy, rx, ry ) / denom;
  if ( !( endpointTolerance < t && t < 1.0 - endpointTolerance &&
          endpointTolerance < u && u < 1.0 - endpointTolerance ) )
    return std::nullopt;
  return Point2{ a.x + t * rx, a.y + t * ry };
}

Polyline collapseGridStairs( const Polyline &points, double gridStep )
{
  if ( points.size() < 4 )
    return points;
  const double step = std::max( gridStep, 1e-9 );
  const bool closed = isClosedPolyline( points, step );
  Polyline core( points.begin(), closed ? points.end() - 1 : points.end() );
  if ( core.size() < 3 )
    return points;

  Polyline out{ core[0] };
  std::size_t i = 1;
  const std::size_t n = core.size();
  while ( i < n - 1 )
  {
    const Point2 p0 = out.back();
    const Point2 p1 = core[i];
    const Point2 p2 = core[i + 1];
    const double d01 = polylinePointDistance( p0, p1 );
    const double d12 = polylinePointDistance( p1, p2 );
    if ( d01 <= step * 2.8 && d12 <= step * 2.8 )
    {
      const double v1x = p1.x - p0.x, v1y = p1.y - p0.y;
      const double v2x = p2.x - p1.x, v2y = p2.y - p1.y;
      const double n1 = std::hypot( v1x, v1y );
      const double n2 = std::hypot( v2x, v2y );
      if ( n1 > 1e-12 && n2 > 1e-12 )
      {
        const double cosA = ( v1x * v2x + v1y * v2y ) / ( n1 * n2 );
        if ( std::abs( cosA ) < 0.35 )
        {
          ++i;
          continue;
        }
      }
    }
    out.push_back( p1 );
    ++i;
  }
  out.push_back( core.back() );
  if ( closed && !out.empty() &&
       ( out.front().x != out.back().x || out.front().y != out.back().y ) )
    out.push_back( out.front() );
  return out.size() >= 2 ? out : points;
}

Polyline movingAveragePolyline( const Polyline &points, double gridStep, int passes = 2 )
{
  if ( points.size() < 4 || passes <= 0 )
    return points;
  const double step = std::max( gridStep, 1e-9 );
  const bool closed = isClosedPolyline( points, step );
  Polyline cur( points.begin(), closed ? points.end() - 1 : points.end() );
  if ( cur.size() < 3 )
    return points;
  for ( int pass = 0; pass < std::max( 1, passes ); ++pass )
  {
    Polyline nxt;
    const std::size_t m = cur.size();
    for ( std::size_t i = 0; i < m; ++i )
    {
      if ( !closed && ( i == 0 || i == m - 1 ) )
      {
        nxt.push_back( cur[i] );
        continue;
      }
      // closed 时 i=0 的 cur[-1] 按 Python 语义取末点。
      const Point2 p0 = cur[( i + m - 1 ) % m];
      const Point2 p1 = cur[i];
      const Point2 p2 = cur[( i + 1 ) % m];
      nxt.push_back( Point2{ ( p0.x + 2.0 * p1.x + p2.x ) * 0.25,
                             ( p0.y + 2.0 * p1.y + p2.y ) * 0.25 } );
    }
    cur = std::move( nxt );
  }
  if ( closed && !cur.empty() )
    cur.push_back( cur.front() );
  return cur;
}

Polyline removePolylineSpikes( const Polyline &points, double gridStep,
                               double minTurnCos = -0.25 )
{
  if ( points.size() < 4 )
    return points;
  const double step = std::max( gridStep, 1e-9 );
  const bool closed = isClosedPolyline( points, step );
  Polyline core( points.begin(), closed ? points.end() - 1 : points.end() );
  if ( core.size() < 3 )
    return points;

  const auto keepVertex = [&]( Point2 prev, Point2 cur, Point2 nxt ) {
    const double v1x = cur.x - prev.x, v1y = cur.y - prev.y;
    const double v2x = nxt.x - cur.x, v2y = nxt.y - cur.y;
    const double n1 = std::hypot( v1x, v1y );
    const double n2 = std::hypot( v2x, v2y );
    if ( n1 < step * 0.15 || n2 < step * 0.15 )
      return false;
    const double cosA = ( v1x * v2x + v1y * v2y ) / ( n1 * n2 );
    if ( cosA < minTurnCos && n1 < step * 4.0 && n2 < step * 4.0 )
      return false;
    return true;
  };

  bool changed = true;
  int guard = 0;
  while ( changed && guard < 4 )
  {
    ++guard;
    changed = false;
    Polyline newCore;
    const std::size_t m = core.size();
    for ( std::size_t i = 0; i < m; ++i )
    {
      if ( !closed && ( i == 0 || i == m - 1 ) )
      {
        newCore.push_back( core[i] );
        continue;
      }
      // closed 时 i=0 的 core[-1] 按 Python 语义取末点。
      const Point2 prev = core[( i + m - 1 ) % m];
      const Point2 cur = core[i];
      const Point2 nxt = core[( i + 1 ) % m];
      if ( keepVertex( prev, cur, nxt ) )
        newCore.push_back( cur );
      else
        changed = true;
    }
    if ( newCore.size() < ( closed ? 3u : 2u ) )
      break;
    core = std::move( newCore );
  }
  if ( closed )
  {
    if ( !core.empty() &&
         ( core.front().x != core.back().x || core.front().y != core.back().y ) )
      core.push_back( core.front() );
    return core;
  }
  return core;
}

Polyline densifyPolylineSegments( const Polyline &points, double maxSeg )
{
  if ( points.size() < 2 )
    return points;
  const double maxS = std::max( maxSeg, 1e-9 );
  Polyline out{ Point2{ points[0].x, points[0].y } };
  for ( std::size_t i = 1; i < points.size(); ++i )
  {
    const double x0 = out.back().x, y0 = out.back().y;
    const double x1 = points[i].x, y1 = points[i].y;
    const double dist = std::hypot( x1 - x0, y1 - y0 );
    if ( dist > maxS * 1.05 )
    {
      const int n = std::max( 1, static_cast<int>( std::ceil( dist / maxS ) ) );
      for ( int k = 1; k < n; ++k )
      {
        const double t = k / static_cast<double>( n );
        out.push_back( Point2{ x0 + ( x1 - x0 ) * t, y0 + ( y1 - y0 ) * t } );
      }
    }
    out.push_back( Point2{ x1, y1 } );
  }
  return out;
}

} // namespace

bool isClosedPolyline( const Polyline &points, double gridStep )
{
  return points.size() > 2 &&
         polylinePointDistance( points.front(), points.back() ) <=
             contourCloseTolerance( gridStep );
}

double polylinePointDistance( Point2 a, Point2 b )
{
  return std::hypot( a.x - b.x, a.y - b.y );
}

double polylinePathLength( const Polyline &points )
{
  double length = 0;
  for ( std::size_t i = 1; i < points.size(); ++i )
    length += polylinePointDistance( points[i - 1], points[i] );
  return length;
}

Polyline dedupeConsecutivePoints( const Polyline &points, double tolerance )
{
  Polyline deduped;
  for ( const Point2 &pt : points )
  {
    const Point2 current{ pt.x, pt.y };
    if ( deduped.empty() ||
         polylinePointDistance( deduped.back(), current ) > tolerance )
      deduped.push_back( current );
  }
  return deduped;
}

Polyline rdpSimplify( const Polyline &points, double tolerance )
{
  if ( points.size() <= 2 )
    return points;
  const Point2 start = points.front();
  const Point2 end = points.back();
  double maxDistance = -1.0;
  std::size_t splitIndex = 0;
  for ( std::size_t index = 1; index < points.size() - 1; ++index )
  {
    const double distance = pointToSegmentDistance( points[index], start, end );
    if ( distance > maxDistance )
    {
      maxDistance = distance;
      splitIndex = index;
    }
  }
  if ( maxDistance > tolerance )
  {
    const Polyline left =
        rdpSimplify( Polyline( points.begin(), points.begin() + splitIndex + 1 ),
                     tolerance );
    const Polyline right =
        rdpSimplify( Polyline( points.begin() + splitIndex, points.end() ), tolerance );
    Polyline out( left.begin(), left.end() - 1 );
    out.insert( out.end(), right.begin(), right.end() );
    return out;
  }
  return Polyline{ start, end };
}

Polyline chaikinSmoothPolyline( const Polyline &points, int iterations, double gridStep )
{
  if ( points.size() <= 2 || iterations <= 0 )
    return points;
  const bool closed = isClosedPolyline( points, gridStep );
  Polyline current( points.begin(), closed ? points.end() - 1 : points.end() );
  if ( closed && current.size() < 3 )
    return points;

  for ( int it = 0; it < iterations; ++it )
  {
    if ( closed )
    {
      Polyline smoothed;
      for ( std::size_t index = 0; index < current.size(); ++index )
      {
        const Point2 p0 = current[index];
        const Point2 p1 = current[( index + 1 ) % current.size()];
        smoothed.push_back( Point2{ 0.75 * p0.x + 0.25 * p1.x,
                                    0.75 * p0.y + 0.25 * p1.y } );
        smoothed.push_back( Point2{ 0.25 * p0.x + 0.75 * p1.x,
                                    0.25 * p0.y + 0.75 * p1.y } );
      }
      current = std::move( smoothed );
    }
    else
    {
      Polyline smoothed{ current.front() };
      for ( std::size_t i = 1; i < current.size(); ++i )
      {
        const Point2 p0 = current[i - 1];
        const Point2 p1 = current[i];
        smoothed.push_back( Point2{ 0.75 * p0.x + 0.25 * p1.x,
                                    0.75 * p0.y + 0.25 * p1.y } );
        smoothed.push_back( Point2{ 0.25 * p0.x + 0.75 * p1.x,
                                    0.25 * p0.y + 0.75 * p1.y } );
      }
      smoothed.push_back( current.back() );
      current = std::move( smoothed );
    }
  }
  if ( closed )
    current.push_back( current.front() );
  return current;
}

bool polylineSelfIntersects( const Polyline &points, double gridStep )
{
  const std::size_t n = points.size();
  if ( n < 4 )
    return false;
  const bool closed = isClosedPolyline( points, gridStep );
  for ( std::size_t i = 0; i + 1 < n; ++i )
  {
    for ( std::size_t j = i + 2; j + 1 < n; ++j )
    {
      if ( closed && i == 0 && j == n - 2 )
        continue;
      if ( j - i <= 1 )
        continue;
      if ( segmentIntersectionPoint( points[i], points[i + 1], points[j], points[j + 1] ) )
        return true;
    }
  }
  return false;
}

Polyline cartographicSmoothPolyline( const Polyline &points, double gridStep,
                                     int iterations )
{
  const double step = std::max( gridStep, 1e-9 );
  Polyline base = collapseGridStairs( points, step );
  base = movingAveragePolyline( base, step, 3 );
  base = removePolylineSpikes( base, step, -0.05 );
  // 只去数值抖动：平滑后再做激进 RDP 会重新拉直出棱角。
  if ( base.size() > 4 )
  {
    const bool wasClosed = isClosedPolyline( base, step );
    Polyline core( base.begin(), wasClosed ? base.end() - 1 : base.end() );
    core = rdpSimplify( core, step * 0.04 );
    if ( wasClosed && core.size() >= 3 )
    {
      base = core;
      base.push_back( core.front() );
    }
    else
    {
      base = core;
    }
  }
  base = densifyPolylineSegments( base, step * 1.35 );
  base = dedupeConsecutivePoints( base, std::max( step * 1e-6, 1e-9 ) );
  if ( base.size() < 2 )
    return base;
  const int it = std::max( 0, std::min( 8, iterations ) );
  if ( it <= 0 )
    return base;
  if ( base.size() < 3 )
    return base;
  for ( int tryIt = it; tryIt > 0; --tryIt )
  {
    Polyline cand = chaikinSmoothPolyline( base, tryIt, step );
    cand = movingAveragePolyline( cand, step, 2 );
    if ( cand.size() > 6 )
    {
      const bool wasClosed = isClosedPolyline( cand, step );
      Polyline core( cand.begin(), wasClosed ? cand.end() - 1 : cand.end() );
      core = rdpSimplify( core, step * 0.025 );
      if ( wasClosed && core.size() >= 3 )
      {
        cand = core;
        cand.push_back( core.front() );
      }
      else
      {
        cand = core;
      }
      cand = densifyPolylineSegments( cand, step * 1.25 );
      cand = movingAveragePolyline( cand, step, 1 );
    }
    cand = dedupeConsecutivePoints( cand, std::max( step * 1e-6, 1e-9 ) );
    if ( cand.size() >= 2 && !polylineSelfIntersects( cand, step ) )
      return cand;
  }
  return base;
}

ContourLineMap cartographicSmoothContours( const ContourLineMap &contours,
                                           double gridStep, int iterations )
{
  const double step = std::max( gridStep, 1e-9 );
  ContourLineMap out;
  for ( const auto &[level, lines] : contours )
  {
    std::vector<Polyline> kept;
    for ( const Polyline &line : lines )
    {
      if ( line.size() < 2 )
        continue;
      const Polyline original = line;
      const bool originalClosed = isClosedPolyline( original, step );
      const double originalLength = polylinePathLength( original );
      Polyline sm = cartographicSmoothPolyline( line, step, iterations );
      // 峰顶最高级别的小环不能被平滑压扁成两点——不改拓扑。
      if ( originalClosed &&
           ( sm.size() < 4 || !isClosedPolyline( sm, step ) ||
             polylinePathLength( sm ) <
                 std::max( step * 0.5, originalLength * 0.25 ) ) )
        sm = original;
      if ( sm.size() >= 2 )
        kept.push_back( std::move( sm ) );
    }
    if ( !kept.empty() )
      out.emplace_back( level, std::move( kept ) );
  }
  return out;
}

} // namespace paleo::singlefactor
