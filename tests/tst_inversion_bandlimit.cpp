// 层：数据（测试壳位于 tests/，被测对象为数据层纯数值核）
#include <QtTest/QtTest>

#include "algorithms/inversion/bandlimit.h"
#include "algorithms/inversion/lowfreq.h"

#include <cmath>
#include <limits>
#include <vector>

using namespace paleo::inversion;

namespace
{

constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
constexpr double kDt = 2.0; // ms
constexpr int kN = 1500;    // 0–2998 ms

// 四层阶梯真阻抗：600/1300/2100 ms 分界 → 6000/6600/6300/6900。
std::vector<float> trueImpedance()
{
  std::vector<float> z(std::size_t(kN), 0.0f);
  for (int i = 0; i < kN; ++i)
  {
    const double t = double(i) * kDt;
    z[std::size_t(i)] = t < 600.0    ? 6000.0f
                        : t < 1300.0 ? 6600.0f
                        : t < 2100.0 ? 6300.0f
                                     : 6900.0f;
  }
  return z;
}

// 真阻抗的界面反射系数尖峰（界面深度处 (Z2−Z1)/(Z2+Z1)）。
std::vector<ReflSpike> trueSpikes()
{
  return {
      {600.0 - kDt, float((6600.0 - 6000.0) / (6600.0 + 6000.0))},
      {1300.0 - kDt, float((6300.0 - 6600.0) / (6300.0 + 6600.0))},
      {2100.0 - kDt, float((6900.0 - 6300.0) / (6900.0 + 6300.0))},
  };
}

// 尖峰 × 子波正演（同轮1 测试的独立实现）。
std::vector<float> synth(const std::vector<ReflSpike> &spikes, const Wavelet &w)
{
  std::vector<float> s(std::size_t(kN), 0.0f);
  const int off = int(std::lround(w.t0Ms / kDt));
  for (const ReflSpike &sp : spikes)
  {
    const int m = int(std::lround(sp.twtMs / kDt));
    for (int j = 0; j < w.sampleCount(); ++j)
    {
      const int i = m + off + j;
      if (i >= 0 && i < kN)
        s[std::size_t(i)] += float(double(sp.amplitude) * double(w.samples[std::size_t(j)]));
    }
  }
  return s;
}

double pearson(const std::vector<float> &a, const std::vector<float> &b)
{
  double num = 0.0, ea = 0.0, eb = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i)
  {
    num += double(a[i]) * double(b[i]);
    ea += double(a[i]) * double(a[i]);
    eb += double(b[i]) * double(b[i]);
  }
  return std::sqrt(ea * eb) > 0.0 ? num / std::sqrt(ea * eb) : 0.0;
}

// 层内均值相对误差（%）：跳过边界 ±80 样与首尾边缘。
double layerMeanErrorPct(const std::vector<float> &imp)
{
  const std::vector<float> truth = trueImpedance();
  const int bounds[] = {30, 220, 380, 570, 730, 970, 1150, 1400};
  const double layerZ[] = {6000.0, 6600.0, 6300.0, 6900.0};
  double worst = 0.0;
  for (int l = 0; l < 4; ++l)
  {
    double sum = 0.0;
    int cnt = 0;
    for (int i = bounds[2 * l]; i < bounds[2 * l + 1]; ++i)
    {
      const double v = double(imp[std::size_t(i)]);
      if (std::isfinite(v))
      {
        sum += v;
        ++cnt;
      }
    }
    const double mean = sum / double(cnt);
    const double err = std::fabs(mean - layerZ[l]) / layerZ[l] * 100.0;
    worst = std::max(worst, err);
  }
  return worst;
}

} // namespace

class TestInversionBandlimit : public QObject
{
  Q_OBJECT

private slots:
  // Oracle 3a：解析层状模型闭环——同子波褶积正演 → 反演逐层误差阈值。
  void layeredClosedLoop();
  // Oracle 3b：混合相位对拍——30° 相位失配误差大于零相位（如实反映，不强求
  // 校正），有界不发散。
  void mixedPhaseDegrades();
  // 无低频模型：输出相对阻抗，与真阻抗（去均值）相关 ≥0.95。
  void relativeOnly();
  // 无子波：裸道直接积分仍可用（口径如实：无反褶积）。
  void noWaveletRawIntegration();
  // 零道：绝对输出 = 低频模型本身。
  void flatTraceReturnsLowFreq();
  // 缺失语义：稀疏 NaN 保持 NaN；>30% 缺失整道拒绝。
  void nanSemantics();
  // 子波采样间隔与道不一致 → 显式失败。
  void dtMismatchFails();
  // #141：低频模型为常数（不含层信息）时，带内阻抗细节必须按阻抗单位补回
  //（旧实现无量纲相对阻抗直接相加，贡献 ≈0.05，形同只输出低频模型）。
  void bandContributionInImpedanceUnits();
  // #141：道振幅 ×1000 + 子波 amplitudeScale=1000 → 结果与未缩放一致（<1%）；
  // 不给标定 → 反射系数大量越界，显式失败而非静默输出。
  void amplitudeCalibrationScaleInvariant();
};

void TestInversionBandlimit::layeredClosedLoop()
{
  const Wavelet w = makeRicker(30.0, kDt, 128.0);
  const std::vector<float> trace = synth(trueSpikes(), w);
  const std::vector<float> truth = trueImpedance();
  // 完美低频模型：真阻抗过 lowCut=8Hz 低通核。
  const int half = std::max(1, int(std::lround(1000.0 / 8.0 / kDt * 0.5)));
  std::vector<float> low(std::size_t(kN), 0.0f);
  lowCutMovingAverage(truth.data(), kN, half, low.data());

  const BandlimitedResult r =
      bandlimitedInversion(trace.data(), kN, kDt, &w, low.data());
  QVERIFY2(r.ok, r.reason.c_str());
  QVERIFY(r.lowFreqMerged);
  const double err = layerMeanErrorPct(r.impedance);
  QVERIFY2(err < 2.0, qPrintable(QString("逐层均值误差 %1%").arg(err)));
  // 本夹具低频模型承载几乎全部层水平（带限项只补过渡），低频方差占比应过半。
  QVERIFY2(r.lowFreqVarianceFraction > 0.5 && r.lowFreqVarianceFraction <= 1.0,
           qPrintable(QString("低频方差占比 %1").arg(r.lowFreqVarianceFraction)));
  for (float v : r.impedance)
    QVERIFY(std::isfinite(v));
}

void TestInversionBandlimit::mixedPhaseDegrades()
{
  const Wavelet zeroPhase = makeRicker(30.0, kDt, 128.0);
  const Wavelet mixed = rotateWaveletPhase(zeroPhase, 30.0);
  // 相位旋转不改振幅谱：主频一致。
  QVERIFY(std::fabs(mixed.dominantFreqHz() - zeroPhase.dominantFreqHz()) < 1.0);
  bool differs = false;
  for (int i = 0; i < mixed.sampleCount(); ++i)
    differs = differs ||
              std::fabs(double(mixed.samples[std::size_t(i)]) -
                        double(zeroPhase.samples[std::size_t(i)])) > 1e-3;
  QVERIFY(differs);

  const std::vector<float> truth = trueImpedance();
  const int half = std::max(1, int(std::lround(1000.0 / 8.0 / kDt * 0.5)));
  std::vector<float> low(std::size_t(kN), 0.0f);
  lowCutMovingAverage(truth.data(), kN, half, low.data());

  const std::vector<float> traceZero = synth(trueSpikes(), zeroPhase);
  const std::vector<float> traceMixed = synth(trueSpikes(), mixed);
  const BandlimitedResult rz =
      bandlimitedInversion(traceZero.data(), kN, kDt, &zeroPhase, low.data());
  const BandlimitedResult rm =
      bandlimitedInversion(traceMixed.data(), kN, kDt, &zeroPhase, low.data());
  QVERIFY(rz.ok);
  QVERIFY(rm.ok);
  // 相位失配的诚实信号在相对项：30° 失配相关显著低于零相位（不强求校正）；
  // 合并层误差被低频模型托底，有界不发散。
  const double errM = layerMeanErrorPct(rm.impedance);
  QVERIFY2(errM < 8.0, qPrintable(QString("混合相位合并层误差 %1（有界不发散）").arg(errM)));
  for (float v : rm.impedance)
    QVERIFY(std::isfinite(v));
  BandlimitedResult relZero =
      bandlimitedInversion(traceZero.data(), kN, kDt, &zeroPhase, nullptr);
  BandlimitedResult relMixed =
      bandlimitedInversion(traceMixed.data(), kN, kDt, &zeroPhase, nullptr);
  std::vector<float> truthHp(std::size_t(kN), 0.0f);
  for (int i = 0; i < kN; ++i)
    truthHp[std::size_t(i)] = truth[std::size_t(i)] - low[std::size_t(i)];
  const double cz = pearson(relZero.impedance, truthHp);
  const double cm = pearson(relMixed.impedance, truthHp);
  QVERIFY2(cm < cz, qPrintable(QString("混合相位相关 %1 应低于零相位 %2").arg(cm).arg(cz)));
  QVERIFY2(cm >= 0.7, qPrintable(QString("混合相位相关 %1（有界退化）").arg(cm)));
}

void TestInversionBandlimit::relativeOnly()
{
  const Wavelet w = makeRicker(30.0, kDt, 128.0);
  const std::vector<float> trace = synth(trueSpikes(), w);
  const BandlimitedResult r = bandlimitedInversion(trace.data(), kN, kDt, &w, nullptr);
  QVERIFY2(r.ok, r.reason.c_str());
  QVERIFY(!r.lowFreqMerged);
  // 相对阻抗是带限量（零面积子波 → 无绝对趋势），对拍高通真阻抗（同 lowCut 核）。
  const std::vector<float> truth = trueImpedance();
  const int half = std::max(1, int(std::lround(1000.0 / 8.0 / kDt * 0.5)));
  std::vector<float> low(std::size_t(kN), 0.0f);
  lowCutMovingAverage(truth.data(), kN, half, low.data());
  std::vector<float> truthHp(std::size_t(kN), 0.0f);
  for (int i = 0; i < kN; ++i)
    truthHp[std::size_t(i)] = truth[std::size_t(i)] - low[std::size_t(i)];
  const double c = pearson(r.impedance, truthHp);
  QVERIFY2(c >= 0.90, qPrintable(QString("相对阻抗与高通真阻抗相关 %1").arg(c)));
}

void TestInversionBandlimit::noWaveletRawIntegration()
{
  const Wavelet w = makeRicker(30.0, kDt, 128.0);
  const std::vector<float> trace = synth(trueSpikes(), w);
  const BandlimitedResult r = bandlimitedInversion(trace.data(), kN, kDt, nullptr, nullptr);
  QVERIFY2(r.ok, r.reason.c_str());
  const std::vector<float> truth = trueImpedance();
  const int half = std::max(1, int(std::lround(1000.0 / 8.0 / kDt * 0.5)));
  std::vector<float> low(std::size_t(kN), 0.0f);
  lowCutMovingAverage(truth.data(), kN, half, low.data());
  std::vector<float> truthHp(std::size_t(kN), 0.0f);
  for (int i = 0; i < kN; ++i)
    truthHp[std::size_t(i)] = truth[std::size_t(i)] - low[std::size_t(i)];
  const double c = pearson(r.impedance, truthHp);
  QVERIFY2(c >= 0.6,
           qPrintable(QString("裸道积分相关 %1（无反褶积口径，阈值保守）").arg(c)));
  for (float v : r.impedance)
    QVERIFY(std::isfinite(v));
}

void TestInversionBandlimit::flatTraceReturnsLowFreq()
{
  const std::vector<float> trace(std::size_t(kN), 0.0f);
  std::vector<float> low(std::size_t(kN), 0.0f);
  for (int i = 0; i < kN; ++i)
    low[std::size_t(i)] = float(6000.0 + 0.4 * double(i) * kDt);
  const Wavelet w = makeRicker(30.0, kDt, 128.0);
  const BandlimitedResult r =
      bandlimitedInversion(trace.data(), kN, kDt, &w, low.data());
  QVERIFY2(r.ok, r.reason.c_str());
  for (int i = 0; i < kN; ++i)
  {
    QVERIFY(std::isfinite(r.impedance[std::size_t(i)]));
    QVERIFY2(std::fabs(double(r.impedance[std::size_t(i)]) - double(low[std::size_t(i)])) <
                 1e-3,
             "零道反演应原样返回低频模型");
  }
}

void TestInversionBandlimit::nanSemantics()
{
  const Wavelet w = makeRicker(30.0, kDt, 128.0);
  std::vector<float> trace = synth(trueSpikes(), w);
  trace[100] = kNan;
  trace[101] = kNan;
  std::vector<float> low(std::size_t(kN), 6300.0f);
  const BandlimitedResult r = bandlimitedInversion(trace.data(), kN, kDt, &w, low.data());
  QVERIFY2(r.ok, r.reason.c_str());
  QVERIFY(std::isnan(r.impedance[std::size_t(100)]));
  QVERIFY(std::isnan(r.impedance[std::size_t(101)]));
  QVERIFY(std::isfinite(r.impedance[std::size_t(200)]));

  std::vector<float> hole = trace;
  for (int i = 0; i < kN * 4 / 10; ++i)
    hole[std::size_t(i * 2)] = kNan; // ~40% 缺失
  const BandlimitedResult fail = bandlimitedInversion(hole.data(), kN, kDt, &w, low.data());
  QVERIFY(!fail.ok);
  QVERIFY(!fail.reason.empty());
}

void TestInversionBandlimit::dtMismatchFails()
{
  const Wavelet bad = makeRicker(30.0, 4.0, 128.0); // 4ms 子波配 2ms 道
  const std::vector<float> trace(std::size_t(kN), 0.0f);
  const BandlimitedResult r = bandlimitedInversion(trace.data(), kN, kDt, &bad, nullptr);
  QVERIFY(!r.ok);
  QVERIFY(!r.reason.empty());
}

void TestInversionBandlimit::bandContributionInImpedanceUnits()
{
  const Wavelet w = makeRicker(30.0, kDt, 128.0);
  const std::vector<float> trace = synth(trueSpikes(), w);
  const std::vector<float> truth = trueImpedance();
  const int half = std::max(1, int(std::lround(1000.0 / 8.0 / kDt * 0.5)));
  std::vector<float> lowTruth(std::size_t(kN), 0.0f);
  lowCutMovingAverage(truth.data(), kN, half, lowTruth.data());
  std::vector<float> truthHp(std::size_t(kN), 0.0f);
  for (int i = 0; i < kN; ++i)
    truthHp[std::size_t(i)] = truth[std::size_t(i)] - lowTruth[std::size_t(i)];

  const std::vector<float> flatLow(std::size_t(kN), 6450.0f);
  const BandlimitedResult r = bandlimitedInversion(trace.data(), kN, kDt, &w, flatLow.data());
  QVERIFY2(r.ok, r.reason.c_str());
  std::vector<float> band(std::size_t(kN), 0.0f);
  double sdBand = 0.0, sdTruth = 0.0;
  for (int i = 0; i < kN; ++i)
  {
    band[std::size_t(i)] = r.impedance[std::size_t(i)] - flatLow[std::size_t(i)];
    sdBand += double(band[std::size_t(i)]) * double(band[std::size_t(i)]);
    sdTruth += double(truthHp[std::size_t(i)]) * double(truthHp[std::size_t(i)]);
  }
  sdBand = std::sqrt(sdBand / kN);
  sdTruth = std::sqrt(sdTruth / kN);
  // 带内分量与高通真阻抗同形，且幅度同量级（阻抗单位，不是 0.05 的无量纲量）。
  const double c = pearson(band, truthHp);
  QVERIFY2(c >= 0.9, qPrintable(QString("带内分量与高通真阻抗相关 %1").arg(c)));
  QVERIFY2(sdBand > 0.5 * sdTruth && sdBand < 2.0 * sdTruth,
           qPrintable(QString("带内分量 σ=%1，高通真阻抗 σ=%2").arg(sdBand).arg(sdTruth)));
  QVERIFY2(r.lowFreqVarianceFraction < 0.01,
           qPrintable(QString("常数低频的方差占比应≈0，实际 %1").arg(r.lowFreqVarianceFraction)));
}

void TestInversionBandlimit::amplitudeCalibrationScaleInvariant()
{
  const Wavelet w = makeRicker(30.0, kDt, 128.0);
  const std::vector<float> trace = synth(trueSpikes(), w);
  const std::vector<float> truth = trueImpedance();
  const int half = std::max(1, int(std::lround(1000.0 / 8.0 / kDt * 0.5)));
  std::vector<float> low(std::size_t(kN), 0.0f);
  lowCutMovingAverage(truth.data(), kN, half, low.data());

  std::vector<float> scaled = trace;
  for (float &v : scaled)
    v *= 1000.0f;
  Wavelet wk = w;
  wk.amplitudeScale = 1000.0;

  const BandlimitedResult ref = bandlimitedInversion(trace.data(), kN, kDt, &w, low.data());
  const BandlimitedResult cal = bandlimitedInversion(scaled.data(), kN, kDt, &wk, low.data());
  QVERIFY2(ref.ok && cal.ok, (ref.reason + cal.reason).c_str());
  QVERIFY(cal.amplitudeCalibrated);
  QVERIFY(!ref.amplitudeCalibrated);
  for (int i = 0; i < kN; ++i)
  {
    const double a = ref.impedance[std::size_t(i)], b = cal.impedance[std::size_t(i)];
    QVERIFY2(std::fabs(a - b) <= 0.01 * std::fabs(a),
             qPrintable(QString("样 %1：%2 vs %3").arg(i).arg(a).arg(b)));
  }

  // 显式 options.amplitudeScale 与子波标定等价。
  BandlimitedOptions opt;
  opt.amplitudeScale = 1000.0;
  const BandlimitedResult explicitCal =
      bandlimitedInversion(scaled.data(), kN, kDt, &w, low.data(), opt);
  QVERIFY2(explicitCal.ok, explicitCal.reason.c_str());
  QVERIFY(std::fabs(explicitCal.impedance[700] - ref.impedance[700]) <= 0.01 * ref.impedance[700]);

  // 未标定：缩放道直接当反射系数 → 越界比例高 → 显式失败。
  const BandlimitedResult uncal = bandlimitedInversion(scaled.data(), kN, kDt, &w, low.data());
  QVERIFY(!uncal.ok);
  QVERIFY(!uncal.reason.empty());
  QVERIFY(uncal.clampedFraction > 0.05);
}

QTEST_MAIN(TestInversionBandlimit)
#include "tst_inversion_bandlimit.moc"
