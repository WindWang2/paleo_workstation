// 层：数据
#include "seismicattr.h"

// seismicattr 实现。FFT 为自研迭代 radix-2（double 内部精度）：仓库 vendor/
// 系统/Qt 均无合规 FFT（FFTW double-only 头且未链接、破坏钉位策略，见
// .goal-loop-ledger-seismic-attributes.md 轮0），自写是零新依赖的唯一路。

#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <utility>

namespace paleo::seisattr
{
namespace
{

// 迭代 Cooley-Tukey radix-2，原地。n 必须为 2 的幂。
// forward: X[k] = Σ x[j]·e^{-i2πjk/n}；inverse 先逆变换再整体除 n。
void fftRadix2(double *re, double *im, int n, bool inverse)
{
  for (int i = 1, j = 0; i < n; ++i)
  {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1)
      j ^= bit;
    j ^= bit;
    if (i < j)
    {
      std::swap(re[i], re[j]);
      std::swap(im[i], im[j]);
    }
  }
  for (int len = 2; len <= n; len <<= 1)
  {
    const double ang = (inverse ? 2.0 : -2.0) * std::numbers::pi / double(len);
    const double wr = std::cos(ang);
    const double wi = std::sin(ang);
    for (int base = 0; base < n; base += len)
    {
      double cr = 1.0;
      double ci = 0.0;
      for (int k = 0; k < len / 2; ++k)
      {
        const int a = base + k;
        const int b = a + len / 2;
        const double tr = re[b] * cr - im[b] * ci;
        const double ti = re[b] * ci + im[b] * cr;
        re[b] = re[a] - tr;
        im[b] = im[a] - ti;
        re[a] += tr;
        im[a] += ti;
        const double ncr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = ncr;
      }
    }
  }
  if (inverse)
  {
    const double inv = 1.0 / double(n);
    for (int i = 0; i < n; ++i)
    {
      re[i] *= inv;
      im[i] *= inv;
    }
  }
}

int nextPowerOfTwoAtLeast(int v)
{
  int p = 1;
  while (p < v)
    p <<= 1;
  return p;
}

// reflect（对称）镜像索引：把相对迹首的任意整数偏移 q 折回 [0, n)。
// q ∈ [0,n) 恒等；越界按周期 2(n-1) 三角波镜像（同 numpy pad mode='reflect'）。
// 用于把有限道延拓成准周期信号，抑制 Hilbert 频域法的边界回绕假象。
// 注意必须以「相对迹首的偏移」为键——若用绝对下标，填充段越过原迹副本
// 末尾后会周期回卷，把原迹窗口的后半偷换成镜像数据（正弦类准周期信号
// 不易察觉，瞬态信号立即穿帮）。
int reflectIndex(int q, int n)
{
  const int period = 2 * (n - 1);
  int r = q % period;
  if (r < 0)
    r += period;
  return r >= n ? period - r : r;
}

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

} // namespace

void analyticSignal(const float *trace, int n, float *outReal, float *outImag)
{
  if (!trace || !outReal || !outImag || n <= 0)
    return;
  for (int i = 0; i < n; ++i)
  {
    outReal[i] = kNaN;
    outImag[i] = kNaN;
  }
  if (n == 1)
  {
    outReal[0] = trace[0];
    outImag[0] = 0.0f;
    return;
  }

  bool anyFinite = false;
  for (int i = 0; i < n && !anyFinite; ++i)
    anyFinite = !std::isnan(trace[i]);
  if (!anyFinite)
    return;

  // 局部缺失（NaN）以 0 参与谱计算，输出位诚实 NaN。
  const int nFft = nextPowerOfTwoAtLeast(2 * n);
  const int leftPad = (nFft - n) / 2;
  std::vector<double> paddedRe(std::size_t(nFft), 0.0);
  std::vector<double> paddedIm(std::size_t(nFft), 0.0);
  for (int j = 0; j < nFft; ++j)
  {
    const double v = double(trace[reflectIndex(j - leftPad, n)]);
    paddedRe[std::size_t(j)] = std::isnan(v) ? 0.0 : v;
  }

  fftRadix2(paddedRe.data(), paddedIm.data(), nFft, /*inverse=*/false);

  // 解析信号谱：正频 ×2、负频清零、DC/Nyquist 保留（scipy.signal.hilbert 同约）。
  for (int k = 1; k < nFft / 2; ++k)
  {
    paddedRe[std::size_t(k)] *= 2.0;
    paddedIm[std::size_t(k)] *= 2.0;
    paddedRe[std::size_t(nFft - k)] = 0.0;
    paddedIm[std::size_t(nFft - k)] = 0.0;
  }

  fftRadix2(paddedRe.data(), paddedIm.data(), nFft, /*inverse=*/true);

  for (int i = 0; i < n; ++i)
  {
    if (std::isnan(trace[i]))
      continue; // 保持 NaN
    outReal[i] = float(paddedRe[std::size_t(leftPad + i)]);
    outImag[i] = float(paddedIm[std::size_t(leftPad + i)]);
  }
}

ComplexTraceResult complexTraceAnalysis(const float *trace, int n,
                                        double sampleIntervalMs)
{
  ComplexTraceResult r;
  r.envelope.resize(std::size_t(n < 0 ? 0 : n), kNaN);
  r.phaseDeg = r.envelope;
  r.freqHz = r.envelope;
  r.quality = r.envelope;
  if (n <= 0 || !trace || !(sampleIntervalMs > 0.0))
    return r;
  if (n == 1)
  {
    // 单样道：包络=|x|，相位按符号 0/180°，频率/Q 无定义。
    const float x = trace[0];
    r.envelope[0] = std::fabs(x);
    r.phaseDeg[0] = x >= 0.0f ? 0.0f : 180.0f;
    r.freqHz[0] = kNaN;
    r.quality[0] = kNaN;
    return r;
  }

  const double dtSec = sampleIntervalMs / 1000.0;
  std::vector<float> re(std::size_t(n), kNaN);
  std::vector<float> im(std::size_t(n), kNaN);
  analyticSignal(trace, n, re.data(), im.data());

  for (int i = 0; i < n; ++i)
  {
    if (std::isnan(trace[i]))
      continue; // 各输出保持 NaN
    const double a = re[std::size_t(i)];
    const double b = im[std::size_t(i)];
    r.envelope[std::size_t(i)] = float(std::sqrt(a * a + b * b));
    double deg = std::atan2(b, a) * (180.0 / std::numbers::pi);
    if (deg <= -180.0)
      deg += 360.0; // atan2 值域 (-π, π]，映射到 (-180, 180]
    r.phaseDeg[std::size_t(i)] = float(deg);
  }

  // 瞬时频率：a(t)·conj(a(t-1)) 的相位 / (2π·dt)（Barnes 2007 差分法）。
  for (int i = 1; i < n; ++i)
  {
    if (std::isnan(trace[i]) || std::isnan(trace[i - 1]))
      continue;
    const double ar = re[std::size_t(i)], ai = im[std::size_t(i)];
    const double br = re[std::size_t(i - 1)], bi = im[std::size_t(i - 1)];
    const double cr = ar * br + ai * bi;
    const double ci = ai * br - ar * bi;
    r.freqHz[std::size_t(i)] =
        float(std::atan2(ci, cr) / (2.0 * std::numbers::pi * dtSec));
  }
  if (!std::isnan(trace[0]) && n > 1 && !std::isnan(r.freqHz[1]))
    r.freqHz[0] = r.freqHz[1];

  // 瞬时 Q 原型：Q = π·f / |d ln(env)/dt|（中心差分；边缘单侧差分）。
  // 包络非正/衰减斜率近零（<1e-6/s）→ NaN：无衰减信息，数值不稳定（原型语义）。
  const double slopeEps = 1e-6;
  for (int i = 0; i < n; ++i)
  {
    if (std::isnan(trace[i]) || std::isnan(r.freqHz[std::size_t(i)]))
      continue;
    const double e = r.envelope[std::size_t(i)];
    if (!(e > 0.0))
      continue;
    const int ia = i > 0 ? i - 1 : i;
    const int ib = i < n - 1 ? i + 1 : i;
    if (std::isnan(trace[ia]) || std::isnan(trace[ib]))
      continue;
    const double ea = r.envelope[std::size_t(ia)];
    const double eb = r.envelope[std::size_t(ib)];
    if (!(ea > 0.0) || !(eb > 0.0) || ib == ia)
      continue;
    const double slope = std::log(eb / ea) / (double(ib - ia) * dtSec);
    const double absSlope = std::fabs(slope);
    if (absSlope < slopeEps)
      continue;
    r.quality[std::size_t(i)] =
        float(std::numbers::pi * r.freqHz[std::size_t(i)] / absSlope);
  }
  return r;
}

namespace
{

// 时窗族公共骨架：visit 窗口 [lo, hi]（闭区间，已与序列边界求交）。
template <typename Accum>
void windowedWalk(const float *x, int n, int halfWindow, float *out,
                  Accum accumulate)
{
  for (int i = 0; i < n; ++i)
  {
    const int lo = i - halfWindow < 0 ? 0 : i - halfWindow;
    const int hi = i + halfWindow > n - 1 ? n - 1 : i + halfWindow;
    out[i] = accumulate(x, lo, hi);
  }
}

} // namespace

void windowedRms(const float *x, int n, int halfWindow, float *out)
{
  if (!x || !out || n <= 0 || halfWindow < 0)
    return;
  windowedWalk(x, n, halfWindow, out, [](const float *v, int lo, int hi)
  {
    double sum = 0.0;
    int cnt = 0;
    for (int k = lo; k <= hi; ++k, ++cnt)
    {
      if (std::isnan(v[k]))
        return kNaN;
      sum += double(v[k]) * double(v[k]);
    }
    return float(std::sqrt(sum / double(cnt > 0 ? cnt : 1)));
  });
}

void windowedMaxAbs(const float *x, int n, int halfWindow, float *out)
{
  if (!x || !out || n <= 0 || halfWindow < 0)
    return;
  windowedWalk(x, n, halfWindow, out, [](const float *v, int lo, int hi)
  {
    float m = 0.0f;
    for (int k = lo; k <= hi; ++k)
    {
      if (std::isnan(v[k]))
        return kNaN;
      const float a = std::fabs(v[k]);
      if (a > m)
        m = a;
    }
    return m;
  });
}

void windowedMeanEnergy(const float *x, int n, int halfWindow, float *out)
{
  if (!x || !out || n <= 0 || halfWindow < 0)
    return;
  windowedWalk(x, n, halfWindow, out, [](const float *v, int lo, int hi)
  {
    double sum = 0.0;
    int cnt = 0;
    for (int k = lo; k <= hi; ++k, ++cnt)
    {
      if (std::isnan(v[k]))
        return kNaN;
      sum += double(v[k]) * double(v[k]);
    }
    return float(sum / double(cnt > 0 ? cnt : 1));
  });
}

void semblanceCoherence(const float *volume, int nIl, int nXl, int nS,
                        int spatialHalf, int timeHalf, float *out)
{
  if (!volume || !out || nIl <= 0 || nXl <= 0 || nS <= 0 ||
      spatialHalf < 0 || timeHalf < 0)
    return;
  const int nOut = nIl * nXl * nS;
  for (int i = 0; i < nOut; ++i)
    out[i] = kNaN;
  if (spatialHalf == 0 && timeHalf == 0)
    return; // 退化窗：单道单样 semblance 恒 1 无意义，保持 NaN

  const int traceStride = nS;                      // (il,xl) 相邻道步长
  const int rowStride = nXl * nS;                  // 相邻 il 步长
  const int spatialDiam = 2 * spatialHalf + 1;
  const double jTraces = double(spatialDiam) * double(spatialDiam);

  for (int il = spatialHalf; il < nIl - spatialHalf; ++il)
  {
    for (int xl = spatialHalf; xl < nXl - spatialHalf; ++xl)
    {
      const int traceBase = il * rowStride + xl * traceStride;
      const int outBase = traceBase;
      for (int s = 0; s < nS; ++s)
      {
        const int lo = s - timeHalf < 0 ? 0 : s - timeHalf;
        const int hi = s + timeHalf > nS - 1 ? nS - 1 : s + timeHalf;
        double num = 0.0;   // Σ_t (Σ_j u)²
        double den = 0.0;   // Σ_t Σ_j u²（最后乘 J）
        bool valid = true;
        for (int t = lo; t <= hi && valid; ++t)
        {
          double stack = 0.0;
          for (int dil = -spatialHalf; dil <= spatialHalf; ++dil)
          {
            for (int dxl = -spatialHalf; dxl <= spatialHalf; ++dxl)
            {
              const double v = double(volume[traceBase + dil * rowStride +
                                             dxl * traceStride + t]);
              if (std::isnan(v))
              {
                valid = false;
                break;
              }
              stack += v;
              den += v * v;
            }
            if (!valid)
              break;
          }
          num += stack * stack;
        }
        if (!valid)
          continue; // 输出保持 NaN
        den *= jTraces;
        // 全零窗是精确 0/0 → NaN；微小但非零的能量（如子波远尾）是合法
        // 窗（同波形时 S=1 成立），不得用绝对阈值误杀（double 平方和非负，
        // 仅精确零窗得 0）。
        out[outBase + s] = den > 0.0 ? float(num / den) : kNaN;
      }
    }
  }
}

void sweetness(const float *envelope, const float *freqHz, int n, float *out)
{
  if (!envelope || !freqHz || !out || n <= 0)
    return;
  const float fMin = 1e-3f;
  for (int i = 0; i < n; ++i)
  {
    if (std::isnan(envelope[i]) || std::isnan(freqHz[i]))
    {
      out[i] = kNaN;
      continue;
    }
    const float f = freqHz[i] > fMin ? freqHz[i] : fMin;
    out[i] = envelope[i] / std::sqrt(f);
  }
}

} // namespace paleo::seisattr
