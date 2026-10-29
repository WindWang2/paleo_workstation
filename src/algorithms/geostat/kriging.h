// 层：数据
#pragma once

#include "types.h"
#include "variogram.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

// geostat/kriging — 普通克里金（OK）。局部邻域（最近 K 点，可选半径过滤）
// 解克氏方程组，输出估值 + 估计方差两张场。
// 方差口径：σ² = Σλᵢγ(xᵢ,x₀) + μ（Lagrange 口径，方程组
// ΣⱼλⱼΓᵢⱼ + μ = γᵢ₀、Σλ = 1）。单样本远点渐近 2×(块金+拱高)
// ——均值未知的代价，如实报告（见 docs/progress/geostat-methods.md）。
// 层：数据
namespace paleo::geostat
{

// PW-018 / #802：OK 权重病态闸——Σλ=1 下 Σ|λ| 远大于 1 意味着大幅正负抵消的
// 权重（近奇异方程组），估值可越出数据范围数个量级。geostat 与 singlefactor
// （localidw 的克里金分支）共用同一口径：旧式只在 geostat 侧设闸，另一处
// 静默放行，同族实现两道闸分叉。
constexpr double kMaxKrigingAbsWeightSum = 10.0;

inline bool krigingWeightsAdmissible( double absWeightSum )
{
  return std::isfinite( absWeightSum ) && absWeightSum <= kMaxKrigingAbsWeightSum;
}

// #802：全局一次分解（PW-079）的样本数上限——按矩阵内存预算推导而非魔数。
// (n+1)² double 的加边矩阵与 luFactor 的工作区副本双份驻留：64 MiB 矩阵预算
// → n+1 ≤ sqrt(64MiB/8) = 2896 → n ≤ 2895（峰值 ≈ 128 MiB；旧魔数 4000 的
// 峰值 ≈ 256 MiB）。超限退逐格求解（该路径对 n>上限本就存在；maxPoints<=0 时
// 逐格用全部样本，井数大时应配 krigingMaxPoints>0 而非追加上限）。
inline constexpr int kMaxGlobalFactorSamples = 2895;

struct KrigingParams
{
  int maxPoints = 16;      // 最近 K 点邻域上限，[1, 64]；<= 0 = 全部样本
  int minPoints = 4;       // 半径模式下少于则该格 nodata（最近 K 模式不设下限）
  double searchRadius = 0; // <= 0 → 纯最近 K 模式
};

struct KrigingPointResult
{
  bool ok = false;
  double estimate = 0;
  double variance = 0;
};

struct KrigingResult
{
  Status status = Status::InvalidInput;
  std::string message;
  std::vector<double> estimate;  // row-major，NaN = nodata
  std::vector<double> variance;  // 同上
  int finiteCells = 0;
  int nodataCells = 0;
  int solverFailures = 0;
  int mergedDuplicates = 0;
};

// 全场估值。重合样本（坐标 ε 内）合并取均值（preserve 均值口径）。
// progress 覆盖 [0,1]；cancelled 置位时返回 Status::Cancelled。
KrigingResult ordinaryKriging( const std::vector<Sample> &samples, const GridSpec &grid,
                               const VariogramModel &model, const KrigingParams &params,
                               const Control &control = {} );

// 单点估值（测试与编排复用），内部走同一邻域选择与求解器。
KrigingPointResult ordinaryKrigingAt( double x, double y, const std::vector<Sample> &samples,
                                      const VariogramModel &model, const KrigingParams &params );

// 预建邻域索引的 OK 求解器。供 singlefactor 插值面等按分量重复查询的调用方
// 复用同一数值核（不复制权重公式、不重复建索引）：重合样本合并、最近 K 邻域 +
// 可选半径覆盖闸、n+1 加边 LU、显著负方差判失败——口径与 ordinaryKriging 逐条一致。
// solveAt().ok=false 表示该查询点无值（覆盖不足、邻域为空或方程奇异/病态）。
// 半径模式下 neighborhood < minPoints 即 nodata；最近 K 模式不设下限。
class KrigingSolver
{
  public:
    KrigingSolver( const std::vector<Sample> &samples, const VariogramModel &model,
                   const KrigingParams &params );
    ~KrigingSolver();
    KrigingSolver( KrigingSolver && ) noexcept;
    KrigingSolver &operator=( KrigingSolver && ) noexcept;
    KrigingSolver( const KrigingSolver & ) = delete;
    KrigingSolver &operator=( const KrigingSolver & ) = delete;

    // 有效样本为空或模型参数非法时为假；此时 solveAt 一律返回 ok=false。
    bool valid() const;
    int sampleCount() const;
    int mergedDuplicates() const;

    KrigingPointResult solveAt( double x, double y ) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace paleo::geostat
