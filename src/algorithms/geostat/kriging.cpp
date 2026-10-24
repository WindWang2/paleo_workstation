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

// 合法 OK 权重的负值来自屏蔽效应，Σ|λ| 通常 ≤ 2~3；超过此阈视为病态。
constexpr double kMaxAbsWeightSum = 10.0;

bool finishOrdinaryKriging( const std::vector<std::uint32_t> &neighborhood,
                            const detail::NeighborIndex &index, const VariogramModel &model,
                            const std::vector<double> &b, const std::vector<double> &solution,
                            KrigingPointResult *out );

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
  return finishOrdinaryKriging( neighborhood, index, model, b, solution, out );
}

// 解向量 → 估值/方差 + 合理性闸（局部邻域与全局一次分解两条路径共用）。
bool finishOrdinaryKriging( const std::vector<std::uint32_t> &neighborhood,
                            const detail::NeighborIndex &index, const VariogramModel &model,
                            const std::vector<double> &b, const std::vector<double> &solution,
                            KrigingPointResult *out )
{
  const int n = static_cast<int>( neighborhood.size() );
  double estimate = 0;
  double variance = 0;
  double absWeightSum = 0;
  for ( int i = 0; i < n; ++i )
  {
    const double weight = solution[static_cast<std::size_t>( i )];
    absWeightSum += std::fabs( weight );
    estimate += weight * index.point( neighborhood[static_cast<std::size_t>( i )] ).value;
    variance += weight * b[static_cast<std::size_t>( i )];
  }
  variance += solution[static_cast<std::size_t>( n )]; // + μ
  if ( !std::isfinite( estimate ) || !std::isfinite( variance ) )
    return false;
  // PW-018：病态闸——Σλ=1 下 Σ|λ| 远大于 1 意味着大幅正负抵消的权重
  //（近奇异方程组），估值可越出数据范围数个量级。如实判失败而非 ok=true。
  if ( absWeightSum > kMaxAbsWeightSum )
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
  // D2：零信号模型（nugget=sill=0，γ≡0）会让加边 OK 矩阵只剩 border 元素，
  // LU 判奇异 → 每格失败而 Status 仍 Ok——入口拒绝（上游 krigingsurface 的
  // resolveVariogram 已有同款闸，此处补裸 API 面）。
  // D4：azimuthDeg/anisotropyRatio 非有限时 semivariance(dx,dy) 静默塌缩（NaN
  // 传播成 γ≡0 或各向同性退化），矩阵无病征地奇异——一并拒绝。
  return model.range > 0 && std::isfinite( model.range ) && model.nugget >= 0 &&
         model.sill >= 0 && std::isfinite( model.nugget ) && std::isfinite( model.sill ) &&
         model.nugget + model.sill > 0 && std::isfinite( model.azimuthDeg ) &&
         std::isfinite( model.anisotropyRatio );
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
    : model( regularizedForKriging( variogram ) )
  {
    params = krigingParams;
    clampParams( &params );
    std::vector<Sample> deduped = detail::dedupeSamples( samples, &merged );
    if ( deduped.empty() || !modelUsable( model ) )
      return;
    index = detail::NeighborIndex::build( deduped );
    valid = true;
    buildGlobalFactor();
  }

  // PW-079：maxPoints<=0 且无半径 = 每格都用全部 n 个样本。方程组矩阵与待估点
  // 无关（只有右端项变），一次分解后每格只做 O(n²) 回代，替代每格 O(n³)。
  // 内存 (n+1)² double，n 超过上限时退回逐格求解（旧口径）。
  static constexpr std::size_t kMaxGlobalFactorSamples = 4000;
  bool global = false;
  bool globalFactorOk = false;
  LuWorkspace globalLu;
  std::vector<std::uint32_t> globalOrder;

  void buildGlobalFactor()
  {
    if ( params.maxPoints > 0 || params.searchRadius > 0 || index.size() > kMaxGlobalFactorSamples )
      return;
    global = true;
    const int n = static_cast<int>( index.size() );
    const int n1 = n + 1;
    globalOrder.resize( index.size() );
    for ( std::size_t i = 0; i < index.size(); ++i )
      globalOrder[i] = static_cast<std::uint32_t>( i );
    std::vector<double> a( static_cast<std::size_t>( n1 ) * n1, 0.0 );
    for ( int i = 0; i < n; ++i )
    {
      const Sample &si = index.point( static_cast<std::uint32_t>( i ) );
      for ( int j = i + 1; j < n; ++j )
      {
        const Sample &sj = index.point( static_cast<std::uint32_t>( j ) );
        const double gamma = model.semivariance( si.x - sj.x, si.y - sj.y );
        a[static_cast<std::size_t>( i ) * n1 + j] = gamma;
        a[static_cast<std::size_t>( j ) * n1 + i] = gamma;
      }
      a[static_cast<std::size_t>( i ) * n1 + n] = 1.0;
      a[static_cast<std::size_t>( n ) * n1 + i] = 1.0;
    }
    globalFactorOk = luFactor( a, n1, globalLu );
  }

  // 全局路径的单点求解；global=false 时调用方走邻域路径。
  bool solveGlobal( double x0, double y0, KrigingPointResult *out ) const
  {
    if ( !globalFactorOk )
      return false;
    const int n = static_cast<int>( index.size() );
    std::vector<double> b( static_cast<std::size_t>( n ) + 1, 1.0 );
    for ( int i = 0; i < n; ++i )
    {
      const Sample &si = index.point( static_cast<std::uint32_t>( i ) );
      b[static_cast<std::size_t>( i )] = model.semivariance( si.x - x0, si.y - y0 );
    }
    std::vector<double> solution;
    luSolve( globalLu, n + 1, b, solution );
    return finishOrdinaryKriging( globalOrder, index, model, b, solution, out );
  }

  bool solve( double x, double y, std::vector<std::uint32_t> *scratch, KrigingPointResult *out ) const
  {
    if ( !valid )
      return false;
    if ( global )
      return solveGlobal( x, y, out );
    if ( !neighborhood( x, y, scratch ) )
      return false;
    return solveOrdinaryKriging( *scratch, index, x, y, model, out );
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
  KrigingPointResult result;
  if ( !m_impl )
    return result;
  std::vector<std::uint32_t> neighborhood;
  m_impl->core.solve( x, y, &neighborhood, &result );
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
  if ( !( model.range > 0 ) || !std::isfinite( model.range ) ||
       !std::isfinite( model.nugget ) || !std::isfinite( model.sill ) ||
       model.nugget < 0 || model.sill < 0 || model.nugget + model.sill <= 0 ||
       !std::isfinite( model.azimuthDeg ) || !std::isfinite( model.anisotropyRatio ) )
  {
    result.message =
        "variogram model invalid (finite range > 0, nugget/sill >= 0, positive total sill)";
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
      const double x0 = grid.cellCenterX( column );
      const double y0 = grid.cellCenterY( row );
      // M3：非全局路径邻域只查一次——旧代码先 neighborhood() 再 core.solve()
      // 内部又查一遍，每格 k-NN 重复两次。
      if ( core.global || core.neighborhood( x0, y0, &neighborhood ) )
      {
        KrigingPointResult point;
        const bool solved = core.global
            ? core.solveGlobal( x0, y0, &point )
            : solveOrdinaryKriging( neighborhood, core.index, x0, y0, core.model, &point );
        if ( solved )
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
  // H1：求解器全军覆没（每格都失败、无一有限值）不得报 Ok——旧行为整幅
  // NaN 仍 Status::Ok，调用方无从区分「无数据」与「数值失败」。
  if ( result.finiteCells == 0 && result.solverFailures > 0 )
  {
    result.status = Status::NumericalFailure;
    result.message = "kriging system singular for every cell (solver failed everywhere)";
    return result;
  }
  result.status = Status::Ok;
  return result;
}

} // namespace paleo::geostat
