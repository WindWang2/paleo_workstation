// 层：数据
#pragma once

#include "types.h"
#include "variogram.h"

#include <functional>
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

// 点对度量变换（方向84）：输入两点坐标，把名义位移 (dx,dy) 改写为「有效位移」，
// 克里金方程组的半方差按有效位移计算（井-井与井-查询点对都过同一变换）。
// 约束：变换需对称——(a,b) 与 (b,a) 必须给出同一有效距离模长，否则方程组矩阵
// 不对称；warp 后矩阵病态由既有 LU 失败口径回落（solveAt().ok=false），不静默输出。
// 空 warp = 恒等，数值逐位不变。邻域选择（最近 K + 半径闸）仍按欧氏距离。
using PairMetricWarp = std::function<void( double ax, double ay, double bx, double by, double *dx, double *dy )>;

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
    // 方向84：带点对度量变换的单点求解（约束线消费入口）。metric 为空时与
    // 上面三参重载逐位一致。
    KrigingPointResult solveAt( double x, double y, const PairMetricWarp &metric ) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace paleo::geostat
