// 层：数据
#pragma once

// inversion/lowfreq — 层位约束的井阻抗低频模型（纯数值 + QtCore 无关）。
//
// 口径：模型频段 0–lowCutHz，垂直平滑窗 = 1000/lowCut ms（移动平均首零点
// 恰在 lowCut Hz）。层模式（有层位）：层内井阻抗均值沿层横向 IDW（井距 0
// 精确命中），逐格分段常数剖面再平滑；层位在该格缺失时该边界回退层位平均
// TWT。回退模式（无层位/层位全缺）：井曲线先做同一低通，再横向 IDW——
// 即「全局趋势」。不假装高频：产物元数据必须携带 lowCutHz 与 layersUsed。
//
// 井阻抗曲线是 TWT 域、已重采样到体时间网格（MD→TWT 是调用方的事，
// 可逐样点携带真实轨迹坐标；空坐标数组保留旧固定井位契约）。

#include "domain/inversionmodel.h"
#include <string>
#include <vector>

namespace paleo::inversion
{

// 建低频模型。层模式要求至少 1 个非全缺层位；否则回退全局趋势。
// 井（≥1，含有限样）缺失即失败。
LowFreqModelResult lowFreqImpedance(const LowFreqModelInput &input);

// 逐格合成低频阻抗道（nS 样，NaN = 无法估计）。只读模型，道并行安全。
// 未 ok 的模型输出全 NaN（不崩）。
void lowFreqTraceAt(const LowFreqModelResult &model, int il, int xl, float *out);

// 中心移动平均（奇数窗全宽 2·halfWindow+1，边缘缩窗）：窗内有限值均值，
// 全窗缺失 → NaN。这是 lowCut 频段语义的低通核（首零点 ≈ 1/(窗宽 ms)），
// 低频模型与带限反演的高通/低通共用同一核。
void lowCutMovingAverage(const float *x, int n, int halfWindow, float *out);

} // namespace paleo::inversion
