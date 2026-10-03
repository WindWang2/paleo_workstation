// 层：数据（测试壳位于 tests/，被测对象为数据层纯数值核）
#include <QtTest/QtTest>

#include "algorithms/singlefactor/partition.h"
#include "algorithms/singlefactor/support.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace paleo::singlefactor;

namespace
{

GridSpec grid10()
{
  GridSpec grid;
  grid.cols = 10;
  grid.rows = 10;
  grid.originX = 0;
  grid.originY = 10;
  grid.pixelWidth = 1;
  grid.pixelHeight = -1;
  return grid;
}

Polygon rectBoundary( double x0, double y0, double x1, double y1 )
{
  Polygon poly;
  poly.exterior.points = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 }, { x0, y0 } };
  return poly;
}

BarrierSpec barrier( const char *id, std::vector<Point2> points )
{
  BarrierSpec out;
  out.lineId = id;
  out.points = std::move( points );
  return out;
}

Sample well( const char *id, double x, double y )
{
  Sample sample;
  sample.stableRowId = id;
  sample.wellId = id;
  sample.x = x;
  sample.y = y;
  sample.value = 1;
  return sample;
}

std::size_t cell( int col, int row )
{
  return static_cast<std::size_t>( row ) * 10 + static_cast<std::size_t>( col );
}

std::vector<std::uint8_t> fullDomain()
{
  return std::vector<std::uint8_t>( 100, std::uint8_t{ 1 } );
}

} // namespace

class SingleFactorPartitionTests : public QObject
{
  Q_OBJECT
  private slots:
    void interpretationPartitionSplitsWithoutSharedWells();
    void freeEndsExtendToDomainBoundary();
    void nodeSafeFloodDoesNotLeakAcrossBarrier();
    void wellAssignmentExclusiveVersusShared();
    void barrierGridAdapterSplitsWhereHardBarrierWraps();
};

void SingleFactorPartitionTests::interpretationPartitionSplitsWithoutSharedWells()
{
  const GridSpec grid = grid10();
  const std::vector<BarrierSpec> walls{ barrier( "w", { { 5, 3 }, { 5, 7 } } ) };
  const std::vector<Point2> wells{ { 2, 5 }, { 8, 5 }, { 2, 8 }, { 8, 2 } };
  const std::vector<Polygon> boundaries{ rectBoundary( 0, 0, 10, 10 ) };

  const PartitionResult result =
      buildPartition( grid, fullDomain(), walls, wells, boundaries, true );
  QCOMPARE( result.mode.c_str(), "interpretation" );
  QCOMPARE( result.regionCount, 2 );
  QVERIFY( result.complete );
  QVERIFY( result.conflicts.empty() );

  // 两侧分区井不共享：左列两口同区，右列两口同区，两区互异，无 -2 共享井。
  const int leftA = result.wellRegionIds[0];
  const int rightA = result.wellRegionIds[1];
  QVERIFY( leftA >= 0 );
  QVERIFY( rightA >= 0 );
  QCOMPARE( leftA, result.wellRegionIds[2] );
  QCOMPARE( rightA, result.wellRegionIds[3] );
  QVERIFY( leftA != rightA );
  for ( int id : result.wellRegionIds )
    QVERIFY( id != kWellShared );

  // 分区 id 稳定：同输入重跑逐格一致。
  const PartitionResult again =
      buildPartition( grid, fullDomain(), walls, wells, boundaries, true );
  QCOMPARE( again.regionIds, result.regionIds );
  QCOMPARE( again.wellRegionIds, result.wellRegionIds );

  // local 模式（不延端）同一屏障可绕自由端回通：单区、两侧井同区。
  const PartitionResult local =
      buildPartition( grid, fullDomain(), walls, wells, boundaries, false );
  QCOMPARE( local.mode.c_str(), "local" );
  QCOMPARE( local.regionCount, 1 );
  QVERIFY( local.wellRegionIds[0] >= 0 );
  QCOMPARE( local.wellRegionIds[0], local.wellRegionIds[1] );
}

void SingleFactorPartitionTests::freeEndsExtendToDomainBoundary()
{
  const std::vector<Polygon> boundaries{ rectBoundary( 0, 0, 10, 10 ) };

  // L 形屏障：两个自由端都延到域边界。
  const std::vector<BarrierSpec> lWalls{ barrier( "L", { { 5, 5 }, { 5, 9 }, { 8, 9 } } ) };
  const ExtendResult lShaped = extendBarriersToDomain( lWalls, boundaries, 0.6, 15.0 );
  QCOMPARE( lShaped.conflicts.size(), std::size_t{ 0 } );
  QCOMPARE( lShaped.extensions.size(), std::size_t{ 2 } );
  QCOMPARE( lShaped.extensions[0].end.c_str(), "start" );
  QVERIFY( std::abs( lShaped.extensions[0].from.x - 5.0 ) <= 1e-9 );
  QVERIFY( std::abs( lShaped.extensions[0].from.y ) <= 1e-9 ); // 延到 y=0
  QCOMPARE( lShaped.extensions[1].end.c_str(), "end" );
  QVERIFY( std::abs( lShaped.extensions[1].to.x - 10.0 ) <= 1e-9 ); // 延到 x=10
  QVERIFY( std::abs( lShaped.extensions[1].to.y - 9.0 ) <= 1e-9 );
  const std::vector<Point2> &extended = lShaped.extended.front().points;
  QCOMPARE( extended.size(), std::size_t{ 5 } );
  QVERIFY( std::abs( extended.front().y ) <= 1e-9 );
  QVERIFY( std::abs( extended.back().x - 10.0 ) <= 1e-9 );

  // 已落在边界上的端不再延长：仅起点端延到对边。
  const std::vector<BarrierSpec> halfWalls{ barrier( "half", { { 2, 2 }, { 2, 0 } } ) };
  const ExtendResult onBoundary = extendBarriersToDomain( halfWalls, boundaries, 0.6, 15.0 );
  QCOMPARE( onBoundary.extensions.size(), std::size_t{ 1 } );
  QCOMPARE( onBoundary.extensions[0].end.c_str(), "start" );
  QVERIFY( std::abs( onBoundary.extensions[0].from.y - 10.0 ) <= 1e-9 );

  // 无域边界：原样返回并记冲突。
  const std::vector<BarrierSpec> plainWalls{ barrier( "w", { { 5, 3 }, { 5, 7 } } ) };
  const ExtendResult noBoundary = extendBarriersToDomain( plainWalls, {}, 0.6, 15.0 );
  QCOMPARE( noBoundary.extended.size(), std::size_t{ 1 } );
  QCOMPARE( noBoundary.conflicts.size(), std::size_t{ 1 } );
  QCOMPARE( noBoundary.conflicts.front().c_str(), "no_domain_boundary" );
}

void SingleFactorPartitionTests::nodeSafeFloodDoesNotLeakAcrossBarrier()
{
  // 贴线相切不算穿，横越算穿。
  const std::vector<BarrierSpec> walls{ barrier( "w", { { 5, 0 }, { 5, 10 } } ) };
  QVERIFY( partitionEdgeBlocked( { 4.9, 5 }, { 5.1, 5 }, walls, 0.25 ) );
  QVERIFY( !partitionEdgeBlocked( { 4, 5 }, { 4.5, 5 }, walls, 0.25 ) );
  QVERIFY( pointOnBarriers( { 5, 5 }, walls, 0.25 ) );
  QVERIFY( !pointOnBarriers( { 5.4, 5 }, walls, 0.25 ) );

  // 墙穿格心：墙上格保持未标（不被单侧吸收），两侧格分区互异。
  const GridSpec grid = grid10();
  const std::vector<BarrierSpec> centerWall{ barrier( "w", { { 5.5, 0 }, { 5.5, 10 } } ) };
  const std::vector<int> labels = buildRegionLabelsNodeSafe( grid, fullDomain(), centerWall );
  QCOMPARE( labels.size(), std::size_t{ 100 } );
  const std::size_t left = cell( 4, 5 );
  const std::size_t right = cell( 6, 5 );
  const std::size_t onWall = cell( 5, 5 );
  QVERIFY( labels[left] >= 0 );
  QVERIFY( labels[right] >= 0 );
  QVERIFY( labels[left] != labels[right] );
  QCOMPARE( labels[onWall], kRegionOutside ); // 两侧各一区，不单侧吸收
}

void SingleFactorPartitionTests::wellAssignmentExclusiveVersusShared()
{
  const GridSpec grid = grid10();
  const std::vector<BarrierSpec> walls{ barrier( "w", { { 5, 0 }, { 5, 10 } } ) };
  const std::vector<int> labels = buildRegionLabelsNodeSafe( grid, fullDomain(), walls );

  const std::vector<Point2> wells{ { 2, 5 }, { 8, 5 }, { 40, 40 } };
  const std::vector<int> exclusive = assignWellRegions( wells, grid, labels, true );
  QCOMPARE( exclusive[0], labels[cell( 2, 5 )] );
  QCOMPARE( exclusive[1], labels[cell( 8, 5 )] );
  QVERIFY( exclusive[0] != exclusive[1] );
  QCOMPARE( exclusive[2], kWellPending ); // 网外井：待定，不给共享值

  const std::vector<int> shared = assignWellRegions( wells, grid, labels, false );
  QCOMPARE( shared[2], kWellShared );

  QVERIFY( wellAllowedForCell( 0, 0, true ) );
  QVERIFY( !wellAllowedForCell( 0, 1, true ) );
  QVERIFY( !wellAllowedForCell( -1, 0, true ) );
  QVERIFY( !wellAllowedForCell( 0, kWellPending, false ) );
  QVERIFY( wellAllowedForCell( -1, 0, false ) );
  QVERIFY( wellAllowedForCell( 0, 0, false ) );
  QVERIFY( !wellAllowedForCell( 0, 1, false ) );
}

void SingleFactorPartitionTests::barrierGridAdapterSplitsWhereHardBarrierWraps()
{
  const GridSpec grid = grid10();
  std::vector<ConstraintLine> lines;
  // 墙穿格心（x=5.5），保证墙上格被记为屏障格。
  lines.push_back( ConstraintLine{ "wall", Semantic::HardBarrier,
                                   { { 5.5, 3 }, { 5.5, 7 } }, true, 8, 0, 0, 0.35, 0, 0, 0, {} } );
  const std::vector<Sample> samples{ well( "a", 2, 5 ), well( "b", 8, 5 ) };

  // 旧路径 grid_connectivity_v1：有限端可绕行 → 单连通区，井同区。
  const BarrierGrid hard = labelHardBarriers( grid, lines, samples, 1e-9 );
  QCOMPARE( hard.componentCount, 1 );
  QCOMPARE( hard.sampleComponent[0], hard.sampleComponent[1] );

  // 解释分区：自由端延界后成两区，井分属两侧。
  const BarrierGrid partitioned = labelInterpretationPartition( grid, lines, samples, 1e-9 );
  QCOMPARE( partitioned.componentCount, 2 );
  QVERIFY( partitioned.sampleComponent[0] >= 0 );
  QVERIFY( partitioned.sampleComponent[1] >= 0 );
  QVERIFY( partitioned.sampleComponent[0] != partitioned.sampleComponent[1] );
  QVERIFY( partitioned.barrierCells > 0 );
  QVERIFY( partitioned.ambiguous.empty() );
  for ( int id : partitioned.component )
    QVERIFY( id >= 0 || id == -2 );

  // 指定连通区恢复归属。
  std::vector<Sample> overridden = samples;
  overridden[0].componentOverride = 1;
  const BarrierGrid forced = labelInterpretationPartition( grid, lines, overridden, 1e-9 );
  QCOMPARE( forced.sampleComponent[0], 1 );
}

QTEST_MAIN( SingleFactorPartitionTests )
#include "tst_singlefactor_partition.moc"
