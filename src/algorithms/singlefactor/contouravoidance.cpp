// 层：数据
#include "contouravoidance.h"

#include "partition.h"
#include "support.h"

#include <algorithm>
#include <cmath>
#include <limits>

// 层：数据
namespace paleo::singlefactor
{
namespace
{

struct WallProjection
{
  double distance = std::numeric_limits<double>::infinity();
  double side = 0; // 最近段左法向符号：>0 左，<0 右
};

WallProjection projectToWall( Point2 point, const std::vector<Point2> &wall )
{
  WallProjection best;
  for ( std::size_t i = 1; i < wall.size(); ++i )
  {
    const Point2 a = wall[i - 1];
    const Point2 b = wall[i];
    const double vx = b.x - a.x;
    const double vy = b.y - a.y;
    const double len2 = vx * vx + vy * vy;
    if ( !( len2 > 0.0 ) )
      continue;
    const double t = std::clamp( ( ( point.x - a.x ) * vx + ( point.y - a.y ) * vy ) / len2, 0.0, 1.0 );
    const double cx = a.x + t * vx;
    const double cy = a.y + t * vy;
    const double dist = std::hypot( point.x - cx, point.y - cy );
    if ( dist < best.distance )
    {
      best.distance = dist;
      const double length = std::sqrt( len2 );
      best.side = ( point.x - cx ) * ( -vy / length ) + ( point.y - cy ) * ( vx / length );
    }
  }
  return best;
}

// 折线密化：相邻点距不超过 spacing。
std::vector<Point2> densify( const std::vector<Point2> &points, double spacing )
{
  if ( points.size() < 2 || !( spacing > 0.0 ) )
    return points;
  std::vector<Point2> out;
  out.push_back( points.front() );
  for ( std::size_t i = 1; i < points.size(); ++i )
  {
    const Point2 a = points[i - 1];
    const Point2 b = points[i];
    const double len = std::hypot( b.x - a.x, b.y - a.y );
    const int steps = std::max( 1, static_cast<int>( std::ceil( len / spacing ) ) );
    for ( int s = 1; s <= steps; ++s )
    {
      const double t = static_cast<double>( s ) / steps;
      out.push_back( Point2{ a.x + t * ( b.x - a.x ), a.y + t * ( b.y - a.y ) } );
    }
  }
  return out;
}

// 缓冲外环（近似 flat-cap 圆端缓冲边界）：左侧链 → 端帽弧 → 右侧链 → 起帽弧。
// 记为一条首尾相接的折线；绕行沿环取弧，尖端绕行因此可表达。
std::vector<Point2> bufferRing( const std::vector<Point2> &wall, double offset, double spacing )
{
  const std::vector<Point2> dense = densify( wall, spacing );
  if ( dense.size() < 2 )
    return dense;
  const auto vertexNormal = [&]( std::size_t i, double &nx, double &ny ) {
    nx = 0;
    ny = 0;
    for ( const std::size_t j : { i == 0 ? i : i - 1, i + 1 == dense.size() ? i : i + 1 } )
    {
      if ( j == i )
        continue;
      const double dx = dense[j].x - dense[i].x;
      const double dy = dense[j].y - dense[i].y;
      const double len = std::hypot( dx, dy );
      if ( len > 1e-12 )
      {
        nx += -dy / len;
        ny += dx / len;
      }
    }
    const double norm = std::hypot( nx, ny );
    if ( norm > 1e-12 )
    {
      nx /= norm;
      ny /= norm;
    }
    else
    {
      nx = 0;
      ny = 1;
    }
  };

  const std::size_t n = dense.size();
  std::vector<Point2> left( n ), right( n );
  for ( std::size_t i = 0; i < n; ++i )
  {
    double nx = 0, ny = 0;
    vertexNormal( i, nx, ny );
    left[i] = Point2{ dense[i].x + nx * offset, dense[i].y + ny * offset };
    right[i] = Point2{ dense[i].x - nx * offset, dense[i].y - ny * offset };
  }

  const double kPi = std::acos( -1.0 );
  const auto capArc = []( Point2 center, double a0, double a1, double radius ) {
    std::vector<Point2> arc;
    constexpr int kSamples = 8;
    for ( int s = 0; s <= kSamples; ++s )
    {
      const double t = static_cast<double>( s ) / kSamples;
      const double ang = a0 + t * ( a1 - a0 );
      arc.push_back( Point2{ center.x + std::cos( ang ) * radius, center.y + std::sin( ang ) * radius } );
    }
    return arc;
  };

  // 端帽：法向角 → 反向经切向外侧扫半圆。
  const auto tangentAngle = []( const Point2 &v, const Point2 &next ) {
    return std::atan2( next.y - v.y, next.x - v.x );
  };
  std::vector<Point2> ring;
  ring.reserve( 2 * n + 20 );
  ring.insert( ring.end(), left.begin(), left.end() );
  {
    const double thetaT = tangentAngle( dense[n - 2], dense[n - 1] );
    const double aLeft = thetaT + kPi / 2.0;
    auto arc = capArc( dense[n - 1], aLeft, aLeft - kPi, offset );
    ring.insert( ring.end(), arc.begin(), arc.end() );
  }
  ring.insert( ring.end(), right.rbegin(), right.rend() );
  {
    const double thetaT = tangentAngle( dense[0], dense[1] );
    const double aRight = thetaT - kPi / 2.0;
    auto arc = capArc( dense[0], aRight, aRight - kPi, offset );
    ring.insert( ring.end(), arc.begin(), arc.end() );
  }
  ring.push_back( ring.front() );
  return ring;
}

bool crossesWall( Point2 a, Point2 b, const std::vector<Point2> &wall )
{
  for ( std::size_t i = 1; i < wall.size(); ++i )
    if ( closedSegmentsIntersect( a, b, wall[i - 1], wall[i] ) )
      return true;
  return false;
}

bool staysOutsideBand( const std::vector<Point2> &points, const std::vector<Point2> &wall,
                       double width )
{
  for ( const Point2 &p : points )
  {
    if ( projectToWall( p, wall ).distance < width * 0.95 )
      return false;
  }
  return true;
}

std::size_t nearestIndex( Point2 point, const std::vector<Point2> &ring )
{
  std::size_t best = 0;
  double bestDist = std::numeric_limits<double>::infinity();
  for ( std::size_t i = 0; i < ring.size(); ++i )
  {
    const double dist = std::hypot( point.x - ring[i].x, point.y - ring[i].y );
    if ( dist < bestDist )
    {
      bestDist = dist;
      best = i;
    }
  }
  return best;
}

// 环上 [i, j] 的两条弧（行进方向两种），返回较短者优先的候选。
std::vector<std::vector<Point2>> ringArcs( const std::vector<Point2> &ring, std::size_t i,
                                           std::size_t j )
{
  std::vector<std::vector<Point2>> arcs;
  const std::size_t n = ring.size();
  if ( n < 2 )
    return arcs;
  std::vector<Point2> forward;
  if ( i <= j )
  {
    for ( std::size_t k = i; k <= j; ++k )
      forward.push_back( ring[k] );
  }
  else
  {
    for ( std::size_t k = i; k < n; ++k )
      forward.push_back( ring[k] );
    for ( std::size_t k = 0; k <= j; ++k )
      forward.push_back( ring[k] );
  }
  std::vector<Point2> backward;
  if ( j <= i )
  {
    for ( std::size_t k = i; k != j; --k )
    {
      backward.push_back( ring[k] );
      if ( k == 0 )
        break;
    }
    backward.push_back( ring[j] );
  }
  else
  {
    for ( std::size_t k = i; k != j; --k )
    {
      backward.push_back( ring[k] );
      if ( k == 0 )
        break;
    }
    backward.push_back( ring[j] );
  }
  if ( forward.size() <= backward.size() )
  {
    arcs.push_back( std::move( forward ) );
    arcs.push_back( std::move( backward ) );
  }
  else
  {
    arcs.push_back( std::move( backward ) );
    arcs.push_back( std::move( forward ) );
  }
  return arcs;
}

// 缓冲外的原始片段（含越线交点）——截断与回退共用。
std::vector<std::vector<Point2>> clipOutsideArcs( const std::vector<Point2> &pts,
                                                  const std::vector<WallProjection> &proj,
                                                  double width )
{
  std::vector<std::vector<Point2>> arcs;
  std::vector<Point2> current;
  for ( std::size_t i = 0; i < pts.size(); ++i )
  {
    if ( proj[i].distance >= width )
    {
      current.push_back( pts[i] );
      continue;
    }
    if ( i > 0 && proj[i - 1].distance >= width )
    {
      const double d0 = proj[i - 1].distance;
      const double d1 = proj[i].distance;
      const double t = std::clamp( ( width - d0 ) / std::max( d1 - d0, 1e-15 ), 0.0, 1.0 );
      current.push_back( Point2{ pts[i - 1].x + t * ( pts[i].x - pts[i - 1].x ),
                                 pts[i - 1].y + t * ( pts[i].y - pts[i - 1].y ) } );
    }
    if ( current.size() >= 2 )
      arcs.push_back( std::move( current ) );
    current.clear();
    if ( i + 1 < pts.size() && proj[i + 1].distance >= width )
    {
      const double d0 = proj[i].distance;
      const double d1 = proj[i + 1].distance;
      const double t = std::clamp( ( width - d0 ) / std::max( d1 - d0, 1e-15 ), 0.0, 1.0 );
      current.push_back( Point2{ pts[i].x + t * ( pts[i + 1].x - pts[i].x ),
                                 pts[i].y + t * ( pts[i + 1].y - pts[i].y ) } );
    }
  }
  if ( current.size() >= 2 )
    arcs.push_back( std::move( current ) );
  return arcs;
}

} // namespace

ContourAvoidResult avoidContourBuffers( const std::vector<ContourPolyline> &contours,
                                        const std::vector<Point2> &wall, double width,
                                        double step )
{
  ContourAvoidResult result;
  if ( wall.size() < 2 || !( width > 0.0 ) || !std::isfinite( width ) )
  {
    result.contours = contours;
    return result;
  }
  const double lane = std::max( step * 0.6, width * 0.08 );
  const double spacing = std::clamp( std::min( step, width * 0.25 ), 1e-6, width + lane );
  const std::vector<Point2> ring = bufferRing( wall, width + lane, spacing );

  struct Working
  {
    ContourPolyline original;
    std::vector<std::vector<Point2>> pieces;
    bool rerouted = false;
  };
  std::vector<Working> working;
  working.reserve( contours.size() );

  for ( const ContourPolyline &contour : contours )
  {
    Working w;
    w.original = contour;
    const std::vector<Point2> &pts = contour.points;
    if ( pts.size() < 2 )
    {
      working.push_back( std::move( w ) );
      continue;
    }
    std::vector<WallProjection> proj( pts.size() );
    for ( std::size_t i = 0; i < pts.size(); ++i )
      proj[i] = projectToWall( pts[i], wall );

    std::vector<Point2> current; // 当前在写的线片段（绕行成功时延续）
    bool anyReroute = false;
    // 绕行尝试：沿缓冲外环的两条弧（短者优先）把 entry→exit 改道；
    // 弧长超过 max(width*6, 弦长*3) 视为绕不过去（上游同口径），改走截断。
    auto tryReroute = [&]( Point2 entry, Point2 exit ) -> bool {
      if ( ring.size() < 2 )
        return false;
      const std::size_t ra = nearestIndex( entry, ring );
      const std::size_t rb = nearestIndex( exit, ring );
      const double chord = std::hypot( exit.x - entry.x, exit.y - entry.y );
      const double lengthCap = std::max( width * 6.0, chord * 3.0 );
      for ( const std::vector<Point2> &arc : ringArcs( ring, ra, rb ) )
      {
        double arcLength = 0;
        for ( std::size_t k = 1; k < arc.size(); ++k )
          arcLength += std::hypot( arc[k].x - arc[k - 1].x, arc[k].y - arc[k - 1].y );
        if ( arcLength > lengthCap )
          continue;
        std::vector<Point2> detour;
        detour.push_back( entry );
        detour.insert( detour.end(), arc.begin(), arc.end() );
        detour.push_back( exit );
        bool ok = staysOutsideBand( detour, wall, width );
        for ( std::size_t k = 1; ok && k < detour.size(); ++k )
          if ( crossesWall( detour[k - 1], detour[k], wall ) )
            ok = false;
        if ( !ok )
          continue;
        current.insert( current.end(), detour.begin() + 1, detour.end() );
        ++result.rerouted;
        anyReroute = true;
        return true;
      }
      return false;
    };
    // 截断：当前片段收尾，下一段从 exit 起线。
    auto cutHere = [&]( Point2 exit ) {
      if ( current.size() >= 2 )
        w.pieces.push_back( std::move( current ) );
      current.clear();
      current.push_back( exit );
      ++result.truncated;
    };
    const auto edgeBandEntryExit = [&]( std::size_t i, Point2 *entry, Point2 *exit ) {
      const Point2 mid{ 0.5 * ( pts[i].x + pts[i + 1].x ), 0.5 * ( pts[i].y + pts[i + 1].y ) };
      const double wm = projectToWall( mid, wall ).distance;
      const double w0 = proj[i].distance;
      const double t0 = std::clamp( ( width - w0 ) / std::max( wm - w0, 1e-15 ), 0.0, 1.0 );
      *entry = Point2{ pts[i].x + t0 * ( mid.x - pts[i].x ), pts[i].y + t0 * ( mid.y - pts[i].y ) };
      const double w1 = proj[i + 1].distance;
      const double t1 = std::clamp( ( width - w1 ) / std::max( wm - w1, 1e-15 ), 0.0, 1.0 );
      *exit = Point2{ pts[i + 1].x + t1 * ( mid.x - pts[i + 1].x ), pts[i + 1].y + t1 * ( mid.y - pts[i + 1].y ) };
    };

    for ( std::size_t i = 0; i < pts.size(); ++i )
    {
      if ( proj[i].distance >= width )
      {
        current.push_back( pts[i] );
        // 跨带边：两端都在带外，但边直接穿墙或中点入带——按合成内段处理。
        if ( i + 1 < pts.size() && proj[i + 1].distance >= width )
        {
          const Point2 mid{ 0.5 * ( pts[i].x + pts[i + 1].x ), 0.5 * ( pts[i].y + pts[i + 1].y ) };
          if ( projectToWall( mid, wall ).distance < width || crossesWall( pts[i], pts[i + 1], wall ) )
          {
            Point2 entry{}, exit{};
            edgeBandEntryExit( i, &entry, &exit );
            current.push_back( entry );
            if ( !tryReroute( entry, exit ) )
              cutHere( exit );
          }
        }
        continue;
      }
      // 进入缓冲：入口交点（若有前置外段）。
      if ( i > 0 && proj[i - 1].distance >= width )
      {
        const double d0 = proj[i - 1].distance;
        const double d1 = proj[i].distance;
        const double t = std::clamp( ( width - d0 ) / std::max( d1 - d0, 1e-15 ), 0.0, 1.0 );
        const Point2 entry{ pts[i - 1].x + t * ( pts[i].x - pts[i - 1].x ),
                            pts[i - 1].y + t * ( pts[i].y - pts[i - 1].y ) };
        if ( current.empty() )
          current.push_back( pts[i - 1] );
        current.push_back( entry );
      }
      // 内段终结处：exit 交点 + 绕行尝试。
      std::size_t runEnd = i;
      while ( runEnd + 1 < pts.size() && proj[runEnd + 1].distance < width )
        ++runEnd;
      const bool bounded = runEnd + 1 < pts.size();
      Point2 exit = pts[runEnd];
      if ( bounded )
      {
        const double d0 = proj[runEnd].distance;
        const double d1 = proj[runEnd + 1].distance;
        const double t = std::clamp( ( width - d0 ) / std::max( d1 - d0, 1e-15 ), 0.0, 1.0 );
        exit = Point2{ pts[runEnd].x + t * ( pts[runEnd + 1].x - pts[runEnd].x ),
                       pts[runEnd].y + t * ( pts[runEnd + 1].y - pts[runEnd].y ) };
      }

      bool rerouted = false;
      if ( bounded && !current.empty() )
        rerouted = tryReroute( current.back(), exit );
      if ( !rerouted )
        cutHere( bounded ? exit : pts[runEnd] );
      i = runEnd; // for 循环 ++i 跳过内段
    }
    if ( current.size() >= 2 )
      w.pieces.push_back( std::move( current ) );
    w.rerouted = anyReroute;
    working.push_back( std::move( w ) );
  }

  // 绕行线不得与他线相交：冲突时回退为纯截断（不删邻居线）。
  std::vector<std::vector<std::vector<Point2>>> finals;
  finals.reserve( working.size() );
  for ( const Working &w : working )
    finals.push_back( w.pieces );
  for ( std::size_t wi = 0; wi < working.size(); ++wi )
  {
    if ( !working[wi].rerouted || finals[wi].empty() )
      continue;
    bool conflict = false;
    for ( const auto &piece : finals[wi] )
    {
      for ( std::size_t k = 1; !conflict && k < piece.size(); ++k )
      {
        for ( std::size_t wj = 0; !conflict && wj < working.size(); ++wj )
        {
          if ( wj == wi )
            continue;
          for ( const auto &other : finals[wj] )
          {
            for ( std::size_t m = 1; m < other.size(); ++m )
            {
              if ( closedSegmentsIntersect( piece[k - 1], piece[k], other[m - 1], other[m] ) )
              {
                conflict = true;
                break;
              }
            }
            if ( conflict )
              break;
          }
        }
      }
      if ( conflict )
        break;
    }
    if ( conflict )
    {
      const std::vector<Point2> &orig = working[wi].original.points;
      std::vector<WallProjection> proj( orig.size() );
      for ( std::size_t k = 0; k < orig.size(); ++k )
        proj[k] = projectToWall( orig[k], wall );
      finals[wi] = clipOutsideArcs( orig, proj, width );
      --result.rerouted;
      ++result.truncated;
    }
  }

  for ( std::size_t wi = 0; wi < working.size(); ++wi )
  {
    for ( auto &piece : finals[wi] )
    {
      ContourPolyline poly;
      poly.points = std::move( piece );
      poly.level = working[wi].original.level;
      if ( poly.points.size() >= 2 )
        result.contours.push_back( std::move( poly ) );
    }
  }
  return result;
}

} // namespace paleo::singlefactor
