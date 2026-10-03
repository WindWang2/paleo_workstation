// 层：数据（测试壳位于 tests/，被测对象为数据层纯数值核）
#include <QtTest/QtTest>

#include "algorithms/inversion/sparse.h"

#include <cmath>
#include <limits>
#include <random>
#include <vector>

using namespace paleo::inversion;

namespace
{

constexpr double kDt = 2.0; // ms
constexpr int kN = 1500;    // 0–2998 ms

// 8 个分离尖峰（间隔 ≥60 样，FISTA 可分辨）。
std::vector<ReflSpike> sparseSpikes()
{
  std::mt19937 rng(11);
  std::uniform_real_distribution<double> tDist(300.0, 2600.0);
  std::uniform_real_distribution<double> aDist(-0.2, 0.2);
  std::vector<ReflSpike> spikes;
  while (spikes.size() < 8)
  {
    ReflSpike sp;
    sp.amplitude = float(aDist(rng));
    if (std::fabs(sp.amplitude) < 0.08)
      continue;
    sp.twtMs = tDist(rng);
    bool far = true;
    for (const ReflSpike &prev : spikes)
      if (std::fabs(prev.twtMs - sp.twtMs) < 200.0)
        far = false;
    if (far)
      spikes.push_back(sp);
  }
  std::sort(spikes.begin(), spikes.end(),
            [](const ReflSpike &a, const ReflSpike &b) { return a.twtMs < b.twtMs; });
  return spikes;
}

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

// 真反射系数落在采样网格上（与稀疏解同表示）。
std::vector<float> spikeGrid(const std::vector<ReflSpike> &spikes)
{
  std::vector<float> r(std::size_t(kN), 0.0f);
  for (const ReflSpike &sp : spikes)
    r[std::size_t(int(std::lround(sp.twtMs / kDt)))] += sp.amplitude;
  return r;
}

double l1Error(const std::vector<float> &est, const std::vector<float> &truth)
{
  double e = 0.0;
  for (std::size_t i = 0; i < est.size(); ++i)
    e += std::fabs(double(est[i]) - double(truth[i]));
  return e;
}

double l1Norm(const std::vector<float> &x)
{
  double e = 0.0;
  for (float v : x)
    e += std::fabs(double(v));
  return e;
}

} // namespace

class TestInversionSparse : public QObject
{
  Q_OBJECT

private slots:
  // Oracle 4a：稀疏反射系数恢复——L1 误差 ≤ 0.15·||r_true||₁；事件位置±2 样
  // 同号；残差能量比 <0.05。
  void recoversSparseReflectivity();
  // Oracle 4b：含噪（σ=5% rms）稳定——不崩不 NaN，残差如实高于无噪，L1 误差
  // 有界（≤ 无噪的 3 倍）。
  void noisyTraceStability();
  // 阻抗递推种子 = 低频首样；层段均值与真阻抗趋势一致。
  void impedanceSeedFromLowFreq();
  // 迭代上限诚实：maxIterations=8 → 不谎报收敛；结果仍有限。
  void iterationCapHonest();
  // 退化输入：空子波 / dt 不匹配 / 零能量道 / >30% 缺失 → 显式失败。
  void invalidInputsFail();
};

void TestInversionSparse::recoversSparseReflectivity()
{
  const std::vector<ReflSpike> spikes = sparseSpikes();
  const Wavelet w = makeRicker(30.0, kDt, 128.0);
  const std::vector<float> trace = synth(spikes, w);
  const std::vector<float> truth = spikeGrid(spikes);

  SparseSpikeOptions opt;
  opt.lambda = 0.02; // 已知稀疏真值，手动压低阈值保恢复
  const SparseSpikeResult r = sparseSpikeInversion(trace.data(), kN, kDt, w, nullptr, opt);
  QVERIFY2(r.ok, r.reason.c_str());
  const double err = l1Error(r.reflectivity, truth);
  const double bound = 0.15 * l1Norm(truth);
  QVERIFY2(err <= bound,
           qPrintable(QString("L1 误差 %1 > 阈 %2（λ=%3, iters=%4）")
                        .arg(err)
                        .arg(bound)
                        .arg(r.lambdaUsed)
                        .arg(r.iterations)));
  QVERIFY2(r.residualEnergyRatio < 0.05,
           qPrintable(QString("残差能量比 %1").arg(r.residualEnergyRatio)));
  // 事件位置与符号：每个真尖峰 ±2 样内找到同号显著解样。
  for (const ReflSpike &sp : spikes)
  {
    const int m = int(std::lround(sp.twtMs / kDt));
    bool found = false;
    for (int i = std::max(0, m - 2); i <= std::min(kN - 1, m + 2); ++i)
    {
      const double v = double(r.reflectivity[std::size_t(i)]);
      if (std::fabs(v) > 0.3 * std::fabs(double(sp.amplitude)) &&
          v * double(sp.amplitude) > 0.0)
        found = true;
    }
    QVERIFY2(found, qPrintable(QString("样 %1 处未恢复事件（amp %2）").arg(m).arg(sp.amplitude)));
  }
  for (float v : r.reflectivity)
    QVERIFY(std::isfinite(v));
}

void TestInversionSparse::noisyTraceStability()
{
  const std::vector<ReflSpike> spikes = sparseSpikes();
  const Wavelet w = makeRicker(30.0, kDt, 128.0);
  const std::vector<float> clean = synth(spikes, w);
  double rms = 0.0;
  for (float v : clean)
    rms += double(v) * double(v);
  rms = std::sqrt(rms / double(clean.size()));

  std::mt19937 rng(3);
  std::normal_distribution<double> noise(0.0, 0.05 * rms);
  std::vector<float> noisy = clean;
  for (float &v : noisy)
    v += float(noise(rng));

  const std::vector<float> truth = spikeGrid(spikes);
  SparseSpikeOptions opt;
  opt.lambda = 0.02;
  const SparseSpikeResult rc =
      sparseSpikeInversion(clean.data(), kN, kDt, w, nullptr, opt);
  const SparseSpikeResult rn =
      sparseSpikeInversion(noisy.data(), kN, kDt, w, nullptr, opt);
  QVERIFY(rc.ok);
  QVERIFY2(rn.ok, rn.reason.c_str());
  for (float v : rn.reflectivity)
    QVERIFY(std::isfinite(v));
  for (float v : rn.impedance)
    QVERIFY(std::isfinite(v));
  QVERIFY2(rn.residualEnergyRatio > rc.residualEnergyRatio,
           qPrintable(QString("含噪残差 %1 应高于无噪 %2（SNR 下降如实反映）")
                        .arg(rn.residualEnergyRatio)
                        .arg(rc.residualEnergyRatio)));
  const double errN = l1Error(rn.reflectivity, truth);
  const double errC = l1Error(rc.reflectivity, truth);
  QVERIFY2(errN <= 3.0 * errC + 0.05 * l1Norm(truth),
           qPrintable(QString("含噪 L1 误差 %1 vs 无噪 %2").arg(errN).arg(errC)));
}

void TestInversionSparse::impedanceSeedFromLowFreq()
{
  const std::vector<ReflSpike> spikes = sparseSpikes();
  const Wavelet w = makeRicker(30.0, kDt, 128.0);
  const std::vector<float> trace = synth(spikes, w);
  std::vector<float> low(std::size_t(kN), 0.0f);
  for (int i = 0; i < kN; ++i)
    low[std::size_t(i)] = float(6000.0 + 0.3 * double(i) * kDt);

  SparseSpikeOptions opt;
  opt.lambda = 0.02;
  const SparseSpikeResult r = sparseSpikeInversion(trace.data(), kN, kDt, w, low.data(), opt);
  QVERIFY2(r.ok, r.reason.c_str());
  QVERIFY2(std::fabs(double(r.impedance[0]) - double(low[0])) < 1e-6, "种子应取低频首样");
  // 相邻正事件后阻抗抬升、负事件后下降（递推方向正确）。
  const std::vector<float> truth = spikeGrid(spikes);
  for (const ReflSpike &sp : spikes)
  {
    const int m = int(std::lround(sp.twtMs / kDt));
    if (m < 10 || m + 20 >= kN)
      continue;
    const double before = double(r.impedance[std::size_t(m - 8)]);
    const double after = double(r.impedance[std::size_t(m + 12)]);
    QVERIFY2((after - before) * double(sp.amplitude) > 0.0,
             qPrintable(QString("样 %1 阻抗阶跃方向与反射系数符号一致").arg(m)));
  }
  Q_UNUSED(truth);
}

void TestInversionSparse::iterationCapHonest()
{
  const std::vector<ReflSpike> spikes = sparseSpikes();
  const Wavelet w = makeRicker(30.0, kDt, 128.0);
  const std::vector<float> trace = synth(spikes, w);
  SparseSpikeOptions opt;
  opt.maxIterations = 8;
  const SparseSpikeResult r = sparseSpikeInversion(trace.data(), kN, kDt, w, nullptr, opt);
  QVERIFY2(r.ok, r.reason.c_str());
  QCOMPARE(r.iterations, 8);
  for (float v : r.reflectivity)
    QVERIFY(std::isfinite(v));
}

void TestInversionSparse::invalidInputsFail()
{
  const std::vector<ReflSpike> spikes = sparseSpikes();
  const Wavelet w = makeRicker(30.0, kDt, 128.0);
  const std::vector<float> trace = synth(spikes, w);

  const Wavelet badDt = makeRicker(30.0, 4.0, 128.0);
  SparseSpikeResult r = sparseSpikeInversion(trace.data(), kN, kDt, badDt, nullptr);
  QVERIFY(!r.ok);
  QVERIFY(!r.reason.empty());

  Wavelet empty;
  empty.sampleIntervalMs = kDt;
  r = sparseSpikeInversion(trace.data(), kN, kDt, empty, nullptr);
  QVERIFY(!r.ok);
  QVERIFY(!r.reason.empty());

  const std::vector<float> zero(std::size_t(kN), 0.0f);
  r = sparseSpikeInversion(zero.data(), kN, kDt, w, nullptr);
  QVERIFY(!r.ok);
  QVERIFY(!r.reason.empty());

  std::vector<float> hole(std::size_t(kN), 0.0f);
  for (int i = 0; i < kN * 4 / 10; ++i)
    hole[std::size_t(i * 2)] = std::numeric_limits<float>::quiet_NaN();
  r = sparseSpikeInversion(hole.data(), kN, kDt, w, nullptr);
  QVERIFY(!r.ok);
}

QTEST_MAIN(TestInversionSparse)
#include "tst_inversion_sparse.moc"
