// 层：数据（测试壳位于 tests/，被测对象为数据层纯数值核）
#include <QElapsedTimer>
#include <QtTest/QtTest>

#include "algorithms/dsp/fft.h"
#include "perfbudget_relax.h"

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

// #559：dsp/FFT 专属套件。此前仅 tst_inversion_wavelet 顺带引用，核本身
// 无对拍覆盖。口径对齐相邻数值核测试（tst_seismicattr / tst_inversion_*）：
// 解析恒等式（δ/直流/单音）+ 独立直求 DFT 对拍 + 往返/守恒性质。
// #478 旋转因子查表后蝶形算术误差 O(eps)，容差统一取 1e-9 相对量级——
// 真回归（位反转/旋转因子符号/逆变换忘除 n）给 O(1) 误差，远超门。

namespace
{

using Signal = std::vector<double>; // re/im 平面分开存放，同 fftRadix2 签名

// 独立直求 O(n²) DFT（与 fft.cpp 实现无共享代码，对拍基准）。
// inverse=true 时算 IDFT（含 1/n 归一化），同 fftRadix2 语义。
void directDft(Signal &re, Signal &im, bool inverse)
{
  const int n = int(re.size());
  Signal xr(n, 0.0), xi(n, 0.0);
  for (int k = 0; k < n; ++k)
  {
    for (int j = 0; j < n; ++j)
    {
      const double ang = (inverse ? 2.0 : -2.0) * 3.14159265358979323846
                         * double(k) * double(j) / double(n);
      const double c = std::cos(ang), s = std::sin(ang);
      xr[std::size_t(k)] += re[std::size_t(j)] * c - im[std::size_t(j)] * s;
      xi[std::size_t(k)] += re[std::size_t(j)] * s + im[std::size_t(j)] * c;
    }
    if (inverse)
    {
      xr[std::size_t(k)] /= double(n);
      xi[std::size_t(k)] /= double(n);
    }
  }
  re = std::move(xr);
  im = std::move(xi);
}

// 固定种子随机复信号。
void randomComplex(Signal &re, Signal &im, int n, std::uint32_t seed)
{
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> dist(-1.0, 1.0);
  re.assign(std::size_t(n), 0.0);
  im.assign(std::size_t(n), 0.0);
  for (int i = 0; i < n; ++i)
  {
    re[std::size_t(i)] = dist(rng);
    im[std::size_t(i)] = dist(rng);
  }
}

double maxAbsDiff(const Signal &a, const Signal &b)
{
  double worst = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i)
    worst = std::max(worst, std::fabs(a[i] - b[i]));
  return worst;
}

} // namespace

class TestDspFft : public QObject
{
  Q_OBJECT

private slots:
  // ---- nextPowerOfTwoAtLeast 契约 ------------------------------------------
  void nextPowerOfTwoContract();

  // ---- 解析恒等式 ----------------------------------------------------------
  // δ(j=0)：平坦谱 X[k]≡1；δ 平移到 j0：X[k]=e^{-i2πj0k/n}，模恒 1。
  void impulseFlatSpectrum();
  void shiftedImpulseUnitMagnitude();
  // 常数 c：全部能量落 DC 箱，X[0]=n·c，其余箱 ~0。
  void constantEnergyInDcBin();
  // 复单音 e^{i2πk0j/n}：谱只在 k0 箱非零，|X[k0]|=n。
  void complexToneSingleBin();
  // 实余弦 cos(2πk0j/n)：±k0 各半（n/2），虚部为 0。
  void realCosineSymmetricPair();

  // ---- 对拍 / 性质 ---------------------------------------------------------
  // 随机复信号：fftRadix2 与独立 O(n²) 直求逐位对拍（正/逆两个方向）——
  // #478 回归门：twiddle 精度劣化在此最先露头。
  void matchesDirectDftForwardAndInverse();
  // ifft(fft(x))=x 机器精度往返，n 覆盖 1/2/1024/4096（含 n=1 恒等档）。
  void inverseRoundTrip();
  // Parseval：Σ|x|² = (1/n)Σ|X|²（谱范数守恒）。
  void parsevalEnergyConservation();
  // 线性：FFT(αx+βy) = αFFT(x)+βFFT(y)。
  void linearity();
  // 实信号 Hermitian 对称：X[n-k] = conj(X[k])。
  void hermitianSymmetryForRealInput();
  // 2^18 档往返 + 能量守恒烟囱测试：防大 n 路径回归（预算走放宽口径）。
  void largePowerOfTwoSmoke();
};

void TestDspFft::nextPowerOfTwoContract()
{
  using paleo::dsp::nextPowerOfTwoAtLeast;
  // v ≤ 1 → 1（含 0 与负数：调用方拿它当容量，最小 1 保 vector 非空）。
  QCOMPARE(nextPowerOfTwoAtLeast(0), 1);
  QCOMPARE(nextPowerOfTwoAtLeast(1), 1);
  QCOMPARE(nextPowerOfTwoAtLeast(-7), 1);
  // 2 的幂恒等。
  for (int p = 1; p <= (1 << 20); p <<= 1)
    QCOMPARE(nextPowerOfTwoAtLeast(p), p);
  // 非 2 的幂向上取整。
  QCOMPARE(nextPowerOfTwoAtLeast(2), 2);
  QCOMPARE(nextPowerOfTwoAtLeast(3), 4);
  QCOMPARE(nextPowerOfTwoAtLeast(5), 8);
  QCOMPARE(nextPowerOfTwoAtLeast(1000), 1024);
  QCOMPARE(nextPowerOfTwoAtLeast((1 << 20) - 1), 1 << 20);
}

void TestDspFft::impulseFlatSpectrum()
{
  const int n = 64;
  Signal re(std::size_t(n), 0.0), im(std::size_t(n), 0.0);
  re[0] = 1.0;
  paleo::dsp::fftRadix2(re.data(), im.data(), n, /*inverse=*/false);
  for (int k = 0; k < n; ++k)
  {
    QCOMPARE(re[std::size_t(k)], 1.0); // Σ_j δ(j)e^{...} 恒为 1
    QCOMPARE(im[std::size_t(k)], 0.0);
  }
}

void TestDspFft::shiftedImpulseUnitMagnitude()
{
  const int n = 128, j0 = 37;
  Signal re(std::size_t(n), 0.0), im(std::size_t(n), 0.0);
  re[std::size_t(j0)] = 1.0;
  paleo::dsp::fftRadix2(re.data(), im.data(), n, /*inverse=*/false);
  for (int k = 0; k < n; ++k)
  {
    const double mag = std::hypot(re[std::size_t(k)], im[std::size_t(k)]);
    QVERIFY2(std::fabs(mag - 1.0) < 1e-12,
             qPrintable(QStringLiteral("k=%1 |X|=%2").arg(k).arg(mag, 0, 'e', 3)));
    // 相位对照解析值 e^{-i2πj0k/n}。
    const double ang = -2.0 * 3.14159265358979323846 * double(j0) * double(k)
                       / double(n);
    QVERIFY2(std::fabs(re[std::size_t(k)] - std::cos(ang)) < 1e-12
                 && std::fabs(im[std::size_t(k)] - std::sin(ang)) < 1e-12,
             qPrintable(QStringLiteral("k=%1 phase drift").arg(k)));
  }
}

void TestDspFft::constantEnergyInDcBin()
{
  const int n = 256;
  const double c = 0.37;
  Signal re(std::size_t(n), c), im(std::size_t(n), 0.0);
  paleo::dsp::fftRadix2(re.data(), im.data(), n, /*inverse=*/false);
  QVERIFY(std::fabs(re[0] - double(n) * c) < 1e-9);
  QVERIFY(std::fabs(im[0]) < 1e-9);
  for (int k = 1; k < n; ++k)
  {
    QVERIFY2(std::fabs(re[std::size_t(k)]) < 1e-9 && std::fabs(im[std::size_t(k)]) < 1e-9,
             qPrintable(QStringLiteral("k=%1 leaked (%2,%3)")
                            .arg(k)
                            .arg(re[std::size_t(k)], 0, 'e', 2)
                            .arg(im[std::size_t(k)], 0, 'e', 2)));
  }
}

void TestDspFft::complexToneSingleBin()
{
  const int n = 512;
  for (int k0 : {1, n / 4, n - 1}) // 低频 / Nyquist 附近 / 负频率（wrap 到 n-1）
  {
    Signal re(std::size_t(n), 0.0), im(std::size_t(n), 0.0);
    for (int j = 0; j < n; ++j)
    {
      const double ang = 2.0 * 3.14159265358979323846 * double(k0) * double(j)
                         / double(n);
      re[std::size_t(j)] = std::cos(ang);
      im[std::size_t(j)] = std::sin(ang);
    }
    paleo::dsp::fftRadix2(re.data(), im.data(), n, /*inverse=*/false);
    for (int k = 0; k < n; ++k)
    {
      const double mag = std::hypot(re[std::size_t(k)], im[std::size_t(k)]);
      const double expect = (k == k0) ? double(n) : 0.0;
      QVERIFY2(std::fabs(mag - expect) < 1e-9 * double(n),
               qPrintable(QStringLiteral("k0=%1 k=%2 |X|=%3").arg(k0).arg(k).arg(mag, 0, 'e', 3)));
    }
  }
}

void TestDspFft::realCosineSymmetricPair()
{
  const int n = 256, k0 = 7;
  Signal re(std::size_t(n), 0.0), im(std::size_t(n), 0.0);
  for (int j = 0; j < n; ++j)
    re[std::size_t(j)] = std::cos(2.0 * 3.14159265358979323846 * double(k0)
                                  * double(j) / double(n));
  paleo::dsp::fftRadix2(re.data(), im.data(), n, /*inverse=*/false);
  // cos = (e^{iθ}+e^{-iθ})/2 → ±k0 各 n/2，且虚部为 0。
  QVERIFY(std::fabs(re[std::size_t(k0)] - double(n) / 2.0) < 1e-9);
  QVERIFY(std::fabs(re[std::size_t(n - k0)] - double(n) / 2.0) < 1e-9);
  QVERIFY(std::fabs(im[std::size_t(k0)]) < 1e-9);
  QVERIFY(std::fabs(im[std::size_t(n - k0)]) < 1e-9);
  for (int k = 0; k < n; ++k)
  {
    if (k == k0 || k == n - k0)
      continue;
    QVERIFY2(std::hypot(re[std::size_t(k)], im[std::size_t(k)]) < 1e-9,
             qPrintable(QStringLiteral("k=%1 leaked").arg(k)));
  }
}

void TestDspFft::matchesDirectDftForwardAndInverse()
{
  const int n = 256; // 直求 O(n²)=65k 次三角对，够分辨 twiddle 级误差
  for (const bool inverse : {false, true})
  {
    Signal re, im;
    randomComplex(re, im, n, inverse ? 7u : 42u);
    Signal refRe = re, refIm = im;
    paleo::dsp::fftRadix2(re.data(), im.data(), n, inverse);
    directDft(refRe, refIm, inverse);
    QVERIFY2(maxAbsDiff(re, refRe) < 1e-9 && maxAbsDiff(im, refIm) < 1e-9,
             qPrintable(QStringLiteral("inverse=%1 re=%2 im=%3")
                            .arg(inverse)
                            .arg(maxAbsDiff(re, refRe), 0, 'e', 2)
                            .arg(maxAbsDiff(im, refIm), 0, 'e', 2)));
  }
}

void TestDspFft::inverseRoundTrip()
{
  for (int n : {1, 2, 1024, 4096})
  {
    Signal re, im;
    randomComplex(re, im, n, 1234u + std::uint32_t(n));
    const Signal re0 = re, im0 = im;
    paleo::dsp::fftRadix2(re.data(), im.data(), n, /*inverse=*/false);
    paleo::dsp::fftRadix2(re.data(), im.data(), n, /*inverse=*/true);
    QVERIFY2(maxAbsDiff(re, re0) < 1e-9 && maxAbsDiff(im, im0) < 1e-9,
             qPrintable(QStringLiteral("n=%1 re=%2 im=%3")
                            .arg(n)
                            .arg(maxAbsDiff(re, re0), 0, 'e', 2)
                            .arg(maxAbsDiff(im, im0), 0, 'e', 2)));
  }
}

void TestDspFft::parsevalEnergyConservation()
{
  const int n = 1024;
  Signal re, im;
  randomComplex(re, im, n, 99u);
  Signal fwdRe = re, fwdIm = im;
  paleo::dsp::fftRadix2(fwdRe.data(), fwdIm.data(), n, /*inverse=*/false);
  double timeEnergy = 0.0, freqEnergy = 0.0;
  for (int i = 0; i < n; ++i)
  {
    timeEnergy += re[std::size_t(i)] * re[std::size_t(i)]
                  + im[std::size_t(i)] * im[std::size_t(i)];
    freqEnergy += fwdRe[std::size_t(i)] * fwdRe[std::size_t(i)]
                  + fwdIm[std::size_t(i)] * fwdIm[std::size_t(i)];
  }
  // Σ|x|² = (1/n)Σ|X|²（未归一化 DFT 的 Parseval 形式）。
  QVERIFY2(std::fabs(timeEnergy - freqEnergy / double(n))
               < 1e-9 * std::max(1.0, timeEnergy),
           qPrintable(QStringLiteral("time=%1 freq/n=%2")
                          .arg(timeEnergy, 0, 'e', 4)
                          .arg(freqEnergy / double(n), 0, 'e', 4)));
}

void TestDspFft::linearity()
{
  const int n = 128;
  Signal xRe, xIm, yRe, yIm;
  randomComplex(xRe, xIm, n, 5u);
  randomComplex(yRe, yIm, n, 6u);
  Signal zRe(std::size_t(n), 0.0), zIm(std::size_t(n), 0.0), sRe(std::size_t(n), 0.0),
      sIm(std::size_t(n), 0.0);
  const double alpha = 0.7, beta = -1.3;
  for (int i = 0; i < n; ++i)
  {
    zRe[std::size_t(i)] = alpha * xRe[std::size_t(i)] + beta * yRe[std::size_t(i)];
    zIm[std::size_t(i)] = alpha * xIm[std::size_t(i)] + beta * yIm[std::size_t(i)];
  }
  paleo::dsp::fftRadix2(xRe.data(), xIm.data(), n, /*inverse=*/false);
  paleo::dsp::fftRadix2(yRe.data(), yIm.data(), n, /*inverse=*/false);
  for (int i = 0; i < n; ++i)
  {
    sRe[std::size_t(i)] = alpha * xRe[std::size_t(i)] + beta * yRe[std::size_t(i)];
    sIm[std::size_t(i)] = alpha * xIm[std::size_t(i)] + beta * yIm[std::size_t(i)];
  }
  paleo::dsp::fftRadix2(zRe.data(), zIm.data(), n, /*inverse=*/false);
  QVERIFY2(maxAbsDiff(zRe, sRe) < 1e-9 && maxAbsDiff(zIm, sIm) < 1e-9,
           qPrintable(QStringLiteral("re=%1 im=%2")
                          .arg(maxAbsDiff(zRe, sRe), 0, 'e', 2)
                          .arg(maxAbsDiff(zIm, sIm), 0, 'e', 2)));
}

void TestDspFft::hermitianSymmetryForRealInput()
{
  const int n = 512;
  Signal re, im;
  randomComplex(re, im, n, 77u);
  for (int i = 0; i < n; ++i)
    im[std::size_t(i)] = 0.0; // 实信号
  paleo::dsp::fftRadix2(re.data(), im.data(), n, /*inverse=*/false);
  for (int k = 1; k < n / 2; ++k)
  {
    QVERIFY2(std::fabs(re[std::size_t(k)] - re[std::size_t(n - k)]) < 1e-9
                 && std::fabs(im[std::size_t(k)] + im[std::size_t(n - k)]) < 1e-9,
             qPrintable(QStringLiteral("k=%1 hermitian violation").arg(k)));
  }
}

void TestDspFft::largePowerOfTwoSmoke()
{
  const int n = 1 << 18; // 262k：bandlimit/wavelet 实际 nFft 量级
  QElapsedTimer t;
  t.start();
  Signal re(std::size_t(n), 0.0), im(std::size_t(n), 0.0);
  std::mt19937 rng(2026u);
  std::uniform_real_distribution<double> dist(-1.0, 1.0);
  double timeEnergy = 0.0;
  for (int i = 0; i < n; ++i)
  {
    re[std::size_t(i)] = dist(rng);
    timeEnergy += re[std::size_t(i)] * re[std::size_t(i)];
  }
  paleo::dsp::fftRadix2(re.data(), im.data(), n, /*inverse=*/false);
  double freqEnergy = 0.0;
  for (int i = 0; i < n; ++i)
    freqEnergy += re[std::size_t(i)] * re[std::size_t(i)]
                  + im[std::size_t(i)] * im[std::size_t(i)];
  QVERIFY2(std::fabs(timeEnergy - freqEnergy / double(n))
               < 1e-9 * std::max(1.0, timeEnergy),
           qPrintable(QStringLiteral("2^18 parseval drift")));
  // 墙钟预算走 #559 放宽口径（sanitizer 档 ×3）——空载 ~30ms，留一个
  // 数量级余量做回归门。
  QVERIFY2(t.elapsed() < paleo::perfbudget::relaxedBudgetMs(500),
           qPrintable(QStringLiteral("2^18 fft took %1 ms").arg(t.elapsed())));
}

QTEST_MAIN(TestDspFft)
#include "tst_dsp_fft.moc"
