// 层：数据
#pragma once

#include "types.h"

#include <cstdint>
#include <string>
#include <vector>

// geostat/linsolve — 自研小线性求解器（矩阵 ≤ 几十阶，不引外部库）。
// 求解口径：普通克里金的加边矩阵 [[Γ, 1], [1ᵀ, 0]] 因 γ(0)=0 对角与
// Lagrange 边界行而不定（conditionally PD，非 PD），Cholesky 不适用；
// 一律走 LU 列主元消元。主元 < eps·scale 判奇异。
// 层：数据
namespace paleo::geostat
{

// LU 工作区（#478）：lu 分解副本与置换向量跨调用复用——SGS 热路径每格
// 一解，重分配 churn（n≤64 时每格 ~130KB × 格数 × 实现数）全部省掉。
// 缓冲仅复用容量、内容逐次重写；非线程安全（每线程各持一份）。
struct LuWorkspace
{
  std::vector<double> lu;
  std::vector<int> permutation;
};

// 解 A·x = b。A 为 n×n 行主序，b 长 n，x 长 n 由调用方预分配。
// 成功返回 true；奇异（主元过小）返回 false，不写 x。
bool solveDenseLu( const std::vector<double> &a, int n,
                   const std::vector<double> &b, std::vector<double> &x );

// 同上，跨调用复用工作区（算术逐位同上，仅省分配；x 是调用方缓冲，
// 与 workspace 无别名约束外无额外要求）。
bool solveDenseLu( const std::vector<double> &a, int n, const std::vector<double> &b,
                   std::vector<double> &x, LuWorkspace &workspace );

} // namespace paleo::geostat
