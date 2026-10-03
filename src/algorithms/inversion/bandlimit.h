// 层：数据
#pragma once

// inversion/bandlimit — 带限反演（道积分，纯数值）。
//
// 链路：可选子波频域 water-level 反褶积（把带限子波整形回尖峰）→ 递归积分
// Z(i+1) = Z(i)·(1+r)/(1−r) 得相对阻抗 → 趋势项与低频模型合并（标准差
// 匹配：绝对 = 低频 + 相对·σ_low/σ_rel）。无子波时直接积分（记口径，不装
// 作白化过）。不假装高频：结果是带限阻抗，频段口径由调用方入产物 extra。
//
// NaN 语义：输入道缺失样以 0 参与谱计算、输出位保持 NaN；缺失 >30% 整道
// 失败。低频道 NaN 样 → 输出该样 NaN（诚实传播）。

#include "wavelet.h"

#include <string>
#include <vector>

namespace paleo::inversion
{

struct BandlimitedOptions
{
  double lowCutHz = 8.0; // 相对阻抗高通截止（与低频模型频段互补；>0）
};

struct BandlimitedResult
{
  bool ok = false;
  std::string reason;
  std::vector<float> reflectivity; // 反褶积后带限反射系数（或原始道，无子波时）
  std::vector<float> impedance;    // 绝对阻抗（低频已合并）；无低频 = 相对阻抗
  bool lowFreqMerged = false;
  double lowFreqVarianceFraction = 0.0; // 低频贡献（方差占比 ∈ [0,1]）
};

// trace/lowFreq 长度均为 n。wavelet 可空（跳过反褶积）；lowFreq 可空
//（只出相对阻抗，lowFreqMerged=false）。
// 合并口径：相对阻抗先做同 lowCut 高通（减 lowCutMovingAverage），再原幅度
// 叠加到低频上（反褶积输出即为物理反射系数量纲；幅度标定由子波提取对反射
// 系数拟合承担）。低频贡献 = σ_low²/(σ_low²+σ_relHP²)。
BandlimitedResult bandlimitedInversion(const float *trace, int n, double sampleIntervalMs,
                                       const Wavelet *wavelet, const float *lowFreq,
                                       const BandlimitedOptions &options = {});

} // namespace paleo::inversion
