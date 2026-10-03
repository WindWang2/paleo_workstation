// 层：数据
#include "buffertransition.h"

#include "support.h"

#include <algorithm>
#include <cmath>
#include <limits>

// 层：数据
namespace paleo::singlefactor
{

BufferTransitionResult applyBufferTransition( const GridSpec &grid,
                                              const std::vector<double> &source,
                                              std::span<const BufferTransitionSpec> specs,
                                              const std::vector<std::uint8_t> &validMask )
{
  BufferTransitionResult result;
  const std::size_t cells = static_cast<std::size_t>( grid.cols ) * static_cast<std::size_t>( grid.rows );
  if ( source.size() != cells )
  {
    result.status = Status::InvalidInput;
    result.message = "缓冲过渡网格尺寸不一致";
    return result;
  }
  result.values = source;

  for ( const BufferTransitionSpec &spec : specs )
  {
    if ( !std::isfinite( spec.halfWidth ) || !std::isfinite( spec.transitionWidth ) ||
         !std::isfinite( spec.floor ) || spec.halfWidth < 0 || !( spec.transitionWidth > 0 ) )
    {
      result.status = Status::InvalidInput;
      result.message = "缓冲半宽应非负，过渡宽度应为正，最低值必须有效";
      return result;
    }
  }

  std::vector<std::uint8_t> affected( cells, std::uint8_t{ 0 } );
  for ( const BufferTransitionSpec &spec : specs )
  {
    if ( spec.points.size() < 2 )
      continue;
    BufferTransitionRecord record;
    record.floor = spec.floor;
    record.halfWidth = spec.halfWidth;
    record.transitionWidth = spec.transitionWidth;
    for ( std::size_t i = 0; i < cells; ++i )
    {
      if ( !std::isfinite( result.values[i] ) )
        continue;
      if ( i < validMask.size() && !validMask[i] )
        continue;
      const Point2 center = cellCenter( grid, static_cast<int>( i % grid.cols ),
                                        static_cast<int>( i / grid.cols ) );
      const double dist = distanceToPolyline( center, spec.points );
      if ( dist <= spec.halfWidth )
        ++record.coreCells;
      if ( !( dist < spec.halfWidth + spec.transitionWidth ) )
        continue;
      ++record.affectedCells;
      const double t = std::clamp( ( dist - spec.halfWidth ) / spec.transitionWidth, 0.0, 1.0 );
      const double blend = t * t * t * ( 10.0 + t * ( -15.0 + 6.0 * t ) );
      // candidate 恒基于原值计算，重叠时两条线的结果才独立可比（上游语义）。
      const double candidate = spec.floor + blend * ( source[i] - spec.floor );
      // 重叠取较低包络：核内可抬也可压，只有重叠侧才取 min（上游语义）。
      if ( !affected[i] )
      {
        result.values[i] = candidate;
        affected[i] = 1;
      }
      else
      {
        result.values[i] = std::min( result.values[i], candidate );
      }
    }
    result.records.push_back( record );
  }
  return result;
}

} // namespace paleo::singlefactor
