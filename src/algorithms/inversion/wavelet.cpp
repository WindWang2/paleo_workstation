// 层：数据
#include "wavelet.h"

// 子波实现。FFT 用 dsp/fft.h 共享 radix-2；正则方程解用带列主元的高斯
// 消元（子波长度 ≤ 数百样，直接解比 Levinson 递推更省心且数值够用）。

#include "algorithms/dsp/fft.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

namespace paleo::inversion
{
namespace
{

// 带列主元的高斯消元解 A·x = b（n×n，A 行主序，b 被覆盖，x 写回 b）。
// 奇异（主元近零）返回 false。
bool solveLinear(std::vector<double> &a, std::vector<double> &b, int n)
{
  for (int col = 0; col < n; ++col)
  {
    int piv = col;
    double best = std::fabs(a[std::size_t(col * n + col)]);
    for (int r = col + 1; r < n; ++r)
    {
      const double v = std::fabs(a[std::size_t(r * n + col)]);
      if (v > best)
      {
        best = v;
        piv = r;
      }
    }
    if (!(best > 1e-12))
      return false;
    if (piv != col)
    {
      for (int c = 0; c < n; ++c)
        std::swap(a[std::size_t(piv * n + c)], a[std::size_t(col * n + c)]);
      std::swap(b[std::size_t(piv)], b[std::size_t(col)]);
    }
    for (int r = col + 1; r < n; ++r)
    {
      const double f = a[std::size_t(r * n + col)] / a[std::size_t(col * n + col)];
      if (f == 0.0)
        continue;
      for (int c = col; c < n; ++c)
        a[std::size_t(r * n + c)] -= f * a[std::size_t(col * n + c)];
      b[std::size_t(r)] -= f * b[std::size_t(col)];
    }
  }
  for (int r = n - 1; r >= 0; --r)
  {
    double s = b[std::size_t(r)];
    for (int c = r + 1; c < n; ++c)
      s -= a[std::size_t(r * n + c)] * b[std::size_t(c)];
    b[std::size_t(r)] = s / a[std::size_t(r * n + r)];
  }
  return true;
}

// 尖峰 train 褶积子波的正演合成道：synth[i] = Σ_k a_k · w[i − m_k − offset]。
std::vector<double> forwardModel(const std::vector<double> &spikeTrain,
                                 const std::vector<double> &wavelet, int offset,
                                 int nTrace)
{
  std::vector<double> synth(std::size_t(nTrace), 0.0);
  const int wl = int(wavelet.size());
  for (int m = 0; m < nTrace; ++m)
  {
    const double a = spikeTrain[std::size_t(m)];
    if (a == 0.0)
      continue;
    for (int j = 0; j < wl; ++j)
    {
      const int i = m + offset + j;
      if (i >= 0 && i < nTrace)
        synth[std::size_t(i)] += a * wavelet[std::size_t(j)];
    }
  }
  return synth;
}

double normalizedCorrelation(const std::vector<double> &a, const std::vector<double> &b)
{
  double num = 0.0, ea = 0.0, eb = 0.0;
  const std::size_t n = std::min(a.size(), b.size());
  for (std::size_t i = 0; i < n; ++i)
  {
    num += a[i] * b[i];
    ea += a[i] * a[i];
    eb += b[i] * b[i];
  }
  const double den = std::sqrt(ea * eb);
  return den > 0.0 ? num / den : 0.0;
}

} // namespace

double Wavelet::lengthMs() const
{
  return samples.empty() ? 0.0 : double(samples.size() - 1) * sampleIntervalMs;
}

double Wavelet::peakAmplitude() const
{
  double peak = 0.0;
  for (float v : samples)
    peak = std::max(peak, std::fabs(double(v)));
  return peak;
}

double Wavelet::dominantFreqHz() const
{
  const int n = int(samples.size());
  if (n < 2 || !(sampleIntervalMs > 0.0))
    return 0.0;
  // 时移不改振幅谱，直接对采样数组做谱（wavelet 居中与否无妨）。
  const int nFft = paleo::dsp::nextPowerOfTwoAtLeast(4 * n);
  std::vector<double> re(std::size_t(nFft), 0.0), im(std::size_t(nFft), 0.0);
  for (int i = 0; i < n; ++i)
    re[std::size_t(i)] = double(samples[std::size_t(i)]);
  paleo::dsp::fftRadix2(re.data(), im.data(), nFft, /*inverse=*/false);
  double best = 0.0;
  int bestK = 0;
  for (int k = 1; k < nFft / 2; ++k) // DC 不算主频
  {
    const double mag = re[std::size_t(k)] * re[std::size_t(k)] +
                       im[std::size_t(k)] * im[std::size_t(k)];
    if (mag > best)
    {
      best = mag;
      bestK = k;
    }
  }
  return double(bestK) / (double(nFft) * sampleIntervalMs / 1000.0);
}

Wavelet makeRicker(double f0Hz, double sampleIntervalMs, double lengthMs)
{
  Wavelet w;
  w.sampleIntervalMs = sampleIntervalMs;
  if (!(f0Hz > 0.0) || !(sampleIntervalMs > 0.0) || !(lengthMs > 0.0))
    return w;
  const int half = std::max(1, int(std::lround(lengthMs * 0.5 / sampleIntervalMs)));
  const int n = 2 * half + 1; // 奇数：t=0 恰有采样
  const double piF0Dt = std::numbers::pi * f0Hz * sampleIntervalMs / 1000.0;
  const double a2 = piF0Dt * piF0Dt;
  w.samples.resize(std::size_t(n));
  double peak = 0.0;
  for (int i = 0; i < n; ++i)
  {
    const double t = double(i - half);
    const double v = (1.0 - 2.0 * a2 * t * t) * std::exp(-a2 * t * t);
    w.samples[std::size_t(i)] = float(v);
    peak = std::max(peak, std::fabs(v));
  }
  if (peak > 0.0)
    for (auto &v : w.samples)
      v = float(double(v) / peak);
  w.t0Ms = -double(half) * sampleIntervalMs;
  return w;
}

Wavelet rotateWaveletPhase(const Wavelet &wavelet, double phaseDeg)
{
  Wavelet out = wavelet;
  const int n = wavelet.sampleCount();
  if (n < 2)
    return out;
  // 解析信号单边谱旋转：正频乘 e^{iθ}、负频取共轭保持实信号、DC/Nyquist 不动。
  const int nFft = paleo::dsp::nextPowerOfTwoAtLeast(4 * n);
  std::vector<double> re(std::size_t(nFft), 0.0), im(std::size_t(nFft), 0.0);
  for (int i = 0; i < n; ++i)
    re[std::size_t(i)] = double(wavelet.samples[std::size_t(i)]);
  paleo::dsp::fftRadix2(re.data(), im.data(), nFft, /*inverse=*/false);
  const double theta = phaseDeg * std::numbers::pi / 180.0;
  const double c = std::cos(theta);
  const double s = std::sin(theta);
  for (int k = 1; k < nFft / 2; ++k)
  {
    const double xr = re[std::size_t(k)] * c - im[std::size_t(k)] * s;
    const double xi = re[std::size_t(k)] * s + im[std::size_t(k)] * c;
    re[std::size_t(k)] = xr;
    im[std::size_t(k)] = xi;
    re[std::size_t(nFft - k)] = xr;
    im[std::size_t(nFft - k)] = -xi;
  }
  paleo::dsp::fftRadix2(re.data(), im.data(), nFft, /*inverse=*/true);
  double peak = 0.0;
  for (int i = 0; i < n; ++i)
    peak = std::max(peak, std::fabs(re[std::size_t(i)]));
  if (peak > 0.0)
  {
    for (int i = 0; i < n; ++i)
      out.samples[std::size_t(i)] = float(re[std::size_t(i)] / peak);
  }
  return out;
}

WaveletExtractResult extractWavelet(const float *trace, int nTrace, double traceT0Ms,
                                    double sampleIntervalMs, const ReflSpike *spikes,
                                    int nSpikes, double waveletT0Ms, int waveletSamples,
                                    const WaveletExtractOptions &options)
{
  WaveletExtractResult result;
  if (!trace || nTrace < 8 || !(sampleIntervalMs > 0.0) || !spikes)
  {
    result.reason = "输入道无效（长度<8 或采样间隔<=0）";
    return result;
  }
  if (waveletSamples < 3 || waveletSamples > 4096)
  {
    result.reason = "子波长度越界 [3, 4096]";
    return result;
  }
  if (options.ridgeFactor < 0.0)
  {
    result.reason = "ridgeFactor < 0";
    return result;
  }

  // 尖峰吸附到道采样网格；道时窗外或零幅度的跳过。
  std::vector<double> spikeTrain(std::size_t(nTrace), 0.0);
  int usedSpikes = 0;
  for (int k = 0; k < nSpikes; ++k)
  {
    if (spikes[k].amplitude == 0.0f || !std::isfinite(spikes[k].twtMs))
      continue;
    const double rel = (spikes[k].twtMs - traceT0Ms) / sampleIntervalMs;
    const int m = int(std::lround(rel));
    if (m < 0 || m >= nTrace)
      continue;
    spikeTrain[std::size_t(m)] += double(spikes[k].amplitude);
    ++usedSpikes;
  }
  if (usedSpikes < 2)
  {
    result.reason = "有效反射系数尖峰不足 2 个（无法定子波）";
    return result;
  }

  // 子波首样在道网格上的偏移（取整吸附）。
  const int offset = int(std::lround((waveletT0Ms - traceT0Ms) / sampleIntervalMs));
  const int wl = waveletSamples;

  // Toeplitz 自相关 R[d]（d ∈ [0, wl)）与互相关 c[j]。
  std::vector<double> autoR(std::size_t(wl), 0.0);
  std::vector<double> rhs(std::size_t(wl), 0.0);
  for (int d = 0; d < wl; ++d)
  {
    for (int m = 0; m + d < nTrace; ++m)
      autoR[std::size_t(d)] += spikeTrain[std::size_t(m)] * spikeTrain[std::size_t(m + d)];
    for (int m = 0; m < nTrace; ++m)
    {
      const int i = m + offset + d;
      if (i >= 0 && i < nTrace)
        rhs[std::size_t(d)] += double(trace[i]) * spikeTrain[std::size_t(m)];
    }
  }
  if (!(autoR[0] > 0.0))
  {
    result.reason = "反射系数自相关为零";
    return result;
  }
  autoR[0] += options.ridgeFactor * autoR[0];

  std::vector<double> mat(std::size_t(wl * wl), 0.0);
  for (int j = 0; j < wl; ++j)
    for (int j2 = 0; j2 < wl; ++j2)
      mat[std::size_t(j * wl + j2)] = autoR[std::size_t(std::abs(j - j2))];
  std::vector<double> waveD(rhs.begin(), rhs.end());
  if (!solveLinear(mat, waveD, wl))
  {
    result.reason = "正则方程奇异（反射系数分布不足以定子波）";
    return result;
  }
  for (double v : waveD)
  {
    if (!std::isfinite(v))
    {
      result.reason = "子波解含非有限值";
      return result;
    }
  }

  double peak = 0.0;
  for (double v : waveD)
    peak = std::max(peak, std::fabs(v));
  if (!(peak > 0.0))
  {
    result.reason = "子波解为零";
    return result;
  }

  Wavelet w;
  w.sampleIntervalMs = sampleIntervalMs;
  w.t0Ms = traceT0Ms + double(offset) * sampleIntervalMs;
  w.samples.resize(std::size_t(wl));
  for (int j = 0; j < wl; ++j)
    w.samples[std::size_t(j)] = float(waveD[std::size_t(j)] / peak);

  const std::vector<double> synth = forwardModel(spikeTrain, waveD, offset, nTrace);
  std::vector<double> traceD(std::size_t(nTrace), 0.0);
  for (int i = 0; i < nTrace; ++i)
    traceD[std::size_t(i)] = double(trace[i]);

  result.ok = true;
  result.wavelet = std::move(w);
  result.fitCorrelation = normalizedCorrelation(traceD, synth);
  return result;
}

QByteArray waveletToJson(const Wavelet &wavelet, const QJsonObject &extra)
{
  QJsonObject root = extra;
  root.insert(QStringLiteral("sampleIntervalMs"), wavelet.sampleIntervalMs);
  root.insert(QStringLiteral("t0Ms"), wavelet.t0Ms);
  QJsonArray arr;
  for (float v : wavelet.samples)
    arr.append(double(v));
  root.insert(QStringLiteral("samples"), arr);
  return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool waveletFromJson(const QByteArray &bytes, Wavelet *wavelet, QJsonObject *extra,
                     QString *error)
{
  if (error)
    error->clear();
  QJsonParseError parse{};
  const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parse);
  if (parse.error != QJsonParseError::NoError || !doc.isObject())
  {
    if (error)
      *error = QStringLiteral("wavelet.json 解析失败: ") + parse.errorString();
    return false;
  }
  const QJsonObject root = doc.object();
  const double dt = root.value(QStringLiteral("sampleIntervalMs")).toDouble(-1.0);
  const double t0 = root.value(QStringLiteral("t0Ms")).toDouble(0.0);
  const QJsonArray arr = root.value(QStringLiteral("samples")).toArray();
  if (!(dt > 0.0) || arr.size() < 3)
  {
    if (error)
      *error = QStringLiteral("wavelet.json 缺 sampleIntervalMs>0 或 samples>=3");
    return false;
  }
  Wavelet w;
  w.sampleIntervalMs = dt;
  w.t0Ms = t0;
  w.samples.resize(std::size_t(arr.size()));
  for (int i = 0; i < arr.size(); ++i)
  {
    const double v = arr.at(i).toDouble();
    if (!std::isfinite(v))
    {
      if (error)
        *error = QStringLiteral("wavelet.json 样本含非有限值");
      return false;
    }
    w.samples[std::size_t(i)] = float(v);
  }
  if (wavelet)
    *wavelet = std::move(w);
  if (extra)
  {
    QJsonObject rest = root;
    rest.remove(QStringLiteral("sampleIntervalMs"));
    rest.remove(QStringLiteral("t0Ms"));
    rest.remove(QStringLiteral("samples"));
    *extra = rest;
  }
  return true;
}

} // namespace paleo::inversion
