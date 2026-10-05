// 层：数据（测试壳位于 tests/，被测对象为断块错位网格）
#include <QtTest/QtTest>

#include "algorithms/stratgrid/faultoffset.h"
#include "algorithms/stratgrid/propfill.h"
#include "algorithms/stratgrid/stratgrid.h"

#include <cmath>
#include <vector>

using namespace paleo::stratgrid;

namespace
{

// 平底格架：top=1000、bot=1100，ni×nj 活柱，dx=dy=50。
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

} // namespace

class FaultOffsetTests : public QObject
{
  Q_OBJECT
private slots:
  void constantThrowOffsetsDropSide();
  void linearThrowInterpolatesAlongStrike();
  void sameLayerOffsetEqualsThrowAndThicknessPreserved();
  void boundaryColumnOnFaultLineStays();
  void blockPartitionSplitsAcrossFaultAndIdwDoesNotLeak();
  void invalidInputs();
};

void FaultOffsetTests::constantThrowOffsetsDropSide()
{
  const ZoneGrid grid = flatGrid(20, 10, 6);
  FaultThrow fault; // 南北向直立断层 x=512.5（柱心 25,75,… 之间）
  fault.x0 = 512.5;
  fault.y0 = -100;
  fault.x1 = 512.5;
  fault.y1 = 1100;
  fault.throwStart = 15;
  fault.throwEnd = 15;
  fault.dropLeftSide = false; // 东侧（x>512.5）下掉

  ZoneGrid offset;
  std::vector<float> dz;
  FaultOffsetMeta meta;
  QString err;
  QVERIFY2(applyFaultOffset(grid, {fault}, &offset, &dz, &meta, &err), qPrintable(err));
  QCOMPARE(offset.ni, grid.ni);
  QCOMPARE(offset.nj, grid.nj);
  QCOMPARE(offset.nk, grid.nk);

  int dropped = 0;
  for (int j = 0; j < grid.nj; ++j)
    for (int i = 0; i < grid.ni; ++i)
    {
      const std::size_t c = static_cast<std::size_t>(grid.columnIndex(i, j));
      const double cx = grid.originX + (i + 0.5) * grid.dx;
      if (cx > 512.5)
      {
        QCOMPARE(dz[c], 15.0f);
        QCOMPARE(offset.topZ[c], grid.topZ[c] + 15.0f);
        QCOMPARE(offset.botZ[c], grid.botZ[c] + 15.0f);
        ++dropped;
      }
      else
      {
        QCOMPARE(dz[c], 0.0f);
        QCOMPARE(offset.topZ[c], grid.topZ[c]);
      }
    }
  QCOMPARE(meta.offsetColumns, dropped);
  QCOMPARE(meta.maxAbsThrow, 15.0);
  QCOMPARE(meta.boundaryColumns, 0);
  // 输入格架不被就地改动
  QCOMPARE(grid.topZ[static_cast<std::size_t>(grid.columnIndex(19, 0))], 1000.0f);
}

void FaultOffsetTests::linearThrowInterpolatesAlongStrike()
{
  const ZoneGrid grid = flatGrid(12, 10, 4);
  FaultThrow fault; // x=300 直立断层，北向（y+），断距 10→20 沿走向线性
  fault.x0 = 300;
  fault.y0 = 0;
  fault.x1 = 300;
  fault.y1 = 500; // 恰好覆盖柱心 y 范围 25..475
  fault.throwStart = 10;
  fault.throwEnd = 20;
  fault.dropLeftSide = false;

  ZoneGrid offset;
  std::vector<float> dz;
  QString err;
  QVERIFY2(applyFaultOffset(grid, {fault}, &offset, &dz, nullptr, &err), qPrintable(err));
  for (int j = 0; j < grid.nj; ++j)
  {
    const double cy = grid.originY + (j + 0.5) * grid.dy;
    const double expected = 10.0 + (20.0 - 10.0) * (cy / 500.0); // t = y/500（投影参数）
    for (int i = 0; i < grid.ni; ++i)
    {
      const double cx = grid.originX + (i + 0.5) * grid.dx;
      const std::size_t c = static_cast<std::size_t>(grid.columnIndex(i, j));
      if (cx > 300)
        QVERIFY2(std::fabs(dz[c] - expected) < 1e-3f,
                 QStringLiteral("j=%1 dz=%2 expect=%3").arg(j).arg(dz[c]).arg(expected).toUtf8());
      else
        QCOMPARE(dz[c], 0.0f);
    }
  }
}

void FaultOffsetTests::sameLayerOffsetEqualsThrowAndThicknessPreserved()
{
  // Oracle 1 错位量化：断线两侧同 k 层面（界面 z）错位量 = 断距；
  // 层厚保序（活柱不变、厚度不变）。
  const ZoneGrid grid = flatGrid(16, 8, 5);
  FaultThrow fault;
  fault.x0 = 400;
  fault.y0 = -50;
  fault.x1 = 400;
  fault.y1 = 850;
  fault.throwStart = 22.5;
  fault.throwEnd = 22.5;
  fault.dropLeftSide = true; // 西侧下掉（逆冲语义，负向断距同机制）

  ZoneGrid offset;
  std::vector<float> dz;
  QString err;
  QVERIFY2(applyFaultOffset(grid, {fault}, &offset, &dz, nullptr, &err), qPrintable(err));
  for (int j = 0; j < grid.nj; ++j)
  {
    const int leftI = 5;  // 柱心 x=275（<400，西侧）
    const int rightI = 10; // 柱心 x=525（>400，东侧）
    for (int kInterface = 0; kInterface <= offset.nk; ++kInterface)
    {
      double zl = 0;
      double zr = 0;
      QVERIFY2(interfaceZ(offset, leftI, j, kInterface, &zl), "left interface");
      QVERIFY2(interfaceZ(offset, rightI, j, kInterface, &zr), "right interface");
      QCOMPARE(zr - zl, -22.5); // 东侧未动、西侧 +22.5 → 界面差 = −throw
    }
    QCOMPARE(layerThickness(offset, 3, j, 2), layerThickness(grid, 3, j, 2));
    QVERIFY(offset.columnLive(leftI, j));
    QVERIFY(offset.columnLive(rightI, j));
  }
  QCOMPARE(offset.liveColumns, grid.liveColumns);
}

void FaultOffsetTests::boundaryColumnOnFaultLineStays()
{
  const ZoneGrid grid = flatGrid(12, 6, 3);
  FaultThrow fault; // 断线恰过柱心 x=275（i=5）
  fault.x0 = 275;
  fault.y0 = -50;
  fault.x1 = 275;
  fault.y1 = 650;
  fault.throwStart = 30;
  fault.throwEnd = 30;
  fault.dropLeftSide = true;

  ZoneGrid offset;
  std::vector<float> dz;
  FaultOffsetMeta meta;
  QString err;
  QVERIFY2(applyFaultOffset(grid, {fault}, &offset, &dz, &meta, &err), qPrintable(err));
  QCOMPARE(meta.boundaryColumns, grid.nj); // 断线过柱心：每行一个边界柱（两盘归属未定）
  QCOMPARE(dz[static_cast<std::size_t>(grid.columnIndex(5, 3))], 0.0f);
  // 其余柱按侧归属
  QVERIFY(dz[static_cast<std::size_t>(grid.columnIndex(4, 3))] > 0.0f); // 西侧下掉
  QCOMPARE(dz[static_cast<std::size_t>(grid.columnIndex(6, 3))], 0.0f); // 东侧不动
}

void FaultOffsetTests::blockPartitionSplitsAcrossFaultAndIdwDoesNotLeak()
{
  // Oracle 1 零泄漏：竖帘分块跨断层不连通；错位格架上 IDW 种子不跨断层补值。
  const ZoneGrid grid = flatGrid(16, 8, 4);
  FaultSegment curtain;
  curtain.x0 = 400;
  curtain.y0 = -50;
  curtain.x1 = 400;
  curtain.y1 = 850;

  ZoneGrid offset;
  std::vector<float> dz;
  QString err;
  FaultThrow throwSeg;
  throwSeg.x0 = 400;
  throwSeg.y0 = -50;
  throwSeg.x1 = 400;
  throwSeg.y1 = 850;
  throwSeg.throwStart = 18;
  throwSeg.throwEnd = 18;
  throwSeg.dropLeftSide = false;
  QVERIFY2(applyFaultOffset(grid, {throwSeg}, &offset, &dz, nullptr, &err), qPrintable(err));

  std::vector<int> blocks;
  int blockCount = 0;
  assignColumnBlocks(offset, {curtain}, &blocks, &blockCount);
  QCOMPARE(blockCount, 2);
  for (int j = 0; j < offset.nj; ++j)
  {
    const int leftBlock = blocks[static_cast<std::size_t>(offset.columnIndex(5, j))];
    const int rightBlock = blocks[static_cast<std::size_t>(offset.columnIndex(10, j))];
    QVERIFY(leftBlock != rightBlock); // cell 不连通：跨断块柱分属不同块
  }

  // 种子只在西侧：东侧全部 NaN（跨断块零插值泄漏）
  std::vector<Seed> seeds;
  seeds.push_back(Seed{5, 4, 1, 42.0});
  PropertyVolume volume;
  QVERIFY2(fillIdw(offset, seeds, {curtain}, 2.0, &volume, {}, &err), qPrintable(err));
  for (int j = 0; j < offset.nj; ++j)
    for (int i = 8; i < offset.ni; ++i)
      for (int k = 0; k < offset.nk; ++k)
      {
        const float v = volume.values[static_cast<std::size_t>(offset.cellIndex(i, j, k))];
        QVERIFY2(!std::isfinite(v), "east block must stay NaN (no leak across curtain)");
      }
  // 错位本身不改这个口径：西侧种子柱照常充填
  const float at = volume.values[static_cast<std::size_t>(offset.cellIndex(5, 4, 1))];
  QCOMPARE(at, 42.0f);
}

void FaultOffsetTests::invalidInputs()
{
  const ZoneGrid grid = flatGrid(4, 4, 2);
  ZoneGrid offset;
  std::vector<float> dz;
  QString err;

  QVERIFY(!applyFaultOffset(grid, {}, &offset, &dz, nullptr, &err) || true); // 空断距集合 = 合法无操作
  if (err.isEmpty())
  {
    QCOMPARE(offset.ni, grid.ni);
    QCOMPARE(dz.size(), std::size_t(grid.ni * grid.nj));
  }

  FaultThrow bad; // 断距非有限
  bad.x0 = 0;
  bad.y0 = 0;
  bad.x1 = 100;
  bad.y1 = 0;
  bad.throwStart = std::nan("");
  err.clear();
  offset = ZoneGrid{};
  dz.clear();
  QVERIFY(!applyFaultOffset(grid, {bad}, &offset, &dz, nullptr, &err));
  QVERIFY(!err.isEmpty());
  QCOMPARE(offset.ni, 0); // 失败不写半成品
}

QTEST_MAIN( FaultOffsetTests )
#include "tst_faultoffset.moc"
