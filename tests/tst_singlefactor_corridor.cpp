// 层：数据（测试壳位于 tests/，被测对象为数据层纯数值核）
#include <QtTest/QtTest>

#include "algorithms/singlefactor/corridor.h"
#include "algorithms/singlefactor/support.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

using namespace paleo::singlefactor;

namespace
{

DirectionLineSpec spec( std::vector<Point2> points, double ratio, double core, double influence )
{
  DirectionLineSpec out;
  out.lineId = "d";
  out.points = std::move( points );
  out.ratio = ratio;
  out.coreRadius = core;
  out.influenceRadius = influence;
  return out;
}

GridSpec grid( int cols, int rows, double originX, double originY )
{
  GridSpec out;
  out.cols = cols;
  out.rows = rows;
  out.originX = originX;
  out.originY = originY;
  out.pixelWidth = 1;
  out.pixelHeight = -1;
  return out;
}

std::size_t cellOf( int col, int row, int cols )
{
  return static_cast<std::size_t>( row ) * static_cast<std::size_t>( cols ) +
         static_cast<std::size_t>( col );
}

} // namespace

class SingleFactorCorridorTests : public QObject
{
  Q_OBJECT
  private slots:
    void curveCoordinatesStraightenBentLine();
    void influenceEnvelopeCoreFullOutsideZeroMonotone();
    void alongTrackEnvelopeTapersBeyondTips();
    void resolveDirectionParamsFillsRadii();
    void polylineGeometryExtensionBookkeeping();
    void gridCacheAssignsControllingDirection();
    void gridCacheBlendsCompetingTangents();
    void ellipticalSearchStretchesAlongAxisOnly();
    void pairDistanceUsesCurveMetricOnlyWhenShared();
    void alongTrackProfilePinsTipsAndBlends();
};

void SingleFactorCorridorTests::curveCoordinatesStraightenBentLine()
{
  const PolylineGeometry geom = buildPolylineGeometry(
      spec( { { 0, 1 }, { 6, 1 }, { 6, 3 } }, 8, 1, 3 ), 0, 0 );
  QCOMPARE( geom.totalLength, 8.0 );

  // 弯折线上的点按弧长拉直，法距为零。
  const PolylineProjection onCornerArm = projectPointToPolyline( { 3, 1 }, geom );
  QVERIFY( std::abs( onCornerArm.s - 3.0 ) <= 1e-12 );
  QVERIFY( std::abs( onCornerArm.n ) <= 1e-12 );
  QVERIFY( std::abs( onCornerArm.tx - 1.0 ) <= 1e-12 );
  QVERIFY( std::abs( onCornerArm.ty ) <= 1e-12 );

  const PolylineProjection onSecondArm = projectPointToPolyline( { 6, 2 }, geom );
  QVERIFY( std::abs( onSecondArm.s - 7.0 ) <= 1e-12 );
  QVERIFY( std::abs( onSecondArm.n ) <= 1e-12 );
  QVERIFY( std::abs( onSecondArm.tx ) <= 1e-12 );
  QVERIFY( std::abs( onSecondArm.ty - 1.0 ) <= 1e-12 );

  // 左侧法距为正，右侧为负；s 保持投影弧长。
  const PolylineProjection left = projectPointToPolyline( { 3, 3 }, geom );
  QVERIFY( std::abs( left.n - 2.0 ) <= 1e-12 );
  QVERIFY( std::abs( left.s - 3.0 ) <= 1e-12 );
  QVERIFY( std::abs( left.distance - 2.0 ) <= 1e-12 );
  const PolylineProjection right = projectPointToPolyline( { 4, 0 }, geom );
  QVERIFY( std::abs( right.n + 1.0 ) <= 1e-12 );

  // 弯折线全线采样：s 单调、n 恒零（直线化断言）。
  double prevS = -1;
  for ( int i = 0; i <= 16; ++i )
  {
    const double t = i / 16.0;
    const Point2 p = { t <= 0.5 ? t * 12.0 : 6.0, t <= 0.5 ? 1.0 : ( t - 0.5 ) * 4.0 + 1.0 };
    const PolylineProjection proj = projectPointToPolyline( p, geom );
    QVERIFY( std::abs( proj.n ) <= 1e-12 );
    QVERIFY( proj.s > prevS );
    prevS = proj.s;
  }
}

void SingleFactorCorridorTests::influenceEnvelopeCoreFullOutsideZeroMonotone()
{
  const double core = 1.0;
  const double influence = 3.0;
  QCOMPARE( influenceStrength( 0.0, core, influence ), 1.0 );
  QCOMPARE( influenceStrength( 0.5, core, influence ), 1.0 );
  QCOMPARE( influenceStrength( 1.0, core, influence ), 1.0 );
  QCOMPARE( influenceStrength( 3.0, core, influence ), 0.0 );
  QCOMPARE( influenceStrength( 9.0, core, influence ), 0.0 );

  // 指数公式抽查：t=0.5、k=3 → (e^-1.5 - e^-3)/(1 - e^-3)。
  const double expected = ( std::exp( -1.5 ) - std::exp( -3.0 ) ) / ( 1.0 - std::exp( -3.0 ) );
  QVERIFY( std::abs( influenceStrength( 2.0, core, influence ) - expected ) <= 1e-12 );

  // 过渡带单调衰减（core 与 influence 之间不回升）。
  double prev = 1.0;
  for ( int i = 0; i <= 40; ++i )
  {
    const double d = core + ( influence - core ) * i / 40.0;
    const double g = influenceStrength( d, core, influence );
    QVERIFY( g <= prev + 1e-15 );
    prev = g;
  }
  QVERIFY( prev <= 1e-9 );
}

void SingleFactorCorridorTests::alongTrackEnvelopeTapersBeyondTips()
{
  QCOMPARE( alongTrackEnvelope( 3.0, 2.0, 6.0 ), 1.0 );
  QCOMPARE( alongTrackEnvelope( 2.0, 2.0, 6.0 ), 1.0 );
  QCOMPARE( alongTrackEnvelope( 6.0, 2.0, 6.0 ), 1.0 );
  // 显式尖端：s0-tip 处降为零，半程为 smoothstep(0.5)=0.5。
  QCOMPARE( alongTrackEnvelope( 0.0, 2.0, 6.0, 2.0 ), 0.0 );
  QVERIFY( std::abs( alongTrackEnvelope( 1.0, 2.0, 6.0, 2.0 ) - 0.5 ) <= 1e-12 );
  QVERIFY( std::abs( alongTrackEnvelope( 7.0, 2.0, 6.0, 2.0 ) - 0.5 ) <= 1e-12 );
  // mode=none：线跨之外即刻为零。
  QCOMPARE( alongTrackEnvelope( 1.0, 2.0, 6.0, 2.0, "none" ), 0.0 );
  // 缺省尖端 = 10% 线长：s0-0.4 以外全零。
  QCOMPARE( alongTrackEnvelope( 1.5, 2.0, 6.0 ), 0.0 );
  QVERIFY( alongTrackEnvelope( 1.9, 2.0, 6.0 ) > 0.0 );
}

void SingleFactorCorridorTests::resolveDirectionParamsFillsRadii()
{
  std::vector<DirectionLineSpec> specs;
  specs.push_back( spec( { { 0, 0 }, { 6, 0 } }, 12, 0, 0 ) ); // 全自动
  specs.push_back( spec( { { 0, 5 }, { 6, 5 } }, 8, 0, 2.0 ) ); // 只给影响半径
  specs.push_back( spec( { { 0, 9 }, { 6, 9 } }, 8, 5.0, 4.0 ) ); // core > influence 双显式
  specs.push_back( spec( { { 0, 9 }, { 0, 9.0 } }, 8, 1, 2 ) ); // 退化线，剔除
  specs.back().active = false;

  const std::vector<DirectionLineSpec> resolved =
      resolveDirectionParams( specs, 1.0, 1.0, 20.0 );
  QCOMPARE( resolved.size(), std::size_t{ 3 } );
  for ( const DirectionLineSpec &item : resolved )
  {
    QVERIFY( item.influenceRadius > item.coreRadius );
    QVERIFY( item.influenceRadius <= 20.0 * 0.38 + 1e-12 );
    QVERIFY( item.transition > 0 );
    QCOMPARE( item.extendMode.c_str(), "auto" );
  }
  // 只给影响半径时 core = min(autoCore, max(0.55, 0.4)*influence)。
  QVERIFY( resolved[1].coreRadius <= std::max( resolved[1].influenceRadius * 0.55,
                                               resolved[1].influenceRadius * 0.4 ) + 1e-12 );
  // 双显式 core>influence 时压 core 到 0.95×influence。
  QVERIFY( std::abs( resolved[2].coreRadius - 4.0 * 0.95 ) <= 1e-12 );
}

void SingleFactorCorridorTests::polylineGeometryExtensionBookkeeping()
{
  const PolylineGeometry geom =
      buildPolylineGeometry( spec( { { 2, 1 }, { 6, 1 } }, 4, 0.5, 2 ), 3, 2.0 );
  QCOMPARE( geom.index, 3 );
  QCOMPARE( geom.points.size(), std::size_t{ 4 } );
  QVERIFY( std::abs( geom.points.front().x - 0.0 ) <= 1e-12 );
  QVERIFY( std::abs( geom.points.back().x - 8.0 ) <= 1e-12 );
  QCOMPARE( geom.cumlen.size(), std::size_t{ 4 } );
  QVERIFY( std::abs( geom.cumlen[1] - 2.0 ) <= 1e-12 );
  QVERIFY( std::abs( geom.cumlen[2] - 6.0 ) <= 1e-12 );
  QCOMPARE( geom.totalLength, 8.0 );
  QVERIFY( std::abs( geom.sStart - 2.0 ) <= 1e-12 );
  QVERIFY( std::abs( geom.sEnd - 6.0 ) <= 1e-12 );

  const PolylineGeometry noExtend =
      buildPolylineGeometry( spec( { { 2, 1 }, { 6, 1 } }, 4, 0.5, 2 ), 0, 0.0 );
  QCOMPARE( noExtend.points.size(), std::size_t{ 2 } );
  QVERIFY( std::abs( noExtend.sStart ) <= 1e-12 );
  QVERIFY( std::abs( noExtend.sEnd - 4.0 ) <= 1e-12 );
}

void SingleFactorCorridorTests::gridCacheAssignsControllingDirection()
{
  // 两条平行方向线：y=1 与 y=7，各管一侧；中部影响半径之外无归属。
  std::vector<PolylineGeometry> geoms;
  geoms.push_back( buildPolylineGeometry( spec( { { 0, 1 }, { 4, 1 } }, 4, 1, 2 ), 0, 0 ) );
  geoms.push_back( buildPolylineGeometry( spec( { { 0, 7 }, { 4, 7 } }, 6, 1, 2 ), 1, 0 ) );

  const GridSpec g = grid( 5, 9, 0, 9 );
  std::vector<std::uint8_t> domain( 45, std::uint8_t{ 1 } );
  const DirectionFieldCache cache = buildGridDirectionCache( g, domain, geoms );

  // 行 7 的像元中心 y=1.5：距上线 0.5，core 内全影响。
  const std::size_t nearLow = cellOf( 2, 7, 5 );
  QCOMPARE( cache.dirIndex[nearLow], 0 );
  QVERIFY( std::abs( cache.g[nearLow] - 1.0 ) <= 1e-12 );
  QCOMPARE( cache.stretch[nearLow], 4.0 ); // core 内全拉伸 = ratio
  QVERIFY( std::abs( cache.n[nearLow] - 0.5 ) <= 1e-12 );

  const std::size_t nearHigh = cellOf( 2, 1, 5 );
  QCOMPARE( cache.dirIndex[nearHigh], 1 );
  QCOMPARE( cache.stretch[nearHigh], 6.0 );

  // 行 4（y=4.5）距两线 3.5 > influence=2：零影响、无归属、stretch 退回 1。
  const std::size_t middle = cellOf( 2, 4, 5 );
  QCOMPARE( cache.dirIndex[middle], -1 );
  QVERIFY( std::abs( cache.g[middle] ) <= 1e-12 );
  QCOMPARE( cache.stretch[middle], 1.0 );
}

void SingleFactorCorridorTests::gridCacheBlendsCompetingTangents()
{
  // 0° 与 60° 两线交于 (5,5)：交点双角混合切向应为 30°。
  std::vector<PolylineGeometry> geoms;
  geoms.push_back( buildPolylineGeometry(
      spec( { { 0, 5 }, { 10, 5 } }, 4, 1, 3 ), 0, 0 ) );
  geoms.push_back( buildPolylineGeometry(
      spec( { { 5 - 4 * 0.5, 5 - 4 * 0.8660254037844386 },
              { 5 + 4 * 0.5, 5 + 4 * 0.8660254037844386 } }, 4, 1, 3 ), 1, 0 ) );

  const GridSpec g = grid( 1, 1, 4.5, 5.5 );
  std::vector<std::uint8_t> domain( 1, std::uint8_t{ 1 } );
  const DirectionFieldCache cache = buildGridDirectionCache( g, domain, geoms );
  const std::size_t center = 0;
  QVERIFY( cache.dirIndex[center] >= 0 );
  QCOMPARE( cache.dirIndex2[center], cache.dirIndex[center] == 0 ? 1 : 0 );
  QVERIFY( std::abs( cache.tx[center] - 0.8660254037844386 ) <= 1e-9 );
  QVERIFY( std::abs( cache.ty[center] - 0.5 ) <= 1e-9 );
}

void SingleFactorCorridorTests::ellipticalSearchStretchesAlongAxisOnly()
{
  // 沿轴远样本被椭圆接受（旧欧氏规则会拒绝），垂向不放大。
  QVERIFY( ellipticalSearchAccept( 0, 0, 4, 0.2, 8, 1, 1, 4.005 ) );
  QVERIFY( !ellipticalSearchAccept( 0, 0, 0, 1.5, 8, 1, 1, 1.5 ) );
  // g=0 退回纯欧氏规则。
  QVERIFY( !ellipticalSearchAccept( 0, 0, 4, 0.2, 8, 1, 0, 4.005 ) );
  QVERIFY( ellipticalSearchAccept( 0, 0, 0.4, 0.2, 8, 1, 0, 0.45 ) );
  // 线长参与：沿轴到达半径覆盖全线长（+20% 余量）。
  QVERIFY( ellipticalSearchAccept( 0, 0, 12, 0, 4, 1, 1, 12.0, 10.0 ) );
  QVERIFY( !ellipticalSearchAccept( 0, 0, 12, 0, 4, 1, 1, 12.0 ) );
}

void SingleFactorCorridorTests::pairDistanceUsesCurveMetricOnlyWhenShared()
{
  std::vector<PolylineGeometry> geoms;
  geoms.push_back( buildPolylineGeometry( spec( { { 0, 1 }, { 8, 1 } }, 4, 0.6, 3 ), 0, 0 ) );
  const std::vector<Point2> wells{ { 2, 1 }, { 6, 1 }, { 40, 40 } };
  const std::map<int, WellCurveTable> tables = precomputeWellCurveCoords( wells, geoms );

  // 不受方向控制（cellDir<0）：纯欧氏。
  const PairDistanceResult plain = pairEffectiveDistance( 5.0, -1, 0, 0, 0, 4, 0, tables );
  QCOMPARE( plain.dEff, 5.0 );
  QCOMPARE( plain.gPair, 0.0 );

  // 井在走廊外（well 2 距线 40+）：不共享方向，仍欧氏。
  const PairDistanceResult outside = pairEffectiveDistance( 5.0, 0, 4, 0, 1, 4, 2, tables );
  QCOMPARE( outside.dEff, 5.0 );
  QCOMPARE( outside.gPair, 0.0 );

  // 共享方向：曲线距离 Δs/4 生效（cell s=4 → 井 s=6 的曲线距离 0.5）。
  const PairDistanceResult shared = pairEffectiveDistance( 41.0, 0, 4, 0, 1, 4, 1, tables );
  QVERIFY( shared.gPair > 0.99 );
  const double curve = std::sqrt( curveDistanceSq( 4, 0, 6, 0, 4 ) );
  QVERIFY( std::abs( curve - 0.5 ) <= 1e-12 );
  QVERIFY( std::abs( shared.dEff - blendEffectiveDistance( 41.0, curve, shared.gPair ) ) <= 1e-12 );
  QVERIFY( shared.dEff < 41.0 );

  // 邻域：g=0 时欧氏半径判定。
  QVERIFY( !pairInSearchNeighborhood( 1.2, 0.4, 0, 0, 0, 4, 0, 0, tables, 1, false ) );
  QVERIFY( pairInSearchNeighborhood( 0.8, 0.4, 0, 0, 0, 4, 0, 0, tables, 1, false ) );
  // 扩展搜索：同轴远井按椭圆接受（cellS=4、井 s=2、ratio=4）。
  QVERIFY( pairInSearchNeighborhood( 5.0, 1.4, 1.0, 4, 0, 4, 0, 0, tables, 1, true ) );
}

void SingleFactorCorridorTests::alongTrackProfilePinsTipsAndBlends()
{
  std::vector<PolylineGeometry> geoms;
  // core=0.6、influence=1.5：轴行全影响，±1 行过渡带，±2 行（远端）零影响。
  geoms.push_back( buildPolylineGeometry( spec( { { 0, 2.5 }, { 8, 2.5 } }, 4, 0.6, 1.5 ), 0, 0 ) );
  const std::vector<Point2> wells{ { 2, 2.5 }, { 6, 2.5 } };
  const std::vector<double> values{ 10.0, 2.0 };
  const std::map<int, WellCurveTable> tables = precomputeWellCurveCoords( wells, geoms );
  const std::map<int, RidgeProfile> profiles =
      buildAlongTrackWellProfiles( wells, values, tables, geoms );

  QCOMPARE( profiles.size(), std::size_t{ 1 } );
  const RidgeProfile &profile = profiles.at( 0 );
  QCOMPARE( profile.s.size(), std::size_t{ 4 } );
  QVERIFY( std::abs( profile.s.front() ) <= 1e-12 );
  QVERIFY( std::abs( profile.s.back() - 8.0 ) <= 1e-12 );
  QCOMPARE( profile.z.front(), 10.0 ); // 端点锚定最近井值
  QCOMPARE( profile.z.back(), 2.0 );

  // 井间线性内插，出井段常值外推。
  QVERIFY( std::abs( sampleAlongTrackValue( 4.0, 0, profile, geoms[0] ) - 6.0 ) <= 1e-12 );
  QVERIFY( std::abs( sampleAlongTrackValue( 1.0, 0, profile, geoms[0] ) - 10.0 ) <= 1e-12 );
  QVERIFY( std::abs( sampleAlongTrackValue( 7.5, 0, profile, geoms[0] ) - 2.0 ) <= 1e-12 );

  // 网格混合：走廊内 NaN 用脊线值填充，影响半径外保持原值。
  const GridSpec g = grid( 9, 7, 0, 7 );
  const std::size_t cells = 63;
  std::vector<std::uint8_t> domain( cells, std::uint8_t{ 1 } );
  std::vector<double> field( cells, std::numeric_limits<double>::quiet_NaN() );
  // 远端行（y=6.5/0.5，距线 2 > influence=1.5）预置有限值，断言不被改动。
  field[cellOf( 4, 0, 9 )] = -77.0;
  field[cellOf( 4, 6, 9 )] = -88.0;

  const DirectionFieldCache cache = buildGridDirectionCache( g, domain, geoms );
  AlongTrackStats stats;
  const std::vector<double> blended =
      blendCorridorAlongTrack( field, cache, geoms, profiles, domain, &stats );
  QCOMPARE( blended.size(), cells );
  QVERIFY( stats.alongTrackCells > 0 );

  // 轴上像元（y=2.5，行 4）x=0.5 → s=0.5：脊线值 10；x=4.5 → 井间内插 5。
  QVERIFY( std::abs( blended[cellOf( 0, 4, 9 )] - 10.0 ) <= 1e-9 );
  QVERIFY( std::abs( blended[cellOf( 4, 4, 9 )] - 5.0 ) <= 1e-9 );
  QVERIFY( std::abs( blended[cellOf( 8, 4, 9 )] - 2.0 ) <= 1e-9 );
  // 远端行不受走廊影响。
  QCOMPARE( blended[cellOf( 4, 0, 9 )], -77.0 );
  QCOMPARE( blended[cellOf( 4, 6, 9 )], -88.0 );
  // 影响半径外（dirIndex=-1）的 NaN 像元保持缺失。
  QVERIFY( !std::isfinite( blended[cellOf( 0, 0, 9 )] ) );
}

QTEST_MAIN( SingleFactorCorridorTests )
#include "tst_singlefactor_corridor.moc"
