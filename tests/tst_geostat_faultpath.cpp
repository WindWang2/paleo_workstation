// 层：数据（测试壳位于 tests/，被测对象为断层绕距核）
#include <QtTest/QtTest>

#include "algorithms/geostat/faultpath.h"

#include <cmath>
#include <vector>

using namespace paleo::geostat;

namespace
{

GridSpec makeGrid( int cols, int rows, double x0, double y0, double cell )
{
  GridSpec grid;
  grid.cols = cols;
  grid.rows = rows;
  grid.originX = x0;
  grid.originY = y0;
  grid.pixelWidth = cell;
  grid.pixelHeight = -cell;
  return grid;
}

// 网格 81×81、像元 1：x∈[0,81)、y∈[0,81)，原点 (0,81)
GridSpec wallGrid()
{
  return makeGrid( 81, 81, 0, 81, 1 );
}

std::size_t cellAt( const GridSpec &grid, double x, double y )
{
  const int column = static_cast<int>( std::floor( ( x - grid.originX ) / grid.pixelWidth ) );
  const int row = static_cast<int>( std::floor( ( y - grid.originY ) / grid.pixelHeight ) );
  return static_cast<std::size_t>( row ) * grid.cols + column;
}

BarrierPolygon rectangle( double x0, double y0, double x1, double y1 )
{
  BarrierPolygon polygon;
  polygon.exterior.points = { Point2{ x0, y0 }, Point2{ x1, y0 }, Point2{ x1, y1 }, Point2{ x0, y1 } };
  return polygon;
}

} // namespace

class GeostatFaultPathTests : public QObject
{
  Q_OBJECT
private slots:
  void noBarrierMatchesEuclidean();
  void straightWallForcesDetour();
  void wallEndpointDetourMatchesAnalytic();
  void enclosedHoleIsUnreachable();
  void sourceOnBarrierSnapsOut();
  void tiltedBandSealsCornerGaps();
  void subCellThinBandSealsCornerGaps();
  void cancelledAndInvalid();
};

void GeostatFaultPathTests::noBarrierMatchesEuclidean()
{
  // 源点对准格心 (61,59)（60×60、像元 2、原点 (0,120)）
  const GridSpec grid = makeGrid( 60, 60, 0, 120, 2 );
  const FaultPathResult result = faultPathMetric( grid, {}, 61, 59 );
  QCOMPARE( result.status, Status::Ok );
  QCOMPARE( result.barrierCells, 0 );
  QCOMPARE( result.unreachableCells, 0 );
  QCOMPARE( result.reachedCells, 60 * 60 );

  // 轴向：10 步 ×2 = 精确欧氏
  QCOMPARE( result.distance[cellAt( grid, 81, 59 )], 20.0 );
  QCOMPARE( result.distance[cellAt( grid, 61, 39 )], 20.0 );
  // 斜向：8 邻接 chamfer 度量对欧氏的偏差上界 ~8.2%，取 9% 门
  const double diagonal = result.distance[cellAt( grid, 81, 45 )];
  const double euclid = std::hypot( 20.0, 14.0 );
  QVERIFY2( diagonal >= euclid, "grid metric never shorter than euclidean" );
  QVERIFY2( ( diagonal - euclid ) / euclid <= 0.09,
            qPrintable( QStringLiteral( "chamfer=%1 euclid=%2" ).arg( diagonal ).arg( euclid ) ) );
}

void GeostatFaultPathTests::straightWallForcesDetour()
{
  // 直墙 x∈[40,42]、y∈[20,60]：源 (20,40)，目标 (60,40)。
  // 直线 40 被墙挡死；绕行必须走端点。对称几何最短路 =
  // 2·sqrt(21²+20²) = 58（绕 (41,60) 或 (41,20)）。
  const GridSpec grid = wallGrid();
  const FaultPathResult result = faultPathMetric( grid, { rectangle( 40, 20, 42, 60 ) }, 20, 40 );
  QCOMPARE( result.status, Status::Ok );
  QVERIFY( result.barrierCells > 0 );
  const double detour = result.distance[cellAt( grid, 60, 40 )];
  QVERIFY2( detour > 45.0,
            qPrintable( QStringLiteral( "detour=%1 must exceed straight-line 40" ).arg( detour ) ) );
  QVERIFY2( detour <= 58.0 * 1.12,
            qPrintable( QStringLiteral( "detour=%1 exceeds analytic 58 by >12%%" ).arg( detour ) ) );
  // 墙正后方相邻格可达（绕行存在）
  QVERIFY( std::isfinite( result.distance[cellAt( grid, 44, 40 )] ) );
}

void GeostatFaultPathTests::wallEndpointDetourMatchesAnalytic()
{
  // 断层端点绕行正确：对称墙（中轴 x=41），源/目标镜像对称，
  // 测地距离 = 2·sqrt(21² + 20²) = 58（chamfer 容差 9%）
  const GridSpec grid = wallGrid();
  const FaultPathResult result = faultPathMetric( grid, { rectangle( 40, 20, 42, 60 ) }, 20, 40 );
  QCOMPARE( result.status, Status::Ok );
  const double analytic = 2.0 * std::hypot( 21.0, 20.0 );
  const double detour = result.distance[cellAt( grid, 60, 40 )];
  QVERIFY2( std::fabs( detour - analytic ) / analytic <= 0.09,
            qPrintable( QStringLiteral( "detour=%1 analytic=%2" ).arg( detour ).arg( analytic ) ) );
}

void GeostatFaultPathTests::enclosedHoleIsUnreachable()
{
  // 环形障碍：外 [30,50]²、洞 [36,44]²——洞内自由格被完全围死 → NaN
  BarrierPolygon annulus = rectangle( 30, 30, 50, 50 );
  annulus.holes.push_back( BarrierRing{ { Point2{ 36, 36 }, Point2{ 44, 36 }, Point2{ 44, 44 }, Point2{ 36, 44 } } } );
  const GridSpec grid = wallGrid();
  const FaultPathResult result = faultPathMetric( grid, { annulus }, 10, 40 );
  QCOMPARE( result.status, Status::Ok );
  QVERIFY( result.barrierCells > 0 );
  QVERIFY2( result.unreachableCells > 0,
            qPrintable( QStringLiteral( "unreachable=%1" ).arg( result.unreachableCells ) ) );
  QVERIFY( std::isnan( result.distance[cellAt( grid, 40, 40 )] ) ); // 洞心
  QVERIFY( std::isfinite( result.distance[cellAt( grid, 20, 40 )] ) ); // 环外
}

void GeostatFaultPathTests::sourceOnBarrierSnapsOut()
{
  // 源点正好落在墙上：螺旋外搜自由格，仍然 Ok
  const GridSpec grid = wallGrid();
  const FaultPathResult result = faultPathMetric( grid, { rectangle( 40, 20, 42, 60 ) }, 41, 40 );
  QCOMPARE( result.status, Status::Ok );
  QVERIFY( result.sourceColumn >= 0 && result.sourceRow >= 0 );
  QVERIFY( std::isfinite( result.distance[static_cast<std::size_t>( result.sourceRow ) * grid.cols + result.sourceColumn] ) );
  QVERIFY( result.reachedCells > 1000 );
}

void GeostatFaultPathTests::tiltedBandSealsCornerGaps()
{
  // issue #292（回归护栏）：30° 斜穿、总宽 1.0（>1 像元）的厚断层带——
  // 栅格化出的格子链足够厚（每列 ≥1 个内部格被 even-odd 填充命中），
  // 不存在只角接的障碍格对，无穿角检查也不漏。本例钉的是穿角检查
  // 不误伤粗带：修复后源（带下侧）仍不可达上侧目标，且不过度封锁。
  // 细带（亚格宽）的角点缝隙复现见 subCellThinBandSealsCornerGaps。
  const GridSpec grid = wallGrid(); // 81×81，x∈[0,81)，y∈[0,81)，原点 (0,81)
  constexpr double kPi = 3.14159265358979323846;
  const double slope = std::tan( 30.0 * kPi / 180.0 ); // ≈0.5774
  const double yLeft = 20.0;
  const double yRight = 20.0 + 81.0 * slope; // ≈66.79
  // 法向半宽 0.5（> 格心最大偏离 0.5/cosθ≈0.578 的一半，保证每列至少一个
  // 内部格），角点略外扩出网格由 cellColumn/Row 钳制到边列，带从左边封到右边。
  const double nlen = std::hypot( 1.0, slope );
  const double nx = -0.5 * slope / nlen;
  const double ny = 0.5 / nlen;
  BarrierPolygon band;
  band.exterior.points = {
      Point2{ 0.0 + nx, yLeft + ny },  Point2{ 81.0 + nx, yRight + ny },
      Point2{ 81.0 - nx, yRight - ny }, Point2{ 0.0 - nx, yLeft - ny } };

  const FaultPathResult result = faultPathMetric( grid, { band }, 40.5, 10.0 );
  QCOMPARE( result.status, Status::Ok );
  QVERIFY2( std::isnan( result.distance[cellAt( grid, 40.5, 75.0 )] ),
            "leak: target across a tilted band must stay unreachable" );
  QVERIFY2( result.unreachableCells > 1000,
            qPrintable( QStringLiteral( "unreachable=%1" ).arg( result.unreachableCells ) ) );
  QVERIFY( std::isfinite( result.distance[cellAt( grid, 40.5, 10.0 )] ) ); // 源自身

  // 对照：无屏障时同一目标可达（chamfer 度量 ≈ 欧氏 65）
  const FaultPathResult open = faultPathMetric( grid, {}, 40.5, 10.0 );
  QCOMPARE( open.status, Status::Ok );
  QVERIFY( std::isfinite( open.distance[cellAt( grid, 40.5, 75.0 )] ) );
}

void GeostatFaultPathTests::subCellThinBandSealsCornerGaps()
{
  // issue #292（复现）：30° 斜穿、总宽 0.2（<1 像元）的亚格宽薄断层带
  // ——even-odd 填充命中不了任何格心，屏障链只剩边界半像元采样出的
  // 单格宽斜链，相邻障碍格大量只角接。8 邻接对角步若无穿角检查会从
  // 角点缝隙斜穿，绕障距离退化成近欧氏距离（无检查时对侧目标可达、
  // 不可达计数归零）；修复后源（带下侧）不可达上侧目标：目标 NaN、
  // 不可达计数如实；无屏障对照组同一目标可达。
  const GridSpec grid = wallGrid(); // 81×81，x∈[0,81)，y∈[0,81)，原点 (0,81)
  constexpr double kPi = 3.14159265358979323846;
  const double slope = std::tan( 30.0 * kPi / 180.0 ); // ≈0.5774
  const double yLeft = 20.0;
  const double yRight = yLeft + 81.0 * slope; // ≈66.79
  // 法向半宽 0.1（总宽 0.2 < 1 像元）：每列格心都落在带外，屏障链
  // 完全由边界半像元采样决定——与 distancetransform 的折线采样同口径。
  const double nlen = std::hypot( 1.0, slope );
  const double nx = -0.1 * slope / nlen;
  const double ny = 0.1 / nlen;
  BarrierPolygon band;
  band.exterior.points = {
      Point2{ 0.0 + nx, yLeft + ny },  Point2{ 81.0 + nx, yRight + ny },
      Point2{ 81.0 - nx, yRight - ny }, Point2{ 0.0 - nx, yLeft - ny } };

  const FaultPathResult result = faultPathMetric( grid, { band }, 40.5, 10.0 );
  QCOMPARE( result.status, Status::Ok );
  QVERIFY2( std::isnan( result.distance[cellAt( grid, 40.5, 75.0 )] ),
            "leak: target across a sub-cell thin tilted band must stay unreachable" );
  QVERIFY2( result.unreachableCells > 2000,
            qPrintable( QStringLiteral( "unreachable=%1" ).arg( result.unreachableCells ) ) );
  QVERIFY( std::isfinite( result.distance[cellAt( grid, 40.5, 10.0 )] ) ); // 源自身

  // 对照：无屏障时同一目标可达（chamfer 度量 ≈ 欧氏 65）
  const FaultPathResult open = faultPathMetric( grid, {}, 40.5, 10.0 );
  QCOMPARE( open.status, Status::Ok );
  QVERIFY( std::isfinite( open.distance[cellAt( grid, 40.5, 75.0 )] ) );
}

void GeostatFaultPathTests::cancelledAndInvalid()
{
  const GridSpec small = makeGrid( 3, 3, 0, 3, 1 );
  QVERIFY( faultPathMetric( GridSpec{}, {}, 0, 0 ).status == Status::InvalidInput );
  // 源被大屏障盖死（无 5 格内自由格）
  const FaultPathResult buried = faultPathMetric( small, { rectangle( 0, 0, 3, 3 ) }, 1, 1 );
  QVERIFY( buried.status == Status::InvalidInput );

  Control control;
  control.cancelled = [] { return true; };
  const GridSpec big = makeGrid( 400, 400, 0, 400, 1 ); // 160k 格 > 65536 检查间隔
  QCOMPARE( faultPathMetric( big, {}, 200, 200, control ).status, Status::Cancelled );
}

QTEST_MAIN( GeostatFaultPathTests )
#include "tst_geostat_faultpath.moc"
