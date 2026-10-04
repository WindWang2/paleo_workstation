// 层：数据
#pragma once

#include "types.h"

#include <string>
#include <vector>

// 方向41：普通克里金走 singlefactor 本地方向插值面。
// 数值核复用 algorithms/geostat（变差函数拟合 + 加边 LU 克里金求解器，方向18
// 落地），这里只做三件事：把样本解析成变差函数、把 methodActual 切到 kriging
// 后跑同一 evaluateLocalIdw 面（成图域/硬屏障分量/覆盖标记逐条同口径），
// 以及把拟合结论、未消费约束和病态回落如实写进 resolved 与 issues。
//
// 诚实面（不冒充克里金）：
//   - 有效样本 < 8 → 变差函数欠定，整面回落 IDW，methodActual="local_direction_idw"
//     + fallbackReason + issue；
//   - 实验变差/最小二乘失败或零信号（块金+拱高=0）→ 同样整面回落并报因；
//   - 单格方程奇异/病态 → 该格用同参数 IDW 权重，计数进 idwFallbackCells 并出 issue；
//   - 方向线/软边界/井群权重不参与克里金权重（v1 语义）：逐条列进 issues，不静默忽略。
//   - nugget > 0 时普通克里金是平滑器，采样点不再精确通过（数学事实）；要精确
//     通过必须 nugget=0。本头不做「精确」承诺以外的事。
// 层：数据
namespace paleo::singlefactor
{

// 变差函数解析结论（显式参数或自动拟合），供测试与 QC 读取。
struct VariogramResolution
{
  bool ok = false;
  bool fitted = false; // true = 由样本自动拟合
  std::string message;
  std::string modelName = "spherical";
  double nugget = 0;
  double sill = 0;
  double range = 0;
  double azimuthDeg = 0;
  double anisotropyRatio = 1;
  double r2 = 0; // 拟合优度，可为负（如实）
  double rmse = 0;
  int usedLags = 0;
  int sampleCount = 0;
};

// 只做变差函数解析（含自动滞后距），不碰网格。失败时 ok=false 并给出原因。
VariogramResolution resolveVariogram( const std::vector<Sample> &samples, const GridSpec &grid,
                                      const ResolvedParameters &parameters );

// 克里金插值面：SurfaceResult 契约与 evaluateLocalIdw 完全一致（成图域、硬屏障
// 分量、井控/外推标记、无井闭合区、取消与预算）。克里金不成立时整面回落 IDW。
SurfaceResult evaluateLocalKriging( const PreparedInput &input, const GridSpec &grid,
                                    const ResolvedParameters &parameters, const Control &control );

} // namespace paleo::singlefactor
