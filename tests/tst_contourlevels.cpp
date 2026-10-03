#include <QtTest>

#include "../src/algorithms/singlefactor/contourlevels.h"

#include <cmath>
#include <vector>

// #149：用户间距等值线级别数上限——间距过小必须返回失败且不抛异常、不分配大内存；
// 正常间距的结果不变。
class TestContourLevels : public QObject
{
  Q_OBJECT
private slots:
  void tinyIntervalFailsWithoutThrowing_data();
  void tinyIntervalFailsWithoutThrowing();
  void normalIntervalUnchanged();
  void capBoundaryInclusive();
  void autoPathUnaffected();
  void estimateHandlesDegenerateInputs();

private:
  static std::vector<double> ramp( double lo, double hi, int n )
  {
    std::vector<double> g( static_cast<std::size_t>( n ) );
    for ( int i = 0; i < n; ++i )
      g[static_cast<std::size_t>( i )] = lo + ( hi - lo ) * i / ( n - 1 );
    return g;
  }
};

void TestContourLevels::tinyIntervalFailsWithoutThrowing_data()
{
  QTest::addColumn<double>( "interval" );
  QTest::newRow( "1e-3" ) << 1e-3;  // 旧代码：500 万级
  QTest::newRow( "1e-5" ) << 1e-5;  // 旧代码：5 亿级（≈4 GB）
  QTest::newRow( "1e-6" ) << 1e-6;  // 旧代码：vector::reserve 抛 length_error
  QTest::newRow( "1e-300" ) << 1e-300; // 旧代码：double→int 溢出（UB）
}

void TestContourLevels::tinyIntervalFailsWithoutThrowing()
{
  QFETCH( double, interval );
  const std::vector<double> grid = ramp( 0.0, 5000.0, 64 );
  paleo::singlefactor::ContourLevelPlan plan;
  bool ok = true;
  bool threw = false;
  try
  {
    ok = paleo::singlefactor::intervalContourLevels( grid, {}, interval, &plan );
  }
  catch ( ... )
  {
    threw = true;
  }
  QVERIFY( !threw );
  QVERIFY( !ok );
  QVERIFY( plan.tooManyLevels );
  QVERIFY( plan.levels.empty() );
  QVERIFY( plan.levels.capacity() == 0 );
  QVERIFY( plan.estimatedLevels > double( paleo::singlefactor::kMaxContourLevels ) );
}

void TestContourLevels::normalIntervalUnchanged()
{
  const std::vector<double> grid = ramp( 0.0, 5000.0, 64 );
  paleo::singlefactor::ContourLevelPlan plan;
  QVERIFY( paleo::singlefactor::intervalContourLevels( grid, {}, 500.0, &plan ) );
  QVERIFY( !plan.tooManyLevels );
  QCOMPARE( plan.levels.size(), std::size_t( 11 ) );
  QCOMPARE( plan.levels.front(), 0.0 );
  QCOMPARE( plan.levels.back(), 5000.0 );
  QCOMPARE( plan.estimatedLevels, 11.0 );
}

void TestContourLevels::capBoundaryInclusive()
{
  // 0~5000 / 0.5 → 10001 条 > 1 万：拒绝；/ 1.0 → 5001 条：放行。
  const std::vector<double> grid = ramp( 0.0, 5000.0, 64 );
  paleo::singlefactor::ContourLevelPlan plan;
  QVERIFY( !paleo::singlefactor::intervalContourLevels( grid, {}, 0.5, &plan ) );
  QCOMPARE( plan.estimatedLevels, 10001.0 );
  QVERIFY( paleo::singlefactor::intervalContourLevels( grid, {}, 1.0, &plan ) );
  QCOMPARE( plan.levels.size(), std::size_t( 5001 ) );
}

void TestContourLevels::autoPathUnaffected()
{
  const std::vector<double> grid = ramp( 0.0, 5000.0, 64 );
  paleo::singlefactor::ContourLevelPlan plan;
  QVERIFY( paleo::singlefactor::autoContourLevels( grid, {}, &plan ) );
  QVERIFY( !plan.tooManyLevels );
  QVERIFY( !plan.levels.empty() );
  QVERIFY( plan.levels.size() <= 20 );
}

void TestContourLevels::estimateHandlesDegenerateInputs()
{
  using paleo::singlefactor::estimateContourLevelCount;
  QVERIFY( std::isinf( estimateContourLevelCount( 10.0, 0.0 ) ) );
  QVERIFY( std::isinf( estimateContourLevelCount( 10.0, -1.0 ) ) );
  QVERIFY( std::isinf( estimateContourLevelCount( std::nan( "" ), 1.0 ) ) );
  QCOMPARE( estimateContourLevelCount( 0.0, 1.0 ), 1.0 );
  QCOMPARE( estimateContourLevelCount( 10.0, 1.0 ), 11.0 );
}

QTEST_APPLESS_MAIN( TestContourLevels )
#include "tst_contourlevels.moc"
