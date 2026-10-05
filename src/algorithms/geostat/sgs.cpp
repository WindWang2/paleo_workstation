// 层：数据
#include "sgs.h"

#include "linsolve.h"
#include "neighborhood.h"
#include "sgs_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <utility>

// 层：数据
namespace paleo::geostat
{

namespace
{

using detail::CondPoint;

// 已模拟格的格点邻域查询（模拟点恰在格心，格上环扫 + 堆剪枝）。
// 访问序是 2D 逐位结果的组成部分（并列距离靠访问序打破）——不动。
class SimulatedLattice
{
public:
  void reset( int cols, int rows )
  {
    m_cols = cols;
    m_rows = rows;
    m_values.assign( static_cast<std::size_t>( cols ) * rows,
                     std::numeric_limits<double>::quiet_NaN() );
  }

  void setValue( int column, int row, double value )
  {
    m_values[static_cast<std::size_t>( row ) * m_cols + column] = value;
  }

  // 最近 K 个已模拟格（按距离升序）。
  void queryNearest( int column, int row, const GridSpec &grid, int k,
                     std::vector<CondPoint> *out ) const
  {
    out->clear();
    const double x0 = grid.cellCenterX( column );
    const double y0 = grid.cellCenterY( row );
    const double minCell = std::min( grid.pixelWidth, -grid.pixelHeight );
    const int maxRing = std::max( { column, m_cols - 1 - column, row, m_rows - 1 - row } );
    std::priority_queue<std::pair<double, std::size_t>> heap; // max-heap on d²
    for ( int ring = 1; ring <= maxRing; ++ring )
    {
      const double ringMin = std::max( 0, ring - 1 ) * minCell;
      if ( static_cast<int>( heap.size() ) == k && ringMin * ringMin >= heap.top().first )
        break;
      for ( int cx = column - ring; cx <= column + ring; ++cx )
      {
        visitCell( cx, row - ring, x0, y0, grid, &heap, k );
        visitCell( cx, row + ring, x0, y0, grid, &heap, k );
      }
      for ( int cy = row - ring + 1; cy <= row + ring - 1; ++cy )
      {
        visitCell( column - ring, cy, x0, y0, grid, &heap, k );
        visitCell( column + ring, cy, x0, y0, grid, &heap, k );
      }
    }
    std::vector<std::pair<double, std::size_t>> collected;
    collected.reserve( heap.size() );
    while ( !heap.empty() )
    {
      collected.push_back( heap.top() );
      heap.pop();
    }
    std::reverse( collected.begin(), collected.end() ); // 升序
    out->reserve( collected.size() );
    for ( const auto &entry : collected )
    {
      const std::size_t cell = entry.second;
      const int cellRow = static_cast<int>( cell / m_cols );
      const int cellColumn = static_cast<int>( cell % m_cols );
      out->push_back( CondPoint{ grid.cellCenterX( cellColumn ), grid.cellCenterY( cellRow ),
                                 0.0, m_values[cell] } );
    }
  }

private:
  void visitCell( int cx, int cy, double x0, double y0, const GridSpec &grid,
                  std::priority_queue<std::pair<double, std::size_t>> *heap, int k ) const
  {
    if ( cx < 0 || cy < 0 || cx >= m_cols || cy >= m_rows )
      return;
    const std::size_t cell = static_cast<std::size_t>( cy ) * m_cols + cx;
    const double value = m_values[cell];
    if ( !std::isfinite( value ) )
      return;
    const double dx = grid.cellCenterX( cx ) - x0;
    const double dy = grid.cellCenterY( cy ) - y0;
    const double d2 = dx * dx + dy * dy;
    if ( static_cast<int>( heap->size() ) < k )
      heap->push( { d2, cell } );
    else if ( d2 < heap->top().first )
    {
      heap->pop();
      heap->push( { d2, cell } );
    }
  }

  int m_cols = 0;
  int m_rows = 0;
  std::vector<double> m_values;
};

} // namespace

SgsResult sgs( const std::vector<Sample> &samples, const GridSpec &grid,
               const VariogramModel &model, const SgsParams &params,
               const Control &control )
{
  SgsResult result;
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
  SgsParams clamped = params;
  clamped.nRealizations = std::clamp( clamped.nRealizations, 1, 64 );
  clamped.maxPoints = std::clamp( clamped.maxPoints, 1, 64 );
  if ( clamped.searchRadius < 0 )
    clamped.searchRadius = 0;

  const std::vector<Sample> deduped = detail::dedupeSamples( samples, &result.mergedDuplicates );
  if ( deduped.empty() )
  {
    result.message = "no finite samples";
    return result;
  }
  std::vector<double> sampleValues;
  sampleValues.reserve( deduped.size() );
  for ( const Sample &sample : deduped )
    sampleValues.push_back( sample.value );
  const detail::NormalScoreTable table = detail::NormalScoreTable::build( sampleValues );
  result.sampleMean = table.sampleMean;
  result.sampleStd = table.sampleStd;

  // 静态样本邻域按索引原位取 deduped（detail::NeighborIndex 语义）。
  const detail::NeighborIndex index = detail::NeighborIndex::build( deduped );

  const std::size_t cells = static_cast<std::size_t>( cells64 );
  const int R = clamped.nRealizations;
  result.realizations.assign( static_cast<std::size_t>( R ),
                              std::vector<double>( cells,
                                                   std::numeric_limits<double>::quiet_NaN() ) );

  std::mt19937_64 rng( clamped.seed );
  SimulatedLattice lattice;
  std::vector<std::uint32_t> staticNeighborhood;
  std::vector<CondPoint> simulatedNeighborhood;
  std::vector<CondPoint> neighborhood;
  std::vector<std::size_t> path( cells );

  for ( int realization = 0; realization < R; ++realization )
  {
    // 随机路径：Fisher–Yates（随机流跨实现连续，先 shuffle 后逐格抽样）
    for ( std::size_t i = 0; i < cells; ++i )
      path[i] = i;
    for ( std::size_t i = cells; i > 1; --i )
    {
      const std::size_t j = static_cast<std::size_t>( rng() % i );
      std::swap( path[i - 1], path[j] );
    }
    lattice.reset( grid.cols, grid.rows );
    std::vector<double> gaussianField( cells, std::numeric_limits<double>::quiet_NaN() );

    std::size_t sinceCheck = 0;
    const std::size_t checkInterval = 2048;
    for ( std::size_t step = 0; step < cells; ++step )
    {
      const std::size_t cell = path[step];
      const int row = static_cast<int>( cell / static_cast<std::size_t>( grid.cols ) );
      const int column = static_cast<int>( cell % static_cast<std::size_t>( grid.cols ) );
      const double x0 = grid.cellCenterX( column );
      const double y0 = grid.cellCenterY( row );

      index.queryNearest( x0, y0, clamped.maxPoints, clamped.searchRadius, &staticNeighborhood );
      lattice.queryNearest( column, row, grid, clamped.maxPoints, &simulatedNeighborhood );

      // 合并邻域：静态样本优先；与静态样本重合的已模拟点剔除——
      // 重合点的 C 矩阵两行完全相同（γ(0)=0），会让 LU 奇异，
      // 且条件值已由静态样本承载（信息等价）。
      neighborhood.clear();
      neighborhood.reserve( staticNeighborhood.size() + simulatedNeighborhood.size() );
      for ( std::uint32_t staticIndex : staticNeighborhood )
      {
        const Sample &sample = deduped[staticIndex];
        neighborhood.push_back( CondPoint{ sample.x, sample.y, 0.0, table.forward( sample.value ) } );
      }
      const std::size_t staticCount = neighborhood.size();
      for ( const CondPoint &simulated : simulatedNeighborhood )
      {
        bool coincidentWithStatic = false;
        for ( std::size_t i = 0; i < staticCount; ++i )
        {
          const double ex = 1e-9 * std::max( { 1.0, std::fabs( simulated.x ), std::fabs( neighborhood[i].x ) } );
          const double ey = 1e-9 * std::max( { 1.0, std::fabs( simulated.y ), std::fabs( neighborhood[i].y ) } );
          if ( std::fabs( simulated.x - neighborhood[i].x ) <= ex &&
               std::fabs( simulated.y - neighborhood[i].y ) <= ey )
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
                                            [x0, y0, radius2]( const CondPoint &point ) {
                                              const double dx = point.x - x0;
                                              const double dy = point.y - y0;
                                              return dx * dx + dy * dy > radius2;
                                            } ),
                            neighborhood.end() );
      }
      if ( static_cast<int>( neighborhood.size() ) > clamped.maxPoints )
      {
        std::partial_sort( neighborhood.begin(), neighborhood.begin() + clamped.maxPoints,
                           neighborhood.end(), [x0, y0]( const CondPoint &a, const CondPoint &b ) {
                             const double da = ( a.x - x0 ) * ( a.x - x0 ) + ( a.y - y0 ) * ( a.y - y0 );
                             const double db = ( b.x - x0 ) * ( b.x - x0 ) + ( b.y - y0 ) * ( b.y - y0 );
                             return da < db;
                           } );
        neighborhood.resize( static_cast<std::size_t>( clamped.maxPoints ) );
      }

      double estimate = 0;
      double variance = 0;
      if ( !neighborhood.empty() &&
           detail::solveSimpleKriging( neighborhood, x0, y0, 0.0, model, &estimate, &variance ) )
      {
        const double draw = estimate + std::sqrt( variance ) * detail::gaussianRandom( rng );
        gaussianField[cell] = draw;
        lattice.setValue( column, row, draw );
      }
      else
      {
        ++result.solverFailures; // 无邻域或数值失败：该格保持 nodata（如实计数）
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
          control.progress( ( static_cast<double>( realization ) + static_cast<double>( step + 1 ) / static_cast<double>( cells ) ) / static_cast<double>( R ) );
      }
    }

    std::vector<double> &output = result.realizations[static_cast<std::size_t>( realization )];
    result.finiteCells = 0;
    result.nodataCells = 0;
    for ( std::size_t cell2 = 0; cell2 < cells; ++cell2 )
    {
      const double gaussian = gaussianField[cell2];
      if ( std::isfinite( gaussian ) )
      {
        output[cell2] = table.backTransform( gaussian );
        ++result.finiteCells;
      }
      else
      {
        ++result.nodataCells;
      }
    }
  }
  if ( control.progress )
    control.progress( 1.0 );
  result.status = Status::Ok;
  return result;
}

} // namespace paleo::geostat
