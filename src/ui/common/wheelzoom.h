#pragma once
// #852：Ctrl+滚轮缩放倍率按 angleDelta 归一（120 = 一个标准滚轮刻度）。
// 原实现只取符号，高精度滚轮/触控板（每事件 ~10–15）一次手势缩放几十次；
// 与同文件平移的 delta/120 归一口径一致。
#include <cmath>

namespace paleo::ui {
inline double wheelZoomFactor(int angleDeltaY, double perNotch)
{
  return std::pow(perNotch, double(angleDeltaY) / 120.0);
}
} // namespace paleo::ui
