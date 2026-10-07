// 预算放宽 helper（方向70）：PALEO_SANITIZER_BUILD（CMake 在
// PALEO_ENABLE_ASAN/UBSAN 任一开启时全局 add_compile_definitions 注入）档
// 下墙钟预算按系数放宽——sanitizer ~2-3x 减速（docs/progress/devex.md
// build-asan 实测）下预算语义仍是回归门：放宽后继续盯相对劣化。非
// sanitizer 构建系数恒为 1，预算零变化。tst_sanitizer_budget(+_on) 钉住
// 两档系数与线性乘法契约。
#pragma once

#include <QtGlobal>

namespace paleo::perfbudget {

inline constexpr int sanitizerRelaxFactor()
{
#ifdef PALEO_SANITIZER_BUILD
  return 3;
#else
  return 1;
#endif
}

inline constexpr qint64 relaxedBudgetMs(qint64 baseMs)
{
  return baseMs * sanitizerRelaxFactor();
}

} // namespace paleo::perfbudget
