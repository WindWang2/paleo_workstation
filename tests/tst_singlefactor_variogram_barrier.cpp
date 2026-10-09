// 层：数据（测试壳位于 tests/，被测对象为 singlefactor 变差解析）
#include <QtTest>

#include "algorithms/singlefactor/krigingsurface.h"

#include <cmath>
#include <string>
#include <vector>

using namespace paleo::singlefactor;

namespace
{

Sample sampleAt( double x, double y, double value )
{
  Sample sample;
  sample.x = x;
  sample.y = y;
  sample.value = value;
  return sample;
}

std::vector<Sample> twoSides()
{
  return { sampleAt( 1.0, 0.5, 1.0 ), sampleAt( 2.0, 4.0, 2.0 ), sampleAt( 3.0, 7.5, 3.0 ),
           sampleAt( 7.0, 1.5, 7.0 ), sampleAt( 8.0, 4.5, 8.0 ), sampleAt( 9.0, 7.0, 9.0 ) };
}

GridSpec smallGrid()
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

bool sameFit( const VariogramResolution &a, const VariogramResolution &b )
{
  return a.ok == b.ok && a.fitted == b.fitted && a.nugget == b.nugget && a.sill == b.sill &&
         a.range == b.range && a.r2 == b.r2 && a.rmse == b.rmse && a.usedLags == b.usedLags &&
         a.barrierAware == b.barrierAware && a.unreachablePairs == b.unreachablePairs;
}

} // namespace

class SingleFactorVariogramBarrierTests : public QObject
{
  Q_OBJECT
private slots:
  void sealedBarrierDropsCrossPairs();
  void switchOffMatchesNoBarrier();
};

void SingleFactorVariogramBarrierTests::sealedBarrierDropsCrossPairs()
{
  ConstraintLine barrier;
  barrier.stableId = "fault";
  barrier.semantic = Semantic::HardBarrier;
  barrier.enabled = true;
  barrier.points = { { 5.0, 0.0 }, { 5.0, 8.0 } };

  ResolvedParameters params;
  params.range = 0;
  params.variogramAzimuthDeg = -1;
  params.variogramBarrierAware = true;
  params.hardBarrierModel = "interpretation_partition_v1";

  const VariogramResolution on = resolveVariogram( twoSides(), smallGrid(), params, { barrier } );
  QVERIFY2( on.ok, on.message.c_str() );
  QVERIFY( on.barrierAware );
  QCOMPARE( on.unreachablePairs, 9 );
}

void SingleFactorVariogramBarrierTests::switchOffMatchesNoBarrier()
{
  ConstraintLine barrier;
  barrier.stableId = "fault";
  barrier.semantic = Semantic::HardBarrier;
  barrier.points = { { 5.0, -10.0 }, { 5.0, 20.0 } };

  ResolvedParameters params;
  params.range = 0;
  params.variogramAzimuthDeg = -1;
  params.variogramBarrierAware = false;
  params.hardBarrierModel = "interpretation_partition_v1";

  const VariogramResolution off = resolveVariogram( twoSides(), smallGrid(), params, { barrier } );
  const VariogramResolution none = resolveVariogram( twoSides(), smallGrid(), params );
  QVERIFY2( off.ok && none.ok, ( off.message + " / " + none.message ).c_str() );
  QVERIFY( !off.barrierAware );
  QCOMPARE( off.unreachablePairs, 0 );
  QVERIFY( sameFit( off, none ) );
}

QTEST_MAIN( SingleFactorVariogramBarrierTests )
#include "tst_singlefactor_variogram_barrier.moc"
