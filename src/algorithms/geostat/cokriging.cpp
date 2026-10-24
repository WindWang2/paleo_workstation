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

// #478：协克里金求解核（CoKrigingSolver 与全场扫描共用同一实现——两套
// 近乎全同的重载曾出现取消/进度语义细微差异，收敛到单一代码路径）。
// 索引/模型/参数构建一次，逐点求解可跨点复用工作区。
struct CokrigingCore
{
  CoSampleIndex primary;
  CoSampleIndex secondary;
  CoKrigingModel model;
  CoKrigingParams params;
  bool valid = false;
};

CokrigingCore buildCore( const std::vector<Sample> &primarySamples,
                         const std::vector<Sample> &secondarySamples, const CoKrigingModel &model,
                         const CoKrigingParams &params )
{
  CokrigingCore core;
  core.primary = buildIndex( primarySamples );
  core.secondary = buildIndex( secondarySamples );
  core.model = model;
  // PW-018：高斯零块金病态——与 kriging/SGS 同口径施加块金下限。
  core.model.primary = regularizedForKriging( model.primary );
  core.model.secondary = regularizedForKriging( model.secondary );
  core.params = params;
  core.params.maxPrimary = clampNeighborhood( params.maxPrimary );
  core.params.maxSecondary = clampNeighborhood( params.maxSecondary );
  if ( params.searchRadius < 0 )
    core.params.searchRadius = 0;
  core.valid = model.valid() && core.primary.index.size() > 0;
  return core;
}

// 逐点求解工作区（#478）：邻域列表、方程组与解缓冲跨点复用（全场扫描
// 每点重分配 m×m 矩阵的 churn 省掉）。非线程安全，每调用序列一份。
struct CokrigingWorkspace
{
  std::vector<std::uint32_t> primaryHood;
  std::vector<std::uint32_t> secondaryHood;
  std::vector<double> a;
  std::vector<double> b;
  std::vector<double> solution;
  LuWorkspace lu;
};

CoKrigingPointResult solveCoreAt( const CokrigingCore &core, double x, double y,
                                  CokrigingWorkspace &ws )
{
  CoKrigingPointResult result;
  if ( !core.valid )
    return result;

  const CoKrigingModel &model = core.model;
  const int kPrimary = core.params.maxPrimary > 0
                           ? core.params.maxPrimary
                           : static_cast<int>( core.primary.index.size() );
  core.primary.index.queryNearest( x, y, kPrimary, core.params.searchRadius, &ws.primaryHood );
  if ( ws.primaryHood.empty() )
    return result;
  if ( core.secondary.index.size() > 0 )
  {
    const int kSecondary = core.params.maxSecondary > 0
                               ? core.params.maxSecondary
                               : static_cast<int>( core.secondary.index.size() );
    core.secondary.index.queryNearest( x, y, kSecondary, core.params.searchRadius,
                                       &ws.secondaryHood );
  }
  else
  {
    ws.secondaryHood.clear();
  }

  const int n1 = static_cast<int>( ws.primaryHood.size() );
  const int n2 = static_cast<int>( ws.secondaryHood.size() );
  // n2 = 0 时省去 μ2 行（全零行会让加边矩阵奇异）：方程组即普通克里金。
  const int nMu = n2 > 0 ? 2 : 1;
  const int m = n1 + n2 + nMu;
  ws.a.assign( static_cast<std::size_t>( m ) * m, 0.0 );
  ws.b.assign( static_cast<std::size_t>( m ), 0.0 );

  // #478：对称填充——主/协块 γ11、γ22 为对称阵（半变差是 h 的偶函数，
  // ±h 的浮点求值逐位相同），上三角评估一次镜像对侧；交叉块 γ12(h) 同为
  // 偶函数，(主 i, 协 k) 与 (协 k, 主 i) 一值两写（旧实现全 m×m 各评
  // 各的，γ 评估量减半后数值逐位不变）。
  for ( int i = 0; i < n1; ++i )
  {
    const Sample &si = core.primary.index.point( ws.primaryHood[static_cast<std::size_t>( i )] );
    for ( int j = i + 1; j < n1; ++j )
    {
      const Sample &sj = core.primary.index.point( ws.primaryHood[static_cast<std::size_t>( j )] );
      const double gamma =
          model.primary.semivariance( si.x - sj.x, si.y - sj.y );
      ws.a[static_cast<std::size_t>( i ) * m + j] = gamma;
      ws.a[static_cast<std::size_t>( j ) * m + i] = gamma;
    }
    for ( int k = 0; k < n2; ++k )
    {
      const Sample &sk =
          core.secondary.index.point( ws.secondaryHood[static_cast<std::size_t>( k )] );
      const double cross =
          model.crossSemivariance( si.x - sk.x, si.y - sk.y );
      ws.a[static_cast<std::size_t>( i ) * m + n1 + k] = cross;
      ws.a[static_cast<std::size_t>( n1 + k ) * m + i] = cross;
    }
    ws.a[static_cast<std::size_t>( i ) * m + n1 + n2] = 1.0; // μ1 列
    ws.b[static_cast<std::size_t>( i )] = model.primary.semivariance( si.x - x, si.y - y );
  }
  for ( int i = 0; i < n2; ++i )
  {
    const int row = n1 + i;
    const Sample &si =
        core.secondary.index.point( ws.secondaryHood[static_cast<std::size_t>( i )] );
    for ( int k = i + 1; k < n2; ++k )
    {
      const Sample &sk =
          core.secondary.index.point( ws.secondaryHood[static_cast<std::size_t>( k )] );
      const double gamma =
          model.secondary.semivariance( si.x - sk.x, si.y - sk.y );
      ws.a[static_cast<std::size_t>( row ) * m + n1 + k] = gamma;
      ws.a[static_cast<std::size_t>( n1 + k ) * m + row] = gamma;
    }
    ws.a[static_cast<std::size_t>( row ) * m + n1 + n2 + 1] = 1.0; // μ2 列
    ws.b[static_cast<std::size_t>( row )] = model.crossSemivariance( si.x - x, si.y - y );
  }
  // 约束行：Σλ = 1（主变量无偏），Σν = 0（协变量不承载趋势）。
  for ( int j = 0; j < n1; ++j )
    ws.a[static_cast<std::size_t>( n1 + n2 ) * m + j] = 1.0;
  ws.b[static_cast<std::size_t>( n1 + n2 )] = 1.0;
  if ( nMu == 2 )
  {
    for ( int k = 0; k < n2; ++k )
      ws.a[static_cast<std::size_t>( n1 + n2 + 1 ) * m + n1 + k] = 1.0;
    ws.b[static_cast<std::size_t>( n1 + n2 + 1 )] = 0.0;
  }

  if ( !solveDenseLu( ws.a, m, ws.b, ws.solution, ws.lu ) )
    return result;

  double estimate = 0;
  double variance = 0;
  for ( int i = 0; i < n1; ++i )
  {
    const double weight = ws.solution[static_cast<std::size_t>( i )];
    estimate +=
        weight * core.primary.index.point( ws.primaryHood[static_cast<std::size_t>( i )] ).value;
    variance += weight * ws.b[static_cast<std::size_t>( i )];
  }
  for ( int k = 0; k < n2; ++k )
  {
    const double weight = ws.solution[static_cast<std::size_t>( n1 + k )];
    estimate += weight *
                core.secondary.index.point( ws.secondaryHood[static_cast<std::size_t>( k )] ).value;
    variance += weight * ws.b[static_cast<std::size_t>( n1 + k )];
  }
  variance += ws.solution[static_cast<std::size_t>( n1 + n2 )]; // + μ1
  const double varianceScale =
      std::max( { 1.0, model.primary.nugget + model.primary.sill, std::fabs( model.crossCorrelation ) } );
  if ( !std::isfinite( estimate ) || !varianceAdmissible( variance, varianceScale ) )
    return result;
  result.estimate = estimate;
  result.variance = variance < 0 ? 0 : variance;
  result.ok = true;
  return result;
}

} // namespace

// ---- 普通协克里金 ----

struct CoKrigingSolver::Impl
{
  CokrigingCore core;
};

CoKrigingSolver::CoKrigingSolver( const std::vector<Sample> &primarySamples,
                                  const std::vector<Sample> &secondarySamples,
                                  const CoKrigingModel &model, const CoKrigingParams &params )
  : m_impl( std::make_unique<Impl>() )
{
  m_impl->core = buildCore( primarySamples, secondarySamples, model, params );
}

CoKrigingSolver::~CoKrigingSolver() = default;
CoKrigingSolver::CoKrigingSolver( CoKrigingSolver && ) noexcept = default;
CoKrigingSolver &CoKrigingSolver::operator=( CoKrigingSolver && ) noexcept = default;

bool CoKrigingSolver::valid() const
{
  return m_impl && m_impl->core.valid;
}

int CoKrigingSolver::primaryCount() const
{
  return m_impl && m_impl->core.valid ? static_cast<int>( m_impl->core.primary.index.size() ) : 0;
}

int CoKrigingSolver::secondaryCount() const
{
  return m_impl ? static_cast<int>( m_impl->core.secondary.index.size() ) : 0;
}

int CoKrigingSolver::mergedPrimaryDuplicates() const
{
  return m_impl ? m_impl->core.primary.merged : 0;
}

int CoKrigingSolver::mergedSecondaryDuplicates() const
{
  return m_impl ? m_impl->core.secondary.merged : 0;
}

CoKrigingPointResult CoKrigingSolver::solveAt( double x, double y ) const
{
  if ( !m_impl )
    return CoKrigingPointResult{};
  // 单点调用自建工作区（每调用独立 → 公开 const API 保持可重入）；
  // 全场扫描走 solveCoreAt 复用一份（见 ordinaryCoKriging）。
  CokrigingWorkspace workspace;
  return solveCoreAt( m_impl->core, x, y, workspace );
}

CoKrigingPointResult coKrigingAt( double x, double y, const std::vector<Sample> &primary,
                                  const std::vector<Sample> &secondary, const CoKrigingModel &model,
                                  const CoKrigingParams &params )
{
  const CoKrigingSolver solver( primary, secondary, model, params );
  return solver.solveAt( x, y );
}

CoKrigingResult ordinaryCoKriging( const std::vector<Sample> &primarySamples,
                                   const std::vector<Sample> &secondarySamples,
                                   const GridSpec &grid,
                                   const CoKrigingModel &model,
                                   const CoKrigingParams &params,
                                   const Control &control )
{
  CoKrigingResult result;
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
  if ( !model.valid() )
  {
    result.message = "cokriging model invalid";
    return result;
  }

  // #478：扫描直用求解核 + 单份跨点工作区（每点 solveAt 重分配 m×m 矩阵
  // 的 churn 省掉）；求解口径与 CoKrigingSolver::solveAt 单点路径逐位一致
  //（同一 solveCoreAt）。
  const CokrigingCore core = buildCore( primarySamples, secondarySamples, model, params );
  result.mergedPrimaryDuplicates = core.primary.merged;
  result.mergedSecondaryDuplicates = core.secondary.merged;
  if ( !core.valid )
  {
    result.message = "no finite primary samples or invalid model";
    return result;
  }

  const std::size_t total = static_cast<std::size_t>( cells );
  result.estimates.assign( total, std::numeric_limits<double>::quiet_NaN() );
  result.variances.assign( total, std::numeric_limits<double>::quiet_NaN() );

  CokrigingWorkspace workspace;
  std::size_t sinceCheck = 0;
  const std::size_t checkInterval = 2048;
  for ( int row = 0; row < grid.rows; ++row )
  {
    for ( int column = 0; column < grid.cols; ++column )
    {
      const std::size_t cell = static_cast<std::size_t>( row ) * grid.cols + column;
      const CoKrigingPointResult pt =
          solveCoreAt( core, grid.cellCenterX( column ), grid.cellCenterY( row ), workspace );
      if ( pt.ok )
      {
        result.estimates[cell] = pt.estimate;
        result.variances[cell] = pt.variance;
      }
      else
      {
        ++result.solverFailures;
      }

      ++sinceCheck;
      if ( sinceCheck >= checkInterval )
      {
        sinceCheck = 0;
        // 先转发进度再轮询取消：bool 进度适配器在回调内置位取消标志，
        // 同一检查点即被捕获（与旧 SamplePoint 重载的取消时机一致）。
        if ( control.progress )
          control.progress( 0.02 + 0.98 * static_cast<double>( cell + 1 ) / static_cast<double>( total ) );
        if ( control.cancelled && control.cancelled() )
        {
          result.status = Status::Cancelled;
          result.message = "cancelled";
          return result;
        }
      }
    }
  }

  for ( double value : result.estimates )
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

CoKrigingResult ordinaryCoKriging( const std::vector<SamplePoint> &primarySamples,
                                   const std::vector<SamplePoint> &secondarySamples,
                                   const GridGeometry &grid,
                                   const CoKrigingModel &model,
                                   const KrigingParams &params,
                                   const std::function<bool( double )> &progress )
{
  // #478：bool 进度 → Control 适配后委托 Control 重载——两套近乎全同的
  // 扫描副本已出现取消语义细微差异，收敛为单一代码路径。
  CoKrigingParams coParams;
  coParams.maxPrimary = params.maxPoints;
  coParams.maxSecondary = params.maxPoints;
  coParams.searchRadius = params.searchRadius;
  bool cancelledByProgress = false;
  Control control;
  control.cancelled = [&cancelledByProgress] { return cancelledByProgress; };
  control.progress = [&cancelledByProgress, &progress]( double fraction ) {
    if ( progress && !progress( fraction ) )
      cancelledByProgress = true;
  };
  return ordinaryCoKriging( primarySamples, secondarySamples, grid, model, coParams, control );
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
  // 去重后下标 → 首个输入下标（usedSamples 契约是输入下标，#417）。
  std::vector<std::uint32_t> dedupedToInput;
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
  // D1/D4 同族（S5-M1）：azimuth/ratio 的 NaN 会让交叉 γ 塌缩、加边矩阵奇异
  //（semivariance(dx,dy) 的 NaN 传播路径与 kriging/sgs 相同），一并要求有限。
  const bool modelOk = model.range > 0 && std::isfinite( model.range ) && model.nugget >= 0 &&
                       model.sill >= 0 && std::isfinite( model.nugget ) && std::isfinite( model.sill ) &&
                       std::isfinite( model.azimuthDeg ) && std::isfinite( model.anisotropyRatio );
  if ( deduped.empty() || !modelOk )
    return;
  // #417：usedSamples 的公共契约是「输入下标」——去重后内部下标在此建
  // 反向映射，solveAt 出参前翻译（重合合并样本取首个输入下标）。
  m_impl->dedupedToInput.assign( deduped.size(),
                                 std::numeric_limits<std::uint32_t>::max() );
  for ( std::size_t i = 0; i < inputToDeduped.size(); ++i )
  {
    const std::uint32_t d = inputToDeduped[i];
    if ( d != std::numeric_limits<std::uint32_t>::max() &&
         m_impl->dedupedToInput[d] == std::numeric_limits<std::uint32_t>::max() )
      m_impl->dedupedToInput[d] = static_cast<std::uint32_t>( i );
  }
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
  // #417：邻域返回的是去重后内部下标——翻译回输入下标（契约见头文件注释）。
  result.usedSamples.reserve( neighborhood.size() );
  for ( const std::uint32_t index : neighborhood )
  {
    result.usedSamples.push_back(
        index < m_impl->dedupedToInput.size()
            ? m_impl->dedupedToInput[static_cast<std::size_t>( index )]
            : index );
  }
  result.weights.assign( solution.begin(), solution.begin() + n );
  result.estimate = estimate;
  result.variance = variance < 0 ? 0 : variance;
  result.penaltyPasses = passes;
  result.ok = true;
  return result;
}

} // namespace paleo::geostat
