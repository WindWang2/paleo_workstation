// tst_welltracelocate — 井旁道：井口 XY → IL/XL/剖面列（#129）。
//
// 合成测网：IL 步长 2、XL 步长 4、旋转 23°、UTM 量级原点、面元 25 m × 12.5 m。
// 钉住：非单位线距下吸附到真实线号、列 = 线号在 xlineValues 中的位置（不是
// xl - xlMin）、边缘半个线距内取边缘道、再远如实拒绝、拟合不可用时不瞎猜。
#include <QtTest>

#include <cmath>

#include "domain/seismic/welltracelocate.h"

using seismic::SgyCoordinateMapper;
using seismic::SgyCoordinateSample;
using seismic::SgyIndex;
using seismic::WellTraceLocation;

namespace
{
constexpr int kIl0 = 1000, kIlStep = 2, kIlCount = 20; // 1000..1038
constexpr int kXl0 = 2000, kXlStep = 4, kXlCount = 50; // 2000..2196
constexpr double kOriginX = 612345.0, kOriginY = 3456789.0;
constexpr double kRotDeg = 23.0;
constexpr double kIlBin = 25.0;   // 相邻 inline（步长 2）间距 m
constexpr double kXlBin = 12.5;   // 相邻 xline（步长 4）间距 m

// 连续 (il, xl) → XY（真值）：按「线序号」铺面元，线号只是标签。
void truth(double il, double xl, double &x, double &y)
{
  const double i = (il - kIl0) / kIlStep;
  const double j = (xl - kXl0) / kXlStep;
  const double r = kRotDeg * 3.14159265358979323846 / 180.0;
  x = kOriginX + kXlBin * j * std::cos(r) - kIlBin * i * std::sin(r);
  y = kOriginY + kXlBin * j * std::sin(r) + kIlBin * i * std::cos(r);
}

SgyIndex makeIndex()
{
  SgyIndex idx;
  idx.coordinateFieldsPresent = true;
  idx.inlineMin = kIl0;
  idx.inlineMax = kIl0 + kIlStep * (kIlCount - 1);
  idx.xlineMin = kXl0;
  idx.xlineMax = kXl0 + kXlStep * (kXlCount - 1);
  for (int i = 0; i < kIlCount; ++i)
    idx.inlineValues.push_back(kIl0 + kIlStep * i);
  for (int j = 0; j < kXlCount; ++j)
    idx.xlineValues.push_back(kXl0 + kXlStep * j);
  // 道头坐标采样：稀疏抽样（真实索引只采部分道），坐标按整分米写入。
  for (int i = 0; i < kIlCount; i += 3)
    for (int j = 0; j < kXlCount; j += 7)
    {
      SgyCoordinateSample s;
      s.inlineNo = idx.inlineValues[std::size_t(i)];
      s.xlineNo = idx.xlineValues[std::size_t(j)];
      truth(s.inlineNo, s.xlineNo, s.x, s.y);
      s.x = std::round(s.x * 10.0) / 10.0;
      s.y = std::round(s.y * 10.0) / 10.0;
      idx.coordinateSamples.push_back(s);
    }
  return idx;
}
} // namespace

class TestWellTraceLocate : public QObject
{
  Q_OBJECT
private slots:
  void fitIsValidForNonUnitSteps()
  {
    const SgyIndex idx = makeIndex();
    const SgyCoordinateMapper mapper = SgyCoordinateMapper::Fit(idx);
    QVERIFY2(mapper.valid(), mapper.fit().rejectionReason.c_str());
    QVERIFY(mapper.fit().maxResidual < 0.2);
    QCOMPARE(seismic::lineStepOf(idx.inlineValues), kIlStep);
    QCOMPARE(seismic::lineStepOf(idx.xlineValues), kXlStep);
  }

  void wellOnTraceMapsToThatTrace_data()
  {
    QTest::addColumn<int>("il");
    QTest::addColumn<int>("xl");
    QTest::addColumn<double>("jitterIl"); // 连续线号扰动（< 半个步长）
    QTest::addColumn<double>("jitterXl");
    QTest::newRow("corner-min") << 1000 << 2000 << 0.0 << 0.0;
    QTest::newRow("corner-max") << 1038 << 2196 << 0.0 << 0.0;
    QTest::newRow("interior") << 1016 << 2100 << 0.0 << 0.0;
    QTest::newRow("interior-jitter") << 1016 << 2100 << 0.9 << -1.9;
    QTest::newRow("interior-jitter2") << 1022 << 2048 << -0.9 << 1.9;
    QTest::newRow("unsampled-trace") << 1002 << 2004 << 0.4 << 0.4;
  }
  void wellOnTraceMapsToThatTrace()
  {
    QFETCH(int, il);
    QFETCH(int, xl);
    QFETCH(double, jitterIl);
    QFETCH(double, jitterXl);
    const SgyIndex idx = makeIndex();
    const SgyCoordinateMapper mapper = SgyCoordinateMapper::Fit(idx);
    double x = 0, y = 0;
    truth(il + jitterIl, xl + jitterXl, x, y);
    const WellTraceLocation loc = seismic::locateWellTrace(idx, mapper, x, y);
    QVERIFY2(loc.ok, loc.error.c_str());
    QCOMPARE(loc.inlineNo, il);
    QCOMPARE(loc.xlineNo, xl);
    // 列 = xl 在实际线号表中的位置；单位步长假设会得到 xl - xlMin（4 倍）。
    QCOMPARE(loc.column, (xl - kXl0) / kXlStep);
    QVERIFY(std::abs(loc.inlineF - (il + jitterIl)) < 0.05);
    QVERIFY(std::abs(loc.xlineF - (xl + jitterXl)) < 0.05);
  }

  // 边缘：越出末线不到半个线距（按实际步长）仍取边缘道；旧口径固定 0.5 个
  // 线号的余量在 XL 步长 4 时只有 1/8 个面元，会把边缘井误拒。
  void edgeWithinHalfStepSnapsToEdgeTrace()
  {
    const SgyIndex idx = makeIndex();
    const SgyCoordinateMapper mapper = SgyCoordinateMapper::Fit(idx);
    double x = 0, y = 0;
    truth(1038 + 0.8, 2196 + 1.6, x, y); // IL 越 0.4 步、XL 越 0.4 步
    const WellTraceLocation loc = seismic::locateWellTrace(idx, mapper, x, y);
    QVERIFY2(loc.ok, loc.error.c_str());
    QCOMPARE(loc.inlineNo, 1038);
    QCOMPARE(loc.xlineNo, 2196);
    QCOMPARE(loc.column, kXlCount - 1);
    QVERIFY(!mapper.InCoverage(x, y, 0.5)); // 旧口径在此处拒绝
  }

  void outsideCoverageIsRejected()
  {
    const SgyIndex idx = makeIndex();
    const SgyCoordinateMapper mapper = SgyCoordinateMapper::Fit(idx);
    double x = 0, y = 0;
    truth(1000 - 3.0, 2100, x, y); // IL 越出 1.5 步
    WellTraceLocation loc = seismic::locateWellTrace(idx, mapper, x, y);
    QVERIFY(!loc.ok);
    QVERIFY(loc.outOfCoverage);
    truth(1016, 2196 + 6.0, x, y);
    loc = seismic::locateWellTrace(idx, mapper, x, y);
    QVERIFY(!loc.ok);
    QVERIFY(loc.outOfCoverage);
    // 米制坐标直接当线号（#129 原始缺陷）会落在测网外/钳到边缘——这里明确
    // 拒绝而不是钳取。
    loc = seismic::locateWellTrace(idx, mapper, 1016.0, 2100.0);
    QVERIFY(!loc.ok);
  }

  void invalidFitIsReportedNotGuessed()
  {
    SgyIndex idx = makeIndex();
    idx.coordinateSamples.resize(3); // 采样太少 → 拟合不可用
    const SgyCoordinateMapper mapper = SgyCoordinateMapper::Fit(idx);
    QVERIFY(!mapper.valid());
    double x = 0, y = 0;
    truth(1016, 2100, x, y);
    const WellTraceLocation loc = seismic::locateWellTrace(idx, mapper, x, y);
    QVERIFY(!loc.ok);
    QVERIFY(!loc.outOfCoverage);
    QVERIFY(!loc.error.empty());
  }
};

QTEST_GUILESS_MAIN(TestWellTraceLocate)
#include "tst_welltracelocate.moc"
