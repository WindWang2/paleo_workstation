// 层：数据
#include "cokriging.h"

#include "linsolve.h"
#include "neighborhood.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

// 层：数据
namespace paleo::geostat
{

namespace
{

// 与 kriging.cpp 的 SolverCore 同口径的重合合并 + 桶格索引（复用 detail 工具，
// 不复制合并规则）。这里不做半径覆盖闸（minPoints 语义属于全场扫描入口）。
struct CoSampleIndex
{
  detail::NeighborIndex index;
  int merged = 0;
};

CoSampleIndex buildIndex( const std::vector<Sample> &samples )
{
  CoSampleIndex built;
  std::vector<Sample> deduped = detail::dedupeSamples( samples, &built.merged );
  built.index = detail::NeighborIndex::build( deduped );
  return built;
}

int clampNeighborhood( int value )
{
  return value > 0 ? std::clamp( value, 1, 64 ) : 0; // 0 = 全部样本
}

// 显著负方差判失败（与 solveOrdinaryKriging 同口径：不静默输出病态解）。
bool varianceAdmissible( double variance, double scale )
{
  if ( !std::isfinite( variance ) )
    return false;
  if ( variance < 0 )
    return variance >= -1e-9 * std::max( 1.0, scale );
  return true;
}

} // namespace

// ---- 普通协克里金 ----

struct CoKrigingSolver::Impl
{
  CoSampleIndex primary;
  CoSampleIndex secondary;
  CoKrigingModel model;
  CoKrigingParams params;
  bool valid = false;
};

CoKrigingSolver::CoKrigingSolver( const std::vector<Sample> &primarySamples,
                                  const std::vector<Sample> &secondarySamples,
                                  const CoKrigingModel &model, const CoKrigingParams &params )
  : m_impl( std::make_unique<Impl>() )
{
  m_impl->primary = buildIndex( primarySamples );
  m_impl->secondary = buildIndex( secondarySamples );
  m_impl->model = model;
  m_impl->params = params;
  m_impl->params.maxPrimary = clampNeighborhood( params.maxPrimary );
  m_impl->params.maxSecondary = clampNeighborhood( params.maxSecondary );
  if ( params.searchRadius < 0 )
    m_impl->params.searchRadius = 0;
  m_impl->valid = model.valid() && m_impl->primary.index.size() > 0;
}

CoKrigingSolver::~CoKrigingSolver() = default;
CoKrigingSolver::CoKrigingSolver( CoKrigingSolver && ) noexcept = default;
CoKrigingSolver &CoKrigingSolver::operator=( CoKrigingSolver && ) noexcept = default;

bool CoKrigingSolver::valid() const
{
  return m_impl && m_impl->valid;
}

int CoKrigingSolver::primaryCount() const
{
  return m_impl && m_impl->valid ? static_cast<int>( m_impl->primary.index.size() ) : 0;
}

int CoKrigingSolver::secondaryCount() const
{
  return m_impl ? static_cast<int>( m_impl->secondary.index.size() ) : 0;
}

CoKrigingPointResult CoKrigingSolver::solveAt( double x, double y ) const
{
  CoKrigingPointResult result;
  if ( !m_impl || !m_impl->valid )
    return result;

  const CoKrigingModel &model = m_impl->model;
  std::vector<std::uint32_t> primaryHood;
  const int kPrimary = m_impl->params.maxPrimary > 0
                           ? m_impl->params.maxPrimary
                           : static_cast<int>( m_impl->primary.index.size() );
  m_impl->primary.index.queryNearest( x, y, kPrimary, m_impl->params.searchRadius, &primaryHood );
  if ( primaryHood.empty() )
    return result;
  std::vector<std::uint32_t> secondaryHood;
  if ( m_impl->secondary.index.size() > 0 )
  {
    const int kSecondary = m_impl->params.maxSecondary > 0
                               ? m_impl->params.maxSecondary
                               : static_cast<int>( m_impl->secondary.index.size() );
    m_impl->secondary.index.queryNearest( x, y, kSecondary, m_impl->params.searchRadius,
                                          &secondaryHood );
  }

  const int n1 = static_cast<int>( primaryHood.size() );
  const int n2 = static_cast<int>( secondaryHood.size() );
  // n2 = 0 时省去 μ2 行（全零行会让加边矩阵奇异）：方程组即普通克里金。
  const int nMu = n2 > 0 ? 2 : 1;
  const int m = n1 + n2 + nMu;
  std::vector<double> a( static_cast<std::size_t>( m ) * m, 0.0 );
  std::vector<double> b( static_cast<std::size_t>( m ), 0.0 );

  for ( int i = 0; i < n1; ++i )
  {
    const Sample &si = m_impl->primary.index.point( primaryHood[static_cast<std::size_t>( i )] );
    for ( int j = 0; j < n1; ++j )
    {
      const Sample &sj = m_impl->primary.index.point( primaryHood[static_cast<std::size_t>( j )] );
      a[static_cast<std::size_t>( i ) * m + j] = model.primary.semivariance( si.x - sj.x, si.y - sj.y );
    }
    for ( int k = 0; k < n2; ++k )
    {
      const Sample &sk = m_impl->secondary.index.point( secondaryHood[static_cast<std::size_t>( k )] );
      a[static_cast<std::size_t>( i ) * m + n1 + k] = model.crossSemivariance( si.x - sk.x, si.y - sk.y );
    }
    a[static_cast<std::size_t>( i ) * m + n1 + n2] = 1.0; // μ1 列
    b[static_cast<std::size_t>( i )] = model.primary.semivariance( si.x - x, si.y - y );
  }
  for ( int i = 0; i < n2; ++i )
  {
    const int row = n1 + i;
    const Sample &si = m_impl->secondary.index.point( secondaryHood[static_cast<std::size_t>( i )] );
    for ( int j = 0; j < n1; ++j )
    {
      const Sample &sj = m_impl->primary.index.point( primaryHood[static_cast<std::size_t>( j )] );
      a[static_cast<std::size_t>( row ) * m + j] = model.crossSemivariance( si.x - sj.x, si.y - sj.y );
    }
    for ( int k = 0; k < n2; ++k )
    {
      const Sample &sk = m_impl->secondary.index.point( secondaryHood[static_cast<std::size_t>( k )] );
      a[static_cast<std::size_t>( row ) * m + n1 + k] =
          model.secondary.semivariance( si.x - sk.x, si.y - sk.y );
    }
    a[static_cast<std::size_t>( row ) * m + n1 + n2 + 1] = 1.0; // μ2 列
    b[static_cast<std::size_t>( row )] = model.crossSemivariance( si.x - x, si.y - y );
  }
  // 约束行：Σλ = 1（主变量无偏），Σν = 0（协变量不承载趋势）。
  for ( int j = 0; j < n1; ++j )
    a[static_cast<std::size_t>( n1 + n2 ) * m + j] = 1.0;
  b[static_cast<std::size_t>( n1 + n2 )] = 1.0;
  if ( nMu == 2 )
  {
    for ( int k = 0; k < n2; ++k )
      a[static_cast<std::size_t>( n1 + n2 + 1 ) * m + n1 + k] = 1.0;
    b[static_cast<std::size_t>( n1 + n2 + 1 )] = 0.0;
  }

  std::vector<double> solution;
  if ( !solveDenseLu( a, m, b, solution ) )
    return result;

  double estimate = 0;
  double variance = 0;
  for ( int i = 0; i < n1; ++i )
  {
    const double weight = solution[static_cast<std::size_t>( i )];
    estimate += weight * m_impl->primary.index.point( primaryHood[static_cast<std::size_t>( i )] ).value;
    variance += weight * b[static_cast<std::size_t>( i )];
  }
  for ( int k = 0; k < n2; ++k )
  {
    const double weight = solution[static_cast<std::size_t>( n1 + k )];
    estimate += weight * m_impl->secondary.index.point( secondaryHood[static_cast<std::size_t>( k )] ).value;
    variance += weight * b[static_cast<std::size_t>( n1 + k )];
  }
  variance += solution[static_cast<std::size_t>( n1 + n2 )]; // + μ1
  const double varianceScale =
      std::max( { 1.0, model.primary.nugget + model.primary.sill, std::fabs( model.crossCorrelation ) } );
  if ( !std::isfinite( estimate ) || !varianceAdmissible( variance, varianceScale ) )
    return result;
  result.estimate = estimate;
  result.variance = variance < 0 ? 0 : variance;
  result.ok = true;
  return result;
}

CoKrigingPointResult coKrigingAt( double x, double y, const std::vector<Sample> &primary,
                                  const std::vector<Sample> &secondary, const CoKrigingModel &model,
                                  const CoKrigingParams &params )
{
  const CoKrigingSolver solver( primary, secondary, model, params );
  return solver.solveAt( x, y );
}

// ---- 带约束普通克里金（软罚主动集）----

namespace
{

// 邻域内的约束投影：组成员 → 邻域位置。空组（成员都不在邻域）不参与。
struct NeighborhoodConstraint
{
  std::vector<int> positions; // 邻域位置（升序）
  double cap = 0;
  double penalty = 1.0;
  bool active = false; // 当前轮是否违约（进正规方程）
};

} // namespace

struct ConstrainedKrigingSolver::Impl
{
  detail::NeighborIndex index;
  VariogramModel model;
  KrigingParams params;
  std::vector<WeightGroupConstraint> constraints;
  int merged = 0;
  bool valid = false;
};

ConstrainedKrigingSolver::ConstrainedKrigingSolver(
    const std::vector<Sample> &samples, const VariogramModel &model, const KrigingParams &params,
    const std::vector<WeightGroupConstraint> &constraints )
  : m_impl( std::make_unique<Impl>() )
{
  m_impl->model = model;
  m_impl->params = params;
  if ( params.maxPoints > 0 )
  {
    m_impl->params.maxPoints = std::clamp( params.maxPoints, 1, 64 );
    m_impl->params.minPoints = std::clamp( params.minPoints, 1, m_impl->params.maxPoints );
  }
  else
  {
    m_impl->params.minPoints = std::max( params.minPoints, 1 );
  }
  if ( params.searchRadius < 0 )
    m_impl->params.searchRadius = 0;
  m_impl->constraints = constraints;
  std::vector<std::uint32_t> inputToDeduped;
  std::vector<Sample> deduped = detail::dedupeSamples( samples, &m_impl->merged, &inputToDeduped );
  const bool modelOk = model.range > 0 && std::isfinite( model.range ) && model.nugget >= 0 &&
                       model.sill >= 0 && std::isfinite( model.nugget ) && std::isfinite( model.sill );
  if ( deduped.empty() || !modelOk )
    return;
  // 非法约束（罚权非正 / 空组）剔除，不让单条坏数据拖垮整个求解器；
  // 输入下标经重合合并映射翻译（非有限样本映射为哨兵 → 组员剔除）。
  const std::uint32_t sentinel = std::numeric_limits<std::uint32_t>::max();
  std::vector<WeightGroupConstraint> mapped;
  mapped.reserve( m_impl->constraints.size() );
  for ( const WeightGroupConstraint &constraint : m_impl->constraints )
  {
    if ( !( constraint.penalty > 0 ) || constraint.sampleIndices.empty() )
      continue;
    WeightGroupConstraint translated;
    translated.cap = constraint.cap;
    translated.penalty = constraint.penalty;
    for ( std::uint32_t index : constraint.sampleIndices )
    {
      if ( index < inputToDeduped.size() && inputToDeduped[index] != sentinel )
        translated.sampleIndices.push_back( inputToDeduped[index] );
    }
    std::sort( translated.sampleIndices.begin(), translated.sampleIndices.end() );
    translated.sampleIndices.erase(
        std::unique( translated.sampleIndices.begin(), translated.sampleIndices.end() ),
        translated.sampleIndices.end() );
    if ( !translated.sampleIndices.empty() )
      mapped.push_back( std::move( translated ) );
  }
  m_impl->constraints = std::move( mapped );
  m_impl->index = detail::NeighborIndex::build( deduped );
  m_impl->valid = true;
}

ConstrainedKrigingSolver::~ConstrainedKrigingSolver() = default;
ConstrainedKrigingSolver::ConstrainedKrigingSolver( ConstrainedKrigingSolver && ) noexcept = default;
ConstrainedKrigingSolver &ConstrainedKrigingSolver::operator=( ConstrainedKrigingSolver && ) noexcept = default;

bool ConstrainedKrigingSolver::valid() const
{
  return m_impl && m_impl->valid;
}

int ConstrainedKrigingSolver::sampleCount() const
{
  return m_impl && m_impl->valid ? static_cast<int>( m_impl->index.size() ) : 0;
}

int ConstrainedKrigingSolver::constraintCount() const
{
  return m_impl ? static_cast<int>( m_impl->constraints.size() ) : 0;
}

ConstrainedKrigingPointResult ConstrainedKrigingSolver::solveAt( double x, double y ) const
{
  ConstrainedKrigingPointResult result;
  if ( !m_impl || !m_impl->valid )
    return result;

  const VariogramModel &model = m_impl->model;
  std::vector<std::uint32_t> neighborhood;
  const int k = m_impl->params.maxPoints > 0 ? m_impl->params.maxPoints
                                             : static_cast<int>( m_impl->index.size() );
  m_impl->index.queryNearest( x, y, k, m_impl->params.searchRadius, &neighborhood );
  if ( neighborhood.empty() )
    return result;
  const int n = static_cast<int>( neighborhood.size() );
  const int n1 = n + 1;

  // 约束投影到本邻域。
  std::vector<NeighborhoodConstraint> local;
  for ( const WeightGroupConstraint &constraint : m_impl->constraints )
  {
    NeighborhoodConstraint projected;
    projected.cap = constraint.cap;
    projected.penalty = constraint.penalty;
    for ( std::uint32_t index : constraint.sampleIndices )
    {
      const auto found = std::find( neighborhood.begin(), neighborhood.end(), index );
      if ( found != neighborhood.end() )
        projected.positions.push_back( static_cast<int>( found - neighborhood.begin() ) );
    }
    if ( !projected.positions.empty() )
      local.push_back( std::move( projected ) );
  }

  // 基础 OK 系统（与 solveOrdinaryKriging 同布局）。
  std::vector<double> gamma( static_cast<std::size_t>( n1 ) * n1, 0.0 );
  std::vector<double> rhs( static_cast<std::size_t>( n1 ), 0.0 );
  for ( int i = 0; i < n; ++i )
  {
    const Sample &si = m_impl->index.point( neighborhood[static_cast<std::size_t>( i )] );
    for ( int j = i + 1; j < n; ++j )
    {
      const Sample &sj = m_impl->index.point( neighborhood[static_cast<std::size_t>( j )] );
      const double gammaValue = model.semivariance( si.x - sj.x, si.y - sj.y );
      gamma[static_cast<std::size_t>( i ) * n1 + j] = gammaValue;
      gamma[static_cast<std::size_t>( j ) * n1 + i] = gammaValue;
    }
    gamma[static_cast<std::size_t>( i ) * n1 + n] = 1.0;
    gamma[static_cast<std::size_t>( n ) * n1 + i] = 1.0;
    rhs[static_cast<std::size_t>( i )] = model.semivariance( si.x - x, si.y - y );
  }
  rhs[static_cast<std::size_t>( n )] = 1.0;

  const double varianceScale = std::max( 1.0, model.nugget + model.sill );
  const double violationTolerance = 1e-9 * varianceScale;
  std::vector<double> solution;
  std::vector<double> a;
  std::vector<double> b;
  int passes = 0;
  // 软罚主动集（单调只增，不回退——回退会在「罚下满足/释放后违约」间振荡）：
  // 每轮解含当前主动集罚项的系统，把仍未满足（> cap）的非主动约束加入主动集；
  // 无新违约即停。主动约束的软罚把组权重拉向 cap，不保证严格 ≤ cap（如实报告
  // 权重，消费方自行核）。上限 4 轮：极端配置未收敛时用末轮解（penaltyPasses
  // 如实记轮数，不冒充收敛）。
  for ( int attempt = 0; attempt < 4; ++attempt )
  {
    a = gamma;
    b = rhs;
    for ( const NeighborhoodConstraint &constraint : local )
    {
      if ( !constraint.active )
        continue;
      for ( int p : constraint.positions )
      {
        b[static_cast<std::size_t>( p )] += constraint.penalty * constraint.cap;
        for ( int q : constraint.positions )
          a[static_cast<std::size_t>( p ) * n1 + q] += constraint.penalty;
      }
    }
    if ( !solveDenseLu( a, n1, b, solution ) )
      return result;
    passes = attempt + 1;
    bool newlyViolated = false;
    for ( NeighborhoodConstraint &constraint : local )
    {
      if ( constraint.active )
        continue;
      double sum = 0;
      for ( int p : constraint.positions )
        sum += solution[static_cast<std::size_t>( p )];
      if ( sum > constraint.cap + violationTolerance )
      {
        constraint.active = true;
        newlyViolated = true;
      }
    }
    if ( !newlyViolated )
      break;
  }

  double estimate = 0;
  double variance = 0;
  double weightSum = 0;
  for ( int i = 0; i < n; ++i )
  {
    const double weight = solution[static_cast<std::size_t>( i )];
    estimate += weight * m_impl->index.point( neighborhood[static_cast<std::size_t>( i )] ).value;
    variance += weight * rhs[static_cast<std::size_t>( i )];
    weightSum += weight;
  }
  variance += solution[static_cast<std::size_t>( n )]; // + μ
  if ( !std::isfinite( estimate ) || !varianceAdmissible( variance, varianceScale ) )
    return result;
  if ( std::fabs( weightSum - 1.0 ) > 1e-6 )
    return result; // 权重和无偏约束被数值破坏：如实判失败
  result.usedSamples = neighborhood;
  result.weights.assign( solution.begin(), solution.begin() + n );
  result.estimate = estimate;
  result.variance = variance < 0 ? 0 : variance;
  result.penaltyPasses = passes;
  result.ok = true;
  return result;
}

} // namespace paleo::geostat
