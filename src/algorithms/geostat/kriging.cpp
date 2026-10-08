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

// 层：数据
namespace paleo::geostat
{

namespace
{

// 解普通克里金方程组（加边 LU）。points 为邻域样本（已去重）。
// metric 非空时井-井与井-查询点对的半方差按 warp 后的有效位移计算。
bool solveOrdinaryKriging( const std::vector<std::uint32_t> &neighborhood,
                           const detail::NeighborIndex &index, double x0, double y0,
                           const VariogramModel &model, const PairMetricWarp &metric,
                           KrigingPointResult *out )
{
  const int n = static_cast<int>( neighborhood.size() );
  const int n1 = n + 1;
  std::vector<double> a( static_cast<std::size_t>( n1 ) * n1, 0.0 );
  std::vector<double> b( static_cast<std::size_t>( n1 ), 0.0 );
  const auto semivarianceOf = [&]( double ax, double ay, double bx, double by ) {
    double dx = ax - bx;
    double dy = ay - by;
    if ( metric )
      metric( ax, ay, bx, by, &dx, &dy );
    return model.semivariance( dx, dy );
  };
  for ( int i = 0; i < n; ++i )
  {
    const Sample &si = index.point( neighborhood[static_cast<std::size_t>( i )] );
    for ( int j = i + 1; j < n; ++j )
    {
      const Sample &sj = index.point( neighborhood[static_cast<std::size_t>( j )] );
      const double gamma = semivarianceOf( si.x, si.y, sj.x, sj.y );
      a[static_cast<std::size_t>( i ) * n1 + j] = gamma;
      a[static_cast<std::size_t>( j ) * n1 + i] = gamma;
    }
    a[static_cast<std::size_t>( i ) * n1 + n] = 1.0;
    a[static_cast<std::size_t>( n ) * n1 + i] = 1.0;
    b[static_cast<std::size_t>( i )] = semivarianceOf( si.x, si.y, x0, y0 );
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
  if ( params->maxPoints > 0 )
  {
    params->maxPoints = std::clamp( params->maxPoints, 1, 64 );
    params->minPoints = std::clamp( params->minPoints, 1, params->maxPoints );
  }
  else
  {
    // maxPoints <= 0 =「分量内全部样本」口径（singlefactor 面的远场行为依赖它）。
    // minPoints 只用于半径模式覆盖闸，与 K 无关。
    params->minPoints = std::max( params->minPoints, 1 );
  }
  if ( params->searchRadius < 0 )
    params->searchRadius = 0;
}

bool modelUsable( const VariogramModel &model )
{
  return model.range > 0 && std::isfinite( model.range ) && model.nugget >= 0 &&
         model.sill >= 0 && std::isfinite( model.nugget ) && std::isfinite( model.sill );
}

// 邻域索引 + 参数的公共核：KrigingSolver（按分量重复查询）与 ordinaryKriging
// （全场扫描）共用，避免两套邻域/求解口径漂移。
struct SolverCore
{
  detail::NeighborIndex index;
  VariogramModel model;
  KrigingParams params;
  int merged = 0;
  bool valid = false;

  SolverCore( const std::vector<Sample> &samples, const VariogramModel &variogram,
              const KrigingParams &krigingParams )
    : model( variogram )
  {
    params = krigingParams;
    clampParams( &params );
    std::vector<Sample> deduped = detail::dedupeSamples( samples, &merged );
    if ( deduped.empty() || !modelUsable( model ) )
      return;
    index = detail::NeighborIndex::build( deduped );
    valid = true;
  }

  bool neighborhood( double x, double y, std::vector<std::uint32_t> *out ) const
  {
    out->clear();
    if ( !valid )
      return false;
    const int k = params.maxPoints > 0 ? params.maxPoints : static_cast<int>( index.size() );
    index.queryNearest( x, y, k, params.searchRadius, out );
    if ( params.searchRadius > 0 && static_cast<int>( out->size() ) < params.minPoints )
      return false; // 半径模式覆盖闸：不足即 nodata
    return !out->empty();
  }
};

} // namespace

struct KrigingSolver::Impl
{
  explicit Impl( const std::vector<Sample> &samples, const VariogramModel &model,
                 const KrigingParams &params )
    : core( samples, model, params )
  {
  }
  SolverCore core;
};

KrigingSolver::KrigingSolver( const std::vector<Sample> &samples, const VariogramModel &model,
                              const KrigingParams &params )
  : m_impl( std::make_unique<Impl>( samples, model, params ) )
{
}

KrigingSolver::~KrigingSolver() = default;
KrigingSolver::KrigingSolver( KrigingSolver && ) noexcept = default;
KrigingSolver &KrigingSolver::operator=( KrigingSolver && ) noexcept = default;

bool KrigingSolver::valid() const
{
  return m_impl && m_impl->core.valid;
}

int KrigingSolver::sampleCount() const
{
  return m_impl && m_impl->core.valid ? static_cast<int>( m_impl->core.index.size() ) : 0;
}

int KrigingSolver::mergedDuplicates() const
{
  return m_impl ? m_impl->core.merged : 0;
}

KrigingPointResult KrigingSolver::solveAt( double x, double y ) const
{
  return solveAt( x, y, PairMetricWarp() );
}

KrigingPointResult KrigingSolver::solveAt( double x, double y, const PairMetricWarp &metric ) const
{
  KrigingPointResult result;
  if ( !m_impl )
    return result;
  std::vector<std::uint32_t> neighborhood;
  if ( !m_impl->core.neighborhood( x, y, &neighborhood ) )
    return result;
  solveOrdinaryKriging( neighborhood, m_impl->core.index, x, y, m_impl->core.model, metric, &result );
  return result;
}

KrigingPointResult ordinaryKrigingAt( double x, double y, const std::vector<Sample> &samples,
                                      const VariogramModel &model, const KrigingParams &params )
{
  const KrigingSolver solver( samples, model, params );
  return solver.solveAt( x, y );
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

  SolverCore core( samples, model, params );
  result.mergedDuplicates = core.merged;
  if ( !core.valid )
  {
    result.message = "no finite samples";
    return result;
  }
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
      if ( core.neighborhood( grid.cellCenterX( column ), grid.cellCenterY( row ), &neighborhood ) )
      {
        KrigingPointResult point;
        if ( solveOrdinaryKriging( neighborhood, core.index,
                                   grid.cellCenterX( column ), grid.cellCenterY( row ),
                                   core.model, PairMetricWarp(), &point ) )
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
