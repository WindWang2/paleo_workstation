// 层：数据（测试壳位于 tests/，被测对象为数据层纯数值核）
#include <QtTest/QtTest>

#include "algorithms/petrophys.h"

#include <cmath>
#include <limits>

using namespace paleo::petrophys;

namespace
{
constexpr double kNan = std::numeric_limits<double>::quiet_NaN();

// 构造封闭形式已知解的 GR 斜坡：[0,10) 线性 20→80（IGR 线性升），[10,15)
// 恒 50（IGR=0.5 平台），[15,20) 越界 0 与 120（IGR 钳 0/1），末位 NaN。
struct GrFixture
{
  std::vector<double> gr;
  GrFixture() : gr(20)
  {
    for (int i = 0; i < 10; ++i)
      gr[i] = 20.0 + 6.0 * i; // 20,26,...,74 → IGR = i/9 …精确有理数
    for (int i = 10; i < 15; ++i)
      gr[i] = 50.0; // IGR = 0.5
    gr[15] = 0.0;   // IGR 钳 0
    gr[16] = 120.0; // IGR 钳 1
    gr[17] = 50.0;
    gr[18] = 50.0;
    gr[19] = kNan;
  }
};
} // namespace

// 容差口径（逐用例注释）：
//   · 有理封闭形式（除法/开方可精确表为双精度路径的）→ 1e-12：仅舍入差；
//   · 含超越函数（pow/sqrt/exp）对照 python 双精度手算常数 → 1e-9：
//     libm 实现差 + 常数誊写差，远小于任何真回归（真回归 O(1)）。
class TestPetroPhys : public QObject
{
  Q_OBJECT
private slots:
  // 线性 Vsh = 钳过的 IGR：斜坡段 Vsh=i/9（有理精确），平台 0.5，越界端
  // 钳 0/1，NaN 传播。基线 grMin=20/grMax=80，分母 60 → i*6/60 = i/10 …
  // 注意：IGR(i)=6i/60=i/10（i=0..9 → 0,0.1,…,0.9）。容差 1e-12（纯除法）。
  void testVshLinearAnalytic();

  // Larionov 年轻/老岩 @IGR=0.5 平台（10..14 号样本）：python 手算常数
  // 0.21621515358679566 / 0.33（后者 2^1−1 有理精确）。容差 1e-9。
  void testVshLarionovAnalytic();

  // Clavier @IGR=0.5 = 1.7−sqrt(1.94) = 0.3071611722815881（python）；
  // 端点 IGR=0 → 1.7−sqrt(3.38−0.49)=1.7−1.7=0 精确、IGR=1 → 1.0 精确
  // （3.38−1.7²=0.49，sqrt 恰 0.7——公式设计端点；仍用 1e-12 因 1.7²
  // 与 3.38−2.89 均二进制可表性一般，实测误差 <1e-15 落在门内）。
  void testVshClavierAnalytic();

  // 非线性变体单调性与端点：IGR 0→0；钳 1 端 LarionovY=0.9956711823610809
  // （python 2^3.7）、LarionovO=0.99（0.33·3 有理精确）。
  void testVshEndpointsAndClamp();

  // 井内极值基线：NaN 剔除后 min=0/max=120；全 NaN → false。
  void testGrExtrema();

  // φD=(2.65−ρb)/1.65：ρb=2.45 → 0.2/1.65 有理；ρb=ρma → 0；ρb=1.0 → 1。
  void testPhiDensityAnalytic();

  // NPHI % → v/v：20 → 0.2、0 → 0、负值（气效应）−5% → −0.05 如实保留。
  void testPhiNeutron();

  // Wyllie：Δtma=182/Δtf=620（µs/m，砂岩/淡水文献值）→ 分母 438；
  // Δt=401 → 219/438=0.5 有理精确；Cp=1.2 → 0.5/1.2=5/12。
  void testPhiSonicWyllie();

  // Archie：a=1/m=2/n=2/Rw=0.1、φ=0.25/Rt=5 → Sw=sqrt(0.1/0.3125)=
  // sqrt(0.32)=0.565685424949238（python）；φ≤0/φ>1/Rt≤0 → NaN。
  void testSwArchieAnalytic();

  // 各族 NaN 传播：输入端一位 NaN → 输出同位 NaN（不跨点扩散）。
  void testNaNPropagation();

  // 基线非法（grMax≤grMin）→ 全 NaN（不静默）；ρma≤ρf 同。
  void testInvalidParamsAllNan();

  // 重采样：src [1000,1002,1004]×[10,20,30] → ref 全网格内插精确中点；
  // 出界 NaN；端点 NaN 不跨接；非单调 src → false。
  void testResampleLinear();

  // QC 统计：{1,2,3,NaN,5} → n=5/valid=4/nullRate=0.2/mean=2.75/
  // stddev=1.707825127659933（python 样本标准差）。
  void testCurveStats();

  // 异常区间（DEN<1.5 或 >3.0 口径示例）：两段越界被 NaN 断开 → 2 段，
  // 深度/样本数/端点逐项断言；全好曲线 → 0 段。
  void testAnomalyIntervals();
};

void TestPetroPhys::testVshLinearAnalytic()
{
  const GrFixture f;
  std::vector<double> out(20);
  vshGrLinear(f.gr.data(), 20, 20.0, 80.0, out.data());
  for (int i = 0; i < 10; ++i)
  {
    const double want = i / 10.0; // 6i/60
    QVERIFY2(std::fabs(out[i] - want) < 1e-12,
             qPrintable(QString("ramp i=%1 got %2 want %3").arg(i).arg(out[i]).arg(want)));
  }
  for (int i = 10; i < 15; ++i)
    QVERIFY2(std::fabs(out[i] - 0.5) < 1e-12, "plateau IGR=0.5");
  QCOMPARE(out[15], 0.0);  // GR=0 钳下限
  QCOMPARE(out[16], 1.0);  // GR=120 钳上限
  QVERIFY(std::isnan(out[19])); // NaN 传播
}

void TestPetroPhys::testVshLarionovAnalytic()
{
  const GrFixture f;
  std::vector<double> out(20);
  vshGrLarionovYoung(f.gr.data(), 20, 20.0, 80.0, out.data());
  for (int i = 10; i < 15; ++i)
    QVERIFY2(std::fabs(out[i] - 0.21621515358679566) < 1e-9,
             qPrintable(QString("young plateau got %1").arg(out[i])));
  vshGrLarionovOld(f.gr.data(), 20, 20.0, 80.0, out.data());
  for (int i = 10; i < 15; ++i)
    QVERIFY2(std::fabs(out[i] - 0.33) < 1e-12,
             qPrintable(QString("old plateau got %1").arg(out[i]))); // 0.33·(2−1) 有理
}

void TestPetroPhys::testVshClavierAnalytic()
{
  const GrFixture f;
  std::vector<double> out(20);
  vshGrClavier(f.gr.data(), 20, 20.0, 80.0, out.data());
  QVERIFY2(std::fabs(out[10] - 0.3071611722815881) < 1e-9,
           qPrintable(QString("clavier mid got %1").arg(out[10])));
  QVERIFY2(std::fabs(out[15] - 0.0) < 1e-12, "clavier IGR=0 -> 0");
  QVERIFY2(std::fabs(out[16] - 1.0) < 1e-12, "clavier IGR=1 -> 1");
}

void TestPetroPhys::testVshEndpointsAndClamp()
{
  const GrFixture f;
  std::vector<double> out(20);
  vshGrLarionovYoung(f.gr.data(), 20, 20.0, 80.0, out.data());
  QVERIFY2(std::fabs(out[15] - 0.0) < 1e-12, "young IGR=0 -> 0");
  QVERIFY2(std::fabs(out[16] - 0.9956711823610809) < 1e-9,
           qPrintable(QString("young IGR=1 got %1").arg(out[16])));
  vshGrLarionovOld(f.gr.data(), 20, 20.0, 80.0, out.data());
  QVERIFY2(std::fabs(out[15] - 0.0) < 1e-12, "old IGR=0 -> 0");
  QVERIFY2(std::fabs(out[16] - 0.99) < 1e-12, "old IGR=1 -> 0.99");
  // 斜坡段单调不减（1..9 与 17→18 相等平台不算违）
  for (int i = 1; i < 10; ++i)
    QVERIFY2(out[i] >= out[i - 1] - 1e-15, "young monotone on ramp");
}

void TestPetroPhys::testGrExtrema()
{
  const GrFixture f;
  double lo = 0, hi = 0;
  QVERIFY(grExtrema(f.gr.data(), 20, &lo, &hi));
  QCOMPARE(lo, 0.0);
  QCOMPARE(hi, 120.0);
  const std::vector<double> allNan(4, kNan);
  QVERIFY(!grExtrema(allNan.data(), 4, &lo, &hi));
}

void TestPetroPhys::testPhiDensityAnalytic()
{
  const std::vector<double> rhob = {2.45, 2.65, 1.0, kNan};
  std::vector<double> out(4);
  phiDensity(rhob.data(), 4, 2.65, 1.0, out.data());
  QVERIFY2(std::fabs(out[0] - 0.2 / 1.65) < 1e-12, "phiD rational");
  QCOMPARE(out[1], 0.0);
  QVERIFY2(std::fabs(out[2] - 1.0) < 1e-12, "water-bearing");
  QVERIFY(std::isnan(out[3]));
}

void TestPetroPhys::testPhiNeutron()
{
  const std::vector<double> nphi = {20.0, 0.0, -5.0, kNan};
  std::vector<double> out(4);
  phiNeutron(nphi.data(), 4, true, out.data());
  QVERIFY2(std::fabs(out[0] - 0.2) < 1e-12, "pct->v/v");
  QCOMPARE(out[1], 0.0);
  QVERIFY2(std::fabs(out[2] + 0.05) < 1e-12, "gas-negative kept honest");
  QVERIFY(std::isnan(out[3]));
  phiNeutron(nphi.data(), 4, false, out.data());
  QCOMPARE(out[0], 20.0); // v/v 口径直读
}

void TestPetroPhys::testPhiSonicWyllie()
{
  const std::vector<double> dt = {401.0, 182.0, 620.0, kNan};
  std::vector<double> out(4);
  phiSonicWyllie(dt.data(), 4, 182.0, 620.0, 1.0, out.data());
  QVERIFY2(std::fabs(out[0] - 0.5) < 1e-12, "219/438 rational");
  QCOMPARE(out[1], 0.0);
  QVERIFY2(std::fabs(out[2] - 1.0) < 1e-12, "fluid point");
  QVERIFY(std::isnan(out[3]));
  phiSonicWyllie(dt.data(), 4, 182.0, 620.0, 1.2, out.data());
  QVERIFY2(std::fabs(out[0] - 0.5 / 1.2) < 1e-12, "Cp divide");
}

void TestPetroPhys::testSwArchieAnalytic()
{
  const std::vector<double> phi = {0.25, 0.25, 0.25, 0.0, 1.5, kNan, 0.25};
  const std::vector<double> rt = {5.0, 50.0, 0.5, 5.0, 5.0, 5.0, -1.0};
  std::vector<double> out(7);
  swArchie(phi.data(), rt.data(), 7, 1.0, 2.0, 2.0, 0.1, out.data());
  // Sw² = 0.1/(0.0625·5)=0.32 → 0.565685…（python sqrt(0.32)）
  QVERIFY2(std::fabs(out[0] - 0.565685424949238) < 1e-9,
           qPrintable(QString("sw got %1").arg(out[0])));
  QVERIFY2(std::fabs(out[1] - 0.565685424949238 / std::sqrt(10.0)) < 1e-9,
           "Rt×10 → Sw/√10");
  QVERIFY2(std::fabs(out[2] - 0.565685424949238 * std::sqrt(10.0)) < 1e-9,
           "Rt/10 → Sw·√10");
  QVERIFY(std::isnan(out[3])); // φ=0 域外
  QVERIFY(std::isnan(out[4])); // φ>1 域外
  QVERIFY(std::isnan(out[5])); // NaN 传播
  QVERIFY(std::isnan(out[6])); // Rt<0 域外
}

void TestPetroPhys::testNaNPropagation()
{
  const GrFixture f;
  std::vector<double> out(20);
  vshGrLinear(f.gr.data(), 20, 20.0, 80.0, out.data());
  QVERIFY(std::isnan(out[19]));
  vshGrClavier(f.gr.data(), 20, 20.0, 80.0, out.data());
  QVERIFY(std::isnan(out[19]));
  QVERIFY(std::isnan(out[18]) == false); // 不跨点扩散
  const std::vector<double> one = {kNan};
  std::vector<double> o1(1);
  phiDensity(one.data(), 1, 2.65, 1.0, o1.data());
  QVERIFY(std::isnan(o1[0]));
  phiSonicWyllie(one.data(), 1, 182.0, 620.0, 1.0, o1.data());
  QVERIFY(std::isnan(o1[0]));
  const std::vector<double> ok = {0.3};
  swArchie(ok.data(), one.data(), 1, 1.0, 2.0, 2.0, 0.1, o1.data());
  QVERIFY(std::isnan(o1[0]));
}

void TestPetroPhys::testInvalidParamsAllNan()
{
  const GrFixture f;
  std::vector<double> out(20);
  vshGrLinear(f.gr.data(), 20, 80.0, 20.0, out.data()); // grMax<grMin
  for (int i = 0; i < 20; ++i)
    QVERIFY2(std::isnan(out[i]), "illegal baseline -> all NaN, no silence");
  const std::vector<double> rhob = {2.45, 2.2};
  std::vector<double> o2(2);
  phiDensity(rhob.data(), 2, 1.0, 2.65, o2.data()); // ρma<ρf
  for (int i = 0; i < 2; ++i)
    QVERIFY(std::isnan(o2[i]));
}

void TestPetroPhys::testResampleLinear()
{
  const std::vector<double> srcD = {1000.0, 1002.0, 1004.0};
  const std::vector<double> srcV = {10.0, 20.0, 30.0};
  const std::vector<double> ref = {999.0, 1000.0, 1001.0, 1002.0, 1003.0,
                                   1004.0, 1005.0};
  std::vector<double> out;
  QVERIFY(resampleLinear(ref, srcD, srcV, out));
  QCOMPARE(out.size(), size_t(7));
  QVERIFY(std::isnan(out[0])); // 出界不外推
  QCOMPARE(out[1], 10.0);      // 恰在样本点
  QVERIFY2(std::fabs(out[2] - 15.0) < 1e-12, "midpoint rational");
  QCOMPARE(out[3], 20.0);
  QVERIFY2(std::fabs(out[4] - 25.0) < 1e-12, "midpoint");
  QCOMPARE(out[5], 30.0);
  QVERIFY(std::isnan(out[6])); // 出界

  // 端点 NaN 不跨接：1002 处 NaN → 1001/1003 两插值位 NaN
  const std::vector<double> srcV2 = {10.0, kNan, 30.0};
  QVERIFY(resampleLinear(ref, srcD, srcV2, out));
  QVERIFY(std::isnan(out[2]));
  QVERIFY(std::isnan(out[4]));
  QCOMPARE(out[1], 10.0); // 恰在样本点仍取值

  // 非单调源 → false
  const std::vector<double> badD = {1000.0, 1002.0, 1001.0};
  QVERIFY(!resampleLinear(ref, badD, srcV, out));
  // 空源 → true + 全 NaN（整源缺失）
  QVERIFY(resampleLinear(ref, {}, {}, out));
  for (double v : out)
    QVERIFY(std::isnan(v));
}

void TestPetroPhys::testCurveStats()
{
  const std::vector<double> x = {1.0, 2.0, 3.0, kNan, 5.0};
  const CurveStats s = curveStats(x.data(), 5);
  QCOMPARE(s.n, 5);
  QCOMPARE(s.valid, 4);
  QCOMPARE(s.nulls, 1);
  QVERIFY2(std::fabs(s.nullRate - 0.2) < 1e-12, "1/5 rational");
  QCOMPARE(s.min, 1.0);
  QCOMPARE(s.max, 5.0);
  QVERIFY2(std::fabs(s.mean - 2.75) < 1e-12, "mean rational");
  QVERIFY2(std::fabs(s.stddev - 1.707825127659933) < 1e-9,
           qPrintable(QString("stddev got %1").arg(s.stddev)));
  const CurveStats empty = curveStats(x.data(), 0);
  QVERIFY(std::isnan(empty.mean));
  const std::vector<double> allNan = {kNan, kNan};
  const CurveStats none = curveStats(allNan.data(), 2);
  QCOMPARE(none.valid, 0);
  QVERIFY(std::isnan(none.min));
  QVERIFY(std::isnan(none.stddev));
}

void TestPetroPhys::testAnomalyIntervals()
{
  // DEN 口径示例（<1.5 或 >3.0 越界）：两段越界被 NaN 与好值分隔
  const std::vector<double> depths = {1000, 1001, 1002, 1003, 1004, 1005,
                                      1006, 1007};
  const std::vector<double> den = {1.4, 1.2, kNan, 3.5, 2.0, 2.4, 3.1, 2.1};
  const auto iv = anomalyIntervals(depths.data(), den.data(), 8, 1.5, 3.0);
  QCOMPARE(static_cast<int>(iv.size()), 3);
  QCOMPARE(iv[0].fromIndex, 0);
  QCOMPARE(iv[0].toIndex, 1);
  QCOMPARE(iv[0].from, 1000.0);
  QCOMPARE(iv[0].to, 1001.0);
  QCOMPARE(iv[0].samples, 2);
  QCOMPARE(iv[1].fromIndex, 3); // NaN(1002) 断段
  QCOMPARE(iv[1].samples, 1);
  QCOMPARE(iv[2].fromIndex, 6);
  QCOMPARE(iv[2].toIndex, 6);
  QCOMPARE(iv[2].to, 1006.0);
  // 全好 → 0 段；全 NaN → 0 段
  const std::vector<double> good(8, 2.0);
  QCOMPARE(static_cast<int>(anomalyIntervals(depths.data(), good.data(), 8, 1.5, 3.0).size()), 0);
  const std::vector<double> nanAll(8, kNan);
  QCOMPARE(static_cast<int>(anomalyIntervals(depths.data(), nanAll.data(), 8, 1.5, 3.0).size()), 0);
}

QTEST_MAIN(TestPetroPhys)
#include "tst_petrophys.moc"
