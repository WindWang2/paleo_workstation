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
// 垂直井语义——方向 19 井轨迹未落地，如实递延）。

#include <string>
#include <vector>

namespace paleo::inversion
{

// 井阻抗曲线（TWT 域，nS 样与体网格一致，NaN = 缺失段）。
struct LowFreqWellTrace
{
  double x = 0.0;
  double y = 0.0;
  std::vector<float> impedance;
};

// 层位 TWT 网格：nIl*nXl 行主序（行 = inline），NaN = 缺失格。
struct HorizonTwtGrid
{
  std::string name;
  std::vector<float> twtMs;
};

struct LowFreqModelInput
{
  // 体时间网格
  double t0Ms = 0.0;
  double dtMs = 2.0;
  int nIl = 0;
  int nXl = 0;
  int nS = 0;

  // 测网仿射几何：格 (i,j) 中心 = origin + i·ilStep + j·xlStep（XY 增量向量）。
  double originX = 0.0;
  double originY = 0.0;
  double ilStepX = 0.0;
  double ilStepY = 0.0;
  double xlStepX = 0.0;
  double xlStepY = 0.0;

  std::vector<LowFreqWellTrace> wells;
  std::vector<HorizonTwtGrid> horizons; // 任意序，内部按平均 TWT 升序

  double lowCutHz = 8.0; // 模型频段上界；平滑窗 = 1000/lowCut ms
  double idwPower = 2.0; // IDW 权重 1/d^power
};

struct LowFreqModelResult
{
  bool ok = false;
  std::string reason;

  double t0Ms = 0.0;
  double dtMs = 2.0;
  int nIl = 0;
  int nXl = 0;
  int nS = 0;
  double originX = 0.0;
  double originY = 0.0;
  double ilStepX = 0.0;
  double ilStepY = 0.0;
  double xlStepX = 0.0;
  double xlStepY = 0.0;
  double idwPower = 2.0;

  double lowCutHz = 0.0;        // 频段上界（产物 extra 必带）
  double smoothingWindowMs = 0.0; // 实际平滑窗 = nWin·dt
  int layersUsed = 0;           // 有效层数；0 = 全局趋势回退

  // 层模式：每格每层阻抗，((il·nXl)+xl)·nL + l；NaN = 该格该层无井控。
  std::vector<float> layerImpedance;
  // 层模式：升序层位边界（拷贝自输入，去掉全缺层位）。
  std::vector<HorizonTwtGrid> boundaries;
  // 层模式：各边界平均 TWT（与 boundaries 同序，预计算免逐道重扫）。
  std::vector<double> boundaryMeanTwt;
  // 回退模式：低通后的井曲线（结构与输入 wells 一致）。
  std::vector<LowFreqWellTrace> smoothedWells;

  int layerCount() const
  {
    return layersUsed > 0 ? layersUsed : 0;
  }
};

// 建低频模型。层模式要求至少 1 个非全缺层位；否则回退全局趋势。
// 井（≥1，含有限样）缺失即失败。
LowFreqModelResult lowFreqImpedance(const LowFreqModelInput &input);

// 逐格合成低频阻抗道（nS 样，NaN = 无法估计）。只读模型，道并行安全。
// 未 ok 的模型输出全 NaN（不崩）。
void lowFreqTraceAt(const LowFreqModelResult &model, int il, int xl, float *out);

} // namespace paleo::inversion
