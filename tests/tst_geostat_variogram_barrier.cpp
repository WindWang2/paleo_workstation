// 层：数据（测试壳位于 tests/，被测对象为数据层 geostat 核）
#include <QtTest/QtTest>

#include "algorithms/geostat/faultpath.h"
#include "algorithms/geostat/variogram.h"

#include <cmath>
#include <vector>

using namespace paleo::geostat;

namespace
{

// 薄墙矩形（宽 width、从 y0 到 y1）：封死型墙体须延伸出样本范围。
BarrierPolygon wall( double x, double width, double y0, double y1 )
{
  BarrierPolygon polygon;
  polygon.exterior.points = { { x - width / 2, y0 }, { x + width / 2, y0 },
                              { x + width / 2, y1 }, { x - width / 2, y1 } };
  return polygon;
}

std::vector<Sample> twoSideSamples()
{
  return { { 1.0, 0.5, 1.0 }, { 2.0, 4.0, 2.0 }, { 3.0, 7.5, 3.0 },   // 左侧 3 口
           { 7.0, 1.5, 7.0 }, { 8.0, 4.5, 8.0 }, { 9.0, 7.0, 9.0 } }; // 右侧 3 口
}

int totalPairs( const ExperimentalVariogram &experimental )
{
  int total = 0;
  for ( int count : experimental.pairCount )
    total += count;
  return total;
}

// 逐位一致（不走 qFuzzyCompare：两轨关闸后必须走同一条代码路径）。
bool bitwiseSame( const ExperimentalVariogram &a, const ExperimentalVariogram &b )
{
  if ( a.pairCount.size() != b.pairCount.size() )
    return false;
  for ( std::size_t i = 0; i < a.pairCount.size(); ++i )
  {
    if ( a.pairCount[i] != b.pairCount[i] || a.semivariance[i] != b.semivariance[i] ||
         a.lagDistance[i] != b.lagDistance[i] )
      return false;
  }
  return true;
}

} // namespace

class BarrierVariogramTests : public QObject
{
  Q_OBJECT
  private slots:
    void sealedWallExcludesCrossSidePairs();
    void disabledTrackMatchesLegacyBitwise();
    void emptyPolygonListFallsBackToPlainTrack();
    void detourPairAccumulatesAtGeodesicLag();
    void fitConsumesBarrierAwareExperimental();
    void rejectsSampleBudget();
};

// Oracle 2：封死隔断两侧样本对不进累积（计数断言）。
// 6 口井、C(6,2)=15 对；跨墙 3×3=9 对不可达被跳过，同侧 6 对照常累积。
void BarrierVariogramTests::sealedWallExcludesCrossSidePairs()
{
  VariogramBarriers barriers;
  barriers.enabled = true;
  barriers.polygons = { wall( 5.0, 0.2, -10.0, 20.0 ) }; // 延伸出样本范围 → 封死
  const ExperimentalVariogram result =
      experimentalVariogram( twoSideSamples(), 20.0, 4, {}, barriers );
  QVERIFY2( result.status == Status::Ok, result.message.c_str() );
  QVERIFY( result.barrierAware );
  QCOMPARE( totalPairs( result ), 6 );
  QCOMPARE( result.unreachablePairs, 9 );

  // 对照：同一样本无隔断 → 全 15 对累积（两侧值差大，隔断语义可见）。
  const ExperimentalVariogram plain = experimentalVariogram( twoSideSamples(), 20.0, 4 );
  QVERIFY( plain.status == Status::Ok );
  QVERIFY( !plain.barrierAware );
  QCOMPARE( totalPairs( plain ), 15 );
}

// Oracle 2：开关关闭（默认参数/显式 enabled=false）与既有口径逐位一致。
void BarrierVariogramTests::disabledTrackMatchesLegacyBitwise()
{
  const std::vector<Sample> samples = twoSideSamples();
  const ExperimentalVariogram legacy = experimentalVariogram( samples, 3.0, 6 );
  VariogramBarriers off;
  off.enabled = false;
  off.polygons = { wall( 5.0, 0.2, -10.0, 20.0 ) }; // 有隔断但开关关 → 不感知
  const ExperimentalVariogram dual = experimentalVariogram( samples, 3.0, 6, {}, off );
  QVERIFY( legacy.status == Status::Ok );
  QVERIFY( dual.status == Status::Ok );
  QVERIFY( !dual.barrierAware );
  QVERIFY2( bitwiseSame( legacy, dual ), "disabled barrier track must be bitwise identical" );
}

// enabled=true 但 polygons 为空：无双轨意义，回落既有口径（barrierAware 如实记 false）。
void BarrierVariogramTests::emptyPolygonListFallsBackToPlainTrack()
{
  VariogramBarriers barriers;
  barriers.enabled = true;
  const ExperimentalVariogram result =
      experimentalVariogram( twoSideSamples(), 3.0, 6, {}, barriers );
  const ExperimentalVariogram legacy = experimentalVariogram( twoSideSamples(), 3.0, 6 );
  QVERIFY( result.status == Status::Ok );
  QVERIFY( !result.barrierAware );
  QVERIFY( bitwiseSame( legacy, result ) );
}

// 不封死的小障碍：对仍累积（可达），滞后距用绕障测地距（> 欧氏距，绕墙折线口径）。
// 两口井取对角布置（同 y 会让自动格网只有两行，小墙被格网边封死——那测的就不是绕行）。
void BarrierVariogramTests::detourPairAccumulatesAtGeodesicLag()
{
  std::vector<Sample> samples = { { 2.0, 5.5, 1.0 }, { 8.0, 4.5, 2.0 } };
  VariogramBarriers barriers;
  barriers.enabled = true;
  // 小方块障碍 (4.9..5.1)×(4.8..5.2)：挡住直线连线，上下可绕。
  barriers.polygons = { wall( 5.0, 0.2, 4.8, 5.2 ) };
  const ExperimentalVariogram result = experimentalVariogram( samples, 20.0, 4, {}, barriers );
  QVERIFY2( result.status == Status::Ok, result.message.c_str() );
  QCOMPARE( totalPairs( result ), 1 ); // 可达：照常累积
  QCOMPARE( result.unreachablePairs, 0 );
  const double euclid = std::hypot( 8.0 - 2.0, 4.5 - 5.5 );
  const double lagDistance = result.lagDistance[0];
  QVERIFY2( lagDistance > euclid + 0.05,
            qPrintable( QStringLiteral( "geodesic lag %1 must exceed euclid %2" )
                            .arg( lagDistance )
                            .arg( euclid ) ) );
  QVERIFY2( lagDistance < euclid + 2.0,
            qPrintable( QStringLiteral( "geodesic lag %1 implausibly large vs euclid %2" )
                            .arg( lagDistance )
                            .arg( euclid ) ) );
}

// 隔断感知实验变差函数照常进 fitVariogram（拟合面零改动，消费面同构）。
void BarrierVariogramTests::fitConsumesBarrierAwareExperimental()
{
  // 左侧 6 口、按距离衰减的场：拟合球状模型应得 Ok 与正拱高。
  std::vector<Sample> samples;
  for ( int i = 0; i < 6; ++i )
    for ( int j = 0; j < 6; ++j )
      samples.push_back( { 0.5 + i * 0.8, 0.5 + j * 1.4, 3.0 * ( i * 0.8 + j * 1.4 ) } );
  VariogramBarriers barriers;
  barriers.enabled = true;
  barriers.polygons = { wall( 9.5, 0.3, -5.0, 15.0 ) }; // 样本范围外右侧 → 无对被跳
  barriers.gridResolution = 96; // 测试提速
  const ExperimentalVariogram experimental =
      experimentalVariogram( samples, 1.0, 10, {}, barriers );
  QVERIFY2( experimental.status == Status::Ok, experimental.message.c_str() );
  QCOMPARE( experimental.unreachablePairs, 0 );
  const VariogramFit fit = fitVariogram( experimental, VariogramModelType::Spherical );
  QVERIFY2( fit.status == Status::Ok, fit.message.c_str() );
  QVERIFY( fit.model.sill > 0 );
}

// n > 512 时如实拒收（测地场 O(n·格网) 的预算闸）。
void BarrierVariogramTests::rejectsSampleBudget()
{
  std::vector<Sample> samples;
  for ( int i = 0; i < 513; ++i )
    samples.push_back( { i * 0.1, ( i % 7 ) * 0.3, 1.0 + ( i % 5 ) * 0.2 } );
  VariogramBarriers barriers;
  barriers.enabled = true;
  barriers.polygons = { wall( 25.0, 0.2, -10.0, 10.0 ) };
  const ExperimentalVariogram result = experimentalVariogram( samples, 5.0, 4, {}, barriers );
  QVERIFY( result.status == Status::InvalidInput );
  QVERIFY( result.message.find( "512" ) != std::string::npos );
}

QTEST_MAIN( BarrierVariogramTests )
#include "tst_geostat_variogram_barrier.moc"
