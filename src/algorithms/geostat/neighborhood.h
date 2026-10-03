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

inline bool coincident( const Sample &a, const Sample &b )
{
  const double ex = 1e-9 * std::max( { 1.0, std::fabs( a.x ), std::fabs( b.x ) } );
  const double ey = 1e-9 * std::max( { 1.0, std::fabs( a.y ), std::fabs( b.y ) } );
  return std::fabs( a.x - b.x ) <= ex && std::fabs( a.y - b.y ) <= ey;
}

// 重合样本合并取均值（preserve 均值口径，防克氏矩阵奇异）。
inline std::vector<Sample> dedupeSamples( const std::vector<Sample> &samples, int *mergedCount )
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
    int bucket = static_cast<int>( std::floor( ( x - m_minX ) / m_bucketSize ) );
    return static_cast<std::uint32_t>( std::clamp( bucket, 0, m_bucketsX - 1 ) );
  }

  std::uint32_t bucketY( double y ) const
  {
    int bucket = static_cast<int>( std::floor( ( y - m_minY ) / m_bucketSize ) );
    return static_cast<std::uint32_t>( std::clamp( bucket, 0, m_bucketsY - 1 ) );
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
