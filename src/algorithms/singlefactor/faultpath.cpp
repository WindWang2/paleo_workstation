// 层：数据
#include "faultpath.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

// 层：数据
namespace paleo::singlefactor
{
namespace
{

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

using Seg = FaultSegment;

bool visible( Point2 a, Point2 b, const std::vector<Seg> &segments )
{
  for ( const Seg &seg : segments )
  {
    if ( segmentBlocksSight( a, b, seg.first, seg.second ) )
      return false;
  }
  return true;
}

// 全点集跨度（上游 np.ptp 取两轴最大）。
double spanOf( const std::vector<Point2> &points )
{
  double minX = kInf, maxX = -kInf, minY = kInf, maxY = -kInf;
  for ( const Point2 &p : points )
  {
    minX = std::min( minX, p.x );
    maxX = std::max( maxX, p.x );
    minY = std::min( minY, p.y );
    maxY = std::max( maxY, p.y );
  }
  if ( minX > maxX )
    return 0.0;
  return std::max( maxX - minX, maxY - minY );
}

// 解析绕行节点，按上游 shapely epsilon-buffer 外环顶点的拓扑逐顶点生成：
// - 中间顶点：双测地 mitre 对角点 v ± ε·(n₁+n₂)/(1+t₁·t₂)（封闭折线因此密封）；
// - 自由端：端点侧偏点 ±n̂ε 与沿切向越出端点的角点 t̂ε±n̂ε（有限断层的绕行出口）。
std::vector<Point2> detourNodes( std::span<const FaultLine> lines, double epsilon )
{
  std::vector<Point2> nodes;
  for ( const FaultLine &line : lines )
  {
    const std::vector<Point2> &pts = line.points;
    if ( pts.size() < 2 )
      continue;
    // 首尾重合视为闭合折线：首/末顶点按中间顶点参与 mitre，不再产自由端出口。
    const bool closed = pts.size() >= 4 &&
                        std::hypot( pts.front().x - pts.back().x, pts.front().y - pts.back().y ) <= 1e-12;
    for ( std::size_t i = 0; i < pts.size(); ++i )
    {
      const Point2 &v = pts[i];
      const bool hasPrev = i > 0 || ( closed && pts.size() >= 2 );
      const bool hasNext = i + 1 < pts.size() || ( closed && pts.size() >= 2 );
      double t1x = 0, t1y = 0, t2x = 0, t2y = 0;
      if ( hasPrev )
      {
        const Point2 &prev = i > 0 ? pts[i - 1] : pts[pts.size() - 2];
        const double dx = v.x - prev.x;
        const double dy = v.y - prev.y;
        const double len = std::hypot( dx, dy );
        if ( len > 1e-12 )
        {
          t1x = dx / len;
          t1y = dy / len;
        }
        else
        {
          // 与前点重合：当作无前段。
          t1x = kNaN;
        }
      }
      if ( hasNext )
      {
        const Point2 &next = i + 1 < pts.size() ? pts[i + 1] : pts[1];
        const double dx = next.x - v.x;
        const double dy = next.y - v.y;
        const double len = std::hypot( dx, dy );
        if ( len > 1e-12 )
        {
          t2x = dx / len;
          t2y = dy / len;
        }
        else
        {
          t2x = kNaN;
        }
      }
      const bool validPrev = hasPrev && !std::isnan( t1x );
      const bool validNext = hasNext && !std::isnan( t2x );
      if ( validPrev && validNext )
      {
        const double denom = 1.0 + ( t1x * t2x + t1y * t2y );
        if ( denom > 1e-9 )
        {
          const double l1x = -t1y, l1y = t1x;
          const double l2x = -t2y, l2y = t2x;
          const double mx = ( l1x + l2x ) / denom * epsilon;
          const double my = ( l1y + l2y ) / denom * epsilon;
          nodes.push_back( Point2{ v.x + mx, v.y + my } );
          nodes.push_back( Point2{ v.x - mx, v.y - my } );
        }
        else
        {
          // 折返顶点：mitre 发散，退化为两侧端点偏移。
          nodes.push_back( Point2{ v.x + -t1y * epsilon, v.y + t1x * epsilon } );
          nodes.push_back( Point2{ v.x - -t1y * epsilon, v.y - t1x * epsilon } );
          nodes.push_back( Point2{ v.x + -t2y * epsilon, v.y + t2x * epsilon } );
          nodes.push_back( Point2{ v.x - -t2y * epsilon, v.y - t2x * epsilon } );
        }
      }
      else if ( validNext )
      {
        // 起端：切向指入线内，越端角点在外侧反向。
        const double nx = -t2y * epsilon;
        const double ny = t2x * epsilon;
        nodes.push_back( Point2{ v.x + nx, v.y + ny } );
        nodes.push_back( Point2{ v.x - nx, v.y - ny } );
        nodes.push_back( Point2{ v.x - t2x * epsilon + nx, v.y - t2y * epsilon + ny } );
        nodes.push_back( Point2{ v.x - t2x * epsilon - nx, v.y - t2y * epsilon - ny } );
      }
      else if ( validPrev )
      {
        // 终端：切向指离线内，越端角点顺切向越出。
        const double nx = -t1y * epsilon;
        const double ny = t1x * epsilon;
        nodes.push_back( Point2{ v.x + nx, v.y + ny } );
        nodes.push_back( Point2{ v.x - nx, v.y - ny } );
        nodes.push_back( Point2{ v.x + t1x * epsilon + nx, v.y + t1y * epsilon + ny } );
        nodes.push_back( Point2{ v.x + t1x * epsilon - nx, v.y + t1y * epsilon - ny } );
      }
    }
  }
  std::sort( nodes.begin(), nodes.end(), []( const Point2 &l, const Point2 &r ) {
    return l.x < r.x || ( l.x == r.x && l.y < r.y );
  } );
  nodes.erase( std::unique( nodes.begin(), nodes.end(), []( const Point2 &l, const Point2 &r ) {
    return l.x == r.x && l.y == r.y;
  } ), nodes.end() );
  return nodes;
}

} // namespace

bool segmentBlocksSight( Point2 a, Point2 b, Point2 c, Point2 d )
{
  const double rx = b.x - a.x;
  const double ry = b.y - a.y;
  const double sx = d.x - c.x;
  const double sy = d.y - c.y;
  const double den = rx * sy - ry * sx;
  if ( std::abs( den ) > 1e-16 )
  {
    const double qx = c.x - a.x;
    const double qy = c.y - a.y;
    const double t = ( qx * sy - qy * sx ) / den;
    const double u = ( qx * ry - qy * rx ) / den;
    return t > 1e-10 && t < 1.0 - 1e-10 && u >= -1e-10 && u <= 1.0 + 1e-10;
  }
  return false;
}

FaultPathMetric::FaultPathMetric( std::span<const Point2> wells, std::span<const FaultLine> barriers,
                                  double ratio, double angleDegrees )
    : mWells( wells.begin(), wells.end() ),
      mRatio( ratio ),
      mAngleRadians( angleDegrees * std::acos( -1.0 ) / 180.0 )
{
  for ( const FaultLine &line : barriers )
  {
    for ( std::size_t i = 1; i < line.points.size(); ++i )
    {
      const Seg seg{ line.points[i - 1], line.points[i] };
      if ( std::hypot( seg.second.x - seg.first.x, seg.second.y - seg.first.y ) > 1e-12 )
        mSegments.push_back( seg );
    }
  }
  if ( mSegments.empty() || mWells.empty() )
    return;

  std::vector<Point2> all = mWells;
  for ( const FaultLine &line : barriers )
    all.insert( all.end(), line.points.begin(), line.points.end() );
  const double epsilon = std::max( spanOf( all ), 1.0 ) * 1e-9;
  const std::vector<Point2> nodes = detourNodes( barriers, epsilon );
  const std::size_t n = nodes.size();
  if ( n == 0 )
    return;

  std::vector<double> graph( n * n, kInf );
  for ( std::size_t i = 0; i < n; ++i )
  {
    graph[i * n + i] = 0.0;
    for ( std::size_t j = i + 1; j < n; ++j )
    {
      double d = kInf;
      if ( visible( nodes[i], nodes[j], mSegments ) )
      {
        const double dx = nodes[i].x - nodes[j].x;
        const double dy = nodes[i].y - nodes[j].y;
        d = std::hypot( dx, dy );
      }
      graph[i * n + j] = d;
      graph[j * n + i] = d;
    }
  }
  // Floyd-Warshall（等价 scipy shortest_path 的最短距离）。
  for ( std::size_t k = 0; k < n; ++k )
  {
    for ( std::size_t i = 0; i < n; ++i )
    {
      const double dik = graph[i * n + k];
      if ( !std::isfinite( dik ) )
        continue;
      for ( std::size_t j = 0; j < n; ++j )
      {
        const double alt = dik + graph[k * n + j];
        if ( alt < graph[i * n + j] )
          graph[i * n + j] = alt;
      }
    }
  }

  const std::size_t wellCount = mWells.size();
  std::vector<double> exitDist( n * wellCount, kInf );
  for ( std::size_t k = 0; k < n; ++k )
  {
    for ( std::size_t i = 0; i < wellCount; ++i )
    {
      if ( visible( nodes[k], mWells[i], mSegments ) )
        exitDist[k * wellCount + i] = metricDistance( nodes[k], mWells[i] );
    }
  }
  std::vector<double> toWells( n * wellCount, kInf );
  for ( std::size_t k = 0; k < n; ++k )
  {
    for ( std::size_t i = 0; i < wellCount; ++i )
    {
      double best = kInf;
      for ( std::size_t k2 = 0; k2 < n; ++k2 )
      {
        const double via = graph[k * n + k2] + exitDist[k2 * wellCount + i];
        if ( via < best )
          best = via;
      }
      toWells[k * wellCount + i] = best;
    }
  }

  NodeGraph built;
  built.nodes = nodes;
  built.allPairs = std::move( graph );
  built.toWells = std::move( toWells );
  mPaths = std::move( built );
}

double FaultPathMetric::metricDistance( Point2 a, Point2 b ) const
{
  const double dx = a.x - b.x;
  const double dy = a.y - b.y;
  if ( mRatio == 1.0 )
    return std::hypot( dx, dy );
  const double c = std::cos( mAngleRadians );
  const double s = std::sin( mAngleRadians );
  const double along = dx * c + dy * s;
  const double across = -dx * s + dy * c;
  return std::hypot( along / mRatio, across );
}

std::vector<double> FaultPathMetric::distances( std::span<const Point2> points ) const
{
  const std::size_t wellCount = mWells.size();
  std::vector<double> lengths( points.size() * wellCount );
  for ( std::size_t q = 0; q < points.size(); ++q )
  {
    for ( std::size_t i = 0; i < wellCount; ++i )
      lengths[q * wellCount + i] = metricDistance( points[q], mWells[i] );
  }
  if ( !mPaths )
    return lengths;

  const auto &graph = *mPaths;
  const std::size_t n = graph.nodes.size();
  std::vector<double> via( n );
  std::vector<unsigned char> viaVisible( n );
  for ( std::size_t q = 0; q < points.size(); ++q )
  {
    bool anyBlocked = false;
    for ( std::size_t i = 0; i < wellCount && !anyBlocked; ++i )
    {
      if ( !visible( points[q], mWells[i], mSegments ) )
        anyBlocked = true;
    }
    if ( !anyBlocked )
      continue;
    for ( std::size_t k = 0; k < n; ++k )
    {
      viaVisible[k] = visible( points[q], graph.nodes[k], mSegments ) ? 1 : 0;
      via[k] = viaVisible[k] ? metricDistance( points[q], graph.nodes[k] ) : kInf;
    }
    for ( std::size_t i = 0; i < wellCount; ++i )
    {
      if ( visible( points[q], mWells[i], mSegments ) )
        continue;
      double best = kInf;
      for ( std::size_t k = 0; k < n; ++k )
      {
        if ( !viaVisible[k] )
          continue;
        const double candidate = via[k] + graph.toWells[k * wellCount + i];
        if ( candidate < best )
          best = candidate;
      }
      // 与上游 np.min 一致：完全不可达时写 inf（样本被隔离），不保留直达距离。
      lengths[q * wellCount + i] = best;
    }
  }
  return lengths;
}

SurferIdwResult interpolateGlobalIdw( std::span<const Point2> points,
                                      std::span<const Point2> wellXy,
                                      std::span<const double> wellValues,
                                      const SurferIdwOptions &options,
                                      std::span<const FaultLine> barriers,
                                      const Control *control )
{
  SurferIdwResult result;
  if ( points.size() && wellXy.size() == 0 )
  {
    result.status = Status::InvalidInput;
    result.message = "IDW 输入必须是 XY 查询点与 XYZ 井点";
    return result;
  }
  if ( wellXy.size() != wellValues.size() )
  {
    result.status = Status::InvalidInput;
    result.message = "IDW 输入必须是 XY 查询点与 XYZ 井点";
    return result;
  }
  auto finiteWells = [&]()
  {
    for ( std::size_t i = 0; i < wellXy.size(); ++i )
      if ( !std::isfinite( wellXy[i].x ) || !std::isfinite( wellXy[i].y ) ||
           !std::isfinite( wellValues[i] ) )
        return false;
    for ( const Point2 &p : points )
      if ( !std::isfinite( p.x ) || !std::isfinite( p.y ) )
        return false;
    return true;
  };
  if ( !finiteWells() )
  {
    result.status = Status::InvalidInput;
    result.message = "IDW 需要有限的坐标和井值";
    return result;
  }
  if ( !std::isfinite( options.power ) || !( options.power > 0 ) ||
       !std::isfinite( options.anisotropyRatio ) || !( options.anisotropyRatio > 0 ) ||
       !std::isfinite( options.anisotropyAngleDegrees ) )
  {
    result.status = Status::InvalidInput;
    result.message = "距离幂次及各向异性比必须大于零";
    return result;
  }

  // 断层绕行图（度量带各向异性；上游 surfer_idw 用 span*1e-8 的数值偏移，
  // FaultPathMetric 构造内统一取节点与图）。
  const FaultPathMetric metric( wellXy, barriers, options.anisotropyRatio,
                                options.anisotropyAngleDegrees );

  const std::size_t wellCount = wellXy.size();
  result.values.assign( points.size(), kNaN );
  std::vector<double> row( wellCount );
  std::vector<double> euclid( wellCount );
  std::vector<std::size_t> order( wellCount );
  for ( std::size_t q = 0; q < points.size(); ++q )
  {
    if ( control && control->cancelled && control->cancelled() )
    {
      result.status = Status::Cancelled;
      result.message = "已取消";
      result.values.clear();
      return result;
    }
    const std::vector<double> distances = metric.distances( { &points[q], 1 } );
    for ( std::size_t i = 0; i < wellCount; ++i )
      row[i] = distances[i];

    if ( options.searchRadius )
    {
      for ( std::size_t i = 0; i < wellCount; ++i )
        euclid[i] = std::hypot( points[q].x - wellXy[i].x, points[q].y - wellXy[i].y );
      for ( std::size_t i = 0; i < wellCount; ++i )
        if ( euclid[i] > *options.searchRadius )
          row[i] = kInf;
    }
    if ( options.maxPoints > 0 && options.maxPoints < static_cast<int>( wellCount ) )
    {
      std::iota( order.begin(), order.end(), std::size_t{ 0 } );
      std::stable_sort( order.begin(), order.end(),
                        [&]( std::size_t a, std::size_t b ) { return row[a] < row[b]; } );
      for ( std::size_t rank = static_cast<std::size_t>( options.maxPoints ); rank < wellCount; ++rank )
        row[order[rank]] = kInf;
    }

    int validCount = 0;
    double nearest = kInf;
    for ( std::size_t i = 0; i < wellCount; ++i )
    {
      if ( std::isfinite( row[i] ) )
      {
        ++validCount;
        nearest = std::min( nearest, row[i] );
      }
    }
    const double safeNearest = std::isfinite( nearest ) ? std::max( nearest, 1e-10 ) : 1.0;

    double weightSum = 0;
    double weighted = 0;
    int exactCount = 0;
    double exactSum = 0;
    for ( std::size_t i = 0; i < wellCount; ++i )
    {
      if ( !std::isfinite( row[i] ) )
        continue;
      if ( row[i] <= 1e-10 )
      {
        ++exactCount;
        exactSum += wellValues[i];
        continue;
      }
      const double scaled = std::max( row[i], 1e-10 ) / safeNearest;
      const double weight = 1.0 / std::pow( scaled, options.power );
      weightSum += weight;
      weighted += weight * wellValues[i];
    }
    double local = weighted / std::max( weightSum, 1e-30 );
    if ( validCount < std::max( 1, options.minPoints ) )
      local = kNaN;
    if ( exactCount > 0 )
      local = exactSum / exactCount;
    result.values[q] = local;
    if ( control && control->progress && ( ( q + 1 ) % 1024 == 0 || q + 1 == points.size() ) )
      control->progress( static_cast<double>( q + 1 ) / static_cast<double>( points.size() ) );
  }
  return result;
}

} // namespace paleo::singlefactor
