// 层：数据（测试壳位于 tests/，被测对象为数据层纯数值核）
#include <QtTest/QtTest>

#include "algorithms/dsp/fft.h"
#include "algorithms/inversion/wavelet.h"

#include <cmath>
#include <limits>
#include <random>
#include <vector>

using namespace paleo::inversion;

namespace
{

// 尖峰 train 褶积子波的正演合成道（与 wavelet.cpp 的 forwardModel 同式，
// 测试侧独立实现做对拍）。
std::vector<float> synthTrace(const std::vector<ReflSpike> &spikes, const Wavelet &w,
                              double traceT0Ms, int n, double dtMs)
{
  std::vector<float> s(std::size_t(n), 0.0f);
  for (const ReflSpike &sp : spikes)
  {
    const double rel = (sp.twtMs - traceT0Ms) / dtMs;
    const int m = int(std::lround(rel));
    const int off = int(std::lround((w.t0Ms - traceT0Ms) / dtMs));
    for (int j = 0; j < w.sampleCount(); ++j)
    {
      const int i = m + off + j;
      if (i >= 0 && i < n)
        s[std::size_t(i)] += float(double(sp.amplitude) * double(w.samples[std::size_t(j)]));
    }
  }
  return s;
}

double corr(const std::vector<float> &a, const std::vector<float> &b)
{
  double num = 0.0, ea = 0.0, eb = 0.0;
  const std::size_t n = std::min(a.size(), b.size());
  for (std::size_t i = 0; i < n; ++i)
  {
    num += double(a[i]) * double(b[i]);
    ea += double(a[i]) * double(a[i]);
    eb += double(b[i]) * double(b[i]);
  }
  const double den = std::sqrt(ea * eb);
  return den > 0.0 ? num / den : 0.0;
}

// 固定种子的反射系数序列：25 个尖峰散布在 [200, 2800] ms，幅度非零。
std::vector<ReflSpike> fixtureSpikes()
{
  std::mt19937 rng(42);
  std::uniform_real_distribution<double> tDist(200.0, 2800.0);
  std::uniform_real_distribution<double> aDist(-0.3, 0.3);
  std::vector<ReflSpike> spikes;
  while (spikes.size() < 25)
  {
    ReflSpike sp;
    sp.amplitude = float(aDist(rng));
    if (std::fabs(sp.amplitude) < 0.05)
      continue;
    sp.amplitude = sp.amplitude > 0 ? sp.amplitude + 0.05f : sp.amplitude - 0.05f;
    sp.twtMs = tDist(rng);
    spikes.push_back(sp);
  }
  return spikes;
}

} // namespace

class TestInversionWavelet : public QObject
{
  Q_OBJECT

private slots:
  // makeRicker：奇数采样、t=0 峰值 1、左右对称、解析谱峰主频 = f0
  //（Ricker 振幅谱 (f²/f0²)e^{−f²/f0²} 峰恰在 f0；FFT 零填充分辨率
  // 1000/(4096·4ms) ≈ 0.06Hz，容差 2Hz 覆盖离散化）。
  void rickerShapeAndSpectrum();
  // Oracle 1：已知 Ricker 褶积已知反射系数 → 提取子波与真子波相关 ≥0.95；
  // 长度 = 请求数；主频误差 ≤3Hz；拟合相关 ≥0.99（正演模型精确可复现）。
  void extractRecoversKnownRicker();
  // 含噪（σ = 0.05·rms）仍稳定：相关 ≥0.9、无 NaN、拟合相关如实下降但 >0.8。
  void extractWithNoise();
  // JSON 往返：样本逐位相等（float 经 double 文本表示无损）、extra 键存活。
  void jsonRoundTrip();
  // 无尖峰 / 单尖峰 / 全零尖峰 → ok=false 且 reason 非空（不装死）。
  void degenerateInputsFail();
  // 4ms 采样同链可用（真工区体常见采样率）。
  void fourMsSampling();
  // dsp::fft 共享后 seismicattr 的复数道链仍在（对拍 cos → Hilbert = sin）。
  void sharedFftCosineRoundtrip();
};

void TestInversionWavelet::rickerShapeAndSpectrum()
{
  const Wavelet w = makeRicker(25.0, 4.0, 128.0);
  QCOMPARE(w.sampleCount() % 2, 1);
  QVERIFY2(std::fabs(w.samples[std::size_t(w.sampleCount() / 2)] - 1.0f) < 1e-6,
           "t=0 峰值应为 1");
  QVERIFY(std::fabs(w.t0Ms + w.lengthMs() * 0.5) < 1e-9);
  for (int i = 0; i < w.sampleCount(); ++i)
    QVERIFY(std::fabs(double(w.samples[std::size_t(i)]) -
                      double(w.samples[std::size_t(w.sampleCount() - 1 - i)])) < 1e-6);
  QVERIFY2(std::fabs(w.dominantFreqHz() - 25.0) < 2.0,
           qPrintable(QString("主频估计 %1").arg(w.dominantFreqHz())));
}

void TestInversionWavelet::extractRecoversKnownRicker()
{
  const std::vector<ReflSpike> spikes = fixtureSpikes();
  const Wavelet truth = makeRicker(25.0, 2.0, 128.0);
  const std::vector<float> trace = synthTrace(spikes, truth, 0.0, 1501, 2.0);

  const WaveletExtractResult r = extractWavelet(trace.data(), int(trace.size()), 0.0, 2.0,
                                                spikes.data(), int(spikes.size()),
                                                -64.0, 65);
  QVERIFY2(r.ok, r.reason.c_str());
  QCOMPARE(r.wavelet.sampleCount(), 65);
  QVERIFY2(std::fabs(r.wavelet.t0Ms - (-64.0)) < 1e-9, "t0 应吸附到请求窗");
  const double c = corr(r.wavelet.samples, truth.samples);
  QVERIFY2(c >= 0.95, qPrintable(QString("与真子波相关 %1").arg(c)));
  QVERIFY2(std::fabs(r.wavelet.dominantFreqHz() - 25.0) < 3.0,
           qPrintable(QString("主频 %1").arg(r.wavelet.dominantFreqHz())));
  QVERIFY2(r.fitCorrelation >= 0.99,
           qPrintable(QString("拟合相关 %1").arg(r.fitCorrelation)));
}

void TestInversionWavelet::extractWithNoise()
{
  const std::vector<ReflSpike> spikes = fixtureSpikes();
  const Wavelet truth = makeRicker(25.0, 2.0, 128.0);
  std::vector<float> clean = synthTrace(spikes, truth, 0.0, 1501, 2.0);
  double rms = 0.0;
  for (float v : clean)
    rms += double(v) * double(v);
  rms = std::sqrt(rms / double(clean.size()));

  std::mt19937 rng(7);
  std::normal_distribution<double> noise(0.0, 0.05 * rms);
  std::vector<float> noisy = clean;
  for (float &v : noisy)
    v += float(noise(rng));

  const WaveletExtractResult r = extractWavelet(noisy.data(), int(noisy.size()), 0.0, 2.0,
                                                spikes.data(), int(spikes.size()),
                                                -64.0, 65);
  QVERIFY2(r.ok, r.reason.c_str());
  const double c = corr(r.wavelet.samples, truth.samples);
  QVERIFY2(c >= 0.9, qPrintable(QString("含噪相关 %1").arg(c)));
  for (float v : r.wavelet.samples)
    QVERIFY(std::isfinite(v));
  QVERIFY(r.fitCorrelation > 0.8);
  // 噪声诚实反映：含噪拟合相关低于无噪。
  const WaveletExtractResult cleanR = extractWavelet(
      clean.data(), int(clean.size()), 0.0, 2.0, spikes.data(), int(spikes.size()), -64.0, 65);
  QVERIFY(r.fitCorrelation < cleanR.fitCorrelation);
}

void TestInversionWavelet::jsonRoundTrip()
{
  const Wavelet w = makeRicker(30.0, 2.0, 96.0);
  QJsonObject extra;
  extra.insert(QStringLiteral("sourceWell"), QStringLiteral("well-A1"));
  extra.insert(QStringLiteral("fitCorrelation"), 0.97);
  const QByteArray bytes = waveletToJson(w, extra);

  Wavelet back;
  QJsonObject extraBack;
  QString err;
  QVERIFY2(waveletFromJson(bytes, &back, &extraBack, &err), qPrintable(err));
  QCOMPARE(back.sampleCount(), w.sampleCount());
  QCOMPARE(back.sampleIntervalMs, w.sampleIntervalMs);
  QCOMPARE(back.t0Ms, w.t0Ms);
  for (int i = 0; i < w.sampleCount(); ++i)
    QCOMPARE(back.samples[std::size_t(i)], w.samples[std::size_t(i)]);
  QCOMPARE(extraBack.value(QStringLiteral("sourceWell")).toString(), QStringLiteral("well-A1"));
  QVERIFY(std::fabs(extraBack.value(QStringLiteral("fitCorrelation")).toDouble() - 0.97) < 1e-12);

  QVERIFY(!waveletFromJson(QByteArrayLiteral("{}"), nullptr, nullptr, &err));
  QVERIFY2(!err.isEmpty(), "坏输入要给 reason");
}

void TestInversionWavelet::degenerateInputsFail()
{
  const std::vector<ReflSpike> spikes = fixtureSpikes();
  const Wavelet truth = makeRicker(25.0, 2.0, 128.0);
  const std::vector<float> trace = synthTrace(spikes, truth, 0.0, 1501, 2.0);

  WaveletExtractResult r = extractWavelet(trace.data(), int(trace.size()), 0.0, 2.0,
                                          nullptr, 0, -64.0, 65);
  QVERIFY(!r.ok);
  QVERIFY(!r.reason.empty());

  const std::vector<ReflSpike> single = {spikes.front()};
  r = extractWavelet(trace.data(), int(trace.size()), 0.0, 2.0, single.data(),
                     int(single.size()), -64.0, 65);
  QVERIFY(!r.ok);
  QVERIFY(!r.reason.empty());

  std::vector<ReflSpike> zeroAmp(3);
  zeroAmp[0].twtMs = 300.0;
  zeroAmp[1].twtMs = 800.0;
  zeroAmp[2].twtMs = 1500.0;
  r = extractWavelet(trace.data(), int(trace.size()), 0.0, 2.0, zeroAmp.data(),
                     int(zeroAmp.size()), -64.0, 65);
  QVERIFY(!r.ok);
  QVERIFY(!r.reason.empty());
}

void TestInversionWavelet::fourMsSampling()
{
  const std::vector<ReflSpike> spikes = fixtureSpikes();
  const Wavelet truth = makeRicker(20.0, 4.0, 160.0);
  const std::vector<float> trace = synthTrace(spikes, truth, 0.0, 801, 4.0);

  const WaveletExtractResult r = extractWavelet(trace.data(), int(trace.size()), 0.0, 4.0,
                                                spikes.data(), int(spikes.size()),
                                                -80.0, 41);
  QVERIFY2(r.ok, r.reason.c_str());
  const double c = corr(r.wavelet.samples, truth.samples);
  QVERIFY2(c >= 0.95, qPrintable(QString("4ms 相关 %1").arg(c)));
  QVERIFY(std::fabs(r.wavelet.dominantFreqHz() - 20.0) < 3.0);
}

void TestInversionWavelet::sharedFftCosineRoundtrip()
{
  // cos → FFT → 逆 FFT 恢复；正负频共轭对称（实信号不变量）。
  const int n = 256;
  std::vector<double> re(std::size_t(n), 0.0), im(std::size_t(n), 0.0);
  for (int i = 0; i < n; ++i)
    re[std::size_t(i)] = std::cos(2.0 * std::numbers::pi * 5.0 * double(i) / double(n));
  paleo::dsp::fftRadix2(re.data(), im.data(), n, false);
  for (int k = 1; k < n / 2; ++k)
  {
    QVERIFY(std::fabs(re[std::size_t(k)] - re[std::size_t(n - k)]) < 1e-9);
    QVERIFY(std::fabs(im[std::size_t(k)] + im[std::size_t(n - k)]) < 1e-9);
  }
  paleo::dsp::fftRadix2(re.data(), im.data(), n, true);
  for (int i = 0; i < n; ++i)
    QVERIFY(std::fabs(re[std::size_t(i)] - std::cos(2.0 * std::numbers::pi * 5.0 * double(i) /
                                                    double(n))) < 1e-9);
}

QTEST_MAIN(TestInversionWavelet)
#include "tst_inversion_wavelet.moc"
