// 层：数据
#pragma once

#include "types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// geostat/linsolve — 自研小线性求解器（矩阵 ≤ 几十阶，不引外部库）。
// 求解口径：普通克里金的加边矩阵 [[Γ, 1], [1ᵀ, 0]] 因 γ(0)=0 对角与
// Lagrange 边界行而不定（conditionally PD，非 PD），Cholesky 不适用；
// 一律走 LU 列主元消元。主元 ≤ 1e-12·该列原始最大幅值判奇异（按列尺度，
// 使 Γ 块与 Lagrange 边量纲不同的加边矩阵在大基台值下不被误判）。
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
  std::vector<double> columnFloor; // 各列主元下限（H1：按列尺度判奇异）
};

// 解 A·x = b。A 为 n×n 行主序，b 长 n，x 长 n 由调用方预分配。
// 成功返回 true；奇异（主元过小）返回 false，不写 x。
bool solveDenseLu( const std::vector<double> &a, int n,
                   const std::vector<double> &b, std::vector<double> &x );

// 同上，跨调用复用工作区（算术逐位同上，仅省分配；x 是调用方缓冲，
// 与 workspace 无别名约束外无额外要求）。
bool solveDenseLu( const std::vector<double> &a, int n, const std::vector<double> &b,
                   std::vector<double> &x, LuWorkspace &workspace );

// PW-079：分解/回代拆分——同一矩阵多右端项（全局 OK：矩阵与待估点无关）
// 只分解一次。luFactor 失败（奇异）返回 false；luSolve 要求先成功分解。
// solveDenseLu 即 luFactor + luSolve，算术逐位不变。
bool luFactor( const std::vector<double> &a, int n, LuWorkspace &workspace );
void luSolve( const LuWorkspace &workspace, int n, const std::vector<double> &b, std::vector<double> &x );

// 克里金方程组行归一化（issue #720 尺度悬崖）。OK 加边矩阵 [[Γ,1],[1ᵀ,0]] 的
// Γ 块随基台值缩放、Lagrange 边固定为 1：LU 消元后末主元 ~ n/sill，而列主元
// 下限按该列最大幅值（Lagrange 列 = 1）取 1e-12——sill≳1e12 时末主元掉到
// 下限被误判奇异，整幅 NumericalFailure（微基台同理由量级悬殊触发）。把前
// physicsRows 行（Γ/协克里金物理块 + 其 Lagrange 列项）与右端同除 scale 后，
// 主元/下限之比与 sill 无关（尺度不变），病态判据不再随基台值漂移。
// 数学后果：行缩放（矩阵与右端同步）不改变解——λ 与 μ 都与未缩放时等价
// （数值上更稳：消元不再跨量级相消）。注意方差组装：缩放后的 b 算出的
// λᵀb 项小一个 scale 因子，须按 scale·(λᵀb) + μ 拼回（μ 本身不缩放）。
// b 非空时其前 physicsRows 项同步缩放（分解/求解同一次完成时用；只分解不
// 求解的全局路径传空，右端由调用方在回代时按返回的 scale 缩放）。
// scale = 物理块（前 physicsRows 行 × 前 physicsRows 列）最大幅值；块全零或
// 非有限时返回 0（调用方按原矩阵解，语义不变）。
inline double scaleKrigingSystemRows( std::vector<double> &a, int n, int physicsRows,
                                      std::vector<double> *b = nullptr )
{
  double scale = 0.0;
  for ( int i = 0; i < physicsRows; ++i )
    for ( int j = 0; j < physicsRows; ++j )
      scale = std::max( scale, std::fabs( a[static_cast<std::size_t>( i ) * n + j] ) );
  if ( !( scale > 0.0 ) || !std::isfinite( scale ) )
    return 0.0;
  const double inverse = 1.0 / scale;
  for ( int i = 0; i < physicsRows; ++i )
  {
    for ( int j = 0; j < n; ++j )
      a[static_cast<std::size_t>( i ) * n + j] *= inverse;
    if ( b && static_cast<int>( b->size() ) > i )
      ( *b )[static_cast<std::size_t>( i )] *= inverse;
  }
  return scale;
}

} // namespace paleo::geostat
