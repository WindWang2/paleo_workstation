// 层：数据
#include "sgs3.h"

#include "neighborhood.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <utility>

// 层：数据
namespace paleo::geostat
{

namespace
{

using detail::CondPoint;

constexpr std::int64_t kMaxTargets = 100'000'000;
constexpr int kIndexAxisMax = 1 << 20; // (ix,iy,iz) 打包进 63 bit 的轴上限

bool coincident3( const Sample3 &a, const Sample3 &b )
{
  const double ex = 1e-9 * std::max( { 1.0, std::fabs( a.x ), std::fabs( b.x ) } );
  const double ey = 1e-9 * std::max( { 1.0, std::fabs( a.y ), std::fabs( b.y ) } );
  const double ez = 1e-9 * std::max( { 1.0, std::fabs( a.z ), std::fabs( b.z ) } );
  return std::fabs( a.x - b.x ) <= ex && std::fabs( a.y - b.y ) <= ey &&
         std::fabs( a.z - b.z ) <= ez;
}

// 重合样本（同组分、坐标 ε 内）合并取均值——口径同 detail::dedupeSamples，
// 加 z 维与组分维。排序序 (x,y,z) 确定，均值合并跨平台可复现。
std::vector<Sample3> dedupeSamples3( const std::vector<Sample3> &samples, int *mergedCount )
{
  std::vector<Sample3> finite;
  finite.reserve( samples.size() );
  for ( const Sample3 &sample : samples )
  {
    if ( std::isfinite( sample.x ) && std::isfinite( sample.y ) && std::isfinite( sample.z ) &&
         std::isfinite( sample.value ) )
      finite.push_back( sample );
  }
  std::sort( finite.begin(), finite.end(), []( const Sample3 &a, const Sample3 &b ) {
    if ( a.x != b.x )
      return a.x < b.x;
    if ( a.y != b.y )
      return a.y < b.y;
    return a.z < b.z;
  } );
  std::vector<Sample3> merged;
  std::vector<int> counts;
  merged.reserve( finite.size() );
  for ( const Sample3 &sample : finite )
  {
    if ( !merged.empty() && sample.group == merged.back().group &&
         coincident3( merged.back(), sample ) )
    {
      merged.back().value += sample.value;
      ++counts.back();
      if ( mergedCount )
        ++( *mergedCount );
      continue;
    }
    merged.push_back( sample );
    counts.push_back( 1 );
  }
  for ( std::size_t i = 0; i < merged.size(); ++i )
    merged[i].value /= static_cast<double>( counts[i] );
  return merged;
}

// 静态样本的三维桶格索引：O(1) 定位 + 索引空间环扩张，同组分过滤。
// 最近 K 的选取用 (d², 下标) 全序堆——与桶迭代顺序无关（确定性）。
class StaticIndex3
{
public:
  static StaticIndex3 build( const std::vector<Sample3> &points )
  {
    StaticIndex3 index;
    index.m_points = points;
    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double minZ = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double maxY = std::numeric_limits<double>::lowest();
    double maxZ = std::numeric_limits<double>::lowest();
    for ( const Sample3 &sample : points )
    {
      minX = std::min( minX, sample.x );
      minY = std::min( minY, sample.y );
      minZ = std::min( minZ, sample.z );
      maxX = std::max( maxX, sample.x );
      maxY = std::max( maxY, sample.y );
      maxZ = std::max( maxZ, sample.z );
    }
    index.m_minX = minX;
    index.m_minY = minY;
    index.m_minZ = minZ;
    const int side = std::max<int>(
        1, static_cast<int>( std::ceil( std::cbrt( static_cast<double>( points.size() ) ) ) ) );
    const double span = std::max( { maxX - minX, maxY - minY, maxZ - minZ } );
    index.m_bucketSize = span > 0 ? span / side : 1.0;
    index.m_side = side;
    index.m_buckets.assign( static_cast<std::size_t>( side ) * side * side, {} );
    for ( std::size_t i = 0; i < points.size(); ++i )
    {
      const std::size_t cell = index.bucketCell( points[i].x, points[i].y, points[i].z );
      index.m_buckets[cell].push_back( static_cast<std::uint32_t>( i ) );
    }
    return index;
  }

  // 同组分最近 K 样本（半径 cutoff > 0 时三维距离过滤），(d², 下标) 升序。
  void queryNearest( const CondPoint &query, int group, int k, double cutoff,
                     std::vector<std::uint32_t> *out ) const
  {
    out->clear();
    const int bx = bucketAxis( query.x - m_minX );
    const int by = bucketAxis( query.y - m_minY );
    const int bz = bucketAxis( query.z - m_minZ );
    const int maxRing = std::max( { bx, m_side - 1 - bx, by, m_side - 1 - by, bz, m_side - 1 - bz } );
    const double radius2 = cutoff > 0 ? cutoff * cutoff : std::numeric_limits<double>::max();
    std::priority_queue<std::pair<double, std::uint32_t>> heap; // max-heap on (d², idx)
    for ( int ring = 0; ring <= maxRing; ++ring )
    {
      const double ringMin = std::max( 0, ring - 1 ) * m_bucketSize;
      if ( cutoff > 0 && ringMin > cutoff )
        break;
      if ( static_cast<int>( heap.size() ) == k && ringMin * ringMin >= heap.top().first )
        break;
      const int lo0 = std::max( 0, bx - ring ), hi0 = std::min( m_side - 1, bx + ring );
      const int lo1 = std::max( 0, by - ring ), hi1 = std::min( m_side - 1, by + ring );
      const int lo2 = std::max( 0, bz - ring ), hi2 = std::min( m_side - 1, bz + ring );
      for ( int cx = lo0; cx <= hi0; ++cx )
        for ( int cy = lo1; cy <= hi1; ++cy )
          for ( int cz = lo2; cz <= hi2; ++cz )
          {
            const int chebyshev = std::max( { std::abs( cx - bx ), std::abs( cy - by ),
                                              std::abs( cz - bz ) } );
            if ( chebyshev != ring )
              continue; // 只访问本壳
            for ( std::uint32_t pointIndex : m_buckets[static_cast<std::size_t>( cz ) * m_side *
                                                       m_side +
                                                   static_cast<std::size_t>( cy ) * m_side +
                                                   static_cast<std::size_t>( cx )] )
            {
              const Sample3 &sample = m_points[pointIndex];
              if ( sample.group != group )
                continue;
              const double dx = sample.x - query.x;
              const double dy = sample.y - query.y;
              const double dz = sample.z - query.z;
              const double d2 = dx * dx + dy * dy + dz * dz;
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
          }
    }
    out->resize( heap.size() );
    for ( std::size_t i = heap.size(); i > 0; --i )
    {
      ( *out )[i - 1] = heap.top().second;
      heap.pop();
    } // 堆弹出从最远到最近 → 逆序写回即升序
  }

  const Sample3 &point( std::uint32_t index ) const { return m_points[index]; }

private:
  int bucketAxis( double offset ) const
  {
    const int bucket = static_cast<int>( std::floor( offset / m_bucketSize ) );
    return std::clamp( bucket, 0, m_side - 1 );
  }

  std::size_t bucketCell( double x, double y, double z ) const
  {
    return static_cast<std::size_t>( bucketAxis( z - m_minZ ) ) * m_side * m_side +
           static_cast<std::size_t>( bucketAxis( y - m_minY ) ) * m_side +
           static_cast<std::size_t>( bucketAxis( x - m_minX ) );
  }

  std::vector<Sample3> m_points;
  std::vector<std::vector<std::uint32_t>> m_buckets;
  double m_bucketSize = 1;
  double m_minX = 0;
  double m_minY = 0;
  double m_minZ = 0;
  int m_side = 1;
};

// 已模拟目标的格架邻域：稀疏 IJK 目标集，(ix,iy,iz) 打包键查已模拟值，
// 索引空间壳扩张 + 度量步长剪枝，同组分过滤。并列距离按目标下标全序。
class SimulatedLattice3
{
public:
  void reset( const std::vector<Sgs3Target> &targets )
  {
    m_targets = &targets;
    m_values.assign( targets.size(), std::numeric_limits<double>::quiet_NaN() );
    if ( m_lookupTargetCount == targets.size() && !m_lookup.empty() )
      return; // 同一目标集的后续实现：只清值，不重建索引（确定性不受影响）
    m_lookup.clear();
    m_lookup.reserve( targets.size() );
    m_minIx = m_minIy = m_minIz = kIndexAxisMax;
    m_maxIx = m_maxIy = m_maxIz = -kIndexAxisMax;
    for ( std::size_t i = 0; i < targets.size(); ++i )
    {
      const Sgs3Target &target = targets[i];
      m_lookup.emplace( packIndex( target.ix, target.iy, target.iz ),
                        static_cast<std::uint32_t>( i ) );
      m_minIx = std::min( m_minIx, target.ix );
      m_maxIx = std::max( m_maxIx, target.ix );
      m_minIy = std::min( m_minIy, target.iy );
      m_maxIy = std::max( m_maxIy, target.iy );
      m_minIz = std::min( m_minIz, target.iz );
      m_maxIz = std::max( m_maxIz, target.iz );
    }
    m_lookupTargetCount = targets.size();
  }

  void setValue( std::size_t targetIndex, double value ) { m_values[targetIndex] = value; }

  // 同组分最近 K 个已模拟目标（(d², 下标) 升序）。
  void queryNearest( const Sgs3Target &query, const Lattice3Steps &steps, int k,
                     std::vector<CondPoint> *out ) const
  {
    out->clear();
    const int maxRing = std::max( { query.ix - m_minIx, m_maxIx - query.ix,
                                    query.iy - m_minIy, m_maxIy - query.iy,
                                    query.iz - m_minIz, m_maxIz - query.iz } );
    const double minStep = std::max( { steps.x, steps.y, steps.zMin, 0.0 } );
    std::priority_queue<std::pair<double, std::uint32_t>> heap; // max-heap on (d², idx)
    for ( int ring = 1; ring <= maxRing; ++ring )
    {
      const double ringMin = static_cast<double>( ring - 1 ) * minStep;
      if ( static_cast<int>( heap.size() ) == k && ringMin * ringMin >= heap.top().first )
        break;
      const int lo0 = std::max( m_minIx, query.ix - ring );
      const int hi0 = std::min( m_maxIx, query.ix + ring );
      const int lo1 = std::max( m_minIy, query.iy - ring );
      const int hi1 = std::min( m_maxIy, query.iy + ring );
      const int lo2 = std::max( m_minIz, query.iz - ring );
      const int hi2 = std::min( m_maxIz, query.iz + ring );
      for ( int cx = lo0; cx <= hi0; ++cx )
        for ( int cy = lo1; cy <= hi1; ++cy )
          for ( int cz = lo2; cz <= hi2; ++cz )
          {
            const int chebyshev = std::max( { std::abs( cx - query.ix ), std::abs( cy - query.iy ),
                                              std::abs( cz - query.iz ) } );
            if ( chebyshev != ring )
              continue;
            const auto found = m_lookup.find( packIndex( cx, cy, cz ) );
            if ( found == m_lookup.end() )
              continue;
            const std::uint32_t index = found->second;
            const double value = m_values[index];
            if ( !std::isfinite( value ) )
              continue;
            if ( ( *m_targets )[index].group != query.group )
              continue;
            const double dx = ( *m_targets )[index].x - query.x;
            const double dy = ( *m_targets )[index].y - query.y;
            const double dz = ( *m_targets )[index].z - query.z;
            const double d2 = dx * dx + dy * dy + dz * dz;
            if ( static_cast<int>( heap.size() ) < k )
              heap.push( { d2, index } );
            else if ( d2 < heap.top().first )
            {
              heap.pop();
              heap.push( { d2, index } );
            }
          }
    }
    std::vector<std::pair<double, std::uint32_t>> collected;
    collected.reserve( heap.size() );
    while ( !heap.empty() )
    {
      collected.push_back( heap.top() );
      heap.pop();
    }
    std::reverse( collected.begin(), collected.end() ); // 升序
    out->reserve( collected.size() );
    for ( const auto &entry : collected )
      out->push_back( CondPoint{ ( *m_targets )[entry.second].x,
                                 ( *m_targets )[entry.second].y,
                                 ( *m_targets )[entry.second].z, m_values[entry.second] } );
  }

private:
  static std::uint64_t packIndex( int ix, int iy, int iz )
  {
    return ( static_cast<std::uint64_t>( static_cast<std::uint32_t>( ix ) ) << 42 ) |
           ( static_cast<std::uint64_t>( static_cast<std::uint32_t>( iy ) ) << 21 ) |
           static_cast<std::uint64_t>( static_cast<std::uint32_t>( iz ) );
  }

  const std::vector<Sgs3Target> *m_targets = nullptr;
  std::vector<double> m_values;
  std::unordered_map<std::uint64_t, std::uint32_t> m_lookup;
  std::size_t m_lookupTargetCount = 0;
  int m_minIx = 0, m_maxIx = 0, m_minIy = 0, m_maxIy = 0, m_minIz = 0, m_maxIz = 0;
};

} // namespace

Sgs3Result sgs3( const std::vector<Sample3> &samples, const std::vector<Sgs3Target> &targets,
                 const Lattice3Steps &steps, const VariogramModel &model,
                 const Sgs3Params &params, const Control &control )
{
  Sgs3Result result;
  if ( targets.empty() )
  {
    result.message = "no targets";
    return result;
  }
  if ( static_cast<std::int64_t>( targets.size() ) > kMaxTargets )
  {
    result.message = "target budget exceeded (> 1e8)";
    return result;
  }
  for ( const Sgs3Target &target : targets )
  {
    if ( target.ix < 0 || target.iy < 0 || target.iz < 0 || target.ix >= kIndexAxisMax ||
         target.iy >= kIndexAxisMax || target.iz >= kIndexAxisMax )
    {
      result.message = "target lattice index out of range";
      return result;
    }
    if ( target.group < 0 )
    {
      result.message = "target group must be >= 0";
      return result;
    }
  }
  {
    // 同格点双目标会让邻域查值错位——直接拒绝（驱动侧一格一目标）。
    std::unordered_set<std::uint64_t> seen;
    seen.reserve( targets.size() );
    for ( const Sgs3Target &target : targets )
    {
      const std::uint64_t key =
          ( static_cast<std::uint64_t>( static_cast<std::uint32_t>( target.ix ) ) << 42 ) |
          ( static_cast<std::uint64_t>( static_cast<std::uint32_t>( target.iy ) ) << 21 ) |
          static_cast<std::uint64_t>( static_cast<std::uint32_t>( target.iz ) );
      if ( !seen.insert( key ).second )
      {
        result.message = "duplicate target lattice index";
        return result;
      }
    }
  }
  for ( const Sample3 &sample : samples )
  {
    if ( sample.group < 0 )
    {
      result.message = "sample group must be >= 0";
      return result;
    }
  }
  if ( !( model.range > 0 ) || model.nugget < 0 || model.sill < 0 )
  {
    result.message = "variogram model invalid";
    return result;
  }
  if ( model.nugget + model.sill <= 0 )
  {
    result.message = "zero total sill: SGS needs nugget + sill > 0";
    return result;
  }
  if ( !( steps.x > 0 ) || !( steps.y > 0 ) || !( steps.zMin > 0 ) )
  {
    result.message = "lattice steps must be positive";
    return result;
  }
  Sgs3Params clamped = params;
  clamped.nRealizations = std::clamp( clamped.nRealizations, 1, 64 );
  clamped.maxPoints = std::clamp( clamped.maxPoints, 1, 64 );
  if ( clamped.searchRadius < 0 )
    clamped.searchRadius = 0;

  const std::vector<Sample3> deduped = dedupeSamples3( samples, &result.mergedDuplicates );
  if ( deduped.empty() )
  {
    result.message = "no finite samples";
    return result;
  }
  std::vector<double> sampleValues;
  sampleValues.reserve( deduped.size() );
  for ( const Sample3 &sample : deduped )
    sampleValues.push_back( sample.value );
  const detail::NormalScoreTable table = detail::NormalScoreTable::build( sampleValues );
  result.sampleMean = table.sampleMean;
  result.sampleStd = table.sampleStd;

  const StaticIndex3 index = StaticIndex3::build( deduped );

  const std::size_t n = targets.size();
  const int R = clamped.nRealizations;
  result.realizations.assign( static_cast<std::size_t>( R ),
                              std::vector<double>( n,
                                                   std::numeric_limits<double>::quiet_NaN() ) );

  std::mt19937_64 rng( clamped.seed );
  SimulatedLattice3 lattice;
  std::vector<std::uint32_t> staticNeighborhood;
  std::vector<CondPoint> simulatedNeighborhood;
  std::vector<CondPoint> neighborhood;
  std::vector<std::size_t> path( n );

  for ( int realization = 0; realization < R; ++realization )
  {
    // 随机路径：Fisher–Yates（随机流跨实现连续——与 2D 核同一口径）
    for ( std::size_t i = 0; i < n; ++i )
      path[i] = i;
    for ( std::size_t i = n; i > 1; --i )
    {
      const std::size_t j = static_cast<std::size_t>( rng() % i );
      std::swap( path[i - 1], path[j] );
    }
    lattice.reset( targets );
    std::vector<double> gaussianField( n, std::numeric_limits<double>::quiet_NaN() );

    std::size_t sinceCheck = 0;
    const std::size_t checkInterval = 2048;
    for ( std::size_t step = 0; step < n; ++step )
    {
      const std::size_t targetIndex = path[step];
      const Sgs3Target &target = targets[targetIndex];
      CondPoint query{ target.x, target.y, target.z, 0.0 };

      index.queryNearest( query, target.group, clamped.maxPoints, clamped.searchRadius,
                          &staticNeighborhood );
      lattice.queryNearest( target, steps, clamped.maxPoints, &simulatedNeighborhood );

      // 合并邻域：静态样本优先；与静态样本重合的已模拟点剔除（口径同 2D 核：
      // 重合行让克氏矩阵奇异，信息已由静态样本承载）。
      neighborhood.clear();
      neighborhood.reserve( staticNeighborhood.size() + simulatedNeighborhood.size() );
      bool queryCoincidentWithStatic = false;
      double queryGaussian = 0;
      for ( std::uint32_t staticIndex : staticNeighborhood )
      {
        const Sample3 &sample = deduped[staticIndex];
        neighborhood.push_back( CondPoint{ sample.x, sample.y, sample.z,
                                           table.forward( sample.value ) } );
        // 查询点与静态样本重合：SK 数学上方差为 0、估值 = 样本高斯得分。
        // 数值解的小正方差开方后会把 ~1e-13 的舍入放大成 ~1e-7 的随机扰动
        // （2D 核靠方差落在钳制侧的数值路径恰好精确），三维路径把这一数学
        // 性质做成显式：直接取样本得分，零方差不抽样。
        const double ex = 1e-9 * std::max( { 1.0, std::fabs( sample.x ), std::fabs( query.x ) } );
        const double ey = 1e-9 * std::max( { 1.0, std::fabs( sample.y ), std::fabs( query.y ) } );
        const double ez = 1e-9 * std::max( { 1.0, std::fabs( sample.z ), std::fabs( query.z ) } );
        if ( std::fabs( sample.x - query.x ) <= ex && std::fabs( sample.y - query.y ) <= ey &&
             std::fabs( sample.z - query.z ) <= ez )
        {
          queryCoincidentWithStatic = true;
          queryGaussian = table.forward( sample.value );
        }
      }
      const std::size_t staticCount = neighborhood.size();
      for ( const CondPoint &simulated : simulatedNeighborhood )
      {
        bool coincidentWithStatic = false;
        for ( std::size_t i = 0; i < staticCount; ++i )
        {
          const double ex = 1e-9 * std::max( { 1.0, std::fabs( simulated.x ),
                                               std::fabs( neighborhood[i].x ) } );
          const double ey = 1e-9 * std::max( { 1.0, std::fabs( simulated.y ),
                                               std::fabs( neighborhood[i].y ) } );
          const double ez = 1e-9 * std::max( { 1.0, std::fabs( simulated.z ),
                                               std::fabs( neighborhood[i].z ) } );
          if ( std::fabs( simulated.x - neighborhood[i].x ) <= ex &&
               std::fabs( simulated.y - neighborhood[i].y ) <= ey &&
               std::fabs( simulated.z - neighborhood[i].z ) <= ez )
          {
            coincidentWithStatic = true;
            break;
          }
        }
        if ( !coincidentWithStatic )
          neighborhood.push_back( simulated );
      }
      if ( clamped.searchRadius > 0 )
      {
        const double radius2 = clamped.searchRadius * clamped.searchRadius;
        neighborhood.erase( std::remove_if( neighborhood.begin(), neighborhood.end(),
                                            [&query, radius2]( const CondPoint &point ) {
                                              const double dx = point.x - query.x;
                                              const double dy = point.y - query.y;
                                              const double dz = point.z - query.z;
                                              return dx * dx + dy * dy + dz * dz > radius2;
                                            } ),
                            neighborhood.end() );
      }
      if ( static_cast<int>( neighborhood.size() ) > clamped.maxPoints )
      {
        const CondPoint &q = query;
        std::partial_sort( neighborhood.begin(), neighborhood.begin() + clamped.maxPoints,
                           neighborhood.end(),
                           [&q]( const CondPoint &a, const CondPoint &b ) {
                             const double da = ( a.x - q.x ) * ( a.x - q.x ) +
                                               ( a.y - q.y ) * ( a.y - q.y ) +
                                               ( a.z - q.z ) * ( a.z - q.z );
                             const double db = ( b.x - q.x ) * ( b.x - q.x ) +
                                               ( b.y - q.y ) * ( b.y - q.y ) +
                                               ( b.z - q.z ) * ( b.z - q.z );
                             return da < db;
                           } );
        neighborhood.resize( static_cast<std::size_t>( clamped.maxPoints ) );
      }

      double estimate = 0;
      double variance = 0;
      bool solved = false;
      if ( queryCoincidentWithStatic )
      {
        estimate = queryGaussian;
        variance = 0;
        solved = true;
      }
      else if ( !neighborhood.empty() &&
                detail::solveSimpleKriging( neighborhood, query.x, query.y, query.z, model,
                                            &estimate, &variance ) )
      {
        solved = true;
      }
      if ( solved )
      {
        const double draw = estimate + std::sqrt( variance ) * detail::gaussianRandom( rng );
        gaussianField[targetIndex] = draw;
        lattice.setValue( targetIndex, draw );
      }
      else
      {
        ++result.solverFailures; // 无同组分邻域或数值失败：该目标保持 nodata（如实计数）
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
          control.progress( ( static_cast<double>( realization ) +
                              static_cast<double>( step + 1 ) / static_cast<double>( n ) ) /
                            static_cast<double>( R ) );
      }
    }

    std::vector<double> &output = result.realizations[static_cast<std::size_t>( realization )];
    result.finitePoints = 0;
    result.nodataPoints = 0;
    for ( std::size_t i = 0; i < n; ++i )
    {
      const double gaussian = gaussianField[i];
      if ( std::isfinite( gaussian ) )
      {
        output[i] = table.backTransform( gaussian );
        ++result.finitePoints;
      }
      else
      {
        ++result.nodataPoints;
      }
    }
  }
  if ( control.progress )
    control.progress( 1.0 );
  result.status = Status::Ok;
  return result;
}

} // namespace paleo::geostat
