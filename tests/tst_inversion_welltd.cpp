// 层：功能（测试壳位于 tests/，被测对象为 header-only 纯数值助手）
#include <QtTest/QtTest>

#include "workflow/wellimpedancetwt.h"

#include <cmath>
#include <vector>

using namespace paleo::inv;

namespace
{
// 逐井时深：常速 v + 时间平移 shift 生成曲线自带的 TWT 轴（与 SectionWorkbench 同口径）。
std::vector<double> twtFor(const std::vector<double> &depths, double v, double shiftMs)
{
  std::vector<double> t;
  for (double d : depths)
    t.push_back(d * 2000.0 / v + shiftMs);
  return t;
}
} // namespace

class TestInversionWellTd : public QObject
{
  Q_OBJECT

private slots:
  // #125：两口井时深不同、各带平移 → 阻抗散样的 TWT 与各自井叠加一致
  //（1000 m、4000 m/s、+40 ms → 540 ms；旧实现用全局 2500 m/s 给出 800 ms）。
  void perWellTimeDepthPreserved();
  // 缺时深（曲线 TWT 全 NaN）→ 不参与，绝不回退默认速度。
  void missingTimeDepthRejected();
  // 部分未对齐：只在有效时深覆盖范围内取样，不外推；非单调样丢弃。
  void partialCoverageNoExtrapolation();
};

void TestInversionWellTd::perWellTimeDepthPreserved()
{
  std::vector<double> depths;
  for (double d = 500.0; d <= 1500.0; d += 10.0)
    depths.push_back(d);
  const std::vector<float> ac(depths.size(), 250.0f);  // µs/m
  const std::vector<float> den(depths.size(), 2.4f);

  struct Case { double v; double shift; } cases[] = {{4000.0, 40.0}, {2000.0, -15.0}};
  for (const Case &c : cases)
  {
    const std::vector<double> twt = twtFor(depths, c.v, c.shift);
    std::vector<std::pair<double, double>> pairs;
    appendTimeDepthPairs(depths, twt, &pairs);
    std::vector<double> tdD, tdT;
    QVERIFY(buildTimeDepthTable(pairs, &tdD, &tdT));
    std::vector<double> outT;
    std::vector<float> outZ;
    QVERIFY(wellImpedanceTwt(depths, ac, depths, den, tdD, tdT, &outT, &outZ));
    QCOMPARE(outT.size(), depths.size());
    for (std::size_t i = 0; i < depths.size(); ++i)
    {
      const double expect = depths[i] * 2000.0 / c.v + c.shift;
      QVERIFY2(std::fabs(outT[i] - expect) < 1e-9,
               qPrintable(QString("d=%1 t=%2 期望 %3").arg(depths[i]).arg(outT[i]).arg(expect)));
      QVERIFY(std::fabs(double(outZ[i]) - 2.4 * 1e6 / 250.0) < 1e-3);
    }
    if (c.v == 4000.0)
      QVERIFY(std::fabs(interpTwtNoExtrapolate(tdD, tdT, 1000.0) - 540.0) < 1e-9);
  }
}

void TestInversionWellTd::missingTimeDepthRejected()
{
  const std::vector<double> depths = {500.0, 600.0, 700.0};
  const std::vector<double> nanTwt(3, std::numeric_limits<double>::quiet_NaN());
  std::vector<std::pair<double, double>> pairs;
  appendTimeDepthPairs(depths, nanTwt, &pairs);
  std::vector<double> tdD, tdT;
  QVERIFY(!buildTimeDepthTable(pairs, &tdD, &tdT));
  std::vector<double> outT;
  std::vector<float> outZ;
  const std::vector<float> ac(3, 250.0f), den(3, 2.4f);
  QVERIFY(!wellImpedanceTwt(depths, ac, depths, den, tdD, tdT, &outT, &outZ));
  QVERIFY(outT.empty());
}

void TestInversionWellTd::partialCoverageNoExtrapolation()
{
  const std::vector<double> depths = {100.0, 200.0, 300.0, 400.0, 500.0};
  const double nan = std::numeric_limits<double>::quiet_NaN();
  // 前两样未对齐、第 4 样非单调（应丢弃）。
  const std::vector<double> twt = {nan, nan, 300.0, 290.0, 500.0};
  std::vector<std::pair<double, double>> pairs;
  appendTimeDepthPairs(depths, twt, &pairs);
  std::vector<double> tdD, tdT;
  QVERIFY(buildTimeDepthTable(pairs, &tdD, &tdT));
  QCOMPARE(tdD, (std::vector<double>{300.0, 500.0}));
  QVERIFY(std::isnan(interpTwtNoExtrapolate(tdD, tdT, 200.0)));
  QVERIFY(std::isnan(interpTwtNoExtrapolate(tdD, tdT, 600.0)));
  QCOMPARE(interpTwtNoExtrapolate(tdD, tdT, 400.0), 400.0);

  const std::vector<float> ac(5, 250.0f), den(5, 2.4f);
  std::vector<double> outT;
  std::vector<float> outZ;
  QVERIFY(wellImpedanceTwt(depths, ac, depths, den, tdD, tdT, &outT, &outZ));
  QCOMPARE(outT, (std::vector<double>{300.0, 400.0, 500.0}));
}

QTEST_MAIN(TestInversionWellTd)
#include "tst_inversion_welltd.moc"
