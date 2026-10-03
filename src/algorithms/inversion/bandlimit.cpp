// 层：数据
#include "bandlimit.h"

// 带限反演实现。FFT 用 dsp/fft.h；反褶积 water-level = 1%（|W|² 分母下限），
// 子波带外噪声不被放大（分母被抬到 1% 峰值能量，带外输出自然衰减）。

#include "algorithms/dsp/fft.h"
#include "lowfreq.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace paleo::inversion
{
namespace
{

constexpr float kNan = std::numeric_limits<float>::quiet_NaN();

// reflect 镜像索引（与 seismicattr 同式：周期 2(n-1) 三角波）。
int reflectIndex(int q, int n)
{
  const int period = 2 * (n - 1);
  int r = q % period;
  if (r < 0)
    r += period;
  return r >= n ? period - r : r;
}

double stdevFinite(const float *x, int n, double *meanOut = nullptr)
{
  double sum = 0.0;
  int cnt = 0;
  for (int i = 0; i < n; ++i)
  {
    if (std::isfinite(x[i]))
    {
      sum += double(x[i]);
      ++cnt;
    }
  }
  if (cnt == 0)
  {
    if (meanOut)
      *meanOut = std::numeric_limits<double>::quiet_NaN();
    return 0.0;
  }
  const double mean = sum / double(cnt);
  double acc = 0.0;
  for (int i = 0; i < n; ++i)
  {
    if (std::isfinite(x[i]))
      acc += (double(x[i]) - mean) * (double(x[i]) - mean);
  }
  if (meanOut)
    *meanOut = mean;
  return cnt > 1 ? std::sqrt(acc / double(cnt - 1)) : 0.0;
}

} // namespace

BandlimitedResult bandlimitedInversion(const float *trace, int n, double sampleIntervalMs,
                                       const Wavelet *wavelet, const float *lowFreq,
                                       const BandlimitedOptions &options)
{
  BandlimitedResult result;
  if (!trace || n < 16 || !(sampleIntervalMs > 0.0))
  {
    result.reason = "输入道无效（长度<16 或采样间隔<=0）";
    return result;
  }
  if (!(options.lowCutHz > 0.0))
  {
    result.reason = "lowCutHz 必须 > 0";
    return result;
  }
  int missing = 0;
  for (int i = 0; i < n; ++i)
    missing += std::isnan(trace[i]) ? 1 : 0;
  if (missing * 10 > n * 3)
  {
    result.reason = "道缺失样 >30%，拒绝反演";
    return result;
  }

  // ---- 反褶积（可选）------------------------------------------------------
  std::vector<double> refl(std::size_t(n), 0.0);
  bool haveWavelet = wavelet && !wavelet->isEmpty() &&
                     std::fabs(wavelet->sampleIntervalMs - sampleIntervalMs) < 1e-9;
  if (wavelet && !wavelet->isEmpty() && !haveWavelet)
  {
    result.reason = "子波采样间隔与道不一致（先重采样子波）";
    return result;
  }

  if (haveWavelet)
  {
    const int wl = wavelet->sampleCount();
    const int nFft = paleo::dsp::nextPowerOfTwoAtLeast(n + 2 * wl + 16);
    const int leftPad = wl + 8;
    std::vector<double> sRe(std::size_t(nFft), 0.0), sIm(std::size_t(nFft), 0.0);
    for (int p = 0; p < nFft; ++p)
    {
      const double v = double(trace[reflectIndex(p - leftPad, n)]);
      sRe[std::size_t(p)] = std::isnan(v) ? 0.0 : v;
    }
    std::vector<double> wRe(std::size_t(nFft), 0.0), wIm(std::size_t(nFft), 0.0);
    for (int j = 0; j < wl; ++j)
    {
      // 子波零时刻对齐数组原点（t=0 样移到下标 0，负时间圆卷到尾部）。
      const int rel = int(std::lround(wavelet->t0Ms / sampleIntervalMs)) + j;
      const int p = (rel % nFft + nFft) % nFft;
      wRe[std::size_t(p)] = double(wavelet->samples[std::size_t(j)]);
    }
    paleo::dsp::fftRadix2(sRe.data(), sIm.data(), nFft, /*inverse=*/false);
    paleo::dsp::fftRadix2(wRe.data(), wIm.data(), nFft, /*inverse=*/false);

    double wPeak = 0.0;
    for (int k = 0; k < nFft; ++k)
    {
      const double m = wRe[std::size_t(k)] * wRe[std::size_t(k)] +
                       wIm[std::size_t(k)] * wIm[std::size_t(k)];
      wPeak = std::max(wPeak, m);
    }
    const double water = 0.01 * wPeak; // |W|² water level（1% 峰值能量）
    for (int k = 0; k < nFft; ++k)
    {
      const double wr = wRe[std::size_t(k)];
      const double wi = wIm[std::size_t(k)];
      const double denom = std::max(wr * wr + wi * wi, water);
      // R = S·conj(W)/denom
      const double nr = sRe[std::size_t(k)] * wr + sIm[std::size_t(k)] * wi;
      const double ni = sIm[std::size_t(k)] * wr - sRe[std::size_t(k)] * wi;
      sRe[std::size_t(k)] = nr / denom;
      sIm[std::size_t(k)] = ni / denom;
    }
    paleo::dsp::fftRadix2(sRe.data(), sIm.data(), nFft, /*inverse=*/true);
    for (int i = 0; i < n; ++i)
      refl[std::size_t(i)] = sRe[std::size_t(leftPad + i)];
  }
  else
  {
    for (int i = 0; i < n; ++i)
      refl[std::size_t(i)] = std::isnan(trace[i]) ? 0.0 : double(trace[i]);
  }

  // ---- 递归积分 → 相对阻抗 -------------------------------------------------
  std::vector<double> relZ(std::size_t(n), 1.0);
  for (int i = 0; i + 1 < n; ++i)
  {
    double r = refl[std::size_t(i)];
    if (!std::isfinite(r))
      r = 0.0;
    r = std::clamp(r, -0.45, 0.45); // 递归稳定护栏（真反射系数 |r|≪1）
    relZ[std::size_t(i + 1)] = relZ[std::size_t(i)] * (1.0 + r) / (1.0 - r);
  }
  double meanRel = 0.0;
  {
    double sum = 0.0;
    for (int i = 0; i < n; ++i)
      sum += relZ[std::size_t(i)];
    meanRel = sum / double(n);
    for (int i = 0; i < n; ++i)
      relZ[std::size_t(i)] -= meanRel; // 去均值（漂移趋势去除）
  }

  result.reflectivity.resize(std::size_t(n));
  for (int i = 0; i < n; ++i)
    result.reflectivity[std::size_t(i)] =
        std::isnan(trace[i]) ? kNan : float(refl[std::size_t(i)]);

  // ---- 低频合并 ------------------------------------------------------------
  // 相对阻抗先做同 lowCut 高通（减同核移动平均），再原幅度叠加——否则相对项
  // 自带的低频漂移会与低频模型双计。无低频模型时输出全带相对阻抗。
  std::vector<float> relOnly(std::size_t(n), kNan);
  for (int i = 0; i < n; ++i)
    relOnly[std::size_t(i)] = std::isnan(trace[i]) ? kNan : float(relZ[std::size_t(i)]);

  if (!lowFreq)
  {
    result.impedance = relOnly;
    result.ok = true;
    result.lowFreqMerged = false;
    return result;
  }

  const int half = std::max(1, int(std::lround(
                        1000.0 / options.lowCutHz / sampleIntervalMs * 0.5)));
  std::vector<float> relTrend(std::size_t(n), 0.0f);
  lowCutMovingAverage(relOnly.data(), n, half, relTrend.data());
  result.impedance.assign(std::size_t(n), kNan);
  for (int i = 0; i < n; ++i)
  {
    if (std::isfinite(relOnly[std::size_t(i)]) && std::isfinite(relTrend[std::size_t(i)]) &&
        std::isfinite(lowFreq[i]))
    {
      result.impedance[std::size_t(i)] =
          float(double(lowFreq[i]) + double(relOnly[std::size_t(i)]) -
                double(relTrend[std::size_t(i)]));
    }
  }

  // 低频贡献 = σ_low² / (σ_low² + σ_relHP²)（合成道的两分量方差占比）。
  {
    std::vector<float> relHp(std::size_t(n), kNan);
    for (int i = 0; i < n; ++i)
      if (std::isfinite(relOnly[std::size_t(i)]) && std::isfinite(relTrend[std::size_t(i)]))
        relHp[std::size_t(i)] = float(double(relOnly[std::size_t(i)]) -
                                      double(relTrend[std::size_t(i)]));
    const double sdLow = stdevFinite(lowFreq, n);
    const double sdHp = stdevFinite(relHp.data(), n);
    const double vLow = sdLow * sdLow;
    const double vHp = sdHp * sdHp;
    result.lowFreqVarianceFraction =
        (vLow + vHp) > 0.0 ? vLow / (vLow + vHp) : 0.0;
  }

  result.ok = true;
  result.lowFreqMerged = true;
  return result;
}

} // namespace paleo::inversion
