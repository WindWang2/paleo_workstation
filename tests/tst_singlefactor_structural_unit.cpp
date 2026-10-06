#include <QtTest>
#include <string>

#include "../src/algorithms/singlefactor/structural.h"

using namespace paleo::singlefactor;

class TestSingleFactorStructuralUnit : public QObject
{
  Q_OBJECT

private slots:
  void blockModeNormalizationAndPredicates();
  void performanceGridResolutionCalculations();
  void barrierBufferDistanceCalculations();
  void structuralStructuresDefaults();
  void structuralBarrierAndDirectionProperties();
  void mutationDemonstration_distinctModes();
};

void TestSingleFactorStructuralUnit::blockModeNormalizationAndPredicates()
{
  QCOMPARE(QString::fromStdString(normalizeBlockMode("FULL_BLOCK")), QStringLiteral("full_block"));
  QCOMPARE(QString::fromStdString(normalizeBlockMode("contour_stop")), QStringLiteral("contour_stop"));
  QCOMPARE(QString::fromStdString(normalizeBlockMode("stop")), QStringLiteral("contour_stop"));
  QCOMPARE(QString::fromStdString(normalizeBlockMode("unknown")), QStringLiteral("full_block"));

  QVERIFY(isFullBlockMode("full_block"));
  QVERIFY(!isFullBlockMode("contour_stop"));

  QVERIFY(isContourStopMode("contour_stop"));
  QVERIFY(!isContourStopMode("full_block"));
}

void TestSingleFactorStructuralUnit::performanceGridResolutionCalculations()
{
  // 1000 x 1000 区域，请求 200 分辨率
  const int res1 = resolvePerformanceGridResolution(1000.0, 1000.0, 200, 200000);
  QCOMPARE(res1, 200);

  // 请求极大分辨率（例如 5000），maxCells 封顶为 200000 (约 sqrt(200000) ~ 447)
  const int res2 = resolvePerformanceGridResolution(1000.0, 1000.0, 5000, 200000);
  QVERIFY(res2 < 5000);
  QVERIFY(res2 <= 448);
}

void TestSingleFactorStructuralUnit::barrierBufferDistanceCalculations()
{
  // 手动指定缓冲距离 120.0，autoEnabled = false
  const auto r1 = resolveBarrierBufferDistance(120.0, 0.0, 10.0, 1000.0, 1000.0, 200.0, true, false);
  QCOMPARE(r1.first, 120.0);
  QCOMPARE(r1.second, false);

  // 自动缓冲距离：autoEnabled = true
  const auto r2 = resolveBarrierBufferDistance(0.0, 0.0, 10.0, 1000.0, 1000.0, 200.0, true, true);
  QVERIFY(r2.first > 0.0);
  QCOMPARE(r2.second, true);

  // 无断层障碍物时
  const auto r3 = resolveBarrierBufferDistance(0.0, 0.0, 10.0, 1000.0, 1000.0, 200.0, false, true);
  QCOMPARE(r3.first, 0.0);
  QCOMPARE(r3.second, false);
}

void TestSingleFactorStructuralUnit::structuralStructuresDefaults()
{
  StructuralRequest req;
  QCOMPARE(req.power, 2.0);
  QCOMPARE(req.resolution, 339);
  QVERIFY(req.extendTrendToBoundary);
  QVERIFY(req.enableBarriers);
  QVERIFY(req.enableDirections);
  QCOMPARE(req.minPoints, 3);
  QCOMPARE(req.maxPoints, 12);
}

void TestSingleFactorStructuralUnit::structuralBarrierAndDirectionProperties()
{
  StructuralBarrier b;
  b.lineId = "barrier_01";
  b.active = true;
  b.blockMode = "full_block";
  b.priority = 2;

  QCOMPARE(QString::fromStdString(b.lineId), QStringLiteral("barrier_01"));
  QCOMPARE(b.priority, 2);

  StructuralDirection d;
  d.lineId = "dir_01";
  d.ratio = 6.0;
  d.zoneId = "zone_A";

  QCOMPARE(d.ratio, 6.0);
  QCOMPARE(QString::fromStdString(d.zoneId), QStringLiteral("zone_A"));
}

void TestSingleFactorStructuralUnit::mutationDemonstration_distinctModes()
{
  // 变异测试示范：full_block 与 contour_stop 互斥，绝不可互相混淆
  const std::string m1 = "full_block";
  const std::string m2 = "contour_stop";

  QVERIFY(isFullBlockMode(m1));
  QVERIFY(!isContourStopMode(m1));

  QVERIFY(isContourStopMode(m2));
  QVERIFY(!isFullBlockMode(m2));
}

QTEST_GUILESS_MAIN(TestSingleFactorStructuralUnit)
#include "tst_singlefactor_structural_unit.moc"
