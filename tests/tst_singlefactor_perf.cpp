// 层：数据（测试壳。被测对象是 QGIS-free 的本地方向核。）
#include <QtTest/QtTest>

#include "algorithms/singlefactor/localidw.h"
#include "algorithms/singlefactor/support.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <chrono>
#include <fstream>
#include <string>
#include <vector>

using namespace paleo::singlefactor;

namespace
{

Polygon square( double x0, double y0, double x1, double y1 )
{
  Polygon poly;
  poly.exterior.points = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 }, { x0, y0 } };
  return poly;
}

GridSpec gridOf( int cells )
{
  GridSpec grid;
  grid.cols = cells;
  grid.rows = cells;
  grid.originX = 0;
  grid.originY = static_cast<double>( cells );
  grid.pixelWidth = 1;
  grid.pixelHeight = -1;
  grid.crs = "EPSG:3857";
  return grid;
}

PreparedInput makeInput( int wells, int cells, int directionLines, int segmentsEach, int softLines )
{
  PreparedInput input;
  input.domain = { square( 0, 0, cells, cells ) };
  input.samples.reserve( static_cast<std::size_t>( wells ) );
  for ( int i = 0; i < wells; ++i )
  {
    Sample sample;
    sample.stableRowId = std::to_string( i );
    sample.wellId = sample.stableRowId;
    sample.x = 1.0 + static_cast<double>( ( i * 97 ) % ( cells - 2 ) );
    sample.y = 1.0 + static_cast<double>( ( i * 57 ) % ( cells - 2 ) );
    sample.value = 1.0 + static_cast<double>( i % 20 );
    input.samples.push_back( std::move( sample ) );
  }
  input.originalCount = wells;
  input.validCount = wells;
  for ( int lineIndex = 0; lineIndex < directionLines; ++lineIndex )
  {
    ConstraintLine line;
    line.stableId = "direction-" + std::to_string( lineIndex );
    line.semantic = Semantic::DirectionGuide;
    line.ratio = 8;
    const double y = cells * ( lineIndex + 1.0 ) / ( directionLines + 1.0 );
    line.points.reserve( static_cast<std::size_t>( segmentsEach + 1 ) );
    for ( int step = 0; step <= segmentsEach; ++step )
      line.points.push_back( { cells * step / static_cast<double>( segmentsEach ), y } );
    input.constraints.push_back( std::move( line ) );
  }
  for ( int lineIndex = 0; lineIndex < softLines; ++lineIndex )
  {
    ConstraintLine line;
    line.stableId = "soft-" + std::to_string( lineIndex );
    line.semantic = Semantic::InterpretiveBoundary;
    line.softStrength = 0.35;
    const double y = cells * ( lineIndex + 1.0 ) / ( softLines + 2.0 );
    line.points = { { 0, y }, { static_cast<double>( cells ), y } };
    input.constraints.push_back( std::move( line ) );
  }
  return input;
}

long rssKiB()
{
  std::ifstream status( "/proc/self/status" );
  std::string key;
  long value = -1;
  while ( status >> key )
  {
    if ( key == "VmRSS:" )
    {
      status >> value;
      break;
    }
    status.ignore( std::numeric_limits<std::streamsize>::max(), '\n' );
  }
  return value;
}

qint64 runOnce( const PreparedInput &input, const GridSpec &grid, const ResolvedParameters &params,
                const Control &control, Status *status )
{
  const auto started = std::chrono::steady_clock::now();
  const SurfaceResult surface = evaluateLocalIdw( input, grid, params, control );
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started );
  if ( status )
    *status = surface.status;
  return elapsed.count();
}

qint64 medianOf( const PreparedInput &input, const GridSpec &grid, const ResolvedParameters &params, int repeats,
                 Status *last )
{
  Control idle;
  Status status = Status::InvalidInput;
  runOnce( input, grid, params, idle, &status );
  std::vector<qint64> samples;
  samples.reserve( static_cast<std::size_t>( repeats ) );
  for ( int i = 0; i < repeats; ++i )
    samples.push_back( runOnce( input, grid, params, idle, &status ) );
  std::sort( samples.begin(), samples.end() );
  if ( last )
    *last = status;
  return samples[samples.size() / 2];
}

ResolvedParameters resolvedFor( const PreparedInput &input, const GridSpec &grid )
{
  ResolvedParameters params;
  params.power = 2;
  params.coverage = CoverageMode::WellSupported;
  params.minPoints = 3;
  params.maxPoints = 12;
  const std::string error = resolveParameters( input, grid, &params );
  if ( !error.empty() || !params.autosApplied )
    params.power = -1;
  return params;
}

} // namespace

class SingleFactorPerfTests : public QObject
{
  Q_OBJECT
  private slots:
    void proposedBudgets();
};

void SingleFactorPerfTests::proposedBudgets()
{
  const PreparedInput small = makeInput( 100, 512, 4, 16, 2 );
  const PreparedInput large = makeInput( 100, 1024, 8, 16, 4 );
  const PreparedInput denser = makeInput( 200, 512, 4, 16, 2 );
  // 像元比只放大网格。井点坐标和约束折线与 S 相同，成图域改成 1024 的整幅。
  PreparedInput scaled = small;
  scaled.domain = { square( 0, 0, 1024, 1024 ) };
  const GridSpec gridS = gridOf( 512 );
  const GridSpec gridL = gridOf( 1024 );
  const ResolvedParameters paramsS = resolvedFor( small, gridS );
  const ResolvedParameters paramsL = resolvedFor( large, gridL );
  const ResolvedParameters paramsD = resolvedFor( denser, gridS );
  const ResolvedParameters paramsScale = resolvedFor( scaled, gridL );
  QVERIFY2( paramsS.autosApplied && paramsL.autosApplied && paramsD.autosApplied && paramsScale.autosApplied,
            "resolveParameters failed" );

  Status status = Status::InvalidInput;
  const qint64 smallMs = medianOf( small, gridS, paramsS, 5, &status );
  QCOMPARE( static_cast<int>( status ), static_cast<int>( Status::Ok ) );
  const qint64 scaleMs = medianOf( scaled, gridL, paramsScale, 5, &status );
  QCOMPARE( static_cast<int>( status ), static_cast<int>( Status::Ok ) );
  const qint64 largeMs = medianOf( large, gridL, paramsL, 5, &status );
  QCOMPARE( static_cast<int>( status ), static_cast<int>( Status::Ok ) );
  const qint64 denseMs = medianOf( denser, gridS, paramsD, 5, &status );
  QCOMPARE( static_cast<int>( status ), static_cast<int>( Status::Ok ) );

  const long before = rssKiB();
  Status held = Status::InvalidInput;
  const SurfaceResult kept = evaluateLocalIdw( large, gridL, paramsL, {} );
  const long after = rssKiB();
  held = kept.status;
  QCOMPARE( static_cast<int>( held ), static_cast<int>( Status::Ok ) );
  const long rssDeltaKiB = after - before;

  std::vector<qint64> cancels;
  for ( int i = 0; i < 5; ++i )
  {
    std::atomic<bool> stop{ false };
    Control control;
    control.cancelled = [&stop] { return stop.load(); };
    control.progress = [&stop]( double ) { stop.store( true ); };
    Status cancelStatus = Status::InvalidInput;
    cancels.push_back( runOnce( large, gridL, paramsL, control, &cancelStatus ) );
    QCOMPARE( static_cast<int>( cancelStatus ), static_cast<int>( Status::Cancelled ) );
  }
  std::sort( cancels.begin(), cancels.end() );
  const qint64 cancelMedian = cancels[cancels.size() / 2];
  const qint64 cancelMax = cancels.back();

  qInfo().noquote() << QStringLiteral( "single-factor perf S=%1ms scale1024=%2ms L=%3ms wells200=%4ms "
                                       "cellRatio=%5 wellRatio=%6 cancelMedian=%7ms cancelMax=%8ms "
                                       "rssDeltaKiB=%9" )
                           .arg( smallMs )
                           .arg( scaleMs )
                           .arg( largeMs )
                           .arg( denseMs )
                           .arg( scaleMs / static_cast<double>( std::max<qint64>( smallMs, 1 ) ), 0, 'f', 2 )
                           .arg( denseMs / static_cast<double>( std::max<qint64>( smallMs, 1 ) ), 0, 'f', 2 )
                           .arg( cancelMedian )
                           .arg( cancelMax )
                           .arg( rssDeltaKiB );

  QVERIFY2( smallMs <= 5000, qPrintable( QStringLiteral( "S median %1 ms" ).arg( smallMs ) ) );
  QVERIFY2( largeMs <= 15000, qPrintable( QStringLiteral( "L median %1 ms" ).arg( largeMs ) ) );
  QVERIFY2( scaleMs <= smallMs * 6,
            qPrintable( QStringLiteral( "cell ratio %1 / %2" ).arg( scaleMs ).arg( smallMs ) ) );
  QVERIFY2( denseMs <= smallMs * 3.5,
            qPrintable( QStringLiteral( "well ratio %1 / %2" ).arg( denseMs ).arg( smallMs ) ) );
  QVERIFY2( cancelMedian <= 250, qPrintable( QStringLiteral( "cancel median %1 ms" ).arg( cancelMedian ) ) );
  QVERIFY2( cancelMax <= 1000, qPrintable( QStringLiteral( "cancel max %1 ms" ).arg( cancelMax ) ) );
  QVERIFY2( rssDeltaKiB <= 512 * 1024,
            qPrintable( QStringLiteral( "RSS increment %1 KiB" ).arg( rssDeltaKiB ) ) );
}

QTEST_MAIN( SingleFactorPerfTests )
#include "tst_singlefactor_perf.moc"
