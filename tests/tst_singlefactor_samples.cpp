// 层：数据（测试壳位于 tests/，被测对象为井点样本过滤与计数）
#include <QtTest/QtTest>

#include "algorithms/singlefactor/samples.h"

#include <cmath>
#include <limits>

using namespace paleo::singlefactor;

namespace
{

Sample row( const char *id, double x, double y, double value )
{
  Sample sample;
  sample.stableRowId = id;
  sample.wellId = id;
  sample.x = x;
  sample.y = y;
  sample.value = value;
  return sample;
}

} // namespace

class SingleFactorSampleTests : public QObject
{
  Q_OBJECT
private slots:
  void keepsZeroAndDuplicateRows();
  void reportsMissingAndRangeWithoutClamping();
  void rejectsMissingAndMixedCrs();
};

void SingleFactorSampleTests::keepsZeroAndDuplicateRows()
{
  SamplePrepRequest request;
  request.rows = { row( "a", 0, 0, 0 ), row( "b", 0, 0, 4 ), row( "c", 1, 0, 0 ), row( "d", 2, 0, 2 ) };
  const SamplePrepResult result = prepareSamples( request );
  QCOMPARE( static_cast<int>( result.status ), static_cast<int>( Status::Ok ) );
  QCOMPARE( result.input.originalCount, 4 );
  QCOMPARE( result.input.validCount, 4 );
  QCOMPARE( result.input.duplicateExtraRows, 1 );
  QCOMPARE( result.input.conflictRows, 2 );
  QCOMPARE( result.input.samples[0].value, 0.0 );
  QCOMPARE( result.input.samples[2].value, 0.0 );
}

void SingleFactorSampleTests::reportsMissingAndRangeWithoutClamping()
{
  SamplePrepRequest request;
  request.valueUnit = "m";
  request.minimum = 0;
  request.maximum = 50;
  request.rows = { row( "a", 0, 0, 12 ),
                   row( "b", 1, 0, std::numeric_limits<double>::quiet_NaN() ),
                   row( "c", 2, 0, 80 ),
                   row( "d", 3, 0, 0 ) };
  const SamplePrepResult result = prepareSamples( request );
  QCOMPARE( static_cast<int>( result.status ), static_cast<int>( Status::Ok ) );
  QCOMPARE( result.input.originalCount, 4 );
  QCOMPARE( result.input.validCount, 2 );
  QCOMPARE( result.input.missingCount, 1 );
  QCOMPARE( result.input.outOfRangeCount, 1 );
  QCOMPARE( result.input.samples[0].value, 12.0 );
  QCOMPARE( result.input.samples[1].value, 0.0 );
  QCOMPARE( result.input.valueUnit, std::string( "m" ) );

  request.percentToFraction = true;
  request.minimum.reset();
  request.maximum.reset();
  request.valueUnit = "%";
  request.rows = { row( "a", 0, 0, 25 ) };
  const SamplePrepResult fraction = prepareSamples( request );
  QCOMPARE( static_cast<int>( fraction.status ), static_cast<int>( Status::Ok ) );
  QCOMPARE( fraction.input.samples[0].value, 0.25 );
  QCOMPARE( fraction.input.valueUnit, std::string( "fraction" ) );
}

void SingleFactorSampleTests::rejectsMissingAndMixedCrs()
{
  SamplePrepRequest request;
  request.rows = { row( "a", 0, 0, 1 ) };
  request.crs = CrsMode::Missing;
  const SamplePrepResult missing = prepareSamples( request );
  QCOMPARE( static_cast<int>( missing.status ), static_cast<int>( Status::InvalidInput ) );
  QVERIFY( missing.input.samples.empty() );

  request.crs = CrsMode::Mixed;
  const SamplePrepResult mixed = prepareSamples( request );
  QCOMPARE( static_cast<int>( mixed.status ), static_cast<int>( Status::InvalidInput ) );

  request.crs = CrsMode::LocalEngineering;
  const SamplePrepResult local = prepareSamples( request );
  QCOMPARE( static_cast<int>( local.status ), static_cast<int>( Status::Ok ) );
  QVERIFY( local.input.localEngineeringGrid );
}

QTEST_MAIN( SingleFactorSampleTests )
#include "tst_singlefactor_samples.moc"
