// 层：数据（测试壳位于 tests/，被测对象为数据层纯数值核）
#include <QtTest/QtTest>

#include "algorithms/singlefactor/faultpath.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace paleo::singlefactor;

namespace
{

FaultLine wall( std::vector<Point2> points )
{
  FaultLine line;
  line.points = std::move( points );
  return line;
}

std::vector<Point2> v( std::initializer_list<Point2> list )
{
  return std::vector<Point2>( list );
}

} // namespace

class SingleFactorFaultPathTests : public QObject
{
  Q_OBJECT
  private slots:
    void detourAddsTravelInsteadOfDropping();
    void noObstacleMeansEuclidean();
    void tangencyIsVisible();
    void anisotropicMetricCompressesAlongAxis();
    void surferIdwKeepsDetourSamples();
    void surferIdwValidationAndOptions();
    void surferIdwExactHitsAverage();
};

void SingleFactorFaultPathTests::detourAddsTravelInsteadOfDropping()
{
  const std::vector<Point2> wells{ { 2, 5 }, { 8, 5 } };
  const std::vector<FaultLine> walls{ wall( { { 5, 0 }, { 5, 10 } } ) };
  const FaultPathMetric metric( wells, walls );
  QVERIFY( metric.hasDetourNodes() );

  const std::vector<Point2> queries{ { 4, 5 } };
  const std::vector<double> d = metric.distances( queries );
  QCOMPARE( d.size(), std::size_t{ 2 } );
  // 同侧井：可见，欧氏距离。
  QVERIFY( std::abs( d[0] - 2.0 ) <= 1e-9 );
  // 跨墙井：绕行测地距离 > 直线欧氏（差值 ≈ 端点绕行长度）。
  const double straight = std::hypot( 4.0 - 8.0, 0.0 );
  QVERIFY( d[1] > straight );
  QVERIFY( d[1] > straight + 4.0 ); // 绕端长度量级
  // 绕行距离 = 经墙端点的折线长度 hypot(1,5) + hypot(3,5)。
  const double viaEnd = std::hypot( 1.0, 5.0 ) + std::hypot( 3.0, 5.0 );
  QVERIFY( std::abs( d[1] - viaEnd ) <= 1e-6 );
}

void SingleFactorFaultPathTests::noObstacleMeansEuclidean()
{
  const std::vector<Point2> wells{ { 2, 5 }, { 8, 5 } };
  const FaultPathMetric metric( wells, {} );
  QVERIFY( !metric.hasDetourNodes() );
  const std::vector<double> d = metric.distances( v( { { 4, 5 }, { 8, 6 } } ) );
  QVERIFY( std::abs( d[0] - 2.0 ) <= 1e-12 );
  QVERIFY( std::abs( d[1 * 2 + 0] - std::hypot( 6.0, 1.0 ) ) <= 1e-12 );
  QVERIFY( std::abs( d[1 * 2 + 1] - 1.0 ) <= 1e-12 );

  // 无墙时 interpolateGlobalIdw 与手写 IDW 一致。
  const std::vector<Point2> wellXy{ { 2, 5 }, { 8, 5 } };
  const std::vector<double> values{ 1.0, 9.0 };
  SurferIdwOptions options;
  const SurferIdwResult result =
      interpolateGlobalIdw( v( { { 4, 5 } } ), wellXy, values, options, {} );
  QCOMPARE( static_cast<int>( result.status ), static_cast<int>( Status::Ok ) );
  // d=2 与 d=4，幂 2：(1/4*1 + 1/16*9)/(1/4+1/16) = 2.6
  QVERIFY( std::abs( result.values[0] - 2.6 ) <= 1e-12 );
}

void SingleFactorFaultPathTests::tangencyIsVisible()
{
  const std::vector<FaultLine> walls{ wall( { { 5, 0 }, { 5, 10 } } ) };

  // 井在墙上、射线终点恰好触墙（相切）：可见，不丢弃。
  const FaultPathMetric touch( v( { { 5, 5 } } ), walls );
  const std::vector<double> d = touch.distances( v( { { 4, 5 } } ) );
  QVERIFY( std::abs( d[0] - 1.0 ) <= 1e-9 );

  // 横越墙体的射线：不可见，走绕行（经墙端折返，远大于欧氏 2）。
  const FaultPathMetric cross( v( { { 4, 5 } } ), walls );
  const std::vector<double> blocked = cross.distances( v( { { 6, 5 } } ) );
  QVERIFY( blocked[0] > 4.0 );
}

void SingleFactorFaultPathTests::anisotropicMetricCompressesAlongAxis()
{
  const std::vector<Point2> wells{ { 6, 5 } };
  const FaultPathMetric metric( wells, {}, 2.0, 0.0 );
  const std::vector<double> d = metric.distances( v( { { 4, 5 } } ) );
  // 沿 x 轴 Δ=2 按 ratio=2 压缩 → 度量距离 1。
  QVERIFY( std::abs( d[0] - 1.0 ) <= 1e-12 );
}

void SingleFactorFaultPathTests::surferIdwKeepsDetourSamples()
{
  const std::vector<Point2> wellXy{ { 2, 5 }, { 8, 5 } };
  const std::vector<double> values{ 1.0, 9.0 };
  const std::vector<FaultLine> walls{ wall( { { 5, 0 }, { 5, 10 } } ) };
  SurferIdwOptions options;

  // 无墙：两井都按欧氏参与 → 2.6（见 noObstacleMeansEuclidean）。
  const SurferIdwResult freeOfWall = interpolateGlobalIdw( v( { { 4, 5 } } ), wellXy, values, options, {} );
  QVERIFY( std::abs( freeOfWall.values[0] - 2.6 ) <= 1e-12 );

  // 有墙：跨墙井不丢弃，改按绕行距离参与（权重被压低）。
  const SurferIdwResult walled = interpolateGlobalIdw( v( { { 4, 5 } } ), wellXy, values, options, walls );
  QCOMPARE( static_cast<int>( walled.status ), static_cast<int>( Status::Ok ) );
  QVERIFY( std::isfinite( walled.values[0] ) );
  QVERIFY( walled.values[0] < 2.6 );            // 跨墙井影响被绕行距离稀释
  QVERIFY( walled.values[0] > 1.0 + 0.1 );      // 但样本仍参与，不是只剩同侧井

  // 用 FaultPathMetric 的绕行距离手算 IDW，断言 interpolate 用的是同一距离。
  const FaultPathMetric metric( wellXy, walls );
  const std::vector<double> dist = metric.distances( v( { { 4, 5 } } ) );
  const double w1 = 1.0 / std::pow( dist[0] / std::min( dist[0], dist[1] ), 2.0 );
  const double w2 = 1.0 / std::pow( dist[1] / std::min( dist[0], dist[1] ), 2.0 );
  const double expected = ( w1 * values[0] + w2 * values[1] ) / ( w1 + w2 );
  QVERIFY( std::abs( walled.values[0] - expected ) <= 1e-12 );
}

void SingleFactorFaultPathTests::surferIdwValidationAndOptions()
{
  const std::vector<Point2> wellXy{ { 2, 5 }, { 8, 5 } };
  const std::vector<double> values{ 1.0, 9.0 };
  SurferIdwOptions options;

  // 空井集 + 非空查询 → 拒绝。
  const SurferIdwResult noWells = interpolateGlobalIdw( v( { { 4, 5 } } ), {}, {}, options, {} );
  QCOMPARE( static_cast<int>( noWells.status ), static_cast<int>( Status::InvalidInput ) );
  QVERIFY( !noWells.message.empty() );

  // 非法幂次 → 拒绝。
  SurferIdwOptions badPower;
  badPower.power = 0;
  const SurferIdwResult rejected =
      interpolateGlobalIdw( v( { { 4, 5 } } ), wellXy, values, badPower, {} );
  QCOMPARE( static_cast<int>( rejected.status ), static_cast<int>( Status::InvalidInput ) );

  // max_points=1：只保留最近井 → 恒为同侧井值。
  SurferIdwOptions nearest;
  nearest.maxPoints = 1;
  const SurferIdwResult truncated = interpolateGlobalIdw( v( { { 4, 5 } } ), wellXy, values, nearest, {} );
  QVERIFY( std::abs( truncated.values[0] - 1.0 ) <= 1e-12 );

  // min_points 高于可达样本数 → NaN，不造假值。
  SurferIdwOptions lonely;
  lonely.minPoints = 2;
  lonely.maxPoints = 1;
  const SurferIdwResult isolated = interpolateGlobalIdw( v( { { 4, 5 } } ), wellXy, values, lonely, {} );
  QVERIFY( !std::isfinite( isolated.values[0] ) );

  // 搜索半径用物理欧氏距离：半径 3 把 4 远的井排除。
  SurferIdwOptions radius;
  radius.searchRadius = 3.0;
  const SurferIdwResult capped = interpolateGlobalIdw( v( { { 4, 5 } } ), wellXy, values, radius, {} );
  QVERIFY( std::abs( capped.values[0] - 1.0 ) <= 1e-12 );

  // 完全隔离的无井仓：闭环屏障围住查询点，无绕行出口 → NaN。
  const std::vector<FaultLine> sealed{ wall( { { 3.9, 4.5 },
                                               { 4.1, 4.5 },
                                               { 4.1, 5.5 },
                                               { 3.9, 5.5 },
                                               { 3.9, 4.5 } } ) };
  const SurferIdwResult sealedResult =
      interpolateGlobalIdw( v( { { 4, 5 } } ), wellXy, values, options, sealed );
  QVERIFY( !std::isfinite( sealedResult.values[0] ) );
}

void SingleFactorFaultPathTests::surferIdwExactHitsAverage()
{
  const std::vector<Point2> wellXy{ { 2, 5 }, { 2, 5 }, { 8, 5 } };
  const std::vector<double> values{ 1.0, 3.0, 9.0 };
  SurferIdwOptions options;
  const SurferIdwResult result = interpolateGlobalIdw( v( { { 2, 5 } } ), wellXy, values, options, {} );
  // 命中井位：精确命中的样本取均值 (1+3)/2，不受幂次影响。
  QVERIFY( std::abs( result.values[0] - 2.0 ) <= 1e-12 );
}

QTEST_MAIN( SingleFactorFaultPathTests )
#include "tst_singlefactor_faultpath.moc"
