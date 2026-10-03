// 层：数据
#include "sparse.h"

// 稀疏脉冲实现。褶积算子 A 与转置 Aᵀ 手写（O(n·wl)，wl ≤ 数百）；
// FISTA 惯例记号照 Beck & Teboulle (2009)。

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

// 褶积正算子：(A·r)[i] = Σ_k r[k]·w[i−k−peakOffset]，越界忽略。
void convSame(const std::vector<double> &r, const std::vector<double> &w, int peakOffset,
              int n, std::vector<double> *out)
{
  const int wl = int(w.size());
  out->assign(std::size_t(n), 0.0);
  for (int i = 0; i < n; ++i)
  {
    const int kLo = i - peakOffset - (wl - 1);
    const int kHi = i - peakOffset;
    double acc = 0.0;
    for (int k = std::max(0, kLo); k <= std::min(n - 1, kHi); ++k)
      acc += r[std::size_t(k)] * w[std::size_t(i - k - peakOffset)];
    (*out)[std::size_t(i)] = acc;
  }
}

// 转置算子：(Aᵀ·y)[k] = Σ_i y[i]·w[i−k−peakOffset]。
void convAdj(const std::vector<double> &y, const std::vector<double> &w, int peakOffset,
             int n, std::vector<double> *out)
{
  const int wl = int(w.size());
  out->assign(std::size_t(n), 0.0);
  for (int k = 0; k < n; ++k)
  {
    const int iLo = k + peakOffset;
    const int iHi = k + peakOffset + (wl - 1);
    double acc = 0.0;
    for (int i = std::max(0, iLo); i <= std::min(n - 1, iHi); ++i)
      acc += y[std::size_t(i)] * w[std::size_t(i - k - peakOffset)];
    (*out)[std::size_t(k)] = acc;
  }
}

double dot(const std::vector<double> &a, const std::vector<double> &b)
{
  double s = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i)
    s += a[i] * b[i];
  return s;
}

} // namespace

SparseSpikeResult sparseSpikeInversion(const float *trace, int n, double sampleIntervalMs,
                                       const Wavelet &wavelet, const float *lowFreq,
                                       const SparseSpikeOptions &options)
{
  SparseSpikeResult result;
  if (!trace || n < 16 || !(sampleIntervalMs > 0.0))
  {
    result.reason = "输入道无效（长度<16 或采样间隔<=0）";
    return result;
  }
  if (wavelet.isEmpty())
  {
    result.reason = "子波为空";
    return result;
  }
  if (std::fabs(wavelet.sampleIntervalMs - sampleIntervalMs) > 1e-9)
  {
    result.reason = "子波采样间隔与道不一致（先重采样子波）";
    return result;
  }
  if (options.maxIterations < 8 || options.maxIterations > 10000)
  {
    result.reason = "maxIterations 越界 [8, 10000]";
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

  const int wl = wavelet.sampleCount();
  const int peakOffset = int(std::lround(wavelet.t0Ms / sampleIntervalMs));
  std::vector<double> w(wl);
  for (int j = 0; j < wl; ++j)
    w[std::size_t(j)] = double(wavelet.samples[std::size_t(j)]);
  std::vector<double> s(std::size_t(n), 0.0);
  for (int i = 0; i < n; ++i)
    s[std::size_t(i)] = std::isnan(trace[i]) ? 0.0 : double(trace[i]);
  const double energyS = dot(s, s);
  if (!(energyS > 0.0))
  {
    result.reason = "道能量为零";
    return result;
  }

  // λ 自适应：0.05·max|Aᵀs|（数据驱动尺度，与道幅度无关）。
  std::vector<double> corr;
  convAdj(s, w, peakOffset, n, &corr);
  double maxCorr = 0.0;
  for (double v : corr)
    maxCorr = std::max(maxCorr, std::fabs(v));
  const double lambda =
      options.lambda > 0.0 ? options.lambda : 0.05 * maxCorr;
  result.lambdaUsed = lambda;
  if (!(lambda > 0.0))
  {
    result.reason = "λ 有效值为零（道与子波正交或 λ 配置无效）";
    return result;
  }

  // 幂迭代估 ||AᵀA||₂（30 轮足够收敛到工程精度，×1.05 安全裕度）。
  std::vector<double> v(std::size_t(n), 1.0);
  std::vector<double> Av, AtAv;
  for (int it = 0; it < 30; ++it)
  {
    convSame(v, w, peakOffset, n, &Av);
    convAdj(Av, w, peakOffset, n, &AtAv);
    const double nv = std::sqrt(dot(AtAv, AtAv));
    if (!(nv > 0.0))
      break;
    for (int i = 0; i < n; ++i)
      v[std::size_t(i)] = AtAv[std::size_t(i)] / nv;
  }
  convSame(v, w, peakOffset, n, &Av);
  convAdj(Av, w, peakOffset, n, &AtAv);
  const double lip = std::sqrt(dot(AtAv, AtAv)) * 1.05;
  if (!(lip > 0.0))
  {
    result.reason = "Lipschitz 估计失败（子波能量为零）";
    return result;
  }

  // FISTA 主循环。
  std::vector<double> x(std::size_t(n), 0.0), y = x, grad, model, resid;
  double t = 1.0;
  const double threshold = lambda / lip;
  for (int it = 1; it <= options.maxIterations; ++it)
  {
    convSame(y, w, peakOffset, n, &model);
    for (int i = 0; i < n; ++i)
      model[std::size_t(i)] -= s[std::size_t(i)];
    convAdj(model, w, peakOffset, n, &grad);

    std::vector<double> xNew(std::size_t(n), 0.0);
    double diff = 0.0, normX = 0.0;
    for (int i = 0; i < n; ++i)
    {
      const double z = y[std::size_t(i)] - grad[std::size_t(i)] / lip;
      xNew[std::size_t(i)] =
          z > threshold ? z - threshold : (z < -threshold ? z + threshold : 0.0);
      diff += std::fabs(xNew[std::size_t(i)] - x[std::size_t(i)]);
      normX += std::fabs(xNew[std::size_t(i)]);
    }
    const double tNew = 0.5 * (1.0 + std::sqrt(1.0 + 4.0 * t * t));
    for (int i = 0; i < n; ++i)
      y[std::size_t(i)] =
          xNew[std::size_t(i)] + ((t - 1.0) / tNew) * (xNew[std::size_t(i)] - x[std::size_t(i)]);
    x = std::move(xNew);
    t = tNew;
    result.iterations = it;
    if (diff <= options.relativeTolerance * std::max(1.0, normX))
    {
      result.converged = true;
      break;
    }
  }

  convSame(x, w, peakOffset, n, &model);
  for (int i = 0; i < n; ++i)
    model[std::size_t(i)] -= s[std::size_t(i)];
  result.residualEnergyRatio = dot(model, model) / energyS;

  result.reflectivity.resize(std::size_t(n));
  for (int i = 0; i < n; ++i)
    result.reflectivity[std::size_t(i)] =
        std::isnan(trace[i]) ? kNan : float(x[std::size_t(i)]);

  // 递推阻抗：种子 = 低频首样（有限）；无低频 → 1.0（相对口径）。
  double seed = 1.0;
  bool hasLow = false;
  if (lowFreq)
  {
    for (int i = 0; i < n; ++i)
    {
      if (std::isfinite(lowFreq[i]))
      {
        seed = double(lowFreq[i]);
        hasLow = true;
        break;
      }
    }
  }
  result.impedance.resize(std::size_t(n), kNan);
  double z = seed;
  for (int i = 0; i < n; ++i)
  {
    if (!std::isfinite(result.reflectivity[std::size_t(i)]))
    {
      result.impedance[std::size_t(i)] = kNan;
      continue;
    }
    double r = std::clamp(double(result.reflectivity[std::size_t(i)]), -0.45, 0.45);
    if (i > 0)
      z = z * (1.0 + r) / (1.0 - r);
    result.impedance[std::size_t(i)] =
        (lowFreq && !std::isfinite(lowFreq[i])) ? kNan : float(z);
  }
  if (!hasLow && lowFreq)
  {
    // 低频全缺：递推值仍给（相对口径），但低频种子语义如实丢失。
    result.reason = "低频模型全缺，阻抗为相对口径";
  }

  result.ok = true;
  return result;
}

} // namespace paleo::inversion
