// 层：视图（纯算法，无 Qt 依赖）
// ui/common/curvescale — 曲线值 → 道内归一横坐标的唯一实现（PW-020/021/103）。
// wellcomposite / wellsection / correlation 共用，统一以下语义：
//  - 反向刻度（min > max，NPHI 0.45→-0.15、DT 140→40 的标准惯例）：负跨度
//    自然翻转，不塌缩、不丢曲线；
//  - 对数刻度：min/max 与样点值都须 > 0，否则该样点不可映射（NaN = 断笔）；
//  - min == max（退化刻度）：不可映射。
#pragma once

#include <cmath>
#include <limits>

namespace paleo::curvescale
{

// 返回未截断的归一值（0 = 左缘 min，1 = 右缘 max；超出刻度时 <0 或 >1）。
// 不可映射时返回 NaN。
inline double normalise(double v, double lo, double hi, bool logScale)
{
  constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
  if (!std::isfinite(v) || !std::isfinite(lo) || !std::isfinite(hi))
    return kNaN;
  if (logScale)
  {
    if (v <= 0.0 || lo <= 0.0 || hi <= 0.0)
      return kNaN;
    lo = std::log10(lo);
    hi = std::log10(hi);
    v = std::log10(v);
  }
  const double span = hi - lo;
  if (span == 0.0)
    return kNaN;
  return (v - lo) / span;
}

// 截断到 [0,1] 的版本（道内绘制用）；不可映射仍为 NaN。
inline double normaliseClamped(double v, double lo, double hi, bool logScale)
{
  const double t = normalise(v, lo, hi, logScale);
  if (std::isnan(t))
    return t;
  return t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
}

} // namespace paleo::curvescale
