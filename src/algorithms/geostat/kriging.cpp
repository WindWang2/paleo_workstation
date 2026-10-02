// 层：数据
#include "kriging.h"

#include "linsolve.h"
#include "neighborhood.h"

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

// 解普通克里金方程组（加边 LU）。points 为邻域样本（已去重）。
bool solveOrdinaryKriging( const std::vector<std::uint32_t> &neighborhood,
                           const detail::NeighborIndex &index, double x0, double y0,
                           const VariogramModel &model, KrigingPointResult *out )
{
  const int n = static_cast<int>( neighborhood.size() );
  const int n1 = n + 1;
  std::vector<double> a( static_cast<std::size_t>( n1 ) * n1, 0.0 );
  std::vector<double> b( static_cast<std::size_t>( n1 ), 0.0 );
  for ( int i = 0; i < n; ++i )
  {
    const Sample &si = index.point( neighborhood[static_cast<std::size_t>( i )] );
    for ( int j = i + 1; j < n; ++j )
    {
      const Sample &sj = index.point( neighborhood[static_cast<std::size_t>( j )] );
      const double gamma = model.semivariance( si.x - sj.x, si.y - sj.y );
      a[static_cast<std::size_t>( i ) * n1 + j] = gamma;
      a[static_cast<std::size_t>( j ) * n1 + i] = gamma;
    }
    a[static_cast<std::size_t>( i ) * n1 + n] = 1.0;
    a[static_cast<std::size_t>( n ) * n1 + i] = 1.0;
    b[static_cast<std::size_t>( i )] = model.semivariance( si.x - x0, si.y - y0 );
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
  const std::vector<Sample> deduped = detail::dedupeSamples( samples, &merged );
  if ( deduped.empty() )
    return result;
  const detail::NeighborIndex index = detail::NeighborIndex::build( deduped );
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

  const std::vector<Sample> deduped = detail::dedupeSamples( samples, &result.mergedDuplicates );
  if ( deduped.empty() )
  {
    result.message = "no finite samples";
    return result;
  }
  const detail::NeighborIndex index = detail::NeighborIndex::build( deduped );
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
