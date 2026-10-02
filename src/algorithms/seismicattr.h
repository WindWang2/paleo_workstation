// 层：数据
#pragma once

// seismicattr — 地震属性核函数库（纯数值，无 Qt/GIS 依赖）。
// 输入输出均为平凡 float 缓冲；体输入布局与 engine::VoxelWindow 逐位同构：
// values[((il * nXl) + xl) * nS + s]，NaN = 缺失道/缺失采样。
// NaN 语义：计算窗内出现 NaN → 该输出位 NaN（诚实传播，不补值）。
// 服务层（SeismicTaskService 属性任务）负责分块/并行/进度/取消，核函数不掺和。
//
// 公式引用（地质约定，禁臆造）：
//   · 复数道分析（包络/瞬时相位/瞬时频率）：Taner, Koehler & Sheriff, 1979,
//     "Complex seismic trace analysis", Geophysics 44(6)。
//   · 瞬时频率的相位差分实现（免解卷绕）：Barnes, 2007, "A tutorial on
//     complex seismic trace analysis", Geophysics 72(6)。
//   · Semblance 相干（C2）：Marfurt, Kirlin, Farmer & Bahorich, 1998,
//     "3-D seismic attributes using a semblance-based coherency algorithm",
//     Geophysics 63(4)。
//   · 甜点：Radovich & Oliveros, 1998 (SEG Abstracts)——env / sqrt(f_inst)。
//   · 瞬时 Q（原型）：Q = π·f_inst / |d ln(env)/dt|，Barnes 同教材形式；
//     衰减不显著（包络斜率≈0）处数值不稳定 → NaN（原型语义，解释时慎用）。
//   · 时窗振幅族（RMS/最大绝对振幅/平均能量）：Chopra & Marfurt, 2005,
//     "Seismic attributes—A historical perspective", Geophysics 70(5)。

#include <vector>

namespace paleo::seisattr
{

// ---- 复数道（瞬时）属性族 ----------------------------------------------------
// 解析信号 a(t) = x(t) + i·H{x}(t)（Hilbert 频域法：道镜像填充 reflect 到
// ≥2n 的 2 的幂，正频 ×2、负频清零，DC/Nyquist 保留——scipy.signal.hilbert
// 同约定；逆变换取原 n 样）。镜像填充抑制周期延拓回绕的边界假象，正弦类
// 信号内部样点可达浮点精度。局部 NaN 以 0 入谱、输出位保持 NaN；全 NaN 道
// 输出全 NaN。outReal/outImag 长度 n。
void analyticSignal(const float *trace, int n, float *outReal, float *outImag);

// 一道实信号 → 解析信号，派生四个属性（长度均为 n）：
//   envelope  反射强度 |a|（数据单位）；
//   phaseDeg  瞬时相位 arg(a)，角度制 (-180, 180]；
//   freqHz    瞬时频率（Hz，相位差分法；可为负——真实数据现象，如实保留）；
//   quality   瞬时 Q 原型（无量纲；包络无衰减/缺失 → NaN）。
// sampleIntervalMs：采样间隔（毫秒），>0。
struct ComplexTraceResult
{
  std::vector<float> envelope;
  std::vector<float> phaseDeg;
  std::vector<float> freqHz;
  std::vector<float> quality;
};

ComplexTraceResult complexTraceAnalysis(const float *trace, int n,
                                        double sampleIntervalMs);

// ---- 时窗振幅族 ---------------------------------------------------------------
// 滑动窗 [i-half, i+half]（闭区间，边缘缩窗至序列边界，最少 1 样）。窗内含
// NaN → 输出 NaN。half=0 时 RMS/MaxAbs=|x|、能量=x²。
//   RMS         sqrt(mean(x²))          —— 数据单位
//   MaxAbs      max(|x|)                —— 数据单位
//   MeanEnergy  mean(x²)                —— 数据单位的平方
void windowedRms(const float *x, int n, int halfWindow, float *out);
void windowedMaxAbs(const float *x, int n, int halfWindow, float *out);
void windowedMeanEnergy(const float *x, int n, int halfWindow, float *out);

// ---- 相干体（semblance C2）----------------------------------------------------
// 对每个 (il, xl, s)：取空间 (2*ilHalf+1)×(2*xlHalf+1) 道窗（IL/XL 向半窗
// 分开设——两向道距不等时各取各的）× 垂直 [s-timeHalf, s+timeHalf] 时窗
// （边缘缩窗），J = 道数：
//   S = Σ_t ( Σ_j u_j(t) )²  /  ( J · Σ_t Σ_j u_j(t)² )   ∈ [0, 1]
// 完全同相（J 道波形一致，任意波形）→ S=1；道间能量全无序 → 趋近 0。
// 体内任一道窗样点 NaN → 输出 NaN；空间窗越界（体边界的道）→ NaN（相干体
// 标准边界带）；全零窗（数学上 0/0 未定义，精确零判）→ NaN。
// out 尺寸 nIl*nXl*nS，布局同输入。
void semblanceCoherence(const float *volume, int nIl, int nXl, int nS,
                        int ilHalf, int xlHalf, int timeHalf, float *out);

// ---- 甜点 ---------------------------------------------------------------------
// sweetness = envelope / sqrt(max(freqHz, fMin))，fMin=1e-3 Hz（负瞬时频率
// 抬到 fMin——甜点语义只关心频率量级，负值是相位差分噪声）。NaN 传播。
void sweetness(const float *envelope, const float *freqHz, int n, float *out);

} // namespace paleo::seisattr
