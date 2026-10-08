// 层：数据
#pragma once

#include "kriging.h"
#include "types.h"
#include "variogram.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// geostat/cokriging — 协克里金（OCK）与带约束普通克里金核。
//
// 协克里金交叉结构（口径钉死，简化起步）：
//   二级协同模型按 Markov MM1 近似——交叉半方差 γ12(h) = ρ·γ1(h)，
//   ρ 是主/协变量配置点相关系数（|ρ| ≤ 1），γ1 为主变量变差函数，
//   γ2 为协变量自身变差函数（独立给定，可由协变量样本拟合）。
//   这是近似不是严格 LMC：LMC 要求 |ρ| ≤ sqrt(c11·c22) 逐结构成立，
//   MM1 只保证交叉形状与主结构成比例；γ1/γ2 结构失配 + |ρ| 接近 1 时
//   方程组可能轻微病态（显著负方差判失败，不静默输出，与 OK 同口径）。
//   ρ = 0 时交叉项消失，方程组严格退化为普通克里金（测试对拍）。
//
// 带约束 OK（软罚口径，注释钉死）：
//   方向线/软边界表达为「样本组权重上限」不等式约束 Σ_{i∈组} λᵢ ≤ cap
//   （跨方向线组 cap=0 即线另一侧井不进权重；软边界=界外组 cap）。
//   不等式硬约束（KKT/等式行）会破坏加边矩阵的条件正定并需要 QP 求解；
//   这里用单边二次软罚：只有违约的约束才进正规方程（主动集迭代 ≤4 轮），
//   权重和恒为 1（Lagrange 行保留）。方差公式沿用 σ² = Σλᵢγᵢ₀ + μ，
//   是近似口径——惩罚把权重拉离无约束最优，报告值不再保证最小方差。
// 层：数据
namespace paleo::geostat
{

// 二级协同模型（MM1 简化）：γ12(h) = crossCorrelation · γ1(h)。
struct CoKrigingModel
{
  VariogramModel primary;      // γ11：主变量
  VariogramModel secondary;    // γ22：协变量
  double crossCorrelation = 0; // ρ（配置点相关系数），|ρ| ≤ 1

  bool valid() const
  {
    return std::fabs( crossCorrelation ) <= 1.0 + 1e-12 && primary.range > 0 &&
           primary.nugget >= 0 && primary.sill >= 0 && secondary.range > 0 &&
           secondary.nugget >= 0 && secondary.sill >= 0;
  }

  // 交叉半方差（对称：γ12 是 h 的偶函数）。
  double crossSemivariance( double dx, double dy ) const
  {
    return crossCorrelation * primary.semivariance( dx, dy );
  }
};

struct CoKrigingParams
{
  int maxPrimary = 16;     // 主变量邻域上限，<= 0 = 全部
  int maxSecondary = 16;   // 协变量邻域上限，<= 0 = 全部
  int minPrimary = 1;      // 半径模式下主邻域少于该值即无值（与 KrigingParams.minPoints 同口径）
  double searchRadius = 0; // <= 0 → 不过滤
};

struct CoKrigingPointResult
{
  bool ok = false;
  double estimate = 0;
  double variance = 0;
};

// 普通协克里金（两变量、无趋势）：Σλ=1（主）、Σν=0（协）双 Lagrange 行。
// 预建索引重复查询；单点函数内部走同一求解器（测试与编排复用）。
class CoKrigingSolver
{
  public:
    CoKrigingSolver( const std::vector<Sample> &primary, const std::vector<Sample> &secondary,
                     const CoKrigingModel &model, const CoKrigingParams &params );
    ~CoKrigingSolver();
    CoKrigingSolver( CoKrigingSolver && ) noexcept;
    CoKrigingSolver &operator=( CoKrigingSolver && ) noexcept;
    CoKrigingSolver( const CoKrigingSolver & ) = delete;
    CoKrigingSolver &operator=( const CoKrigingSolver & ) = delete;

    // 样本为空或模型非法时为假；此时 solveAt 一律返回 ok=false。
    bool valid() const;
    int primaryCount() const;
    int secondaryCount() const;

    CoKrigingPointResult solveAt( double x, double y ) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

CoKrigingPointResult coKrigingAt( double x, double y, const std::vector<Sample> &primary,
                                  const std::vector<Sample> &secondary, const CoKrigingModel &model,
                                  const CoKrigingParams &params );

// 带约束 OK：样本组权重上限（软罚）。sampleIndices 引用构造时输入的
// samples 下标（不随邻域变化）；邻域外的成员不参与该条约束（权重恒 0）。
struct WeightGroupConstraint
{
  std::vector<std::uint32_t> sampleIndices; // 组成员（指示向量系数）
  double cap = 0;                           // Σ_{i∈组} λᵢ ≤ cap
  double penalty = 1.0;                     // 软罚权重 w > 0（量纲同半方差）
};

struct ConstrainedKrigingPointResult
{
  bool ok = false;
  double estimate = 0;
  double variance = 0;
  std::vector<std::uint32_t> usedSamples; // 邻域样本（输入下标，按邻域序）
  std::vector<double> weights;            // 与 usedSamples 对齐的克里金权重
  int penaltyPasses = 0;                  // 软罚主动集实际迭代轮数
};

class ConstrainedKrigingSolver
{
  public:
    ConstrainedKrigingSolver( const std::vector<Sample> &samples, const VariogramModel &model,
                              const KrigingParams &params,
                              const std::vector<WeightGroupConstraint> &constraints );
    ~ConstrainedKrigingSolver();
    ConstrainedKrigingSolver( ConstrainedKrigingSolver && ) noexcept;
    ConstrainedKrigingSolver &operator=( ConstrainedKrigingSolver && ) noexcept;
    ConstrainedKrigingSolver( const ConstrainedKrigingSolver & ) = delete;
    ConstrainedKrigingSolver &operator=( const ConstrainedKrigingSolver & ) = delete;

    bool valid() const;
    int sampleCount() const;
    int constraintCount() const;

    // 邻域选择与求解口径同 KrigingSolver；约束只在邻域内生效。
    ConstrainedKrigingPointResult solveAt( double x, double y ) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace paleo::geostat
