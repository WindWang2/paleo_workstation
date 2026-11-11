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
    void hugeQueryCoordinatesClampSafely();
    void dedupeNonAdjacentNearDuplicates();
  void coincidentToleranceCheck();
  void dedupeSamplesAveraging();
  void dedupePreservesOriginalTieOrderAndMapping();
  void neighborIndexEmptyAndSingle();
  void neighborIndexKNearestOrder();
  void neighborIndexCutoffFiltering();
  void neighborIndexPreservesBucketOrderAtDistanceTies();
  void neighborIndexOwnsMovedAndCopiedSamples();
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
  std::vector<std::uint32_t> out{99};
  const auto empty = NeighborIndex::build({});
  empty.queryNearest(0, 0, 1, 0, &out);
  QVERIFY(out.empty());
  std::vector<Sample> pts = {{5.0, 5.0, 42.0}};
  NeighborIndex idx = NeighborIndex::build(pts);
  QCOMPARE(idx.size(), 1);

  idx.queryNearest(5.0, 5.0, 1, 0.0, &out);
  QCOMPARE(out.size(), 1);
  QCOMPARE(out[0], 0U);

  // cutoff 过滤：查询点在 (10, 10)，距离约 7.07，cutoff 设为 2.0 -> 应过滤为空
  idx.queryNearest(10.0, 10.0, 1, 2.0, &out);
  QVERIFY(out.empty());
}

void TestGeostatNeighborhood::dedupePreservesOriginalTieOrderAndMapping()
{
  const double missing = std::numeric_limits<double>::quiet_NaN();
  const std::vector<Sample> input = {
    {missing, 0, 0}, {500000.0002, 4000000, 30}, {10, 20, 1e16},
    {missing, 20, 1}, {500000, 4000000, 10}, {10, 20, -1e16},
    {10, 20, 1}, {500000.0001, 4000100, 20}, {1, 2, missing}
  };
  int merged = 0;
  std::vector<std::uint32_t> mapping;
  const auto result = dedupeSamples(input, &merged, &mapping);
  QCOMPARE(merged, 3);
  QCOMPARE(result.size(), std::size_t(3));
  QCOMPARE(result[0].value, 1. / 3.); // (1e16 - 1e16 + 1) / 3, in original tie order.
  QCOMPARE(result[1].value, 20.);
  QCOMPARE(result[2].value, 20.);
  const auto invalid = std::numeric_limits<std::uint32_t>::max();
  QCOMPARE(mapping, (std::vector<std::uint32_t>{invalid, 1, 0, invalid, 1, 0, 0, 2, invalid}));
  int withoutMappingMerged = 0;
  const auto withoutMapping = dedupeSamples(input, &withoutMappingMerged);
  QCOMPARE(withoutMappingMerged, merged);
  QCOMPARE(withoutMapping.size(), result.size());
  for (std::size_t i = 0; i < result.size(); ++i) {
    QCOMPARE(withoutMapping[i].x, result[i].x);
    QCOMPARE(withoutMapping[i].y, result[i].y);
    QCOMPARE(withoutMapping[i].value, result[i].value);
  }
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

void TestGeostatNeighborhood::neighborIndexPreservesBucketOrderAtDistanceTies()
{
  // The distant anchors put all four unit-distance points in the same bucket.
  // Its first K candidates must remain selected when later distances tie.
  const std::vector<Sample> points = {
    {1, 0, 10}, {0, 1, 20}, {-1, 0, 30}, {0, -1, 40},
    {-1000, -1000, 50}, {100, 100, 60}
  };
  const auto index = NeighborIndex::build(points);
  std::vector<std::uint32_t> result;
  index.queryNearest(0, 0, 2, 0, &result);
  QCOMPARE(result, (std::vector<std::uint32_t>{0, 1}));
  index.queryNearest(0, 0, 2, .99, &result);
  QVERIFY(result.empty());
}

void TestGeostatNeighborhood::neighborIndexOwnsMovedAndCopiedSamples()
{
  const std::vector<Sample> expected = {{1, 0, 10}, {0, 1, 20}, {-1, 0, 30}, {0, -1, 40}};
  auto source = expected;
  const auto copied = NeighborIndex::build(source);
  source[0] = {100, 100, 99};
  source.clear();

  auto compact = expected;
  const auto moved = NeighborIndex::build(std::move(compact));
  std::vector<Sample> duplicates(4096, expected[0]);
  int merged = 0;
  auto deduped = dedupeSamples(duplicates, &merged);
  QCOMPARE(merged, 4095);
  const auto duplicateHeavy = NeighborIndex::build(std::move(deduped));
  QCOMPARE(duplicateHeavy.size(), std::size_t(1));
  QCOMPARE(duplicateHeavy.point(0).value, expected[0].value);

  for (const auto *index : {&copied, &moved}) {
    QCOMPARE(index->size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
      QCOMPARE(index->point(static_cast<std::uint32_t>(i)).x, expected[i].x);
      QCOMPARE(index->point(static_cast<std::uint32_t>(i)).y, expected[i].y);
      QCOMPARE(index->point(static_cast<std::uint32_t>(i)).value, expected[i].value);
    }
  }
  std::vector<std::uint32_t> copiedNearest, movedNearest;
  copied.queryNearest(0, 0, 2, 0, &copiedNearest);
  moved.queryNearest(0, 0, 2, 0, &movedNearest);
  QCOMPARE(movedNearest, copiedNearest);
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

void TestGeostatNeighborhood::dedupeNonAdjacentNearDuplicates()
{
  // PW-078：x 排序后近重合点被 y 不同的点隔开，旧实现只比 back() 漏合并。
  const std::vector<Sample> input = { { 500000.0, 4000000.0, 10 }, { 500000.0001, 4000100.0, 20 },
                                      { 500000.0002, 4000000.0, 30 }, { 500100, 4000050, 15 } };
  int mergedCount = 0;
  const std::vector<Sample> deduped = dedupeSamples( input, &mergedCount );
  QCOMPARE( mergedCount, 1 );
  QCOMPARE( deduped.size(), std::size_t( 3 ) );
  QCOMPARE( deduped[0].value, 20.0 );

}

void TestGeostatNeighborhood::hugeQueryCoordinatesClampSafely()
{
  // PW-128：查询坐标远超 int 范围/NaN 时桶号须在 double 域夹紧（不触发 UB）。
  const std::vector<Sample> pts = { { 0, 0, 1 }, { 10, 10, 2 } };
  const NeighborIndex idx = NeighborIndex::build( pts );
  std::vector<std::uint32_t> out;
  idx.queryNearest( 1e12, -1e12, 1, 0.0, &out ); // floor 远超 INT_MAX
  QCOMPARE( out.size(), std::size_t( 1 ) );
  idx.queryNearest( std::numeric_limits<double>::quiet_NaN(), 0, 1, 0.0, &out );

}

QTEST_GUILESS_MAIN(TestGeostatNeighborhood)
#include "tst_geostat_neighborhood.moc"
