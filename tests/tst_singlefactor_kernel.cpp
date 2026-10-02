// 层：数据（测试壳位于 tests/，被测对象为数据层纯数值核）
#include <QtTest/QtTest>

#include "algorithms/singlefactor/curvekernel.h"
#include "algorithms/singlefactor/localidw.h"
#include "algorithms/singlefactor/support.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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

Sample well( const char *id, double x, double y, double value )
{
  Sample sample;
  sample.stableRowId = id;
  sample.wellId = id;
  sample.x = x;
  sample.y = y;
  sample.value = value;
  return sample;
}

ResolvedParameters plain()
{
  ResolvedParameters params;
  params.autosApplied = true;
  params.power = 2;
  params.coverage = CoverageMode::DomainExtrapolation;
  params.minPoints = 1;
  params.maxPoints = 0;
  params.supportedMinPoints = 1;
  params.supportedRadius = 1e9;
  params.tolerance = 1e-9;
  return params;
}

GridSpec grid5()
{
  GridSpec grid;
  grid.cols = 5;
  grid.rows = 5;
  grid.originX = 0;
  grid.originY = 5;
  grid.pixelWidth = 1;
  grid.pixelHeight = -1;
  return grid;
}

PreparedInput domainInput( std::vector<Sample> samples )
{
  PreparedInput input;
  input.samples = std::move( samples );
  input.domain = { square( 0, 0, 5, 5 ) };
  input.validCount = static_cast<int>( input.samples.size() );
  input.originalCount = input.validCount;
  return input;
}

double at( const SurfaceResult &surface, int column, int row, int cols )
{
  return surface.values[static_cast<std::size_t>( row * cols + column )];
}

#define EXPECT_STATUS( actual, expected ) \
  QCOMPARE( static_cast<int>( actual ), static_cast<int>( expected ) )

} // namespace

class SingleFactorKernelTests : public QObject
{
  Q_OBJECT
  private slots:
    void basisAndReverseKernel();
    void plainIdwIdentities();
    void duplicatesKeepRows();
    void directionDegenerates();
    void softBoundaryDoesNotStack();
    void clustersAreNotAMask();
    void hardBarrierPartitions();
    void autoParameters();
    void domainGridAndBudget();
    void cartographicWorkTouchesOnlyCrossings();
};

void SingleFactorKernelTests::basisAndReverseKernel()
{
  QCOMPARE( CurveKernel::basis( 0 ), 1.0 );
  QCOMPARE( CurveKernel::basis( 1 ), 0.0 );
  QCOMPARE( CurveKernel::basis( 2 ), 0.0 );
  const std::vector<Point2> forward{ { 0, 0 }, { 10, 0 }, { 10, 6 } };
  std::vector<Point2> backward = forward;
  std::reverse( backward.begin(), backward.end() );
  const CurveKernel a( forward, 8, 2 );
  const CurveKernel b( backward, 8, 2 );
  QVERIFY( a.coreMass() > 0 );
  QCOMPARE( a.centerCount(), b.centerCount() );
  const std::vector<Point2> queries{ { 5, 1 }, { 10, 3 }, { 30, 0 }, { 2, -2 } };
  const auto ea = a.evaluate( queries );
  const auto eb = b.evaluate( queries );
  for ( std::size_t i = 0; i < queries.size(); ++i )
  {
    QVERIFY( std::abs( ea.gate[i] - eb.gate[i] ) <= 1e-12 );
    QVERIFY( std::abs( tangentEnergy( 1.5, -0.25, ea.tensor[i] ) -
                        tangentEnergy( 1.5, -0.25, eb.tensor[i] ) ) <= 1e-12 );
  }
  QVERIFY( ea.gate[2] == 0.0 );
}

void SingleFactorKernelTests::plainIdwIdentities()
{
  PreparedInput input;
  input.samples = { well( "a", 0, 0, 10 ), well( "b", 10, 0, 20 ), well( "c", 0, 10, 10 ),
                    well( "d", 10, 10, 20 ) };
  const ResolvedParameters params = plain();
  const std::vector<Point2> queries{ { 0, 0 }, { 5, 0 }, { 3, 4 }, { 100, 100 } };
  const QueryResult result = evaluateAt( input, queries, params, {} );
  EXPECT_STATUS( result.status, Status::Ok );
  QCOMPARE( result.values[0], 10.0 );
  QCOMPARE( result.values[1], 15.0 );
  const double d1 = 3 * 3 + 4 * 4;
  const double wA = 1.0 / d1;
  const double wB = 1.0 / ( 7 * 7 + 4 * 4 );
  const double wC = 1.0 / ( 3 * 3 + 6 * 6 );
  const double wD = 1.0 / ( 7 * 7 + 6 * 6 );
  const double expected = ( wA * 10 + wB * 20 + wC * 10 + wD * 20 ) / ( wA + wB + wC + wD );
  QVERIFY( std::abs( result.values[2] - expected ) <= 1e-12 );
  for ( double value : result.values )
  {
    QVERIFY( value >= 10.0 - 1e-12 );
    QVERIFY( value <= 20.0 + 1e-12 );
  }

  PreparedInput constant = input;
  for ( Sample &sample : constant.samples )
    sample.value = 7;
  const QueryResult flat = evaluateAt( constant, queries, params, {} );
  for ( double value : flat.values )
    QVERIFY( std::abs( value - 7.0 ) <= 1e-12 );

  PreparedInput one;
  one.samples = { well( "only", 1, 2, 4.5 ) };
  ResolvedParameters strict = params;
  strict.minPoints = 3;
  const std::vector<Point2> singleQueries{ { 1, 2 }, { 5, 5 } };
  const QueryResult single = evaluateAt( one, singleQueries, strict, {} );
  QCOMPARE( single.values[0], 4.5 );
  QVERIFY( std::isnan( single.values[1] ) );

  PreparedInput empty;
  EXPECT_STATUS( evaluateAt( empty, queries, params, {} ).status, Status::InvalidInput );
  ResolvedParameters bad = params;
  bad.power = 0;
  EXPECT_STATUS( evaluateAt( input, queries, bad, {} ).status, Status::InvalidInput );
  Control cancel;
  cancel.cancelled = [] { return true; };
  const QueryResult stopped = evaluateAt( input, queries, params, cancel );
  EXPECT_STATUS( stopped.status, Status::Cancelled );
  QVERIFY( stopped.values.empty() );
}

void SingleFactorKernelTests::duplicatesKeepRows()
{
  PreparedInput input;
  input.samples = { well( "a", 0, 0, 1 ), well( "b", 0, 0, 3 ), well( "c", 8, 0, 9 ) };
  ResolvedParameters params = plain();
  params.maxPoints = 1;
  params.minPoints = 1;
  const std::vector<Point2> dupQueries{ { 0, 0 }, { 1, 0 } };
  const QueryResult result = evaluateAt( input, dupQueries, params, {} );
  EXPECT_STATUS( result.status, Status::Ok );
  QCOMPARE( result.values[0], 2.0 );
  QVERIFY( std::isnan( result.values[1] ) );

  params.maxPoints = 0;
  const std::vector<Point2> away{ { 4, 0 } };
  const QueryResult kept = evaluateAt( input, away, params, {} );
  const double dDup = 16;
  const double dFar = 16;
  const double expected = ( 1.0 / dDup + 3.0 / dDup + 9.0 / dFar ) / ( 1.0 / dDup + 1.0 / dDup + 1.0 / dFar );
  QVERIFY( std::abs( kept.values[0] - expected ) <= 1e-12 );
  QCOMPARE( meanNearestSpacing( { { 0, 0 }, { 0, 0 }, { 10, 0 } } ), 10.0 / 3.0 );
}

void SingleFactorKernelTests::directionDegenerates()
{
  PreparedInput input;
  input.samples = { well( "a", 2, 1, 3 ), well( "b", 8, -1, 7 ) };
  const std::vector<Point2> queries{ { 5, 2 }, { 2, 1 }, { 20, -4 } };
  const QueryResult base = evaluateAt( input, queries, plain(), {} );

  ResolvedParameters ratioOne = plain();
  ratioOne.directions.push_back( ResolvedDirection{ "d", 1, 20, 4, { { 0, 0 }, { 10, 0 } } } );
  const QueryResult off = evaluateAt( input, queries, ratioOne, {} );
  for ( std::size_t i = 0; i < queries.size(); ++i )
    QVERIFY( std::abs( off.values[i] - base.values[i] ) <= 1e-12 );

  ResolvedParameters far = plain();
  far.directions.push_back( ResolvedDirection{ "far", 8, 5, 1, { { -20, 500 }, { 20, 500 } } } );
  const QueryResult outside = evaluateAt( input, queries, far, {} );
  for ( std::size_t i = 0; i < queries.size(); ++i )
    QVERIFY( std::abs( outside.values[i] - base.values[i] ) <= 1e-12 );

  auto run = [&]( const std::vector<Point2> &line, Point2 shift, bool rotate ) {
    PreparedInput moved;
    std::vector<Point2> movedQueries;
    auto map = [&]( Point2 p ) {
      p.x += shift.x;
      p.y += shift.y;
      if ( rotate )
        p = Point2{ -p.y, p.x };
      return p;
    };
    for ( const Sample &sample : input.samples )
    {
      Sample copy = sample;
      const Point2 p = map( Point2{ copy.x, copy.y } );
      copy.x = p.x;
      copy.y = p.y;
      moved.samples.push_back( copy );
    }
    for ( Point2 query : queries )
      movedQueries.push_back( map( query ) );
    std::vector<Point2> mapped;
    for ( Point2 p : line )
      mapped.push_back( map( p ) );
    ResolvedParameters params = plain();
    params.directions.push_back( ResolvedDirection{ "d", 4, 20, 2, mapped } );
    return evaluateAt( moved, movedQueries, params, {} );
  };
  const std::vector<Point2> line{ { 0, 0 }, { 10, 0 } };
  const QueryResult forward = run( line, { 0, 0 }, false );
  const QueryResult backward = run( { { 10, 0 }, { 0, 0 } }, { 0, 0 }, false );
  const QueryResult shifted = run( line, { 1000, -400 }, false );
  const QueryResult turned = run( line, { 0, 0 }, true );
  EXPECT_STATUS( forward.status, Status::Ok );
  for ( std::size_t i = 0; i < queries.size(); ++i )
  {
    QVERIFY( std::abs( forward.values[i] - backward.values[i] ) <= 1e-9 );
    QVERIFY( std::abs( forward.values[i] - shifted.values[i] ) <= 1e-8 );
    QVERIFY( std::abs( forward.values[i] - turned.values[i] ) <= 1e-8 );
  }
  QVERIFY( std::abs( forward.values[0] - base.values[0] ) > 1e-6 );
  QCOMPARE( forward.values[1], base.values[1] );
}

void SingleFactorKernelTests::softBoundaryDoesNotStack()
{
  PreparedInput input;
  input.samples = { well( "left", -2, 0, 0 ), well( "right", 2, 0, 10 ) };
  const std::vector<Point2> queries{ { 6, 0 } };
  const QueryResult base = evaluateAt( input, queries, plain(), {} );
  ResolvedParameters soft = plain();
  soft.soft.push_back( ResolvedSoft{ "s", 10, 0.5, { { 0, -20 }, { 0, 20 } } } );
  const QueryResult once = evaluateAt( input, queries, soft, {} );
  ResolvedParameters twice = soft;
  twice.soft.push_back( soft.soft.front() );
  const QueryResult stacked = evaluateAt( input, queries, twice, {} );
  QVERIFY( once.values[0] > base.values[0] );
  QVERIFY( once.values[0] < 10.0 );
  QVERIFY( std::abs( once.values[0] - stacked.values[0] ) <= 1e-12 );

  ResolvedParameters off = plain();
  off.soft.push_back( ResolvedSoft{ "s", 10, 0, { { 0, -20 }, { 0, 20 } } } );
  const QueryResult disabled = evaluateAt( input, queries, off, {} );
  QVERIFY( std::abs( disabled.values[0] - base.values[0] ) <= 1e-12 );
}

void SingleFactorKernelTests::clustersAreNotAMask()
{
  PreparedInput input;
  input.samples = { well( "a", 0, 0, 0 ), well( "b", 1, 0, 0 ), well( "c", 0, 1, 0 ), well( "d", 50, 0, 10 ) };
  const std::vector<Point2> queries{ { 0.2, 0.2 }, { 500, 500 } };
  const QueryResult base = evaluateAt( input, queries, plain(), {} );
  ResolvedParameters local = plain();
  local.wellClusterLocality = true;
  local.clusterSpan = 100;
  const QueryResult clustered = evaluateAt( input, queries, local, {} );
  EXPECT_STATUS( clustered.status, Status::Ok );
  QVERIFY( std::isfinite( clustered.values[0] ) );
  QVERIFY( std::isfinite( clustered.values[1] ) );
  QVERIFY( clustered.values[0] < base.values[0] );
  QVERIFY( clustered.values[0] > 0.0 );
  QVERIFY( clustered.values[1] >= 0.0 );
  QVERIFY( clustered.values[1] <= 10.0 );
}

void SingleFactorKernelTests::hardBarrierPartitions()
{
  const GridSpec grid = grid5();
  PreparedInput input = domainInput( { well( "L", 0.5, 2.5, 1 ), well( "R", 4.5, 2.5, 9 ) } );
  input.constraints.push_back(
      ConstraintLine{ "wall", Semantic::HardBarrier, { { 2.5, 0 }, { 2.5, 5 } }, true, 8, 0, 0, 0.35, 0, 50, 0, {} } );
  ResolvedParameters params = plain();
  params.tolerance = 1e-6;
  const SurfaceResult split = evaluateLocalIdw( input, grid, params, {} );
  EXPECT_STATUS( split.status, Status::Ok );
  QCOMPARE( at( split, 0, 2, 5 ), 1.0 );
  QCOMPARE( at( split, 4, 2, 5 ), 9.0 );
  QCOMPARE( split.marks[static_cast<std::size_t>( 2 * 5 + 2 )], static_cast<std::uint8_t>( 3 ) );
  QVERIFY( std::isnan( at( split, 2, 2, 5 ) ) );
  QCOMPARE( split.marks[static_cast<std::size_t>( 2 * 5 + 1 )], static_cast<std::uint8_t>( 1 ) );

  input.samples[1].value = 40;
  const SurfaceResult changed = evaluateLocalIdw( input, grid, params, {} );
  QCOMPARE( at( changed, 0, 2, 5 ), 1.0 );
  QCOMPARE( at( changed, 4, 2, 5 ), 40.0 );

  input.samples.pop_back();
  input.samples[0].value = 1;
  const SurfaceResult gap = evaluateLocalIdw( input, grid, params, {} );
  EXPECT_STATUS( gap.status, Status::Ok );
  QVERIFY( std::isnan( at( gap, 4, 2, 5 ) ) );
  QVERIFY( !gap.unsupported.empty() );
  params.requireFullCoverage = true;
  const SurfaceResult refused = evaluateLocalIdw( input, grid, params, {} );
  EXPECT_STATUS( refused.status, Status::InvalidInput );
  QVERIFY( std::isnan( at( refused, 4, 2, 5 ) ) );

  PreparedInput tip = domainInput( { well( "L", 0.5, 2.5, 1 ), well( "R", 4.5, 2.5, 9 ) } );
  tip.constraints.push_back(
      ConstraintLine{ "tip", Semantic::HardBarrier, { { 2.5, 0 }, { 2.5, 2 } }, true, 8, 0, 0, 0.35, 0, 0, 0, {} } );
  const SurfaceResult around = evaluateLocalIdw( tip, grid, plain(), {} );
  EXPECT_STATUS( around.status, Status::Ok );
  const double dxL = 4.5 - 0.5;
  const double dy = 4.5 - 2.5;
  const double dL = dxL * dxL + dy * dy;
  const double dR = dy * dy;
  const double blended = ( 1.0 / dL + 9.0 / dR ) / ( 1.0 / dL + 1.0 / dR );
  QVERIFY( std::abs( at( around, 4, 0, 5 ) - blended ) <= 1e-9 );

  PreparedInput ambiguous = domainInput( { well( "on", 2.5, 2.5, 3 ), well( "L", 0.5, 2.5, 1 ) } );
  ambiguous.constraints = input.constraints;
  const SurfaceResult blocked = evaluateLocalIdw( ambiguous, grid, plain(), {} );
  EXPECT_STATUS( blocked.status, Status::InvalidInput );
  QCOMPARE( static_cast<int>( blocked.ambiguous.size() ), 1 );
  QVERIFY( blocked.values.empty() );

  ambiguous.samples[0].componentOverride = 0;
  const SurfaceResult assigned = evaluateLocalIdw( ambiguous, grid, plain(), {} );
  EXPECT_STATUS( assigned.status, Status::Ok );
  QVERIFY( assigned.ambiguous.empty() );
}

void SingleFactorKernelTests::autoParameters()
{
  PreparedInput input;
  input.samples = { well( "a", 0, 0, 1 ), well( "b", 10, 0, 2 ) };
  input.domain = { square( 0, 0, 100, 100 ) };
  input.constraints.push_back(
      ConstraintLine{ "dir", Semantic::DirectionGuide, { { 0, 50 }, { 50, 50 } }, true, 8, 0, 0, 0, 0, 0, 0, "m" } );
  input.constraints.push_back( ConstraintLine{ "soft",
                                                Semantic::InterpretiveBoundary,
                                                { { 20, 0 }, { 20, 40 } },
                                                true,
                                                1,
                                                0,
                                                0,
                                                0.35,
                                                0,
                                                12,
                                                0,
                                                "m" } );
  GridSpec grid;
  grid.cols = 100;
  grid.rows = 100;
  grid.originX = 0;
  grid.originY = 100;
  grid.pixelWidth = 1;
  grid.pixelHeight = -1;
  ResolvedParameters params = plain();
  params.coverage = CoverageMode::WellSupported;
  params.minPoints = 3;
  params.maxPoints = 12;
  params.searchRadius.reset();
  QVERIFY( resolveParameters( input, grid, &params ).empty() );
  QVERIFY( params.autosApplied );
  QCOMPARE( params.spacing, 10.0 );
  QCOMPARE( static_cast<int>( params.directions.size() ), 1 );
  QVERIFY( std::abs( params.directions[0].influence - 15.0 ) <= 1e-12 );
  QVERIFY( std::abs( params.directions[0].core - 4.5 ) <= 1e-12 );
  QVERIFY( params.searchRadius.has_value() );
  QVERIFY( std::abs( *params.searchRadius - 20.0 ) <= 1e-12 );
  QCOMPARE( static_cast<int>( params.soft.size() ), 1 );
  QVERIFY( std::abs( params.soft[0].radius - 4.0 ) <= 1e-12 );
  QCOMPARE( params.soft[0].strength, 0.35 );
  QVERIFY( params.soft[0].radius != 12.0 );

  params.coverage = CoverageMode::DomainExtrapolation;
  params.searchRadius = 5;
  QVERIFY( resolveParameters( input, grid, &params ).empty() );
  QVERIFY( !params.searchRadius.has_value() );
  QCOMPARE( params.minPoints, 1 );
  QCOMPARE( params.maxPoints, 0 );
  QVERIFY( std::abs( params.supportedRadius - 20.0 ) <= 1e-12 );

  input.constraints[0].ratio = 200;
  const std::string ratioError = resolveParameters( input, grid, &params );
  QVERIFY( !ratioError.empty() );
  QVERIFY( !params.autosApplied );

  PreparedInput lone;
  lone.samples = { well( "a", 0, 0, 1 ) };
  ResolvedParameters noScale = plain();
  noScale.coverage = CoverageMode::WellSupported;
  noScale.searchRadius.reset();
  QVERIFY( !resolveParameters( lone, GridSpec{}, &noScale ).empty() );
}

void SingleFactorKernelTests::domainGridAndBudget()
{
  Polygon ring = square( 0, 0, 10, 10 );
  ring.holes.push_back( Ring{ { { 4, 4 }, { 6, 4 }, { 6, 6 }, { 4, 6 }, { 4, 4 } } } );
  const std::vector<Polygon> domain{ ring };
  QVERIFY( pointInDomain( domain, { 1, 1 }, 1e-9 ) );
  QVERIFY( pointInDomain( domain, { 0, 5 }, 1e-9 ) );
  QVERIFY( !pointInDomain( domain, { 5, 5 }, 1e-9 ) );
  QVERIFY( !pointInDomain( domain, { 4, 5 }, 1e-9 ) );
  QVERIFY( !pointInDomain( domain, { -1, 5 }, 1e-9 ) );
  QVERIFY( !pointInDomain( {}, { 1, 1 }, 0 ) );

  const GridSpec grid = grid5();
  const Point2 north = cellCenter( grid, 0, 0 );
  QCOMPARE( north.x, 0.5 );
  QCOMPARE( north.y, 4.5 );
  QVERIFY( north.y > cellCenter( grid, 0, 1 ).y );

  std::string error;
  GridSpec bad = grid;
  bad.pixelHeight = 1;
  QVERIFY( !gridBudgetOk( bad, 1, &error ) );
  bad = grid;
  bad.cols = 100000;
  bad.rows = 1001;
  QVERIFY( !gridBudgetOk( bad, 1, &error ) );
  QVERIFY( segmentsCross( { 0, 0 }, { 1, 1 }, { 0, 1 }, { 1, 0 } ) );
  QVERIFY( !segmentsCross( { 0, 0 }, { 1, 0 }, { 0, 0 }, { 0, 1 } ) );
  QVERIFY( !segmentsCross( { 0, 0 }, { 2, 0 }, { 0.5, 0 }, { 1.5, 0 } ) );
}

void SingleFactorKernelTests::cartographicWorkTouchesOnlyCrossings()
{
  const GridSpec grid = grid5();
  std::vector<double> analysis( 25 );
  for ( int row = 0; row < 5; ++row )
    for ( int column = 0; column < 5; ++column )
      analysis[static_cast<std::size_t>( row * 5 + column )] = cellCenter( grid, column, row ).x;
  const std::vector<double> original = analysis;
  const ConstraintLine stop{ "stop", Semantic::ContourStop, { { 2, 0 }, { 2, 5 } }, true, 1, 0, 0, 0, 0, 0, 0.2, {} };
  const ContourPolyline crossing{ { { 0, 2 }, { 5, 2 } }, 2 };
  const WorkField changed = buildCartographicWork( grid, analysis, {}, { stop }, { crossing }, { 2 }, 0 );
  EXPECT_STATUS( changed.status, Status::Ok );
  QVERIFY( !changed.unchanged );
  QVERIFY( changed.modifiedCells > 0 );
  QVERIFY( analysis == original );
  QVERIFY( std::abs( changed.coreValue - 2.0 ) < 1.5 );

  const ContourPolyline miss{ { { 3, 2 }, { 5, 2 } }, 2 };
  const WorkField untouched = buildCartographicWork( grid, analysis, {}, { stop }, { miss }, { 2 }, 0 );
  QVERIFY( untouched.unchanged );
  QVERIFY( untouched.values == original );

  const ContourPolyline overlap{ { { 2, 0 }, { 2, 5 } }, 2 };
  const WorkField collinear = buildCartographicWork( grid, analysis, {}, { stop }, { overlap }, { 2 }, 0 );
  QVERIFY( collinear.unchanged );
}

QTEST_MAIN( SingleFactorKernelTests )
#include "tst_singlefactor_kernel.moc"
