// 层：数据
#include "sgs.h"

#include "linsolve.h"
#include "neighborhood.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <random>
#include <utility>

// 层：数据
namespace paleo::geostat
{

namespace
{

struct GaussianPoint
{
  double x = 0;
  double y = 0;
  double value = 0; // 高斯域值
};

// 确定性 (0,1) 均匀：mt19937_64 原始 u64 → 53bit 尾数。
double unitRandom( std::mt19937_64 &rng )
{
  return ( static_cast<double>( rng() >> 11 ) + 0.5 ) * 0x1.0p-53;
}

double gaussianRandom( std::mt19937_64 &rng )
{
  // Box-Muller（cos 分支；u1 ∈ (0,1) 保证 log 有限）
  const double u1 = unitRandom( rng );
  const double u2 = unitRandom( rng );
  return std::sqrt( -2.0 * std::log( u1 ) ) * std::cos( 2.0 * std::numbers::pi * u2 );
}

// Φ⁻¹：erf 二分（erf 严格单调、std 实现可移植；100 次迭代到双精度
// 饱和。建表每样本只调一次，成本可忽略。不用有理逼近——系数手打
// 易错，尾部分支爆过 e300（见 ledger 轮3 纠错记录）。
double standardNormalQuantile( double p )
{
  const double target = 2.0 * std::clamp( p, 1e-12, 1.0 - 1e-12 ) - 1.0;
  double lo = -9.0; // erf(±9) = ±1 − <1e-17，覆盖双精度全域
  double hi = 9.0;
  for ( int iteration = 0; iteration < 100; ++iteration )
  {
    const double mid = 0.5 * ( lo + hi );
    if ( std::erf( mid ) < target )
      lo = mid;
    else
      hi = mid;
  }
  return std::numbers::sqrt2 * 0.5 * ( lo + hi );
}

// 正态得分变换：排序 → (i+0.5)/n 分位 → Φ⁻¹。同值并列取平均分位。
struct NormalScoreTable
{
  std::vector<double> z; // 升序原值（并列合组）
  std::vector<double> y; // 对应高斯得分（升序）
  double sampleMean = 0;
  double sampleStd = 0;

  static NormalScoreTable build( const std::vector<Sample> &samples )
  {
    NormalScoreTable table;
    std::vector<double> values;
    values.reserve( samples.size() );
    double sum = 0;
    double sumSq = 0;
    for ( const Sample &sample : samples )
    {
      values.push_back( sample.value );
      sum += sample.value;
      sumSq += sample.value * sample.value;
    }
    std::sort( values.begin(), values.end() );
    const double n = static_cast<double>( values.size() );
    table.sampleMean = sum / n;
    table.sampleStd = std::sqrt( std::max( 0.0, sumSq / n - table.sampleMean * table.sampleMean ) );
    std::size_t i = 0;
    while ( i < values.size() )
    {
      std::size_t j = i;
      while ( j + 1 < values.size() &&
              values[j + 1] - values[i] <= 1e-12 * std::max( 1.0, std::fabs( values[i] ) ) )
        ++j;
      double rankSum = 0;
      for ( std::size_t k = i; k <= j; ++k )
        rankSum += ( static_cast<double>( k ) + 0.5 );
      const double p = rankSum / static_cast<double>( j - i + 1 ) / n;
      table.z.push_back( values[i] );
      table.y.push_back( standardNormalQuantile( std::clamp( p, 1e-9, 1.0 - 1e-9 ) ) );
      i = j + 1;
    }
    return table;
  }

  // 样本值 → 高斯得分（build 的样本必命中某组；越界值线性外推兜底）。
  double forward( double value ) const
  {
    if ( z.size() == 1 )
      return y.front();
    if ( value <= z.front() )
    {
      const double slope = ( y[1] - y[0] ) / std::max( z[1] - z[0], 1e-12 );
      return y[0] + slope * ( value - z.front() );
    }
    if ( value >= z.back() )
    {
      const std::size_t m = z.size();
      const double slope = ( y[m - 1] - y[m - 2] ) / std::max( z[m - 1] - z[m - 2], 1e-12 );
      return y[m - 1] + slope * ( value - z.back() );
    }
    const std::size_t k = static_cast<std::size_t>(
        std::upper_bound( z.begin(), z.end(), value ) - z.begin() ) - 1;
    const double t = ( value - z[k] ) / std::max( z[k + 1] - z[k], 1e-12 );
    return y[k] + t * ( y[k + 1] - y[k] );
  }

  // 反变换：线性内插 + 端部按末段斜率线性外推（直方图忠实样本分布）。
  double backTransform( double gaussian ) const
  {
    if ( z.size() == 1 )
      return z.front(); // 单样本退化：任何高斯值都映回该值（如实退化）
    if ( gaussian <= y.front() )
    {
      const double slope = ( z[1] - z[0] ) / std::max( y[1] - y[0], 1e-12 );
      return z[0] + slope * ( gaussian - y.front() );
    }
    if ( gaussian >= y.back() )
    {
      const std::size_t m = z.size();
      const double slope = ( z[m - 1] - z[m - 2] ) / std::max( y[m - 1] - y[m - 2], 1e-12 );
      return z[m - 1] + slope * ( gaussian - y.back() );
    }
    const std::size_t k = static_cast<std::size_t>(
        std::upper_bound( y.begin(), y.end(), gaussian ) - y.begin() ) - 1;
    const double t = ( gaussian - y[k] ) / std::max( y[k + 1] - y[k], 1e-12 );
    return z[k] + t * ( z[k + 1] - z[k] );
  }
};

// 已模拟格的格点邻域查询（模拟点恰在格心，格上环扫 + 堆剪枝）。
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
                     std::vector<GaussianPoint> *out ) const
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
      out->push_back( GaussianPoint{ grid.cellCenterX( cellColumn ), grid.cellCenterY( cellRow ),
                                     m_values[cell] } );
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

// 简单克里金（高斯域，均值 0 已知）：C(h) = S − γ(h)，解 Cw = c0。
// 估计 = Σwᵢyᵢ，方差 = S − Σwᵢc0ᵢ。
bool solveSimpleKriging( const std::vector<GaussianPoint> &data, double x0, double y0,
                         const VariogramModel &model, double *estimate, double *variance )
{
  const int n = static_cast<int>( data.size() );
  if ( n <= 0 )
    return false;
  const double totalSill = model.nugget + model.sill;
  std::vector<double> a( static_cast<std::size_t>( n ) * n );
  std::vector<double> b( static_cast<std::size_t>( n ) );
  for ( int i = 0; i < n; ++i )
  {
    for ( int j = i; j < n; ++j )
    {
      if ( i == j )
      {
        a[static_cast<std::size_t>( i ) * n + i] = totalSill; // C(0) = S
        continue;
      }
      const double gamma = model.semivariance(
          data[static_cast<std::size_t>( i )].x - data[static_cast<std::size_t>( j )].x,
          data[static_cast<std::size_t>( i )].y - data[static_cast<std::size_t>( j )].y );
      const double covariance = totalSill - gamma;
      a[static_cast<std::size_t>( i ) * n + j] = covariance;
      a[static_cast<std::size_t>( j ) * n + i] = covariance;
    }
    b[static_cast<std::size_t>( i )] =
        totalSill - model.semivariance( data[static_cast<std::size_t>( i )].x - x0,
                                        data[static_cast<std::size_t>( i )].y - y0 );
  }
  std::vector<double> weights;
  if ( !solveDenseLu( a, n, b, weights ) )
    return false;
  double est = 0;
  double var = totalSill;
  for ( int i = 0; i < n; ++i )
  {
    est += weights[static_cast<std::size_t>( i )] * data[static_cast<std::size_t>( i )].value;
    var -= weights[static_cast<std::size_t>( i )] * b[static_cast<std::size_t>( i )];
  }
  if ( !std::isfinite( est ) || !std::isfinite( var ) )
    return false;
  if ( var < 0 )
  {
    if ( var < -1e-9 * std::max( 1.0, totalSill ) )
      return false;
    var = 0;
  }
  *estimate = est;
  *variance = var;
  return true;
}

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
  const NormalScoreTable table = NormalScoreTable::build( deduped );
  result.sampleMean = table.sampleMean;
  result.sampleStd = table.sampleStd;

  // 静态样本（高斯域）
  std::vector<GaussianPoint> staticPoints;
  staticPoints.reserve( deduped.size() );
  for ( const Sample &sample : deduped )
    staticPoints.push_back( GaussianPoint{ sample.x, sample.y, table.forward( sample.value ) } );
  const detail::NeighborIndex index = detail::NeighborIndex::build( deduped );

  const std::size_t cells = static_cast<std::size_t>( cells64 );
  const int R = clamped.nRealizations;
  result.realizations.assign( static_cast<std::size_t>( R ),
                              std::vector<double>( cells,
                                                   std::numeric_limits<double>::quiet_NaN() ) );

  std::mt19937_64 rng( clamped.seed );
  SimulatedLattice lattice;
  std::vector<std::uint32_t> staticNeighborhood;
  std::vector<GaussianPoint> simulatedNeighborhood;
  std::vector<GaussianPoint> neighborhood;
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
        neighborhood.push_back( GaussianPoint{ sample.x, sample.y, table.forward( sample.value ) } );
      }
      const std::size_t staticCount = neighborhood.size();
      for ( const GaussianPoint &simulated : simulatedNeighborhood )
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
                                            [x0, y0, radius2]( const GaussianPoint &point ) {
                                              const double dx = point.x - x0;
                                              const double dy = point.y - y0;
                                              return dx * dx + dy * dy > radius2;
                                            } ),
                            neighborhood.end() );
      }
      if ( static_cast<int>( neighborhood.size() ) > clamped.maxPoints )
      {
        std::partial_sort( neighborhood.begin(), neighborhood.begin() + clamped.maxPoints,
                           neighborhood.end(), [x0, y0]( const GaussianPoint &a, const GaussianPoint &b ) {
                             const double da = ( a.x - x0 ) * ( a.x - x0 ) + ( a.y - y0 ) * ( a.y - y0 );
                             const double db = ( b.x - x0 ) * ( b.x - x0 ) + ( b.y - y0 ) * ( b.y - y0 );
                             return da < db;
                           } );
        neighborhood.resize( static_cast<std::size_t>( clamped.maxPoints ) );
      }

      double estimate = 0;
      double variance = 0;
      if ( !neighborhood.empty() && solveSimpleKriging( neighborhood, x0, y0, model, &estimate, &variance ) )
      {
        const double draw = estimate + std::sqrt( variance ) * gaussianRandom( rng );
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
