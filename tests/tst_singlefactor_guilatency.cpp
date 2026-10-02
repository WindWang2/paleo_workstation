// 层：测试壳
#include <QtTest/QtTest>

#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "algorithms/singlefactor/localidw.h"
#include "algorithms/singlefactor/support.h"
#include "../src/services/paleotaskservice.h"

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

struct Probe
{
  bool resolved = false;
  PaleoTask::State state = PaleoTask::State::Failed;
  QString error;
  qint64 p95Ms = -1;
  qint64 maxGapMs = -1;
  qint64 wallMs = 0;
  int gaps = 0;
  int cells = 0;
};

Probe probeLoad( int wells, int cells, int directionLines, int segmentsEach, int softLines )
{
  Probe probe;
  probe.cells = cells;
  const PreparedInput input = makeInput( wells, cells, directionLines, segmentsEach, softLines );
  const GridSpec grid = gridOf( cells );
  const ResolvedParameters params = resolvedFor( input, grid );
  probe.resolved = params.autosApplied;
  if ( !probe.resolved )
  {
    probe.error = QStringLiteral( "resolveParameters failed" );
    return probe;
  }

  std::vector<qint64> gaps;
  qint64 previous = -1;
  QElapsedTimer ticks;
  QTimer timer;
  timer.setTimerType( Qt::PreciseTimer );
  timer.setInterval( 10 );
  QObject::connect( &timer, &QTimer::timeout, [&]() {
    const qint64 now = ticks.elapsed();
    if ( previous >= 0 )
      gaps.push_back( now - previous );
    previous = now;
  } );

  PaleoTaskService service;
  ticks.start();
  timer.start();
  QElapsedTimer wall;
  wall.start();
  PaleoTask *task = service.start(
      QStringLiteral( "local-idw" ),
      [input, grid, params]( PaleoTask * ) {
        const SurfaceResult surface = evaluateLocalIdw( input, grid, params, {} );
        if ( surface.status == Status::Ok )
          return QString();
        if ( !surface.message.empty() )
          return QString::fromStdString( surface.message );
        return QString::fromLatin1( statusName( surface.status ) );
      },
      QString(), PaleoTask::Priority::High, true );
  QEventLoop loop;
  QObject::connect( task, &PaleoTask::finished, &loop, &QEventLoop::quit );
  loop.exec();
  probe.wallMs = wall.elapsed();
  timer.stop();
  timer.disconnect();

  probe.state = task->state();
  probe.error = task->errorText();
  probe.gaps = static_cast<int>( gaps.size() );
  if ( !gaps.empty() )
  {
    std::sort( gaps.begin(), gaps.end() );
    const double rank = std::ceil( 0.95 * static_cast<double>( gaps.size() ) );
    std::size_t index = static_cast<std::size_t>( rank );
    if ( index > 0 )
      --index;
    if ( index >= gaps.size() )
      index = gaps.size() - 1;
    probe.p95Ms = gaps[index];
    probe.maxGapMs = gaps.back();
  }
  return probe;
}

} // namespace

class SingleFactorGuiLatencyTests : public QObject
{
  Q_OBJECT

private slots:
  void highPriorityTaskKeepsEventLoopUnder100ms();
};

void SingleFactorGuiLatencyTests::highPriorityTaskKeepsEventLoopUnder100ms()
{
  // §15 GUI row: the same S and L loads as tst_singlefactor_perf, one background run each.
  // p95 is across 10 ms event-loop gaps during that run.
  const Probe small = probeLoad( 100, 512, 4, 16, 2 );
  QVERIFY2( small.resolved, qPrintable( small.error ) );
  QVERIFY2( small.state == PaleoTask::State::Succeeded, qPrintable( small.error ) );
  qInfo().noquote() << QStringLiteral( "gui S p95=%1ms maxGap=%2ms wall=%3ms grid=512 gaps=%4 wells=100 directions=4 soft=2" )
                           .arg( small.p95Ms )
                           .arg( small.maxGapMs )
                           .arg( small.wallMs )
                           .arg( small.gaps );
  QVERIFY2( small.gaps >= 5, qPrintable( QStringLiteral( "only %1 inter-tick gaps on S" ).arg( small.gaps ) ) );
  QVERIFY2( small.p95Ms <= 100, qPrintable( QStringLiteral( "S p95 %1 ms" ).arg( small.p95Ms ) ) );

  const Probe large = probeLoad( 100, 1024, 8, 16, 4 );
  QVERIFY2( large.resolved, qPrintable( large.error ) );
  QVERIFY2( large.state == PaleoTask::State::Succeeded, qPrintable( large.error ) );
  qInfo().noquote() << QStringLiteral( "gui L p95=%1ms maxGap=%2ms wall=%3ms grid=1024 gaps=%4 wells=100 directions=8 soft=4" )
                           .arg( large.p95Ms )
                           .arg( large.maxGapMs )
                           .arg( large.wallMs )
                           .arg( large.gaps );
  QVERIFY2( large.gaps >= 5, qPrintable( QStringLiteral( "only %1 inter-tick gaps on L" ).arg( large.gaps ) ) );
  QVERIFY2( large.p95Ms <= 100, qPrintable( QStringLiteral( "L p95 %1 ms" ).arg( large.p95Ms ) ) );
}

QTEST_MAIN( SingleFactorGuiLatencyTests )
#include "tst_singlefactor_guilatency.moc"
