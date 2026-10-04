// 层：数据
#pragma once

// inversion/wavelet — 子波值对象、解析子波生成、井旁道-反射系数对提取与
// JSON 序列化（纯数值 + QtCore 序列化；无 QtWidgets/GIS）。
//
// 提取法：Wiener 最小二乘（确定性叠后常规做法，自研不引库）。模型
//   s(t) = Σ_k r_k · w(t − t_k)
// 对 w 的最小二乘正则方程是反射系数尖峰 train 的自相关 Toeplitz 阵 × w =
// 道与尖峰 train 的互相关；高斯消元求解 + 对角加载防病态（反射稀疏/带限
// 时阵病态是常态）。输出归一化子波（max|x| = 1）与拟合相关 QC。
//
// 频谱估计用共享自研 radix-2 FFT（dsp/fft.h）。时间域语义：所有时间均为
// TWT 毫秒。

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <string>
#include <vector>

namespace paleo::inversion
{

// 归一化子波（max|x| = 1）。零相位子波峰对齐 t=0 时 t0Ms = -lengthMs/2。
struct Wavelet
{
  double sampleIntervalMs = 2.0;
  double t0Ms = 0.0;
  std::vector<float> samples;
  // 振幅标定（#141）：地震道振幅 ≈ 反射系数 ⊛ (amplitudeScale · samples)。
  // 井旁道提取时由最小二乘解的峰值给出（归一化前的尺度）；0 = 未标定
  //（解析子波 / 旧资产），反演按 1 处理并在结果中标 amplitudeCalibrated=false。
  double amplitudeScale = 0.0;

  int sampleCount() const { return int(samples.size()); }
  bool isEmpty() const { return samples.empty(); }
  double lengthMs() const;       // (n-1)·dt；空 → 0
  double peakAmplitude() const;  // max|x|；空 → 0
  double dominantFreqHz() const; // 振幅谱峰值频率（FFT 零填充；空 → 0）
};

// Ricker（墨西哥帽）零相位子波：w(t) = (1−2π²f²t²)e^{−π²f²t²}，峰对齐
// t=0，采样数为奇数使 t=0 恰有采样。lengthMs 会吸附到奇数采样步。
Wavelet makeRicker(double f0Hz, double sampleIntervalMs, double lengthMs);

// 相位旋转（Hilbert 解析信号单边谱相位 e^{iθ}）：w_rot = w·cosθ − H{w}·sinθ。
// 振幅谱不变，用于混合相位正演对拍与相位扫描。θ=90° 时为零相位→奇对称化。
Wavelet rotateWaveletPhase(const Wavelet &wavelet, double phaseDeg);

// 单个反射系数尖峰（时间域 TWT ms）。
struct ReflSpike
{
  double twtMs = 0.0;
  float amplitude = 0.0f;
};

// 反演递推护栏：|r| 超过此值视为非物理（未标定振幅的典型症状）。
inline constexpr double kMaxReflectivity = 0.45;

// 振幅标定解析（#141）：显式 explicitScale>0 优先，其次子波自带 amplitudeScale，
// 都没有 → 1.0 并置 *calibrated=false（地震振幅被直接当作反射系数，口径如实上报）。
inline double resolveAmplitudeScale(double explicitScale, const Wavelet *wavelet,
                                    bool *calibrated)
{
  double k = 0.0;
  if (explicitScale > 0.0)
    k = explicitScale;
  else if (wavelet && wavelet->amplitudeScale > 0.0)
    k = wavelet->amplitudeScale;
  if (calibrated)
    *calibrated = k > 0.0;
  return k > 0.0 ? k : 1.0;
}

struct WaveletExtractOptions
{
  // 对角加载 = ridgeFactor · 自相关[0]（>0 防病态；0 亦允许——奇异时失败）。
  double ridgeFactor = 1e-6;
};

struct WaveletExtractResult
{
  bool ok = false;
  std::string reason;
  Wavelet wavelet;
  double fitCorrelation = 0.0; // conv(spikes, w_est) 与输入道的归一化相关 QC
};

// 井旁道-反射系数对提取子波。trace[i] 采样于 traceT0Ms + i·sampleIntervalMs；
// 提取窗 [waveletT0Ms, waveletT0Ms + (waveletSamples-1)·dt] 是**相对反射尖峰的
// 时滞**（与 Wavelet::t0Ms 同义，零相位子波为 -L/2），与道起始时间 traceT0Ms
// 无关；traceT0Ms 只用于把尖峰绝对 TWT 换算到道样号（#140）。尖峰时间吸附到
// 最近采样；落在道时窗外的尖峰跳过。尖峰（有效）不足 2 个 → 失败。
WaveletExtractResult extractWavelet(const float *trace, int nTrace, double traceT0Ms,
                                    double sampleIntervalMs, const ReflSpike *spikes,
                                    int nSpikes, double waveletT0Ms, int waveletSamples,
                                    const WaveletExtractOptions &options = {});

// JSON 序列化（DERIVED 资产 wavelet.json 的载荷）。保留键 sampleIntervalMs/
// t0Ms/samples；其余键经 extra 原样往返（井号/拟合相关等元数据）。
QByteArray waveletToJson(const Wavelet &wavelet, const QJsonObject &extra = {});
bool waveletFromJson(const QByteArray &bytes, Wavelet *wavelet, QJsonObject *extra = {},
                     QString *error = nullptr);

} // namespace paleo::inversion
