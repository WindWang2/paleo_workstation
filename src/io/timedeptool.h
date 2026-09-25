#pragma once
#include "wellfileparsers.h"

// io/ — 时深工具（plan §10）：TVD 对 TD 表 TVD 列线性插值得 TIME(ms)；
// 井 TVD 空则用 MD 对 MD 列；-99999 的行不进插值。
// 区间外外推最近端点值（剖面上标层需要闭合解），无可用行则失败。
namespace TimeDepthTool
{
  // depth→time(ms)。useMd=false 走 TVD 列（井 TVD 有效时的首选）；
  // useMd=true 走 MD 列（井 TVD 空的兜底）。
  double interpolateTimeMs(const TimeDepthTable &td, double depth, bool useMd, bool *ok = nullptr);
} // namespace TimeDepthTool
