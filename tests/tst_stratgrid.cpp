// 层：测试壳
#include <QtTest>

#include "../src/algorithms/stratgrid/stratgrid.h"

#include <cmath>
#include <limits>

using namespace paleo::stratgrid;

namespace
{

SurfaceGrid flat(int cols, int rows, float z, double dx = 10.0, double dy = 10.0,
                 double originX = 0.0, double originY = 0.0)
{
  SurfaceGrid g;
  g.cols = cols;
  g.rows = rows;
  g.dx = dx;
  g.dy = dy;
  g.originX = originX;
  g.originY = originY;
  g.z.assign(static_cast<std::size_t>(cols * rows), z);
  return g;
}

} // namespace

class TestStratGrid : public QObject
{
  Q_OBJECT
private slots:
  void liveCellsHavePositiveVolumeAndMonotoneInterfaces();
  void roundTripCellCenters();
  void neighborsSkipDeadColumns();
  void singularSurfaceIsRejected();
  void nanPixelIsDeadNotFilled();
  void northUpDyRoundTrips();
};

void TestStratGrid::liveCellsHavePositiveVolumeAndMonotoneInterfaces()
{
  ZoneGrid grid;
  QString err;
  QVERIFY2(buildZoneGrid(flat(4, 3, 0.0f), flat(4, 3, 30.0f), 3, &grid, &err),
           qPrintable(err));
  QCOMPARE(grid.ni, 4);
  QCOMPARE(grid.nj, 3);
  QCOMPARE(grid.nk, 3);
  QCOMPARE(grid.liveColumns, 12);
  for (int j = 0; j < grid.nj; ++j)
  {
    for (int i = 0; i < grid.ni; ++i)
    {
      double prev = 0;
      QVERIFY(interfaceZ(grid, i, j, 0, &prev));
      QCOMPARE(prev, 0.0);
      for (int k = 0; k < grid.nk; ++k)
      {
        QVERIFY(cellVolume(grid, i, j, k) > 0.0);
        QCOMPARE(cellVolume(grid, i, j, k), 1000.0);
        QCOMPARE(layerThickness(grid, i, j, k), 10.0);
        double z = 0;
        QVERIFY(interfaceZ(grid, i, j, k + 1, &z));
        QVERIFY(z > prev);
        prev = z;
      }
      QCOMPARE(prev, 30.0);
    }
  }
  QCOMPARE(cellVolume(grid, 0, 0, 9), 0.0);

  // 部分柱颠倒：整幅仍可建，颠倒柱是死柱，体积为 0。
  SurfaceGrid bot = flat(2, 1, 20.0f);
  bot.z[1] = 0.0f;
  ZoneGrid mixed;
  QVERIFY(buildZoneGrid(flat(2, 1, 10.0f), bot, 2, &mixed, &err));
  QCOMPARE(mixed.liveColumns, 1);
  QVERIFY(mixed.columnLive(0, 0));
  QVERIFY(!mixed.columnLive(1, 0));
  QCOMPARE(cellVolume(mixed, 1, 0, 0), 0.0);

  ZoneGrid bad;
  QVERIFY(!buildZoneGrid(flat(2, 2, 5.0f), flat(2, 2, 5.0f), 4, &bad, &err));
  QVERIFY(err.contains(QStringLiteral("奇异面")));
  QCOMPARE(bad.ni, 0);

  QVERIFY(!buildZoneGrid(flat(2, 2, 10.0f), flat(2, 2, 0.0f), 2, &bad, &err));
  QVERIFY(err.contains(QStringLiteral("颠倒")));

  SurfaceGrid mismatch = flat(3, 2, 1.0f);
  QVERIFY(!buildZoneGrid(flat(2, 2, 0.0f), mismatch, 2, &bad, &err));
  QVERIFY(err.contains(QStringLiteral("几何不一致")));
  QVERIFY(!buildZoneGrid(flat(2, 2, 0.0f), flat(2, 2, 1.0f), 0, &bad, &err));
}

void TestStratGrid::roundTripCellCenters()
{
  ZoneGrid grid;
  QVERIFY(buildZoneGrid(flat(5, 4, 100.0f, 25.0, -40.0, 1000.0, 8000.0),
                        flat(5, 4, 160.0f, 25.0, -40.0, 1000.0, 8000.0), 6, &grid, nullptr));
  for (int k = 0; k < grid.nk; ++k)
  {
    for (int j = 0; j < grid.nj; ++j)
    {
      for (int i = 0; i < grid.ni; ++i)
      {
        Point3 p;
        QVERIFY(cellCenter(grid, i, j, k, &p));
        Index3 back;
        QVERIFY2(ijkAt(grid, p.x, p.y, p.z, &back), "cell center must land in its own cell");
        QCOMPARE(back.i, i);
        QCOMPARE(back.j, j);
        QCOMPARE(back.k, k);
      }
    }
  }

  // 界面归属：s = k/nk 归更深一层；顶归 0；底归末层。
  Index3 at;
  QVERIFY(ijkAt(grid, grid.originX + 0.5 * grid.dx, grid.originY + 0.5 * grid.dy, 100.0, &at));
  QCOMPARE(at.k, 0);
  const double iface = 100.0 + (1.0 / 6.0) * 60.0;
  QVERIFY(ijkAt(grid, grid.originX + 0.5 * grid.dx, grid.originY + 0.5 * grid.dy, iface, &at));
  QCOMPARE(at.k, 1);
  QVERIFY(ijkAt(grid, grid.originX + 0.5 * grid.dx, grid.originY + 0.5 * grid.dy, 160.0, &at));
  QCOMPARE(at.k, 5);
  QVERIFY(!ijkAt(grid, grid.originX + 0.5 * grid.dx, grid.originY + 0.5 * grid.dy, 99.0, &at));
  QVERIFY(!ijkAt(grid, grid.originX + 0.5 * grid.dx, grid.originY + 0.5 * grid.dy, 161.0, &at));
  QVERIFY(!ijkAt(grid, grid.originX - 1.0, grid.originY, 120.0, &at));
}

void TestStratGrid::neighborsSkipDeadColumns()
{
  SurfaceGrid top = flat(3, 3, 0.0f, 1.0, 1.0);
  SurfaceGrid bot = flat(3, 3, 9.0f, 1.0, 1.0);
  top.z[static_cast<std::size_t>(1 * 3 + 1)] = std::numeric_limits<float>::quiet_NaN();
  ZoneGrid grid;
  QVERIFY(buildZoneGrid(top, bot, 3, &grid, nullptr));
  QVERIFY(!grid.columnLive(1, 1));
  QCOMPARE(grid.deadColumns, 1);

  const auto mid = neighbors6(grid, 1, 0, 1);
  QCOMPARE(static_cast<int>(mid.size()), 4); // +j 死柱，-j 越界
  bool sawDead = false;
  for (const Index3 &n : mid)
    if (n.i == 1 && n.j == 1)
      sawDead = true;
  QVERIFY(!sawDead);

  const auto centerDead = neighbors6(grid, 1, 1, 1);
  QVERIFY(centerDead.empty());

  const auto corner = neighbors6(grid, 0, 0, 0);
  QCOMPARE(static_cast<int>(corner.size()), 3);
  const auto interior = neighbors6(grid, 0, 1, 1);
  QCOMPARE(static_cast<int>(interior.size()), 4); // +i 是死柱，-i 越界
}

void TestStratGrid::singularSurfaceIsRejected()
{
  SurfaceGrid top = flat(3, 1, 5.0f);
  SurfaceGrid bot = flat(3, 1, 5.0f);
  bot.z[0] = std::numeric_limits<float>::quiet_NaN();
  ZoneGrid grid;
  QString err;
  QVERIFY(!buildZoneGrid(top, bot, 2, &grid, &err));
  QVERIFY2(err.contains(QStringLiteral("奇异面")), qPrintable(err));
  QCOMPARE(grid.liveColumns, 0);
}

void TestStratGrid::nanPixelIsDeadNotFilled()
{
  SurfaceGrid top = flat(3, 2, 0.0f);
  SurfaceGrid bot = flat(3, 2, 12.0f);
  bot.z[4] = std::numeric_limits<float>::quiet_NaN();
  ZoneGrid grid;
  QVERIFY(buildZoneGrid(top, bot, 4, &grid, nullptr));
  QCOMPARE(grid.liveColumns, 5);
  QCOMPARE(grid.deadColumns, 1);
  QVERIFY(!grid.columnLive(1, 1));
  Point3 p;
  QVERIFY(!cellCenter(grid, 1, 1, 0, &p));
  QVERIFY(cellVolume(grid, 0, 0, 0) > 0.0);
}

void TestStratGrid::northUpDyRoundTrips()
{
  // 北向上：originY 是顶边，dy < 0。
  ZoneGrid grid;
  QVERIFY(buildZoneGrid(flat(2, 2, 0.0f, 20.0, -40.0, 0.0, 16406.0),
                        flat(2, 2, 40.0f, 20.0, -40.0, 0.0, 16406.0), 2, &grid, nullptr));
  Point3 p;
  QVERIFY(cellCenter(grid, 1, 1, 1, &p));
  QCOMPARE(p.x, 0.0 + 1.5 * 20.0);
  QCOMPARE(p.y, 16406.0 + 1.5 * -40.0);
  Index3 back;
  QVERIFY(ijkAt(grid, p.x, p.y, p.z, &back));
  QCOMPARE(back.i, 1);
  QCOMPARE(back.j, 1);
  QCOMPARE(back.k, 1);
}

QTEST_GUILESS_MAIN(TestStratGrid)
#include "tst_stratgrid.moc"
