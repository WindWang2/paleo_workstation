// 层：数据
#pragma once

// inversion/bandlimit — 带限反演（道积分，纯数值）。
//
// 链路：可选子波频域 water-level 反褶积（把带限子波整形回尖峰）→ 递归积分
// Z(i+1) = Z(i)·(1+r)/(1−r) 得相对阻抗 → 趋势项与低频模型合并（标准差
// 对数域乘性合并，见下方 bandlimitedInversion 注释）。无子波时直接积分（记口径，不装
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
  // 道振幅 / 单位反射系数（#141）。<=0 → 用子波 amplitudeScale；仍无 → 1（未标定）。
  double amplitudeScale = 0.0;
  // 反射系数触及 ±kMaxReflectivity 钳位的样点比例上限；超过判失败（振幅未标定）。
  double maxClampedFraction = 0.05;
};

struct BandlimitedResult
{
  bool ok = false;
  std::string reason;
  std::vector<float> reflectivity; // 反褶积后带限反射系数（或原始道，无子波时）
  std::vector<float> impedance;    // 绝对阻抗（低频已合并）；无低频 = 相对阻抗
  bool lowFreqMerged = false;
  double lowFreqVarianceFraction = 0.0; // 低频贡献（方差占比 ∈ [0,1]，绝对阻抗单位）
  double amplitudeScaleUsed = 1.0;      // 实际使用的振幅标定
  bool amplitudeCalibrated = false;     // false = 未标定（地震振幅直接当反射系数）
  double clampedFraction = 0.0;         // 触及 ±kMaxReflectivity 的有限样比例
};

// trace/lowFreq 长度均为 n。wavelet 可空（跳过反褶积）；lowFreq 可空
//（只出相对阻抗，lowFreqMerged=false）。
// 振幅标定（#141）：反褶积输出 / amplitudeScale 才是反射系数；钳位比例超过
// maxClampedFraction 判失败，不再静默发散。
// 合并口径（对数域，量纲一致）：L(i) = Σ ln((1+r)/(1−r))（= ln Z/Z₀），高通
// HP(L) 与低频乘性合并 Z = Z_low · exp(HP(L))。无低频时输出去均值的 L
//（无量纲，≈ ΔZ/Z）。低频贡献 = σ_low²/(σ_low²+σ_band²)，σ_band 为
// Z_low·(exp(HP)−1) 的标准差（同为阻抗单位）。
BandlimitedResult bandlimitedInversion(const float *trace, int n, double sampleIntervalMs,
                                       const Wavelet *wavelet, const float *lowFreq,
                                       const BandlimitedOptions &options = {});

} // namespace paleo::inversion
