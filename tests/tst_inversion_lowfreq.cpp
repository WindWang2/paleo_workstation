// 层：数据（测试壳位于 tests/，被测对象为数据层纯数值核）
#include <QtTest/QtTest>

#include "algorithms/inversion/lowfreq.h"

#include <cmath>
#include <limits>
#include <vector>

using namespace paleo::inversion;

namespace
{

constexpr float kNan = std::numeric_limits<float>::quiet_NaN();

// 5×5 网格、格距 200m、正交测网。t0=0ms dt=2ms nS=1251（0–2500ms）。
LowFreqModelInput baseGrid()
{
  LowFreqModelInput in;
  in.t0Ms = 0.0;
  in.dtMs = 2.0;
  in.nIl = 5;
  in.nXl = 5;
  in.nS = 1251;
  in.originX = 0.0;
  in.originY = 0.0;
  in.ilStepX = 0.0;
  in.ilStepY = -200.0; // inline 增 → y 减（北向上行）
  in.xlStepX = 200.0;
  in.xlStepY = 0.0;
  in.lowCutHz = 8.0;
  return in;
}

// 平层位：全体格同一 TWT。
HorizonTwtGrid flatHorizon(const QString &name, float twtMs, int nIl, int nXl)
{
  HorizonTwtGrid h;
  h.name = name.toStdString();
  h.twtMs.assign(std::size_t(nIl * nXl), twtMs);
  return h;
}

// 三层阶梯阻抗曲线（0–1000ms: zTop, 1000–2000ms: zMid, 2000–2500: zBot）。
std::vector<float> steppedImpedance(int nS, double dtMs, float zTop, float zMid, float zBot)
{
  std::vector<float> z(std::size_t(nS), 0.0f);
  for (int i = 0; i < nS; ++i)
  {
    const double t = double(i) * dtMs;
    z[std::size_t(i)] = t <= 1000.0 ? zTop : (t <= 2000.0 ? zMid : zBot);
  }
  return z;
}

// 单独复刻平滑核（有限值均值、边缘缩窗），用于井位断言的期望值。
std::vector<float> smoothReference(const std::vector<float> &x, int half)
{
  std::vector<float> out(x.size(), kNan);
  for (int i = 0; i < int(x.size()); ++i)
  {
    const int lo = std::max(0, i - half);
    const int hi = std::min(int(x.size()) - 1, i + half);
    double sum = 0.0;
    int cnt = 0;
    for (int k = lo; k <= hi; ++k)
    {
      if (std::isfinite(x[std::size_t(k)]))
      {
        sum += double(x[std::size_t(k)]);
        ++cnt;
      }
    }
    out[std::size_t(i)] = cnt > 0 ? float(sum / double(cnt)) : kNan;
  }
  return out;
}

} // namespace

class TestInversionLowFreq : public QObject
{
  Q_OBJECT

private slots:
  // Oracle 2a：层模式，井位处低频场 = 井阻抗低频（同平滑核逐样相等；
  // 井曲线本身分段常数与层一致，模型在井位精确重现）。
  void layeredAtWellMatchesWellLowFreq();
  // Oracle 2b：两井等距中点 = 均值（IDW p=2 对称）。
  void lateralInterpolationMidpoint();
  // Oracle 2c：无层位回退全局趋势——单井趋势曲线，任意格 = 低通井曲线。
  void noHorizonGlobalTrend();
  // Oracle 2d：频段标注——lowCutHz/smoothingWindowMs/layersUsed 进结果
  //（extra 记录在编排层，见 tst_inversion_workflow）。
  void bandAndLayerCountRecorded();
  // 倾斜层位：边界跟随该格层位 TWT（沿层内插），过渡带中心 ± 半窗。
  void tiltedHorizonFollowsStructure();
  // 井某层全缺且无他井 → 该层 NaN（诚实缺失）；其余层有限。
  void missingLayerPropagatesNaN();
  // 退化输入：无井 / 曲线长度不符 / dt<=0 → 显式失败。
  void invalidInputsFail();
};

void TestInversionLowFreq::layeredAtWellMatchesWellLowFreq()
{
  LowFreqModelInput in = baseGrid();
  // 一口井放在格 (2,2) 中心。
  LowFreqWellTrace w;
  w.x = in.originX + 2 * in.xlStepX; // 400
  w.y = in.originY + 2 * in.ilStepY; // -400
  w.impedance = steppedImpedance(in.nS, in.dtMs, 6000.0f, 6500.0f, 7000.0f);
  in.wells.push_back(w);
  in.horizons.push_back(flatHorizon(QStringLiteral("H1"), 1000.0f, in.nIl, in.nXl));
  in.horizons.push_back(flatHorizon(QStringLiteral("H2"), 2000.0f, in.nIl, in.nXl));

  const LowFreqModelResult r = lowFreqImpedance(in);
  QVERIFY2(r.ok, r.reason.c_str());
  QCOMPARE(r.layersUsed, 3);

  std::vector<float> trace(std::size_t(in.nS));
  lowFreqTraceAt(r, 2, 2, trace.data());
  // 井位期望 = 井曲线过同一平滑核（模型与井同构）。
  const int half = std::max(1, int(std::lround(1000.0 / in.lowCutHz / in.dtMs * 0.5)));
  const std::vector<float> expect = smoothReference(w.impedance, half);
  for (int i = 0; i < in.nS; ++i)
  {
    QVERIFY2(std::fabs(double(trace[std::size_t(i)]) - double(expect[std::size_t(i)])) < 1e-3,
             qPrintable(QString("样 %1: %2 vs %3")
                          .arg(i)
                          .arg(trace[std::size_t(i)])
                          .arg(expect[std::size_t(i)])));
  }
}

void TestInversionLowFreq::lateralInterpolationMidpoint()
{
  LowFreqModelInput in = baseGrid();
  LowFreqWellTrace w1;
  w1.x = 0.0;  // 格 (0,0)
  w1.y = 0.0;
  w1.impedance = steppedImpedance(in.nS, in.dtMs, 6000.0f, 6000.0f, 6000.0f);
  LowFreqWellTrace w2;
  w2.x = 800.0; // 格 (0,4)
  w2.y = 0.0;
  w2.impedance = steppedImpedance(in.nS, in.dtMs, 8000.0f, 8000.0f, 8000.0f);
  in.wells = {w1, w2};
  in.horizons.push_back(flatHorizon(QStringLiteral("H1"), 1000.0f, in.nIl, in.nXl));

  const LowFreqModelResult r = lowFreqImpedance(in);
  QVERIFY2(r.ok, r.reason.c_str());

  std::vector<float> mid(std::size_t(in.nS));
  lowFreqTraceAt(r, 0, 2, mid.data()); // (0,2) 距两井各 400m → 等权均值
  const double expectMid = (6000.0 + 8000.0) * 0.5;
  for (int i : {100, 1250, 600}) // 层内避开过渡带与首尾边缘
  {
    if (i >= in.nS)
      continue;
    QVERIFY2(std::fabs(double(mid[std::size_t(i)]) - expectMid) < 1.0,
             qPrintable(QString("样 %1: %2").arg(i).arg(mid[std::size_t(i)])));
  }
  // 单调性：靠 w1 的格更接近 6000。
  std::vector<float> nearW1(std::size_t(in.nS));
  lowFreqTraceAt(r, 0, 1, nearW1.data());
  QVERIFY(double(nearW1[std::size_t(600)]) < double(mid[std::size_t(600)]));
  QVERIFY(double(nearW1[std::size_t(600)]) > 6000.0);
}

void TestInversionLowFreq::noHorizonGlobalTrend()
{
  LowFreqModelInput in = baseGrid();
  LowFreqWellTrace w;
  w.x = 0.0;
  w.y = 0.0;
  w.impedance.resize(std::size_t(in.nS));
  for (int i = 0; i < in.nS; ++i)
    w.impedance[std::size_t(i)] = float(5000.0 + 1.0 * double(i) * in.dtMs); // 线性趋势
  in.wells.push_back(w);

  const LowFreqModelResult r = lowFreqImpedance(in);
  QVERIFY2(r.ok, r.reason.c_str());
  QCOMPARE(r.layersUsed, 0); // 全局趋势回退

  const int half = std::max(1, int(std::lround(1000.0 / in.lowCutHz / in.dtMs * 0.5)));
  const std::vector<float> expect = smoothReference(w.impedance, half);
  for (int cell = 0; cell < 25; ++cell)
  {
    std::vector<float> trace(std::size_t(in.nS));
    lowFreqTraceAt(r, cell / 5, cell % 5, trace.data());
    for (int i = 0; i < in.nS; ++i)
      QVERIFY(std::fabs(double(trace[std::size_t(i)]) - double(expect[std::size_t(i)])) < 1e-3);
  }
}

void TestInversionLowFreq::bandAndLayerCountRecorded()
{
  LowFreqModelInput in = baseGrid();
  LowFreqWellTrace w;
  w.x = 0.0;
  w.y = 0.0;
  w.impedance = steppedImpedance(in.nS, in.dtMs, 6000.0f, 6500.0f, 7000.0f);
  in.wells.push_back(w);
  in.lowCutHz = 10.0;

  LowFreqModelResult r = lowFreqImpedance(in);
  QVERIFY2(r.ok, r.reason.c_str());
  QCOMPARE(r.lowCutHz, 10.0);
  QCOMPARE(r.layersUsed, 0);
  const int half = std::max(1, int(std::lround(1000.0 / 10.0 / in.dtMs * 0.5)));
  QCOMPARE(r.smoothingWindowMs, double(2 * half) * in.dtMs);

  in.horizons.push_back(flatHorizon(QStringLiteral("H"), 1200.0f, in.nIl, in.nXl));
  r = lowFreqImpedance(in);
  QVERIFY2(r.ok, r.reason.c_str());
  QCOMPARE(r.layersUsed, 2);
  QCOMPARE(int(r.boundaries.size()), 1);
  QCOMPARE(int(r.boundaryMeanTwt.size()), 1);
  QVERIFY(std::fabs(r.boundaryMeanTwt[0] - 1200.0) < 1e-6);
}

void TestInversionLowFreq::tiltedHorizonFollowsStructure()
{
  LowFreqModelInput in = baseGrid();
  // 层位 TWT 随 inline 线性加深：H = 1000 + 60·il ms。
  HorizonTwtGrid h;
  h.name = "tilt";
  h.twtMs.assign(std::size_t(in.nIl * in.nXl), 0.0f);
  for (int il = 0; il < in.nIl; ++il)
    for (int xl = 0; xl < in.nXl; ++xl)
      h.twtMs[std::size_t(il * in.nXl + xl)] = float(1000.0 + 60.0 * il);
  in.horizons.push_back(h);

  LowFreqWellTrace w;
  w.x = in.originX + 0 * in.xlStepX;
  w.y = in.originY + 0 * in.ilStepY; // 格 (0,0)
  w.impedance.resize(std::size_t(in.nS));
  for (int i = 0; i < in.nS; ++i)
    w.impedance[std::size_t(i)] = double(i) * in.dtMs <= 1000.0 ? 6000.0f : 7500.0f;
  in.wells.push_back(w);

  const LowFreqModelResult r = lowFreqImpedance(in);
  QVERIFY2(r.ok, r.reason.c_str());

  // 格 (4,0)：层位局部 TWT = 1240ms；过渡带中点应在 1240 ± 半窗。
  std::vector<float> trace(std::size_t(in.nS));
  lowFreqTraceAt(r, 4, 0, trace.data());
  const int half = std::max(1, int(std::lround(1000.0 / in.lowCutHz / in.dtMs * 0.5)));
  int cross = -1;
  for (int i = 1; i < in.nS; ++i)
  {
    const double prev = double(trace[std::size_t(i - 1)]);
    const double cur = double(trace[std::size_t(i)]);
    if (prev < 6750.0 && cur >= 6750.0)
    {
      cross = i;
      break;
    }
  }
  QVERIFY(cross > 0);
  const double crossMs = double(cross) * in.dtMs;
  QVERIFY2(std::fabs(crossMs - 1240.0) <= double(half) * in.dtMs + in.dtMs,
           qPrintable(QString("过渡点 %1ms，期望 1240±%2ms")
                        .arg(crossMs)
                        .arg(double(half) * in.dtMs)));
  // 远端层下阻抗仍收敛到 7500。
  QVERIFY(std::fabs(double(trace[std::size_t(in.nS - 1)]) - 7500.0) < 1.0);
}

void TestInversionLowFreq::missingLayerPropagatesNaN()
{
  LowFreqModelInput in = baseGrid();
  LowFreqWellTrace w;
  w.x = 0.0;
  w.y = 0.0;
  w.impedance = steppedImpedance(in.nS, in.dtMs, 6000.0f, kNan, 7000.0f); // 中层缺失
  in.wells.push_back(w);
  in.horizons.push_back(flatHorizon(QStringLiteral("H1"), 1000.0f, in.nIl, in.nXl));
  in.horizons.push_back(flatHorizon(QStringLiteral("H2"), 2000.0f, in.nIl, in.nXl));

  const LowFreqModelResult r = lowFreqImpedance(in);
  QVERIFY2(r.ok, r.reason.c_str());
  std::vector<float> trace(std::size_t(in.nS));
  lowFreqTraceAt(r, 3, 3, trace.data());
  // 中层（1000–2000ms，让开半窗过渡）应 NaN；上下层有限。
  QVERIFY(std::isnan(trace[std::size_t(1100 / 2)]));
  QVERIFY(std::isnan(trace[std::size_t(1500 / 2)]));
  QVERIFY(std::isfinite(trace[std::size_t(400 / 2)]));
  QVERIFY(std::isfinite(trace[std::size_t(2300 / 2)]));
}

void TestInversionLowFreq::invalidInputsFail()
{
  LowFreqModelInput in = baseGrid();
  LowFreqWellTrace w;
  w.x = 0.0;
  w.y = 0.0;
  w.impedance = steppedImpedance(in.nS, in.dtMs, 6000.0f, 6500.0f, 7000.0f);

  LowFreqModelResult r = lowFreqImpedance(in);
  QVERIFY(!r.ok);
  QVERIFY(!r.reason.empty());

  in.wells.push_back(w);
  in.dtMs = 0.0;
  r = lowFreqImpedance(in);
  QVERIFY(!r.ok);

  in.dtMs = 2.0;
  in.wells[0].impedance.pop_back();
  r = lowFreqImpedance(in);
  QVERIFY(!r.ok);
  QVERIFY(!r.reason.empty());

  in.wells[0].impedance.push_back(6500.0f);
  HorizonTwtGrid bad = flatHorizon(QStringLiteral("bad"), 1000.0f, in.nIl, in.nXl);
  bad.twtMs.pop_back();
  in.horizons.push_back(bad);
  r = lowFreqImpedance(in);
  QVERIFY(!r.ok);
  QVERIFY(!r.reason.empty());
}

QTEST_MAIN(TestInversionLowFreq)
#include "tst_inversion_lowfreq.moc"
