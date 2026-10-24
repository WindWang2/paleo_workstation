// 层：数据
#pragma once

#include "types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

// geostat/neighborhood — 克里金系共用的内部工具（非公开 API 面）：
// 重合样本合并 + 桶格最近 K 点索引。kriging.cpp 与 sgs.cpp 共用。
// 层：数据
namespace paleo::geostat
{

namespace detail
{

// PW-078/B6：全流水线唯一的「同点」容差（相对 1e-9，UTM 坐标下 ~0.5 mm）。
// dedupe、精确内插命中等调用方一律走它，不再各自写绝对 d² 阈值。
inline bool samePoint( double ax, double ay, double bx, double by )
{
  const double ex = 1e-9 * std::max( { 1.0, std::fabs( ax ), std::fabs( bx ) } );
  const double ey = 1e-9 * std::max( { 1.0, std::fabs( ay ), std::fabs( by ) } );
  return std::fabs( ax - bx ) <= ex && std::fabs( ay - by ) <= ey;
}

inline bool coincident( const Sample &a, const Sample &b )
{
  return samePoint( a.x, a.y, b.x, b.y );
}

// 重合样本合并取均值（preserve 均值口径，防克氏矩阵奇异）。
// inputToDeduped 非空时回给「输入下标 → 合并后下标」映射（长度 = samples.size()，
// 非有限样本映射到 UINT32_MAX）——带约束克里金等需要按输入下标引用样本的
// 调用方用；不传则零开销（kriging/sgs 既有调用不动）。
inline std::vector<Sample> dedupeSamples( const std::vector<Sample> &samples, int *mergedCount,
                                          std::vector<std::uint32_t> *inputToDeduped = nullptr )
{
  if ( inputToDeduped )
    inputToDeduped->assign( samples.size(), std::numeric_limits<std::uint32_t>::max() );
  std::vector<Sample> finite;
  std::vector<std::uint32_t> finiteSource; // finite[i] 的输入下标
  finite.reserve( samples.size() );
  for ( std::size_t inputIndex = 0; inputIndex < samples.size(); ++inputIndex )
  {
    const Sample &sample = samples[inputIndex];
    if ( std::isfinite( sample.x ) && std::isfinite( sample.y ) && std::isfinite( sample.value ) )
    {
      finite.push_back( sample );
      finiteSource.push_back( static_cast<std::uint32_t>( inputIndex ) );
    }
  }
  std::vector<std::pair<Sample, std::uint32_t>> sorted; // (样本, finite 下标)
  sorted.reserve( finite.size() );
  for ( std::size_t i = 0; i < finite.size(); ++i )
    sorted.push_back( { finite[i], static_cast<std::uint32_t>( i ) } );
  std::sort( sorted.begin(), sorted.end(),
             []( const std::pair<Sample, std::uint32_t> &a, const std::pair<Sample, std::uint32_t> &b ) {
               if ( a.first.x != b.first.x )
                 return a.first.x < b.first.x;
               if ( a.first.y != b.first.y )
                 return a.first.y < b.first.y;
               // 完全重合点互为等价：合并求值的累加序是实现定义（末位 bit
               // 跨标准库漂移）——用输入下标次键钉死（full-audit F8）。
               return a.second < b.second;
             } );
  std::vector<Sample> merged;
  std::vector<std::size_t> mergedCounts;
  std::vector<std::uint32_t> finiteToMerged( finite.size(), 0 );
  merged.reserve( sorted.size() );
  for ( const auto &[sample, finiteIndex] : sorted )
  {
    // PW-078：按 x 排序后近重合点未必相邻（(0,0)、(1e-12,5)、(2e-12,0)：
    // 中间点 y 不同却插在两者之间）——只比 back() 会漏合并 → 克氏矩阵出现
    // 重复行。代表点 x 单调不减，向回扫描 x 容差窗内的全部代表点。
    std::size_t target = merged.size();
    for ( std::size_t j = merged.size(); j > 0; --j )
    {
      const Sample &candidate = merged[j - 1];
      const double ex = 1e-9 * std::max( { 1.0, std::fabs( candidate.x ), std::fabs( sample.x ) } );
      if ( sample.x - candidate.x > ex )
        break;
      if ( coincident( candidate, sample ) )
        target = j - 1;
    }
    if ( target < merged.size() )
    {
      merged[target].value += sample.value;
      ++mergedCounts[target];
      if ( mergedCount )
        ++( *mergedCount );
    }
    else
    {
      merged.push_back( sample );
      mergedCounts.push_back( 1 );
      target = merged.size() - 1;
    }
    finiteToMerged[finiteIndex] = static_cast<std::uint32_t>( target );
  }
  for ( std::size_t i = 0; i < merged.size(); ++i )
    merged[i].value /= static_cast<double>( mergedCounts[i] );
  if ( inputToDeduped )
  {
    for ( std::size_t i = 0; i < finite.size(); ++i )
      ( *inputToDeduped )[finiteSource[i]] = finiteToMerged[i];
  }
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
    const int side = std::max<int>( 1, static_cast<int>( std::ceil( std::sqrt( static_cast<double>( points.size() ) ) ) ) );
    const double span = std::max( maxX - minX, maxY - minY );
    index.m_bucketSize = span > 0 ? span / side : 1.0;
    index.m_bucketsX = side;
    index.m_bucketsY = side;
    index.m_buckets.assign( static_cast<std::size_t>( side ) * side, {} );
    for ( std::size_t i = 0; i < points.size(); ++i )
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
  std::uint32_t bucketX( double x ) const
  {
    // PW-128：先在 double 域夹紧再转 int（越界/NaN 的 double→int 是 UB）。
    const double raw = std::floor( ( x - m_minX ) / m_bucketSize );
    if ( !( raw > 0 ) )
      return 0;
    return static_cast<std::uint32_t>( std::min( raw, static_cast<double>( m_bucketsX - 1 ) ) );
  }

  std::uint32_t bucketY( double y ) const
  {
    // PW-128：先在 double 域夹紧再转 int（越界/NaN 的 double→int 是 UB）。
    const double raw = std::floor( ( y - m_minY ) / m_bucketSize );
    if ( !( raw > 0 ) )
      return 0;
    return static_cast<std::uint32_t>( std::min( raw, static_cast<double>( m_bucketsY - 1 ) ) );
  }

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

  std::vector<Sample> m_points;
  std::vector<std::vector<std::uint32_t>> m_buckets;
  double m_bucketSize = 1;
  double m_minX = 0;
  double m_minY = 0;
  int m_bucketsX = 1;
  int m_bucketsY = 1;
};

} // namespace detail

} // namespace paleo::geostat
