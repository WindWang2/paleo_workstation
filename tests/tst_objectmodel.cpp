// 层：数据（测试壳位于 tests/，被测对象为对象建模最小骨架）
#include <QtTest/QtTest>

#include "algorithms/stratgrid/objectmodel.h"
#include "algorithms/stratgrid/propfill.h"
#include "algorithms/stratgrid/stratgrid.h"

#include <cmath>
#include <vector>

using namespace paleo::stratgrid;

namespace
{

ZoneGrid flatGrid(int ni, int nj, int nk)
{
  SurfaceGrid top;
  top.cols = ni;
  top.rows = nj;
  top.originX = 0;
  top.originY = 0;
  top.dx = 50;
  top.dy = 50;
  top.z.assign(static_cast<std::size_t>(ni) * nj, 1000.f);
  SurfaceGrid bot = top;
  for (float &v : bot.z)
    v = 1100.f;
  ZoneGrid grid;
  QString err;
  if (!buildZoneGrid(top, bot, nk, &grid, &err))
    qFatal("buildZoneGrid failed: %s", qPrintable(err));
  return grid;
}

// NaN 视为相等（未命中格的复现断言口径）。
bool sameCell(float a, float b)
{
  if (std::isnan(a))
    return std::isnan(b);
  return a == b;
}

} // namespace

class ObjectModelTests : public QObject
{
  Q_OBJECT
private slots:
  void channelGeometryRecoverableFromField();
  void pointBarEllipseGeometry();
  void objectPriorityOverridesBackground();
  void seededReproducibleAndZoneSeeding();
  void verticalThicknessAnchoredAtBase();
  void invalidInputs();
};

void ObjectModelTests::channelGeometryRecoverableFromField()
{
  // Oracle 5：河道放置后其几何参数（宽度/走向/厚度）在属性场可复现。
  const ZoneGrid grid = flatGrid(40, 30, 10); // 层厚 10 m
  ObjectSpec channel;
  channel.type = ObjectType::Channel;
  channel.azimuthDeg = 90; // 正东
  channel.length = 1400;
  channel.width = 300;
  channel.thickness = 20;  // → 2 层
  channel.curvature = 0;
  channel.verticalFrac = 1;
  channel.value = 6.5;
  channel.count = 1;

  PropertyVolume volume;
  ObjectModelMeta meta;
  QString err;
  QVERIFY2(placeObjects(grid, nullptr, 42, {channel}, &volume, &meta, &err), qPrintable(err));
  QCOMPARE(meta.placements.size(), std::size_t(1));
  const ObjectPlacementRecord &record = meta.placements.front();
  QCOMPARE(record.azimuthDeg, 90.0);
  QCOMPARE(record.width, 300.0);
  QCOMPARE(record.thickness, 20.0);
  QVERIFY(record.cells > 0);
  QVERIFY(meta.caliber.contains(QStringLiteral("对象优先")));

  // 走向/宽度从 cell 场恢复：取河道中部 k 层的命中柱心做 PCA。
  std::vector<double> xs;
  std::vector<double> ys;
  std::vector<int> ks;
  for (int k = 0; k < grid.nk; ++k)
    for (int j = 0; j < grid.nj; ++j)
      for (int i = 0; i < grid.ni; ++i)
        if (std::isfinite(volume.values[static_cast<std::size_t>(grid.cellIndex(i, j, k))]))
        {
          xs.push_back(grid.originX + ( i + 0.5 ) * grid.dx);
          ys.push_back(grid.originY + ( j + 0.5 ) * grid.dy);
          ks.push_back(k);
        }
  QVERIFY(xs.size() > 50);
  double meanX = 0;
  double meanY = 0;
  for (std::size_t n = 0; n < xs.size(); ++n)
  {
    meanX += xs[n];
    meanY += ys[n];
  }
  meanX /= xs.size();
  meanY /= ys.size();
  double sxx = 0, syy = 0, sxy = 0;
  for (std::size_t n = 0; n < xs.size(); ++n)
  {
    sxx += ( xs[n] - meanX ) * ( xs[n] - meanX );
    syy += ( ys[n] - meanY ) * ( ys[n] - meanY );
    sxy += ( xs[n] - meanX ) * ( ys[n] - meanY );
  }
  // 主方向特征向量：协方差矩阵 [[sxx,sxy],[sxy,syy]]
  const double trace = sxx + syy;
  const double diff = sxx - syy;
  const double disc = std::sqrt(std::max(0.0, diff * diff + 4 * sxy * sxy));
  const double lambdaMax = 0.5 * ( trace + disc );
  double vx = lambdaMax - syy;
  double vy = sxy;
  const double norm = std::hypot(vx, vy);
  QVERIFY(norm > 1e-9);
  vx /= norm;
  vy /= norm;
  // 走向 90°（正东）= 主方向沿 x 轴
  const double recoveredAzimuth = std::atan2(vx, vy) * 180.0 / M_PI; // 从北顺时针
  QVERIFY2(std::fabs(recoveredAzimuth - 90.0) < 5.0 || std::fabs(recoveredAzimuth + 90.0) < 5.0,
           QStringLiteral("recovered azimuth %1").arg(recoveredAzimuth).toUtf8().constData());
  // 宽度 = 主方向垂直 extent（±半个柱距量化）
  double minAcross = 1e18;
  double maxAcross = -1e18;
  const double acrossX = -vy;
  const double acrossY = vx;
  for (std::size_t n = 0; n < xs.size(); ++n)
  {
    const double across = ( xs[n] - meanX ) * acrossX + ( ys[n] - meanY ) * acrossY;
    minAcross = std::min(minAcross, across);
    maxAcross = std::max(maxAcross, across);
  }
  const double recoveredWidth = maxAcross - minAcross + std::fabs(grid.dy); // 离散柱量化补偿
  QVERIFY2(std::fabs(recoveredWidth - 300.0) <= 150.0,
           QStringLiteral("recovered width %1").arg(recoveredWidth).toUtf8().constData());
  // 厚度：命中 k 层数 × 层厚
  std::vector<int> distinctKs;
  for (int k : ks)
    if (std::find(distinctKs.begin(), distinctKs.end(), k) == distinctKs.end())
      distinctKs.push_back(k);
  const double recoveredThickness = distinctKs.size() * 10.0;
  QVERIFY2(std::fabs(recoveredThickness - 20.0) <= 10.0,
           QStringLiteral("recovered thickness %1").arg(recoveredThickness).toUtf8().constData());
  // 值域：所有命中 cell 均为对象属性值
  for (int k : distinctKs)
    for (int j = 0; j < grid.nj; ++j)
      for (int i = 0; i < grid.ni; ++i)
      {
        const float v = volume.values[static_cast<std::size_t>(grid.cellIndex(i, j, k))];
        if (std::isfinite(v))
          QCOMPARE(v, 6.5f);
      }
}

void ObjectModelTests::pointBarEllipseGeometry()
{
  const ZoneGrid grid = flatGrid(30, 30, 6);
  ObjectSpec bar;
  bar.type = ObjectType::PointBar;
  bar.azimuthDeg = 0; // 长轴南北
  bar.length = 600;
  bar.width = 300;
  bar.thickness = 50;
  bar.value = 3.25;
  bar.count = 1; // 单对象：包围盒方向语义才成立（多对象联合包围盒无长轴主导语义）

  PropertyVolume volume;
  ObjectModelMeta meta;
  QString err;
  QVERIFY2(placeObjects(grid, nullptr, 7, {bar}, &volume, &meta, &err), qPrintable(err));
  QCOMPARE(meta.placements.size(), std::size_t(1));
  QVERIFY(meta.objectCells > 0);
  for (const ObjectPlacementRecord &record : meta.placements)
  {
    QCOMPARE(record.type, ObjectType::PointBar);
    QCOMPARE(record.length, 600.0);
    QCOMPARE(record.width, 300.0);
    QVERIFY(record.cells > 0);
  }
  double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
  for (int j = 0; j < grid.nj; ++j)
    for (int i = 0; i < grid.ni; ++i)
      for (int k = 0; k < grid.nk; ++k)
        if (std::isfinite(volume.values[static_cast<std::size_t>(grid.cellIndex(i, j, k))]))
        {
          minX = std::min(minX, grid.originX + ( i + 0.5 ) * grid.dx);
          maxX = std::max(maxX, grid.originX + ( i + 0.5 ) * grid.dx);
          minY = std::min(minY, grid.originY + ( j + 0.5 ) * grid.dy);
          maxY = std::max(maxY, grid.originY + ( j + 0.5 ) * grid.dy);
        }
  QVERIFY2(( maxY - minY ) >= ( maxX - minX ),
           "point bar long axis (N-S) must dominate the map extent");
  QVERIFY2(maxX - minX <= 300.0 + 100.0,
           QStringLiteral("point bar across extent %1").arg(maxX - minX).toUtf8().constData());
}

void ObjectModelTests::objectPriorityOverridesBackground()
{
  // 口径钉死：对象内 cell 标记与连续场优先级——对象硬覆盖。
  const ZoneGrid grid = flatGrid(20, 16, 4);
  std::vector<Seed> seeds;
  seeds.push_back(Seed{4, 4, 1, 15.0});
  seeds.push_back(Seed{12, 10, 2, 25.0});
  PropertyVolume background;
  QString err;
  QVERIFY2(fillIdw(grid, seeds, std::vector<FaultSegment>{}, 2.0, &background, {}, &err),
           qPrintable(err));
  const int filledBefore = background.filledCells;

  ObjectSpec channel;
  channel.azimuthDeg = 45;
  channel.length = 800;
  channel.width = 250;
  channel.thickness = 30;
  channel.value = 99.5;
  PropertyVolume objects;
  ObjectModelMeta meta;
  QVERIFY2(placeObjects(grid, nullptr, 5, {channel}, &objects, &meta, &err), qPrintable(err));
  const std::vector<float> before = background.values;
  QVERIFY2(applyObjectOverride(objects, &background, &err), qPrintable(err));

  int overridden = 0;
  for (std::size_t cell = 0; cell < background.values.size(); ++cell)
  {
    if (std::isfinite(objects.values[cell]))
    {
      QCOMPARE(background.values[cell], 99.5f);
      QVERIFY(before[cell] != background.values[cell] ||
              before[cell] == 99.5f); // 覆盖生效（或背景恰好同值的退化格）
      ++overridden;
    }
    else
    {
      QCOMPARE(background.values[cell], before[cell]); // 非对象格逐位不变
    }
  }
  QVERIFY(overridden > 0);
  QVERIFY(background.filledCells >= filledBefore);
}

void ObjectModelTests::seededReproducibleAndZoneSeeding()
{
  const ZoneGrid grid = flatGrid(24, 24, 4);
  std::vector<int> zones(static_cast<std::size_t>(grid.ni) * grid.nj, 1);
  for (int j = 0; j < grid.nj; ++j)
    for (int i = 12; i < grid.ni; ++i)
      zones[static_cast<std::size_t>(grid.columnIndex(i, j))] = 2;

  ObjectSpec channel; // 南北走向，长贯格架：播种在西半带则整条在西半带
  channel.azimuthDeg = 0;
  channel.length = 1300;
  channel.width = 250;
  channel.thickness = 40;
  channel.zoneCode = 1;
  channel.count = 3;

  PropertyVolume a;
  ObjectModelMeta metaA;
  QString err;
  QVERIFY2(placeObjects(grid, &zones, 99, {channel}, &a, &metaA, &err), qPrintable(err));
  PropertyVolume b;
  ObjectModelMeta metaB;
  QVERIFY2(placeObjects(grid, &zones, 99, {channel}, &b, &metaB, &err), qPrintable(err));
  QCOMPARE(a.values.size(), b.values.size());
  for (std::size_t cell = 0; cell < a.values.size(); ++cell)
    QVERIFY2(sameCell(a.values[cell], b.values[cell]), "same seed must be bitwise equal");

  // 播种限制相带：全部命中柱都在西半带（i ≤ 11）
  for (int j = 0; j < grid.nj; ++j)
    for (int i = 12; i < grid.ni; ++i)
      for (int k = 0; k < grid.nk; ++k)
        QVERIFY2(!std::isfinite(a.values[static_cast<std::size_t>(grid.cellIndex(i, j, k))]),
                 "channel seeded in zone 1 must not appear in zone 2 columns");

  // 不同种子 → 放置不同
  PropertyVolume c;
  ObjectModelMeta metaC;
  QVERIFY2(placeObjects(grid, &zones, 100, {channel}, &c, &metaC, &err), qPrintable(err));
  QVERIFY(metaA.placements.front().centerX != metaC.placements.front().centerX ||
          metaA.placements.front().centerY != metaC.placements.front().centerY);

  // 空相带：如实拒绝
  ObjectSpec orphan = channel;
  orphan.zoneCode = 7;
  PropertyVolume d;
  ObjectModelMeta metaD;
  err.clear();
  QVERIFY(!placeObjects(grid, &zones, 1, {orphan}, &d, &metaD, &err));
  QVERIFY(!err.isEmpty());
}

void ObjectModelTests::verticalThicknessAnchoredAtBase()
{
  // 底锚定：命中层只出现在底部层段；thickness=25 → 100m 柱厚的 1/4 → nk=8 中底部 2 层
  const ZoneGrid grid = flatGrid(12, 12, 8);
  ObjectSpec channel;
  channel.azimuthDeg = 90;
  channel.length = 500;
  channel.width = 400;
  channel.thickness = 25;
  channel.verticalFrac = 1;
  PropertyVolume volume;
  ObjectModelMeta meta;
  QString err;
  QVERIFY2(placeObjects(grid, nullptr, 3, {channel}, &volume, &meta, &err), qPrintable(err));
  std::vector<int> ks;
  for (int k = 0; k < grid.nk; ++k)
    for (int j = 0; j < grid.nj; ++j)
      for (int i = 0; i < grid.ni; ++i)
        if (std::isfinite(volume.values[static_cast<std::size_t>(grid.cellIndex(i, j, k))]))
        {
          if (std::find(ks.begin(), ks.end(), k) == ks.end())
            ks.push_back(k);
          break;
        }
  QCOMPARE(ks.size(), std::size_t(2)); // 25m / 12.5m 层厚 = 2 层
  QVERIFY(ks.front() >= grid.nk - 2);  // 底部
}

void ObjectModelTests::invalidInputs()
{
  const ZoneGrid grid = flatGrid(8, 8, 3);
  PropertyVolume volume;
  ObjectModelMeta meta;
  QString err;

  ObjectSpec badWidth;
  badWidth.width = 0;
  QVERIFY(!placeObjects(grid, nullptr, 1, {badWidth}, &volume, &meta, &err));
  QVERIFY(!err.isEmpty());

  ObjectSpec badFrac;
  badFrac.verticalFrac = 1.5;
  QVERIFY(!placeObjects(grid, nullptr, 1, {badFrac}, &volume, &meta, &err));

  std::vector<int> badZones(5, 0);
  ObjectSpec ok;
  QVERIFY(!placeObjects(grid, &badZones, 1, {ok}, &volume, &meta, &err));
}

QTEST_MAIN( ObjectModelTests )
#include "tst_objectmodel.moc"
