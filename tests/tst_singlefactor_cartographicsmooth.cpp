#include <QtTest>
#include <cmath>
#include <vector>

#include "../src/algorithms/singlefactor/cartographicsmooth.h"

using namespace paleo::singlefactor;

class TestSingleFactorCartographicSmooth : public QObject
{
  Q_OBJECT

private slots:
  void polylineDistanceAndPathLength();
  void isClosedPolylineCheck();
  void dedupeConsecutivePointsTest();
  void rdpSimplifyCollinearPoints();
  void chaikinSmoothPolylinePoints();
  void selfIntersectionDetection();
  void cartographicSmoothContoursFlow();
  void mutationDemonstration_pathLengthPositive();
};

void TestSingleFactorCartographicSmooth::polylineDistanceAndPathLength()
{
  Point2 p1{0.0, 0.0};
  Point2 p2{3.0, 4.0};
  Point2 p3{3.0, 0.0};

  QCOMPARE(polylinePointDistance(p1, p2), 5.0);

  Polyline poly{p1, p3, p2}; // (0,0) -> (3,0) 长度 3，(3,0) -> (3,4) 长度 4，总计 7
  QCOMPARE(polylinePathLength(poly), 7.0);
}

void TestSingleFactorCartographicSmooth::isClosedPolylineCheck()
{
  Polyline openLine{{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}};
  QVERIFY(!isClosedPolyline(openLine, 1.0));

  Polyline closedLine{{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.1, 0.1}};
  QVERIFY(isClosedPolyline(closedLine, 0.5));
}

void TestSingleFactorCartographicSmooth::dedupeConsecutivePointsTest()
{
  Polyline input{{0.0, 0.0}, {0.0, 0.001}, {5.0, 5.0}, {5.0, 5.0}, {10.0, 10.0}};
  const Polyline deduped = dedupeConsecutivePoints(input, 0.01);
  QCOMPARE(deduped.size(), 3);
  QCOMPARE(deduped[0].x, 0.0);
  QCOMPARE(deduped[1].x, 5.0);
  QCOMPARE(deduped[2].x, 10.0);
}

void TestSingleFactorCartographicSmooth::rdpSimplifyCollinearPoints()
{
  Polyline line;
  for (int i = 0; i <= 10; ++i)
  {
    line.push_back({static_cast<double>(i), static_cast<double>(i * 2)});
  }
  const Polyline simplified = rdpSimplify(line, 0.1);
  // 共线点应被化简为仅首末 2 点
  QCOMPARE(simplified.size(), 2);
  QCOMPARE(simplified[0].x, 0.0);
  QCOMPARE(simplified[1].x, 10.0);
}

void TestSingleFactorCartographicSmooth::chaikinSmoothPolylinePoints()
{
  Polyline corner{{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}};
  const Polyline smoothed = chaikinSmoothPolyline(corner, 2, 1.0);
  // Chaikin 细分使得折点变圆滑，点数增加
  QVERIFY(smoothed.size() > corner.size());
}

void TestSingleFactorCartographicSmooth::selfIntersectionDetection()
{
  Polyline straight{{0.0, 0.0}, {5.0, 5.0}, {10.0, 10.0}};
  QVERIFY(!polylineSelfIntersects(straight, 1.0));

  // 8 字自相交线
  Polyline selfCross{{0.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {10.0, 0.0}};
  QVERIFY(polylineSelfIntersects(selfCross, 1.0));
}

void TestSingleFactorCartographicSmooth::cartographicSmoothContoursFlow()
{
  Polyline l1{{0.0, 0.0}, {5.0, 1.0}, {10.0, 0.0}};
  ContourLineMap contours;
  contours.push_back({100.0, {l1}});

  const ContourLineMap smoothed = cartographicSmoothContours(contours, 1.0, 2);
  QCOMPARE(smoothed.size(), 1);
  QCOMPARE(smoothed[0].first, 100.0); // 标高 level 保持
  QVERIFY(!smoothed[0].second.empty());
  QVERIFY(!smoothed[0].second[0].empty());
}

void TestSingleFactorCartographicSmooth::mutationDemonstration_pathLengthPositive()
{
  // 变异测试示范：非平凡折线的长度必须严格为正
  Polyline line{{1.0, 2.0}, {4.0, 6.0}};
  QVERIFY(polylinePathLength(line) > 0.0);
  QCOMPARE(polylinePathLength(line), 5.0);
}

QTEST_GUILESS_MAIN(TestSingleFactorCartographicSmooth)
#include "tst_singlefactor_cartographicsmooth.moc"
