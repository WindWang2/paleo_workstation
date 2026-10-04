// 层：数据（测试壳位于 tests/，被测对象为数据层纯数值核）
#include <QtTest/QtTest>

#include "algorithms/evolution/compare.h"
#include "algorithms/evolution/types.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace paleo::evolution;

namespace
{

constexpr double kEps = 1e-6;

// 域 = [originX, originX+cols*cell] × [originY-rows*cell, originY]（北向上，
// originY 是左上像元边）。缺省原点让域落在正象限，多边形坐标可直接写正值。
GridSpec makeGrid( int cols, int rows, double cell, double originX = 0 )
{
  GridSpec grid;
  grid.cols = cols;
  grid.rows = rows;
  grid.originX = originX;
  grid.originY = rows * cell;
  grid.pixelWidth = cell;
  grid.pixelHeight = -cell;
  grid.crs = "ENGCRS[\"test local grid\"]";
  return grid;
}

FaciesPatch patch( int code, double x0, double y0, double x1, double y1 )
{
  FaciesPatch p;
  p.faciesCode = code;
  p.geometry.exterior.points = {
    { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 }, { x0, y0 }
  };
  return p;
}

FaciesCoverage coverage( const std::string &horizon, const GridSpec &grid,
                         std::initializer_list<FaciesPatch> patches )
{
  FaciesCoverage c;
  c.horizon = horizon;
  c.grid = grid;
  c.patches.assign( patches );
  return c;
}

const FaciesChange *changeFor( const EvolutionResult &result, int code )
{
  for ( const FaciesChange &change : result.changes )
    if ( change.faciesCode == code )
      return &change;
  return nullptr;
}

double overlapArea( const EvolutionResult &result, int earlierCode, int laterCode )
{
  for ( const OverlapCell &cell : result.overlap )
    if ( cell.earlierCode == earlierCode && cell.laterCode == laterCode )
      return cell.area;
  return 0;
}

} // namespace

class TestEvolutionKernel : public QObject
{
  Q_OBJECT
  private slots:
    // Oracle 1：不同格网/坐标域 → 明确拒算，原因可读。
    void refusesDifferentGridSize();
    void refusesDifferentOrigin();
    void refusesDifferentCrs();
    void refusesEmptyCoverage();

    // Oracle 2：构造已知位移的相带对 → 位移方向/幅度断言。
    void syntheticDisplacement();

    // Oracle 3：三相连三期 → 面积增减/重叠率逐格断言。
    void threePeriodAreaMatrix();

    void vanishedAndNewFacies();
    void cancellationStops();
    void identicalCoverageIsStable();
    void methodNoteIsHonest();
};

void TestEvolutionKernel::refusesDifferentGridSize()
{
  const FaciesCoverage a = coverage( "H2", makeGrid( 20, 20, 10 ),
                                     { patch( 1, 0, 0, 100, 100 ) } );
  const FaciesCoverage b = coverage( "H1", makeGrid( 30, 20, 10 ),
                                     { patch( 1, 0, 0, 100, 100 ) } );
  const EvolutionResult result = compareFacies( a, b );
  QCOMPARE( result.status, Status::InvalidInput );
  QVERIFY( result.message.find( "格网尺寸" ) != std::string::npos );
  QVERIFY( result.message.find( "拒算" ) != std::string::npos );
}

void TestEvolutionKernel::refusesDifferentOrigin()
{
  const FaciesCoverage a = coverage( "H2", makeGrid( 20, 20, 10 ),
                                     { patch( 1, 0, 0, 100, 100 ) } );
  const FaciesCoverage b = coverage( "H1", makeGrid( 20, 20, 10, 50 ),
                                     { patch( 1, 50, 0, 150, 100 ) } );
  const EvolutionResult result = compareFacies( a, b );
  QCOMPARE( result.status, Status::InvalidInput );
  QVERIFY( result.message.find( "原点" ) != std::string::npos );
}

void TestEvolutionKernel::refusesDifferentCrs()
{
  FaciesCoverage a = coverage( "H2", makeGrid( 20, 20, 10 ), { patch( 1, 0, 0, 100, 100 ) } );
  FaciesCoverage b = coverage( "H1", makeGrid( 20, 20, 10 ), { patch( 1, 0, 0, 100, 100 ) } );
  b.grid.crs = "ENGCRS[\"another grid\"]";
  const EvolutionResult result = compareFacies( a, b );
  QCOMPARE( result.status, Status::InvalidInput );
  QVERIFY( result.message.find( "坐标参考系" ) != std::string::npos );
}

void TestEvolutionKernel::refusesEmptyCoverage()
{
  const FaciesCoverage a = coverage( "H2", makeGrid( 20, 20, 10 ),
                                     { patch( 1, 0, 0, 100, 100 ) } );
  const FaciesCoverage b = coverage( "H1", makeGrid( 20, 20, 10 ), {} );
  const EvolutionResult result = compareFacies( a, b );
  QCOMPARE( result.status, Status::InvalidInput );
  QVERIFY( !result.message.empty() );
}

void TestEvolutionKernel::syntheticDisplacement()
{
  // 单前缘进积构造（无拐角污染）：相 1 = 域南半部整幅条带，域 [0,200]²、
  // 像元 10。早期 y∈[0,50]，晚期前缘进积到 y=90。带的三条边贴图幅框
  //（非前缘，被剔除），唯一前缘 = 顶边：每采样向北 40、全进。
  const FaciesCoverage earlier = coverage( "H2", makeGrid( 20, 20, 10 ),
                                           { patch( 1, 0, 0, 200, 50 ) } );
  const FaciesCoverage later = coverage( "H1", makeGrid( 20, 20, 10 ),
                                         { patch( 1, 0, 0, 200, 90 ) } );
  const EvolutionResult result = compareFacies( earlier, later );
  QCOMPARE( result.status, Status::Ok );

  QCOMPARE( result.faciesCodes.size(), static_cast<std::size_t>( 1 ) );
  const FaciesChange *change = changeFor( result, 1 );
  QVERIFY( change != nullptr );

  QCOMPARE( change->areaEarlier, 10000.0 );
  QCOMPARE( change->areaLater, 18000.0 );
  QVERIFY( std::fabs( change->areaChange - 8000.0 ) < kEps );
  QVERIFY( std::fabs( change->areaChangeRatio - 0.8 ) < kEps );

  // 质心：条带单侧增长 40（y:50→90 顶面），面积同步翻近倍 → 质心仅北移 20
  //（质心位移是面积加权结果，不等于前缘位移——两者都要如实报）。
  QVERIFY( change->centroidsValid );
  QVERIFY( std::fabs( change->centroidLater.x - change->centroidEarlier.x ) < kEps );
  QVERIFY( std::fabs( change->centroidLater.y - change->centroidEarlier.y - 20.0 ) < kEps );
  QVERIFY( std::fabs( change->centroidDisplacement - 20.0 ) < kEps );

  QVERIFY( std::fabs( overlapArea( result, 1, 1 ) - 10000.0 ) < kEps );
  QVERIFY( std::fabs( result.totalAreaEarlier - 10000.0 ) < kEps );
  QVERIFY( std::fabs( result.unchangedArea - 10000.0 ) < kEps );
  QVERIFY( std::fabs( result.faciesTurnoverRatio ) < kEps );

  // 前缘位移场：全部正北 40m、全进、方向一致（合长度 1）。
  QVERIFY( change->boundarySamples > 0 );
  QVERIFY( std::fabs( change->boundaryMedianShift - 40.0 ) < 1e-3 );
  QVERIFY( std::fabs( change->boundaryAdvanceRatio - 1.0 ) < 1e-3 );
  // 北 = 0°：方位角按环绕角断言（359.9° 与 0.1° 同为近北——纯北位移的
  // dx 浮点残差可正可负，归一化后落在 360 邻域而非负值域）。
  const double az = change->boundaryMedianAzimuthDeg;
  const double northDist = std::min( std::fabs( az ), std::fabs( 360.0 - az ) );
  QVERIFY2( northDist < 1.0,
            qPrintable( QStringLiteral( "azimuth=%1" ).arg( az ) ) );
  QVERIFY( std::fabs( change->boundaryResultant - 1.0 ) < 1e-3 );

  QCOMPARE( result.boundaryField.size(), static_cast<std::size_t>( change->boundarySamples ) );
  for ( const BoundaryVector &vector : result.boundaryField )
  {
    QVERIFY( vector.advance );
    QVERIFY( vector.faciesCode == 1 );
    QVERIFY( std::fabs( vector.to.y - vector.from.y - 40.0 ) < 1e-3 );
    QVERIFY( std::fabs( vector.to.x - vector.from.x ) < 1e-3 );
  }
}

void TestEvolutionKernel::threePeriodAreaMatrix()
{
  // 三相连三期，三条南北向整幅相带（域 [0,300]×[0,300]，像元 10）：
  //   P3（最老）：1=[0,100] 2=[100,200] 3=[200,300]（满铺）
  //   P2：整体东移 50——西缘让出 (0,50) 无相缺口，3 收窄到 [250,300]
  //   P1（最新）：整体西移 40（退积）——3=[210,260]
  const GridSpec grid = makeGrid( 30, 30, 10 );
  const FaciesCoverage p3 = coverage( "P3", grid,
                                      { patch( 1, 0, 0, 100, 300 ),
                                        patch( 2, 100, 0, 200, 300 ),
                                        patch( 3, 200, 0, 300, 300 ) } );
  const FaciesCoverage p2 = coverage( "P2", grid,
                                      { patch( 1, 50, 0, 150, 300 ),
                                        patch( 2, 150, 0, 250, 300 ),
                                        patch( 3, 250, 0, 300, 300 ) } );
  const FaciesCoverage p1 = coverage( "P1", grid,
                                      { patch( 1, 10, 0, 110, 300 ),
                                        patch( 2, 110, 0, 210, 300 ),
                                        patch( 3, 210, 0, 260, 300 ) } );

  // ---- P3 → P2（东移 50 进积）----
  const EvolutionResult forward = compareFacies( p3, p2 );
  QCOMPARE( forward.status, Status::Ok );
  QCOMPARE( forward.faciesCodes.size(), static_cast<std::size_t>( 3 ) );

  const FaciesChange *f1 = changeFor( forward, 1 );
  const FaciesChange *f2 = changeFor( forward, 2 );
  const FaciesChange *f3 = changeFor( forward, 3 );
  QVERIFY( f1 && f2 && f3 );
  // 相 1/2 面积守恒、质心东移 50；相 3 西界不动东界东移 → 收窄 10000。
  QVERIFY( std::fabs( f1->areaEarlier - 30000.0 ) < kEps );
  QVERIFY( std::fabs( f1->areaChange ) < kEps );
  QVERIFY( f1->centroidsValid );
  QVERIFY( std::fabs( f1->centroidDisplacement - 50.0 ) < kEps );
  QVERIFY( std::fabs( f2->areaChange ) < kEps );
  QVERIFY( std::fabs( f2->centroidDisplacement - 50.0 ) < kEps );
  QVERIFY( std::fabs( f3->areaEarlier - 30000.0 ) < kEps );
  QVERIFY( std::fabs( f3->areaLater - 15000.0 ) < kEps );
  QVERIFY( std::fabs( f3->areaChange + 15000.0 ) < kEps );
  QVERIFY( std::fabs( f3->areaChangeRatio + 0.5 ) < kEps );
  QVERIFY( std::fabs( f3->centroidDisplacement - 25.0 ) < kEps ); // [200,300]→[250,300]

  // 逐格重叠（东移 50、西缘留缺口）：各相一半留住原位、一半让给西侧邻相
  // 的东半——注意换相发生在 (2,1)/(3,2)，不是 (1,2)/(2,3)。
  QVERIFY( std::fabs( overlapArea( forward, 1, 1 ) - 15000.0 ) < kEps );
  QVERIFY( std::fabs( overlapArea( forward, 1, 2 ) ) < kEps );
  QVERIFY( std::fabs( overlapArea( forward, 2, 1 ) - 15000.0 ) < kEps );
  QVERIFY( std::fabs( overlapArea( forward, 2, 2 ) - 15000.0 ) < kEps );
  QVERIFY( std::fabs( overlapArea( forward, 2, 3 ) ) < kEps );
  QVERIFY( std::fabs( overlapArea( forward, 3, 2 ) - 15000.0 ) < kEps );
  QVERIFY( std::fabs( overlapArea( forward, 3, 3 ) - 15000.0 ) < kEps );
  QVERIFY( std::fabs( overlapArea( forward, 3, 1 ) ) < kEps );
  QVERIFY( std::fabs( forward.totalAreaEarlier - 90000.0 ) < kEps );
  QVERIFY( std::fabs( forward.totalAreaLater - 75000.0 ) < kEps );
  // 变更率 = 1 − 45000/90000 = 0.5（西缘缺口一半换相）。
  QVERIFY( std::fabs( forward.faciesTurnoverRatio - 0.5 ) < kEps );

  // ---- P2 → P1（西移 40 退积）----
  const EvolutionResult backward = compareFacies( p2, p1 );
  QCOMPARE( backward.status, Status::Ok );
  for ( const FaciesChange &change : backward.changes )
  {
    QVERIFY( change.centroidsValid );
    QVERIFY( std::fabs( change.centroidDisplacement - 40.0 ) < kEps );
    QVERIFY( std::fabs( change.centroidEarlier.x - change.centroidLater.x - 40.0 ) < kEps );
  }
  QVERIFY( std::fabs( overlapArea( backward, 1, 1 ) - 18000.0 ) < kEps );
  QVERIFY( std::fabs( overlapArea( backward, 1, 2 ) - 12000.0 ) < kEps );
  QVERIFY( std::fabs( overlapArea( backward, 3, 3 ) - 3000.0 ) < kEps );
  QVERIFY( std::fabs( backward.faciesTurnoverRatio - ( 1.0 - 39000.0 / 75000.0 ) ) < kEps );
}

void TestEvolutionKernel::vanishedAndNewFacies()
{
  // 早期相 5 在晚期消失；晚期新增相 6。
  const GridSpec grid = makeGrid( 20, 20, 10 );
  const FaciesCoverage earlier = coverage( "H2", grid, { patch( 5, 10, 10, 60, 60 ) } );
  const FaciesCoverage later = coverage( "H1", grid, { patch( 6, 10, 10, 60, 60 ) } );
  const EvolutionResult result = compareFacies( earlier, later );
  QCOMPARE( result.status, Status::Ok );
  QCOMPARE( result.faciesCodes.size(), static_cast<std::size_t>( 2 ) );

  const FaciesChange *vanished = changeFor( result, 5 );
  QVERIFY( vanished != nullptr );
  QVERIFY( std::fabs( vanished->areaEarlier - 2500.0 ) < kEps );
  QVERIFY( std::fabs( vanished->areaLater ) < kEps );
  QVERIFY( std::fabs( vanished->areaChange + 2500.0 ) < kEps );
  QVERIFY( std::fabs( vanished->areaChangeRatio + 1.0 ) < kEps ); // 全失
  QVERIFY( !vanished->centroidsValid );
  QVERIFY( std::isnan( vanished->centroidDisplacement ) );
  QVERIFY( vanished->boundarySamples == 0 ); // 对期无同相边界，无可评前缘

  const FaciesChange *appeared = changeFor( result, 6 );
  QVERIFY( appeared != nullptr );
  QVERIFY( std::isnan( appeared->areaChangeRatio ) ); // 早期为零 → 无比值
  QVERIFY( !appeared->centroidsValid );

  // 完全换相：对角重叠为零，变更率 1。
  QVERIFY( std::fabs( overlapArea( result, 5, 5 ) ) < kEps );
  QVERIFY( std::fabs( overlapArea( result, 5, 6 ) - 2500.0 ) < kEps );
  QVERIFY( std::fabs( result.faciesTurnoverRatio - 1.0 ) < kEps );
}

void TestEvolutionKernel::cancellationStops()
{
  const GridSpec grid = makeGrid( 20, 20, 10 );
  const FaciesCoverage a = coverage( "H2", grid, { patch( 1, 0, 0, 100, 100 ) } );
  const FaciesCoverage b = coverage( "H1", grid, { patch( 1, 0, 0, 100, 100 ) } );
  Control control;
  int calls = 0;
  control.cancelled = [&calls] {
    return ++calls > 1; // 第二次检查起取消
  };
  const EvolutionResult result = compareFacies( a, b, CompareOptions(), control );
  QCOMPARE( result.status, Status::Cancelled );
}

void TestEvolutionKernel::identicalCoverageIsStable()
{
  const GridSpec grid = makeGrid( 20, 20, 10 );
  const FaciesCoverage a = coverage( "H2", grid, { patch( 1, 0, 0, 100, 100 ) } );
  const FaciesCoverage b = coverage( "H1", grid, { patch( 1, 0, 0, 100, 100 ) } );
  const EvolutionResult result = compareFacies( a, b );
  QCOMPARE( result.status, Status::Ok );
  const FaciesChange *change = changeFor( result, 1 );
  QVERIFY( change );
  QVERIFY( std::fabs( change->centroidDisplacement ) < kEps );
  QVERIFY( std::fabs( result.faciesTurnoverRatio ) < kEps );
  // 同形不动：前缘采样到对期边界的最近点就是自身 → 位移近零。
  QVERIFY( std::isnan( change->boundaryMedianShift ) || change->boundaryMedianShift < 1e-6 );
}

void TestEvolutionKernel::methodNoteIsHonest()
{
  const GridSpec grid = makeGrid( 20, 20, 10 );
  const FaciesCoverage a = coverage( "H2", grid, { patch( 1, 0, 0, 200, 50 ) } );
  const FaciesCoverage b = coverage( "H1", grid, { patch( 1, 0, 0, 200, 90 ) } );
  const EvolutionResult result = compareFacies( a, b );
  QCOMPARE( result.status, Status::Ok );
  // 口径注记必须如实标注：几何近似 + 无井控加权 + 图幅框剔除。
  QVERIFY( result.methodNote.find( "几何近似" ) != std::string::npos );
  QVERIFY( result.methodNote.find( "井控密度加权" ) != std::string::npos );
  QVERIFY( result.methodNote.find( "图幅边框" ) != std::string::npos );
}

QTEST_MAIN( TestEvolutionKernel )
#include "tst_evolution_kernel.moc"

