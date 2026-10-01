// 层：数据（测试壳位于 tests/，被测对象为数据层纯数值核）
#include <QtTest/QtTest>

#include "algorithms/seismicattr.h"

#include <cmath>
#include <limits>
#include <numbers>
#include <random>
#include <vector>

using namespace paleo::seisattr;

namespace
{

constexpr float kNan = std::numeric_limits<float>::quiet_NaN();

// Ricker 子波（墨西哥帽）：频率 f0 主频、长度 n、峰位居中。
std::vector<float> ricker(int n, double f0Hz, double dtSec, int peakSample)
{
  std::vector<float> w(static_cast<std::size_t>(n), 0.0f);
  const double a2 = (std::numbers::pi * f0Hz * dtSec) * (std::numbers::pi * f0Hz * dtSec);
  for (int i = 0; i < n; ++i)
  {
    const double t = double(i - peakSample);
    w[std::size_t(i)] = float((1.0 - 2.0 * a2 * t * t) * std::exp(-a2 * t * t));
  }
  return w;
}

float wrappedDegDiff(float a, float b) // 折到 (-180,180] 的相位差
{
  float d = a - b;
  while (d > 180.0f)
    d -= 360.0f;
  while (d <= -180.0f)
    d += 360.0f;
  return d;
}

} // namespace

class TestSeismicAttr : public QObject
{
  Q_OBJECT

private slots:
  // ---- 瞬时族：Hilbert 核对拍 ---------------------------------------------
  // H{cos(ωt)} = sin(ωt)。实部达机器精度（~1e-8）；虚部带镜像端折点的
  // Hilbert 尾偏置（实测 ~5e-3@1000 样/30Hz，见 ledger 轮1）——有限道
  // Hilbert 固有边缘效应，工业实现同样接受。容差 1e-2=显示级（8-bit 色
  // 深 ~0.4%），护栏带 ±16 样；真回归（符号/频率错）给 O(1) 误差，远超门。
  void hilbertKernelCosine();
  // 解析包络解析值：|1 + 0.5·e^{i2π·30t}| = sqrt(1.25 + cos(2π·30t))
  //（两正弦之和的解析信号 = 两解析信号之和，Bedrosian 条件满足——20/50Hz
  // 谱不重叠）。容差 1e-2（镜像折点 Hilbert 尾偏置 ~2e-3 实测，同上）。
  void envelopeTwoToneAnalytic();

  // ---- 瞬时族：相位/频率/Q -------------------------------------------------
  // sin(30Hz)：瞬时频率=30Hz；相位步进=30·0.001·360=10.8°/样。
  void instantaneousPhaseFrequency();
  // x=e^{-αt}cos(2πf0t)（α=4/s, f0=30Hz）：Q=π·f0/α≈23.56。
  // 容差 10%：Bedrosian 近似（指数包络非窄带）+ 包络数值微分，原型精度。
  void instantaneousQDecay();

  // ---- 时窗振幅族：手算值 ---------------------------------------------------
  void windowedRmsHandComputed();
  void windowedNaNPropagation();

  // ---- 相干：方向性用例 -----------------------------------------------------
  // 同一波形 9 道 → S=1（任意波形，数学恒等）；边界带 NaN。
  void coherenceIdenticalTraces();
  // 沿层连续高相干 / 跨断层低相干 / 白噪低相干。
  void coherenceDirectionalFault();
  void coherenceRandomNoise();

  // ---- 甜点 / NaN 道 -------------------------------------------------------
  void sweetnessHandComputed();
  void allNanTrace();
};

void TestSeismicAttr::hilbertKernelCosine()
{
  const int n = 501;
  const double dt = 0.002;      // 2ms
  const double f0 = 25.0;       // 25Hz，Nyquist 250Hz 内
  std::vector<float> x(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i)
    x[std::size_t(i)] = float(std::cos(2.0 * std::numbers::pi * f0 * dt * i));

  std::vector<float> re(static_cast<std::size_t>(n)), im(static_cast<std::size_t>(n));
  analyticSignal(x.data(), n, re.data(), im.data());

  for (int i = 16; i < n - 16; ++i)
  {
    const double expectRe = std::cos(2.0 * std::numbers::pi * f0 * dt * i);
    const double expectIm = std::sin(2.0 * std::numbers::pi * f0 * dt * i);
    QVERIFY2(std::fabs(re[std::size_t(i)] - expectRe) < 1e-2,
             qPrintable(QStringLiteral("re@%1").arg(i)));
    QVERIFY2(std::fabs(im[std::size_t(i)] - expectIm) < 1e-2,
             qPrintable(QStringLiteral("im@%1").arg(i)));
  }
}

void TestSeismicAttr::envelopeTwoToneAnalytic()
{
  const int n = 1000;
  const double dt = 0.001; // 1ms
  std::vector<float> x(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i)
  {
    const double t = dt * i;
    x[std::size_t(i)] = float(std::cos(2.0 * std::numbers::pi * 20.0 * t) +
                              0.5 * std::cos(2.0 * std::numbers::pi * 50.0 * t));
  }

  const ComplexTraceResult r = complexTraceAnalysis(x.data(), n, dt * 1000.0);
  QCOMPARE(int(r.envelope.size()), n);
  // 护栏带 5%：折点 Hilbert 尾在拍频包络上实测 ~1.5e-2（近边缘），门 2e-2。
  for (int i = n / 20; i < n - n / 20; ++i)
  {
    const double t = dt * i;
    const double expect = std::sqrt(1.25 + std::cos(2.0 * std::numbers::pi * 30.0 * t));
    QVERIFY2(std::fabs(r.envelope[std::size_t(i)] - expect) < 2e-2,
             qPrintable(QStringLiteral("env@%1 got=%2 want=%3")
                          .arg(i)
                          .arg(r.envelope[std::size_t(i)])
                          .arg(expect)));
  }
}

void TestSeismicAttr::instantaneousPhaseFrequency()
{
  const int n = 1000;
  const double dt = 0.001;
  const double f0 = 30.0;
  std::vector<float> x(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i)
    x[std::size_t(i)] = float(std::sin(2.0 * std::numbers::pi * f0 * dt * i));

  const ComplexTraceResult r = complexTraceAnalysis(x.data(), n, dt * 1000.0);

  // 瞬时频率：纯单频正弦 → 恒 f0。镜像折点（迹缘 C1 不连续，sin 迹缘
  // 斜率最大处最强）的 Hilbert 尾经相位差分放大，实测按 ~1/d 衰减：
  // 0.035@d=100、0.019@d=200（n=1000/30Hz）——整道 FFT Hilbert 的公认
  // 边缘行为，工业实现同级。护栏 20% 内实测最坏 0.565Hz，门 0.8Hz。
  for (int i = n / 5; i < n - n / 5; ++i)
    QVERIFY2(std::fabs(r.freqHz[std::size_t(i)] - f0) < 0.8,
             qPrintable(QStringLiteral("f@%1=%2").arg(i).arg(r.freqHz[std::size_t(i)])));

  // 瞬时相位步进：10.8°/样。护栏 20%（同上折点尾），门 1.5°（实测 <1°）。
  for (int i = n / 5; i < n - n / 5; ++i)
  {
    const float d = wrappedDegDiff(r.phaseDeg[std::size_t(i)],
                                   r.phaseDeg[std::size_t(i - 1)]);
    QVERIFY2(std::fabs(d - 10.8f) < 1.5f,
             qPrintable(QStringLiteral("dphase@%1=%2").arg(i).arg(d)));
  }

  // 包络=1（纯单位正弦）。护栏 20% + 门 2.5e-2（折点尾 ~1/d：护栏内实测
  // 最坏 1.88e-2@200）。
  for (int i = n / 5; i < n - n / 5; ++i)
    QVERIFY2(std::fabs(r.envelope[std::size_t(i)] - 1.0) < 2.5e-2,
             qPrintable(QStringLiteral("env@%1=%2").arg(i).arg(r.envelope[std::size_t(i)])));
}

void TestSeismicAttr::instantaneousQDecay()
{
  const int n = 1000;
  const double dt = 0.001;
  const double f0 = 30.0;
  const double alpha = 4.0;
  std::vector<float> x(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i)
    x[std::size_t(i)] =
        float(std::exp(-alpha * dt * i) * std::cos(2.0 * std::numbers::pi * f0 * dt * i));

  const ComplexTraceResult r = complexTraceAnalysis(x.data(), n, dt * 1000.0);
  const double expectQ = std::numbers::pi * f0 / alpha; // ≈ 23.56
  int checked = 0;
  // 内段 [200,500]：实测 i≤500 偏差 ≤5%；i≥600 尾端镜像延拓抬升 >10%（见
  // ledger 轮1），首端有启动瞬态。容差 10% 覆盖 Bedrosian 近似（指数包络
  // 非窄带）+ 包络中心差分。
  for (int i = 200; i <= 500; ++i)
  {
    const float q = r.quality[std::size_t(i)];
    QVERIFY2(!std::isnan(q), qPrintable(QStringLiteral("q@%1 NaN").arg(i)));
    QVERIFY2(std::fabs(q - expectQ) < 0.1 * expectQ,
             qPrintable(QStringLiteral("q@%1=%2 want≈%3")
                          .arg(i)
                          .arg(q)
                          .arg(expectQ)));
    ++checked;
  }
  QVERIFY(checked > 250);
}

void TestSeismicAttr::windowedRmsHandComputed()
{
  // x = {0,3,4,0}，half=1：闭窗 [i-1,i+1] 交 [0,3]。
  const std::vector<float> x = {0.0f, 3.0f, 4.0f, 0.0f};
  const int n = int(x.size());
  std::vector<float> rms(static_cast<std::size_t>(n)), mx(static_cast<std::size_t>(n)), en(static_cast<std::size_t>(n));
  windowedRms(x.data(), n, 1, rms.data());
  windowedMaxAbs(x.data(), n, 1, mx.data());
  windowedMeanEnergy(x.data(), n, 1, en.data());

  const double sqrt45 = std::sqrt(4.5);
  const double sqrt25_3 = std::sqrt(25.0 / 3.0);
  const double sqrt8 = std::sqrt(8.0);
  // RMS：窗{0,1}、{0,1,2}、{1,2,3}、{2,3}（边缘缩窗）
  QVERIFY(std::fabs(rms[0] - sqrt45) < 1e-5);
  QVERIFY(std::fabs(rms[1] - sqrt25_3) < 1e-5);
  QVERIFY(std::fabs(rms[2] - sqrt25_3) < 1e-5);
  QVERIFY(std::fabs(rms[3] - sqrt8) < 1e-5);
  // 最大绝对振幅
  QCOMPARE(mx[0], 3.0f);
  QCOMPARE(mx[1], 4.0f);
  QCOMPARE(mx[2], 4.0f);
  QCOMPARE(mx[3], 4.0f);
  // 平均能量
  QVERIFY(std::fabs(en[0] - 4.5) < 1e-5);
  QVERIFY(std::fabs(en[1] - 25.0 / 3.0) < 1e-5);
  QVERIFY(std::fabs(en[2] - 25.0 / 3.0) < 1e-5);
  QVERIFY(std::fabs(en[3] - 8.0) < 1e-5);

  // half=0 退化：RMS=|x|、Max=|x|、能量=x²。
  std::vector<float> r0(static_cast<std::size_t>(n)), m0(static_cast<std::size_t>(n)), e0(static_cast<std::size_t>(n));
  windowedRms(x.data(), n, 0, r0.data());
  windowedMaxAbs(x.data(), n, 0, m0.data());
  windowedMeanEnergy(x.data(), n, 0, e0.data());
  QCOMPARE(r0[1], 3.0f);
  QCOMPARE(m0[2], 4.0f);
  QVERIFY(std::fabs(e0[2] - 16.0) < 1e-5);
}

void TestSeismicAttr::windowedNaNPropagation()
{
  const std::vector<float> x = {1.0f, kNan, 2.0f, 2.0f};
  const int n = int(x.size());
  std::vector<float> rms(static_cast<std::size_t>(n)), mx(static_cast<std::size_t>(n)), en(static_cast<std::size_t>(n));
  windowedRms(x.data(), n, 1, rms.data());
  windowedMaxAbs(x.data(), n, 1, mx.data());
  windowedMeanEnergy(x.data(), n, 1, en.data());
  // 窗含 NaN → NaN；窗 {2,3}（i=3 的 {1,2,3}? i=3 窗={2,3} 不含 NaN）→ 有限。
  QVERIFY(std::isnan(rms[0]) && std::isnan(rms[1]) && std::isnan(rms[2]));
  QVERIFY(std::isnan(mx[0]) && std::isnan(mx[1]) && std::isnan(mx[2]));
  QVERIFY(std::isnan(en[0]) && std::isnan(en[1]) && std::isnan(en[2]));
  QVERIFY(std::fabs(rms[3] - 2.0) < 1e-5);
  QVERIFY(std::fabs(mx[3] - 2.0) < 1e-5);
  QVERIFY(std::fabs(en[3] - 4.0) < 1e-5);
}

void TestSeismicAttr::coherenceIdenticalTraces()
{
  const int nIl = 5, nXl = 5, nS = 64;
  const auto w = ricker(nS, 40.0, 0.002, 32);
  std::vector<float> vol(std::size_t(nIl * nXl * nS));
  for (int p = 0; p < nIl * nXl; ++p)
    for (int s = 0; s < nS; ++s)
      vol[std::size_t(p * nS + s)] = w[std::size_t(s)];

  std::vector<float> out(std::size_t(vol.size()));
  semblanceCoherence(vol.data(), nIl, nXl, nS, 1, 1, 2, out.data());

  const auto at = [&](int il, int xl, int s)
  { return out[std::size_t((il * nXl + xl) * nS + s)]; };

  // 内部：J 道同波形 → S=1（数学恒等，容差 1e-4 覆盖 double→float）。
  for (int il = 1; il < nIl - 1; ++il)
    for (int xl = 1; xl < nXl - 1; ++xl)
      for (int s = 2; s < nS - 2; ++s)
        QVERIFY2(at(il, xl, s) > 0.9999f,
                 qPrintable(QStringLiteral("S@%1,%2,%3=%4")
                              .arg(il)
                              .arg(xl)
                              .arg(s)
                              .arg(at(il, xl, s))));

  // 边界带：空间窗越界 → NaN。
  QVERIFY(std::isnan(at(0, 2, 32)));
  QVERIFY(std::isnan(at(2, 4, 32)));
}

void TestSeismicAttr::coherenceDirectionalFault()
{
  // 3 IL × 12 XL × 128 S：xl<6 波形峰位 64，xl≥6 峰位 72（断层错动 8 样=16ms）。
  const int nIl = 3, nXl = 12, nS = 128;
  const int faultXl = 6;
  const int shift = 8;
  std::vector<float> vol(std::size_t(nIl * nXl * nS), 0.0f);
  for (int il = 0; il < nIl; ++il)
    for (int xl = 0; xl < nXl; ++xl)
    {
      const int peak = xl < faultXl ? 64 : 64 + shift;
      const auto w = ricker(nS, 40.0, 0.002, peak);
      for (int s = 0; s < nS; ++s)
        vol[std::size_t((il * nXl + xl) * nS + s)] = w[std::size_t(s)];
    }

  std::vector<float> out(std::size_t(vol.size()));
  semblanceCoherence(vol.data(), nIl, nXl, nS, 1, 1, 2, out.data());

  const auto at = [&](int il, int xl, int s)
  { return out[std::size_t((il * nXl + xl) * nS + s)]; };

  // 沿层连续（同断块内、窗不跨断层）：高相干。
  for (int xl : {2, 3, 9, 10})
    QVERIFY2(at(1, xl, 64) > 0.99f,
             qPrintable(QStringLiteral("S@xl=%1 = %2 (want >0.99)")
                          .arg(xl)
                          .arg(at(1, xl, 64))));

  // 跨断层（窗 4..6 / 5..7 混两侧波形）：低相干。
  for (int xl : {5, 6})
    QVERIFY2(at(1, xl, 64) < 0.85f,
             qPrintable(QStringLiteral("S@xl=%1 = %2 (want <0.85)")
                          .arg(xl)
                          .arg(at(1, xl, 64))));

  // 方向性第二面：同几何无错动（shift=0）→ 全内部高相干（断层响应来自
  // 波形错动本身，非窗几何）。
  std::vector<float> vol0(std::size_t(vol.size()), 0.0f);
  for (int p = 0; p < nIl * nXl; ++p)
    for (int s = 0; s < nS; ++s)
      vol0[std::size_t(p * nS + s)] = vol[std::size_t(p * nS + s)] * 0.0f +
                                      vol[std::size_t(((p / nXl) * nXl + 0) * nS + s)];
  // xl=0 是左块波形（峰 64）：全体取它 = 无断层参照体。
  std::vector<float> out0(std::size_t(vol0.size()));
  semblanceCoherence(vol0.data(), nIl, nXl, nS, 1, 1, 2, out0.data());
  const auto at0 = [&](int il, int xl, int s)
  { return out0[std::size_t((il * nXl + xl) * nS + s)]; };
  for (int xl = 1; xl < nXl - 1; ++xl)
    QVERIFY2(at0(1, xl, 64) > 0.99f,
             qPrintable(QStringLiteral("S0@xl=%1 = %2 (want >0.99)")
                          .arg(xl)
                          .arg(at0(1, xl, 64))));
}

void TestSeismicAttr::coherenceRandomNoise()
{
  const int nIl = 3, nXl = 6, nS = 64;
  std::mt19937 rng(42);
  std::normal_distribution<float> dist(0.0f, 1.0f);
  std::vector<float> vol(std::size_t(nIl * nXl * nS));
  for (auto &v : vol)
    v = dist(rng);

  std::vector<float> out(std::size_t(vol.size()));
  semblanceCoherence(vol.data(), nIl, nXl, nS, 1, 1, 2, out.data());

  // 白噪 9 道 × T=5 时样：期望 S≈1/T=0.2 量级（叠加无增益）。门 <0.5。
  const auto at = [&](int il, int xl, int s)
  { return out[std::size_t((il * nXl + xl) * nS + s)]; };
  for (int xl = 1; xl < nXl - 1; ++xl)
    for (int s = 2; s < nS - 2; ++s)
      QVERIFY2(at(1, xl, s) < 0.5f,
               qPrintable(QStringLiteral("S@xl=%1,s=%2=%3")
                            .arg(xl)
                            .arg(s)
                            .arg(at(1, xl, s))));
}

void TestSeismicAttr::sweetnessHandComputed()
{
  const std::vector<float> env = {4.0f, 1.0f, 2.0f, kNan};
  const std::vector<float> freq = {16.0f, 4.0f, -3.0f, 10.0f};
  const int n = int(env.size());
  std::vector<float> s(static_cast<std::size_t>(n));
  sweetness(env.data(), freq.data(), n, s.data());
  QVERIFY(std::fabs(s[0] - 1.0) < 1e-5);            // 4/sqrt(16)
  QVERIFY(std::fabs(s[1] - 0.5) < 1e-5);            // 1/sqrt(4)
  // 负频率抬到 fMin=1e-3：2/sqrt(1e-3)=63.245…
  QVERIFY(std::fabs(s[2] - 2.0 / std::sqrt(1e-3)) < 1e-2);
  QVERIFY(std::isnan(s[3]));
}

void TestSeismicAttr::allNanTrace()
{
  const int n = 32;
  std::vector<float> x(static_cast<std::size_t>(n), kNan);
  const ComplexTraceResult r = complexTraceAnalysis(x.data(), n, 2.0);
  for (int i = 0; i < n; ++i)
  {
    QVERIFY(std::isnan(r.envelope[std::size_t(i)]));
    QVERIFY(std::isnan(r.phaseDeg[std::size_t(i)]));
    QVERIFY(std::isnan(r.freqHz[std::size_t(i)]));
    QVERIFY(std::isnan(r.quality[std::size_t(i)]));
  }

  // 单样道：包络=|x|，相位按符号，频率/Q 无定义。
  const float one[1] = {-2.0f};
  const ComplexTraceResult r1 = complexTraceAnalysis(one, 1, 2.0);
  QCOMPARE(r1.envelope.size(), std::size_t(1));
  QVERIFY(std::fabs(r1.envelope[0] - 2.0) < 1e-6);
  QVERIFY(std::fabs(r1.phaseDeg[0] - 180.0) < 1e-6);
  QVERIFY(std::isnan(r1.freqHz[0]));
}

QTEST_MAIN(TestSeismicAttr)
#include "tst_seismicattr.moc"
