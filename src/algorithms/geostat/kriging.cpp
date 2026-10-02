// 层：数据
#include "kriging.h"

#include "linsolve.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <utility>

namespace paleo::geostat
{

namespace
{

bool coincident( const Sample &a, const Sample &b )
{
  const double ex = 1e-9 * std::max( { 1.0, std::fabs( a.x ), std::fabs( b.x ) } );
  const double ey = 1e-9 * std::max( { 1.0, std::fabs( a.y ), std::fabs( b.y ) } );
  return std::fabs( a.x - b.x ) <= ex && std::fabs( a.y - b.y ) <= ey;
}

// 重合样本合并取均值（preserve 均值口径，防克氏矩阵奇异）。
std::vector<Sample> dedupeSamples( const std::vector<Sample> &samples, int *mergedCount )
{
  std::vector<Sample> finite;
  finite.reserve( samples.size() );
  for ( const Sample &sample : samples )
  {
    if ( std::isfinite( sample.x ) && std::isfinite( sample.y ) && std::isfinite( sample.value ) )
      finite.push_back( sample );
  }
  std::sort( finite.begin(), finite.end(), []( const Sample &a, const Sample &b ) {
    if ( a.x != b.x )
      return a.x < b.x;
    return a.y < b.y;
  } );
  std::vector<Sample> merged;
  std::vector<std::size_t> mergedCounts;
  merged.reserve( finite.size() );
  for ( const Sample &sample : finite )
  {
    if ( !merged.empty() && coincident( merged.back(), sample ) )
    {
      merged.back().value += sample.value;
      ++mergedCounts.back();
      if ( mergedCount )
        ++( *mergedCount );
      continue;
    }
    merged.push_back( sample );
    mergedCounts.push_back( 1 );
  }
  for ( std::size_t i = 0; i < merged.size(); ++i )
    merged[i].value /= static_cast<double>( mergedCounts[i] );
  return merged;
}

// 桶格空间索引：O(1) 定位 + 环形扩张收集最近 K 点。
class NeighborIndex
{
public:
  static NeighborIndex build( const std::vector<Sample> &points )
  {
    NeighborIndex index;
    index.m_points = points;
    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double maxY = std::numeric_limits<double>::lowest();
    for ( const Sample &sample : points )
    {
      minX = std::min( minX, sample.x );
      minY = std::min( minY, sample.y );
      maxX = std::max( maxX, sample.x );
      maxY = std::max( maxY, sample.y );
    }
    index.m_minX = minX;
    index.m_minY = minY;
    const std::size_t n = points.size();
    const int side = std::max<int>( 1, static_cast<int>( std::ceil( std::sqrt( static_cast<double>( n ) ) ) ) );
    const double span = std::max( maxX - minX, maxY - minY );
    index.m_bucketSize = span > 0 ? span / side : 1.0;
    index.m_bucketsX = side;
    index.m_bucketsY = side;
    index.m_buckets.assign( static_cast<std::size_t>( side ) * side, {} );
    for ( std::size_t i = 0; i < n; ++i )
    {
      const std::uint32_t bx = index.bucketX( points[i].x );
      const std::uint32_t by = index.bucketY( points[i].y );
      index.m_buckets[static_cast<std::size_t>( by ) * index.m_bucketsX + bx].push_back( static_cast<std::uint32_t>( i ) );
    }
    return index;
  }

  // 最近 K 点（半径 cutoff > 0 时过滤），按距离升序。
  void queryNearest( double x, double y, int k, double cutoff,
                     std::vector<std::uint32_t> *out ) const
  {
    out->clear();
    const int bx = static_cast<int>( bucketX( x ) );
    const int by = static_cast<int>( bucketY( y ) );
    const int maxRing = std::max( { bx, m_bucketsX - 1 - bx, by, m_bucketsY - 1 - by } );
    const double radius2 = cutoff > 0 ? cutoff * cutoff : std::numeric_limits<double>::max();
    std::priority_queue<std::pair<double, std::uint32_t>> heap; // max-heap on d²
    for ( int ring = 0; ring <= maxRing; ++ring )
    {
      const double ringMin = std::max( 0, ring - 1 ) * m_bucketSize;
      if ( cutoff > 0 && ringMin > cutoff )
        break;
      if ( static_cast<int>( heap.size() ) == k && ringMin * ringMin >= heap.top().first )
        break;
      visitRing( bx, by, ring, [&]( std::uint32_t cell ) {
        for ( std::uint32_t pointIndex : m_buckets[cell] )
        {
          const Sample &sample = m_points[pointIndex];
          const double dx = sample.x - x;
          const double dy = sample.y - y;
          const double d2 = dx * dx + dy * dy;
          if ( d2 > radius2 )
            continue;
          if ( static_cast<int>( heap.size() ) < k )
            heap.push( { d2, pointIndex } );
          else if ( d2 < heap.top().first )
          {
            heap.pop();
            heap.push( { d2, pointIndex } );
          }
        }
      } );
    }
    out->resize( heap.size() );
    for ( std::size_t i = heap.size(); i > 0; --i )
    {
      ( *out )[i - 1] = heap.top().second;
      heap.pop();
    } // 堆弹出从最远到最近 → 逆序写回即升序
  }

  const Sample &point( std::uint32_t index ) const { return m_points[index]; }
  std::size_t size() const { return m_points.size(); }

private:
  template <typename Visitor>
  void visitRing( int bx, int by, int ring, Visitor &&visitor ) const
  {
    if ( ring == 0 )
    {
      visitCell( bx, by, visitor );
      return;
    }
    for ( int cx = bx - ring; cx <= bx + ring; ++cx )
    {
      visitCell( cx, by - ring, visitor );
      visitCell( cx, by + ring, visitor );
    }
    for ( int cy = by - ring + 1; cy <= by + ring - 1; ++cy )
    {
      visitCell( bx - ring, cy, visitor );
      visitCell( bx + ring, cy, visitor );
    }
  }

  template <typename Visitor>
  void visitCell( int cx, int cy, Visitor &&visitor ) const
  {
    if ( cx < 0 || cy < 0 || cx >= m_bucketsX || cy >= m_bucketsY )
      return;
    visitor( static_cast<std::uint32_t>( cy ) * m_bucketsX + cx );
  }

  std::uint32_t bucketX( double x ) const
  {
    int bucket = static_cast<int>( std::floor( ( x - m_minX ) / m_bucketSize ) );
    return static_cast<std::uint32_t>( std::clamp( bucket, 0, m_bucketsX - 1 ) );
  }

  std::uint32_t bucketY( double y ) const
  {
    int bucket = static_cast<int>( std::floor( ( y - m_minY ) / m_bucketSize ) );
    return static_cast<std::uint32_t>( std::clamp( bucket, 0, m_bucketsY - 1 ) );
  }

  std::vector<Sample> m_points;
  std::vector<std::vector<std::uint32_t>> m_buckets;
  double m_bucketSize = 1;
  double m_minX = 0;
  double m_minY = 0;
  int m_bucketsX = 1;
  int m_bucketsY = 1;
};

// 解普通克里金方程组（加边 LU）。points 为邻域样本（已去重）。
bool solveOrdinaryKriging( const std::vector<std::uint32_t> &neighborhood,
                           const NeighborIndex &index, double x0, double y0,
                           const VariogramModel &model, KrigingPointResult *out )
{
  const int n = static_cast<int>( neighborhood.size() );
  const int n1 = n + 1;
  std::vector<double> a( static_cast<std::size_t>( n1 ) * n1, 0.0 );
  std::vector<double> b( static_cast<std::size_t>( n1 ), 0.0 );
  std::vector<double> distances0( static_cast<std::size_t>( n ) );
  for ( int i = 0; i < n; ++i )
  {
    const Sample &si = index.point( neighborhood[static_cast<std::size_t>( i )] );
    distances0[static_cast<std::size_t>( i )] = std::hypot( si.x - x0, si.y - y0 );
    for ( int j = i + 1; j < n; ++j )
    {
      const Sample &sj = index.point( neighborhood[static_cast<std::size_t>( j )] );
      const double gamma = model.semivariance( std::hypot( si.x - sj.x, si.y - sj.y ) );
      a[static_cast<std::size_t>( i ) * n1 + j] = gamma;
      a[static_cast<std::size_t>( j ) * n1 + i] = gamma;
    }
    a[static_cast<std::size_t>( i ) * n1 + n] = 1.0;
    a[static_cast<std::size_t>( n ) * n1 + i] = 1.0;
    b[static_cast<std::size_t>( i )] = model.semivariance( distances0[static_cast<std::size_t>( i )] );
  }
  b[static_cast<std::size_t>( n )] = 1.0;

  std::vector<double> solution;
  if ( !solveDenseLu( a, n1, b, solution ) )
    return false;

  double estimate = 0;
  double variance = 0;
  for ( int i = 0; i < n; ++i )
  {
    const double weight = solution[static_cast<std::size_t>( i )];
    estimate += weight * index.point( neighborhood[static_cast<std::size_t>( i )] ).value;
    variance += weight * b[static_cast<std::size_t>( i )];
  }
  variance += solution[static_cast<std::size_t>( n )]; // + μ
  if ( !std::isfinite( estimate ) || !std::isfinite( variance ) )
    return false;
  const double varianceScale = std::max( 1.0, model.nugget + model.sill );
  if ( variance < 0 )
  {
    if ( variance < -1e-9 * varianceScale )
      return false; // 显著负方差 = 数值失败，不静默输出
    variance = 0;
  }
  out->estimate = estimate;
  out->variance = variance;
  out->ok = true;
  return true;
}

void clampParams( KrigingParams *params )
{
  params->maxPoints = std::clamp( params->maxPoints, 1, 64 );
  params->minPoints = std::clamp( params->minPoints, 1, params->maxPoints );
  if ( params->searchRadius < 0 )
    params->searchRadius = 0;
}

} // namespace

KrigingPointResult ordinaryKrigingAt( double x, double y, const std::vector<Sample> &samples,
                                      const VariogramModel &model, const KrigingParams &params )
{
  KrigingPointResult result;
  KrigingParams clamped = params;
  clampParams( &clamped );
  int merged = 0;
  const std::vector<Sample> deduped = dedupeSamples( samples, &merged );
  if ( deduped.empty() )
    return result;
  const NeighborIndex index = NeighborIndex::build( deduped );
  std::vector<std::uint32_t> neighborhood;
  index.queryNearest( x, y, clamped.maxPoints, clamped.searchRadius, &neighborhood );
  if ( static_cast<int>( neighborhood.size() ) < clamped.minPoints && clamped.searchRadius > 0 )
    return result; // 半径模式覆盖闸：不足即 nodata
  if ( neighborhood.empty() )
    return result;
  solveOrdinaryKriging( neighborhood, index, x, y, model, &result );
  return result;
}

KrigingResult ordinaryKriging( const std::vector<Sample> &samples, const GridSpec &grid,
                               const VariogramModel &model, const KrigingParams &params,
                               const Control &control )
{
  KrigingResult result;
  if ( !grid.isValid() )
  {
    result.message = "grid is not valid";
    return result;
  }
  const std::int64_t cells = static_cast<std::int64_t>( grid.rows ) * grid.cols;
  if ( cells > 100'000'000 )
  {
    result.message = "grid budget exceeded (cells > 1e8)";
    return result;
  }
  if ( !( model.range > 0 ) || model.nugget < 0 || model.sill < 0 )
  {
    result.message = "variogram model invalid (range > 0, nugget/sill >= 0)";
    return result;
  }
  KrigingParams clamped = params;
  clampParams( &clamped );

  const std::vector<Sample> deduped = dedupeSamples( samples, &result.mergedDuplicates );
  if ( deduped.empty() )
  {
    result.message = "no finite samples";
    return result;
  }
  const NeighborIndex index = NeighborIndex::build( deduped );
  const std::size_t total = static_cast<std::size_t>( cells );
  result.estimate.assign( total, std::numeric_limits<double>::quiet_NaN() );
  result.variance.assign( total, std::numeric_limits<double>::quiet_NaN() );

  std::vector<std::uint32_t> neighborhood;
  std::size_t sinceCheck = 0;
  const std::size_t checkInterval = 2048;
  for ( int row = 0; row < grid.rows; ++row )
  {
    for ( int column = 0; column < grid.cols; ++column )
    {
      const std::size_t cell = static_cast<std::size_t>( row ) * grid.cols + column;
      index.queryNearest( grid.cellCenterX( column ), grid.cellCenterY( row ),
                          clamped.maxPoints, clamped.searchRadius, &neighborhood );
      bool covered = true;
      if ( clamped.searchRadius > 0 && static_cast<int>( neighborhood.size() ) < clamped.minPoints )
        covered = false; // 半径模式覆盖闸
      if ( covered && !neighborhood.empty() )
      {
        KrigingPointResult point;
        if ( solveOrdinaryKriging( neighborhood, index,
                                   grid.cellCenterX( column ), grid.cellCenterY( row ),
                                   model, &point ) )
        {
          result.estimate[cell] = point.estimate;
          result.variance[cell] = point.variance;
        }
        else
        {
          ++result.solverFailures;
        }
      }
      ++sinceCheck;
      if ( sinceCheck >= checkInterval )
      {
        sinceCheck = 0;
        if ( control.cancelled && control.cancelled() )
        {
          result.status = Status::Cancelled;
          result.message = "cancelled";
          return result;
        }
        if ( control.progress )
          control.progress( 0.02 + 0.98 * static_cast<double>( cell + 1 ) / static_cast<double>( total ) );
      }
    }
  }
  for ( double value : result.estimate )
  {
    if ( std::isfinite( value ) )
      ++result.finiteCells;
    else
      ++result.nodataCells;
  }
  if ( control.progress )
    control.progress( 1.0 );
  result.status = Status::Ok;
  return result;
}

} // namespace paleo::geostat
