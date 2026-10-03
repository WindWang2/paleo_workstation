// 层：数据
#pragma once

#include "types.h"
#include "variogram.h"

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
  int maxPoints = 16;      // 最近 K 点邻域上限，[1, 64]
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

} // namespace paleo::geostat
