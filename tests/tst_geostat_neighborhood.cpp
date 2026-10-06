#include <QtTest>
#include <cmath>
#include <vector>

#include "../src/algorithms/geostat/neighborhood.h"

using namespace paleo::geostat;
using namespace paleo::geostat::detail;

class TestGeostatNeighborhood : public QObject
{
  Q_OBJECT

private slots:
  void coincidentToleranceCheck();
  void dedupeSamplesAveraging();
  void neighborIndexEmptyAndSingle();
  void neighborIndexKNearestOrder();
  void neighborIndexCutoffFiltering();
  void mutationDemonstration_ascendingDistanceOrder();
};

void TestGeostatNeighborhood::coincidentToleranceCheck()
{
  Sample a{100.0, 200.0, 10.0};
  Sample b{100.0, 200.0, 20.0};
  QVERIFY(coincident(a, b));

  Sample c{100.0 + 1e-10, 200.0, 15.0};
  QVERIFY(coincident(a, c));

  Sample d{100.01, 200.0, 10.0};
  QVERIFY(!coincident(a, d));
}

void TestGeostatNeighborhood::dedupeSamplesAveraging()
{
  std::vector<Sample> input = {
    {10.0, 20.0, 1.0},
    {10.0, 20.0, 3.0}, // 重合点，均值应为 2.0
    {30.0, 40.0, 5.0},
    {std::numeric_limits<double>::quiet_NaN(), 10.0, 0.0}, // 包含非有限值 -> 剔除
    {10.0, std::numeric_limits<double>::infinity(), 0.0}
  };

  int mergedCount = 0;
  std::vector<Sample> deduped = dedupeSamples(input, &mergedCount);

  QCOMPARE(mergedCount, 1);
  QCOMPARE(deduped.size(), 2);
  QCOMPARE(deduped[0].x, 10.0);
  QCOMPARE(deduped[0].y, 20.0);
  QCOMPARE(deduped[0].value, 2.0);
  QCOMPARE(deduped[1].x, 30.0);
  QCOMPARE(deduped[1].y, 40.0);
  QCOMPARE(deduped[1].value, 5.0);
}

void TestGeostatNeighborhood::neighborIndexEmptyAndSingle()
{
  std::vector<Sample> pts = {{5.0, 5.0, 42.0}};
  NeighborIndex idx = NeighborIndex::build(pts);
  QCOMPARE(idx.size(), 1);

  std::vector<std::uint32_t> out;
  idx.queryNearest(5.0, 5.0, 1, 0.0, &out);
  QCOMPARE(out.size(), 1);
  QCOMPARE(out[0], 0U);

  // cutoff 过滤：查询点在 (10, 10)，距离约 7.07，cutoff 设为 2.0 -> 应过滤为空
  idx.queryNearest(10.0, 10.0, 1, 2.0, &out);
  QVERIFY(out.empty());
}

void TestGeostatNeighborhood::neighborIndexKNearestOrder()
{
  std::vector<Sample> pts = {
    {0.0, 0.0, 1.0}, // 0: 距原点 0
    {1.0, 0.0, 2.0}, // 1: 距原点 1
    {0.0, 1.0, 3.0}, // 2: 距原点 1
    {2.0, 0.0, 4.0}, // 3: 距原点 2
    {10.0, 10.0, 5.0} // 4: 距原点 14.14
  };

  NeighborIndex idx = NeighborIndex::build(pts);
  std::vector<std::uint32_t> out;
  idx.queryNearest(0.0, 0.0, 3, 0.0, &out);

  QCOMPARE(out.size(), 3);
  QCOMPARE(out[0], 0U); // 最近的点为 0
  // 次近的两个点为 1 或 2，距离均为 1
  QVERIFY((out[1] == 1U && out[2] == 2U) || (out[1] == 2U && out[2] == 1U));
}

void TestGeostatNeighborhood::neighborIndexCutoffFiltering()
{
  std::vector<Sample> pts = {
    {0.0, 0.0, 1.0},
    {1.0, 0.0, 2.0},
    {5.0, 0.0, 3.0}
  };

  NeighborIndex idx = NeighborIndex::build(pts);
  std::vector<std::uint32_t> out;
  idx.queryNearest(0.0, 0.0, 10, 1.5, &out);

  // 只有 (0,0) 和 (1,0) 在 cutoff 1.5 内
  QCOMPARE(out.size(), 2);
  QCOMPARE(out[0], 0U);
  QCOMPARE(out[1], 1U);
}

void TestGeostatNeighborhood::mutationDemonstration_ascendingDistanceOrder()
{
  // 变异测试示范：查询结果必须严格按与目标点的距离升序排列
  std::vector<Sample> pts;
  for (int i = 0; i < 20; ++i)
  {
    pts.push_back({static_cast<double>(i), static_cast<double>(i * 2), static_cast<double>(i)});
  }

  NeighborIndex idx = NeighborIndex::build(pts);
  std::vector<std::uint32_t> out;
  const double qx = 5.2;
  const double qy = 10.1;
  idx.queryNearest(qx, qy, 5, 0.0, &out);

  QCOMPARE(out.size(), 5);
  double prevD2 = -1.0;
  for (std::uint32_t pIdx : out)
  {
    const Sample &s = idx.point(pIdx);
    const double dx = s.x - qx;
    const double dy = s.y - qy;
    const double d2 = dx * dx + dy * dy;
    QVERIFY(d2 >= prevD2);
    prevD2 = d2;
  }
}

QTEST_GUILESS_MAIN(TestGeostatNeighborhood)
#include "tst_geostat_neighborhood.moc"
