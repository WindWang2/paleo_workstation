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

// 解 A·x = b。A 为 n×n 行主序，b 长 n，x 长 n 由调用方预分配。
// 成功返回 true；奇异（主元过小）返回 false，不写 x。
bool solveDenseLu( const std::vector<double> &a, int n,
                   const std::vector<double> &b, std::vector<double> &x );

} // namespace paleo::geostat
