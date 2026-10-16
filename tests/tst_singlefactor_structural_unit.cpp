#include <QtTest>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

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
  void buildSurfaceRejectsNonPositivePowerHonestly();
};

void TestSingleFactorStructuralUnit::blockModeNormalizationAndPredicates()
{
  QCOMPARE(QString::fromStdString(normalizeBlockMode("FULL_BLOCK")), QStringLiteral("full_block"));
  QCOMPARE(QString::fromStdString(normalizeBlockMode("contour_stop")), QStringLiteral("contour_stop"));
  QCOMPARE(QString::fromStdString(normalizeBlockMode("  Contour_Stop ")), QStringLiteral("contour_stop"));

  QVERIFY(isFullBlockMode("full_block"));
  QVERIFY(isFullBlockMode("contour_stop")); // 不在非阻断词表即按阻断处理
  QVERIFY(!isFullBlockMode("display_only"));
  QVERIFY(!isFullBlockMode("none"));

  QVERIFY(isContourStopMode("contour_stop"));
  QVERIFY(isContourStopMode("full_block"));
  QVERIFY(isContourStopMode("display_only")); // 停线不动趋势
  QVERIFY(!isContourStopMode("none"));
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
  // 变异测试示范：阻断模（full_block/contour_stop）与非阻断模不可混淆
  const std::string m1 = "full_block";
  const std::string m2 = "display_only";

  QVERIFY(isFullBlockMode(m1));
  QVERIFY(isContourStopMode(m1)); // 阻断模必然停等值线

  QVERIFY(!isFullBlockMode(m2));
  QVERIFY(isContourStopMode(m2)); // display_only 只停线
}

namespace
{

// 3 口井 + 30×30 矩形边界：最小可成图场景（buildStructuralSurface 至少 3 井）。
std::vector<AcquiredWell> threeWells()
{
  std::vector<AcquiredWell> wells;
  for ( int i = 0; i < 3; ++i )
    wells.push_back( AcquiredWell{ QStringLiteral( "w%1" ).arg( i ).toStdString(),
                                   10.0 + 5.0 * i, 10.0 + 7.0 * i, 0.2 + 0.15 * i, false } );
  return wells;
}

Polygon boxBoundary()
{
  Polygon poly;
  poly.exterior.points = { { 0, 0 }, { 30, 0 }, { 30, 30 }, { 0, 30 }, { 0, 0 } };
  return poly;
}

} // namespace

// U3：power=0 / NaN 时 InvalidInput 的真实原因（power 必须为有限正数）必须
// 从 evaluateAt 透传出来——extend 分支此前把 message 吞掉，统一改报
// 「有效成图范围内缺少可用井控」，用户会误以为缺井。power=2 回归仍 Ok。
void TestSingleFactorStructuralUnit::buildSurfaceRejectsNonPositivePowerHonestly()
{
  const std::vector<Polygon> boundaries{ boxBoundary() };
  const std::vector<AcquiredWell> wells = threeWells();
  for ( const double badPower : { 0.0, -1.0, std::numeric_limits<double>::quiet_NaN() } )
  {
    StructuralRequest request;
    request.power = badPower;
    request.resolution = 20;
    const StructuralResult result =
        buildStructuralSurface( wells, boundaries, {}, {}, {}, request, {} );
    QCOMPARE( result.status, Status::InvalidInput );
    const QString message = QString::fromStdString( result.message );
    QVERIFY2( message.contains( QStringLiteral( "power" ), Qt::CaseInsensitive ),
              qPrintable( QStringLiteral( "power=%1 message=%2" ).arg( badPower ).arg( message ) ) );
    QVERIFY2( !message.contains( QStringLiteral( "缺少可用井控" ) ),
              qPrintable( QStringLiteral( "power=%1 被吞成缺井文案: %2" ).arg( badPower ).arg( message ) ) );
  }

  StructuralRequest okRequest;
  okRequest.power = 2.0;
  okRequest.resolution = 20;
  const StructuralResult ok = buildStructuralSurface( wells, boundaries, {}, {}, {}, okRequest, {} );
  QCOMPARE( ok.status, Status::Ok );
  bool finiteSeen = false;
  for ( const double value : ok.grid )
    finiteSeen = finiteSeen || std::isfinite( value );
  QVERIFY( finiteSeen );
}

QTEST_GUILESS_MAIN(TestSingleFactorStructuralUnit)
#include "tst_singlefactor_structural_unit.moc"
