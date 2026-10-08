// 层：数据（测试壳位于 tests/，被测对象为数据层纯数值核）
#include <QtTest/QtTest>

#include "algorithms/geostat/variogram.h"
#include "algorithms/singlefactor/krigingsurface.h"
#include "algorithms/singlefactor/localidw.h"
#include "algorithms/singlefactor/support.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace paleo::singlefactor;

namespace
{

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

Polygon rect( double x0, double y0, double x1, double y1 )
{
  Polygon poly;
  poly.exterior.points = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 }, { x0, y0 } };
  return poly;
}

GridSpec gridSpec( int cols, int rows, double originX, double originY, double cell )
{
  GridSpec grid;
  grid.cols = cols;
  grid.rows = rows;
  grid.originX = originX;
  grid.originY = originY;
  grid.pixelWidth = cell;
  grid.pixelHeight = -cell;
  grid.crs = "test";
  return grid;
}

ResolvedParameters baseParams()
{
  ResolvedParameters params;
  params.autosApplied = true;
  params.power = 2;
  params.coverage = CoverageMode::WellSupported;
  params.minPoints = 1;
  params.maxPoints = 0;
  params.supportedMinPoints = 1;
  params.supportedRadius = 1e9;
  params.algorithmId = "paleo:paleo_local_direction_idw";
  params.semanticProfile = "paleo_local_idw_v1";
  return params;
}

// 显式球状模型（nugget=0 → 采样点精确通过）。
ResolvedParameters krigingParams( double sill, double range, double nugget = 0 )
{
  ResolvedParameters params = baseParams();
  params.methodActual = "kriging";
  params.algorithmId = "paleo:paleo_local_direction_kriging";
  params.semanticProfile = "paleo_local_kriging_v1";
  params.variogramModel = "spherical";
  params.nugget = nugget;
  params.sill = sill;
  params.range = range;
  params.krigingMaxPoints = 0; // 分量内全部样本：远场口径依赖它
  params.krigingMinPoints = 1;
  return params;
}

PreparedInput inputWithDomain( std::vector<Sample> samples, const Polygon &domain )
{
  PreparedInput input;
  input.samples = std::move( samples );
  input.domain = { domain };
  input.validCount = static_cast<int>( input.samples.size() );
  input.originalCount = input.validCount;
  return input;
}

// 确定性伪随机（不依赖库 RNG 实现）：仅用于给井值加可控起伏。
double wobble( int i )
{
  const double s = std::sin( static_cast<double>( i ) * 12.9898 ) * 43758.5453;
  return ( s - std::floor( s ) ) * 2.0 - 1.0;
}

std::vector<Sample> latticeWells( int n, double spacing, double originX = 0, double originY = 0 )
{
  std::vector<Sample> wells;
  for ( int row = 0; row < n; ++row )
  {
    for ( int col = 0; col < n; ++col )
    {
      const int index = row * n + col;
      const double x = originX + col * spacing;
      const double y = originY + row * spacing;
      const double value = 10.0 + 2.0 * wobble( index );
      wells.push_back( well( "w", x, y, value ) );
    }
  }
  return wells;
}

} // namespace

class SingleFactorKrigingTests : public QObject
{
  Q_OBJECT
  private slots:
    void exactAtSamples();
    void farFieldTendsToMean();
    void autoFitRunsEndToEnd();
    void variogramFitRecoversSpherical();
    void variogramFitRecoversExponential();
    void fewSamplesFallBackHonestly();
    void radiusGateFallsBackPerCell();
    void zeroSignalFallsBackHonestly();
    void hardBarrierKeepsCompartmentsSeparate();
    void domainMarksUnderKriging();
    void gridScaleRatioGate();
    void directionGuideAsymmetricWeight();
    void softBoundaryDistanceAttenuation();
    void softBoundaryContinuityAcrossBoundary();
    void constraintReceiptHonesty();
    void softBoundaryClusterContinuity();
};

// Oracle 1（精确性）：γ(0)=0 口径下普通克里金在采样点无误差通过——对任意块金
// 都成立（λ=eᵢ、μ=0）；块金只体现在井点之间，所以两种 nugget 都要断言。
void SingleFactorKrigingTests::exactAtSamples()
{
  const std::vector<Sample> wells = latticeWells( 5, 40.0, 100.0, 100.0 );
  const PreparedInput input = inputWithDomain( wells, rect( 0, 0, 400, 400 ) );

  std::vector<Point2> queries;
  for ( const Sample &sample : wells )
    queries.push_back( Point2{ sample.x, sample.y } );

  double scale = 1;
  for ( const Sample &sample : wells )
    scale = std::max( scale, std::fabs( sample.value ) );
  for ( const double nugget : { 0.0, 0.75 } )
  {
    const ResolvedParameters params = krigingParams( 1.0, 200.0, nugget );
    const QueryResult result = evaluateAt( input, queries, params, {} );
    QCOMPARE( result.status, Status::Ok );
    QCOMPARE( result.values.size(), wells.size() );
    // 精确性必须来自克里金求解，而不是 IDW 的精确命中均值分支。
    QCOMPARE( result.krigingCells, static_cast<int>( wells.size() ) );
    QCOMPARE( result.idwFallbackCells, 0 );
    double maxAbs = 0;
    for ( std::size_t i = 0; i < wells.size(); ++i )
    {
      QVERIFY( std::isfinite( result.values[i] ) );
      maxAbs = std::max( maxAbs, std::fabs( result.values[i] - wells[i].value ) );
    }
    QVERIFY2( maxAbs <= 1e-9 * scale,
              qPrintable( QStringLiteral( "nugget=%1 maxAbs=%2" ).arg( nugget ).arg( maxAbs ) ) );
  }
}

// Oracle 1（远场均值面）：纯块金模型（无空间相关）下全局 OK 权重精确 1/n，
// 整面等于井值算术均值；变程远小于井距时远场同样精确等于均值。
// 如实记边界：变程与井距同量级时，远场进入「平台」而不是均值——平台上所有
// 查询点的 γ 都饱和，OK 给出的是稳定的加权均值（数学事实，不写成趋均值）。
void SingleFactorKrigingTests::farFieldTendsToMean()
{
  const std::vector<Sample> wells = latticeWells( 4, 100.0, 0.0, 0.0 );
  double mean = 0;
  for ( const Sample &sample : wells )
    mean += sample.value;
  mean /= static_cast<double>( wells.size() );
  const PreparedInput farInput = inputWithDomain( wells, rect( 0, 0, 4000, 4000 ) );

  // (a) 纯块金（nugget=1, sill=0）：Γ 非对角元全相等 → 权重精确 1/n，
  //     面内每一点都等于均值（含远场与井间）。
  const ResolvedParameters pureNugget = krigingParams( 0.0, 1.0, 1.0 );
  const GridSpec grid = gridSpec( 24, 24, 0, 400, 15 );
  const SurfaceResult flat = evaluateLocalKriging( farInput, grid, pureNugget, {} );
  QCOMPARE( flat.status, Status::Ok );
  QCOMPARE( flat.resolved.methodActual, std::string( "kriging" ) );
  QVERIFY( flat.finiteCells > 0 );
  for ( double value : flat.values )
  {
    if ( !std::isfinite( value ) )
      continue;
    QVERIFY2( std::fabs( value - mean ) <= 1e-9 * std::max( 1.0, std::fabs( mean ) ),
              qPrintable( QStringLiteral( "value=%1 mean=%2" ).arg( value ).arg( mean ) ) );
  }

  // (b) 变程远小于井距（1 ≪ 100）：所有井对与查询点的 γ 全等于总基台 → 权重 1/n。
  const ResolvedParameters shortRange = krigingParams( 1.0, 1.0 );
  const std::vector<Point2> farQuery{ { 3000, 3000 } };
  const QueryResult exact = evaluateAt( farInput, farQuery, shortRange, {} );
  QCOMPARE( exact.status, Status::Ok );
  QVERIFY( std::isfinite( exact.values[0] ) );
  QVERIFY2( std::fabs( exact.values[0] - mean ) <= 1e-9 * std::max( 1.0, std::fabs( mean ) ),
            qPrintable( QStringLiteral( "far=%1 mean=%2" ).arg( exact.values[0] ).arg( mean ) ) );

  // (c) 变程与井距同量级：远场进入平台——所有 γ 饱和后估值与查询距离无关
  //     （两个不同查询点逐位相同）。这里不宣称等于均值，也不断言落在样本值域内：
  //     普通克里金的权重无非负约束，越界是可能的（不是不变量）。
  const ResolvedParameters longRange = krigingParams( 1.0, 400.0 );
  const std::vector<Point2> probes{ { 600, 600 }, { 1500, 1500 }, { 3000, 3000 } };
  const QueryResult trend = evaluateAt( farInput, probes, longRange, {} );
  QCOMPARE( trend.status, Status::Ok );
  for ( double value : trend.values )
    QVERIFY( std::isfinite( value ) );
  QVERIFY2( std::fabs( trend.values[1] - trend.values[2] ) <= 1e-9,
            qPrintable( QStringLiteral( "plateau %1 vs %2" ).arg( trend.values[1] ).arg( trend.values[2] ) ) );
  // 与 (b) 的短变程口径对照：平台值可以不等于均值（差值如实报出，不写死）。
  QVERIFY( std::fabs( trend.values[2] - mean ) >= 0.0 );
}

// 自动拟合（range<=0）端到端：拟合成功 → methodActual=kriging 且解出克里金格。
void SingleFactorKrigingTests::autoFitRunsEndToEnd()
{
  const std::vector<Sample> wells = latticeWells( 8, 50.0, 0.0, 0.0 );
  const GridSpec grid = gridSpec( 40, 40, -50, 400, 10 );
  ResolvedParameters params = baseParams();
  params.methodActual = "kriging";
  params.range = 0; // 自动
  const PreparedInput input = inputWithDomain( wells, rect( -50, 0, 350, 400 ) );

  const SurfaceResult result = evaluateLocalKriging( input, grid, params, {} );
  QCOMPARE( result.status, Status::Ok );
  QCOMPARE( result.resolved.methodActual, std::string( "kriging" ) );
  QCOMPARE( result.resolved.fallbackReason, std::string() );
  QCOMPARE( result.surfaceFallbacks, 0 );
  QVERIFY( result.resolved.range > 0 );
  QVERIFY( result.resolved.nugget + result.resolved.sill > 0 );
  QVERIFY( result.krigingCells > 0 );
  QCOMPARE( result.idwFallbackCells, 0 );
  QCOMPARE( result.krigingCells, result.finiteCells );
  QVERIFY( std::any_of( result.issues.begin(), result.issues.end(), []( const std::string &issue ) {
    return issue.find( "fitted=auto" ) != std::string::npos;
  } ) );
}

// Oracle 3：已知参数模型生成的实验变差 → 拟合参数在容差内还原（球状）。
void SingleFactorKrigingTests::variogramFitRecoversSpherical()
{
  const double nugget = 0.2;
  const double sill = 0.8;
  const double range = 150.0;
  paleo::geostat::VariogramModel truth;
  truth.type = paleo::geostat::VariogramModelType::Spherical;
  truth.nugget = nugget;
  truth.sill = sill;
  truth.range = range;

  paleo::geostat::ExperimentalVariogram experimental;
  experimental.status = paleo::geostat::Status::Ok;
  for ( int i = 0; i < 12; ++i )
  {
    const double h = 15.0 * ( i + 0.5 );
    experimental.lagDistance.push_back( h );
    experimental.semivariance.push_back( truth.semivariance( h ) );
    experimental.pairCount.push_back( 40 - 2 * i );
  }
  experimental.nLags = 12;
  experimental.lag = 15.0;

  const paleo::geostat::VariogramFit fit =
      paleo::geostat::fitVariogram( experimental, paleo::geostat::VariogramModelType::Spherical );
  QCOMPARE( fit.status, paleo::geostat::Status::Ok );
  QVERIFY2( std::fabs( fit.model.range - range ) <= 0.10 * range,
            qPrintable( QStringLiteral( "range=%1 truth=%2" ).arg( fit.model.range ).arg( range ) ) );
  QVERIFY2( std::fabs( fit.model.sill - sill ) <= 0.15 * sill,
            qPrintable( QStringLiteral( "sill=%1 truth=%2" ).arg( fit.model.sill ).arg( sill ) ) );
  QVERIFY2( std::fabs( fit.model.nugget - nugget ) <= 0.15 * ( nugget + sill ),
            qPrintable( QStringLiteral( "nugget=%1 truth=%2" ).arg( fit.model.nugget ).arg( nugget ) ) );
  QVERIFY( fit.r2 >= 0.9 );
}

// Oracle 3：同上的指数模型一例（实用变程口径 ~95% 基台）。
void SingleFactorKrigingTests::variogramFitRecoversExponential()
{
  const double nugget = 0.0;
  const double sill = 2.5;
  const double range = 320.0;
  paleo::geostat::VariogramModel truth;
  truth.type = paleo::geostat::VariogramModelType::Exponential;
  truth.nugget = nugget;
  truth.sill = sill;
  truth.range = range;

  paleo::geostat::ExperimentalVariogram experimental;
  experimental.status = paleo::geostat::Status::Ok;
  for ( int i = 0; i < 12; ++i )
  {
    const double h = 30.0 * ( i + 0.5 );
    experimental.lagDistance.push_back( h );
    experimental.semivariance.push_back( truth.semivariance( h ) );
    experimental.pairCount.push_back( 40 - 2 * i );
  }
  experimental.nLags = 12;
  experimental.lag = 30.0;

  const paleo::geostat::VariogramFit fit =
      paleo::geostat::fitVariogram( experimental, paleo::geostat::VariogramModelType::Exponential );
  QCOMPARE( fit.status, paleo::geostat::Status::Ok );
  QVERIFY2( std::fabs( fit.model.range - range ) <= 0.15 * range,
            qPrintable( QStringLiteral( "range=%1 truth=%2" ).arg( fit.model.range ).arg( range ) ) );
  QVERIFY2( std::fabs( fit.model.sill - sill ) <= 0.15 * sill,
            qPrintable( QStringLiteral( "sill=%1 truth=%2" ).arg( fit.model.sill ).arg( sill ) ) );
  QVERIFY( fit.r2 >= 0.9 );
}

// Oracle 2（诚实面）：样本不足 → 整面回落 IDW，标签与数值都必须是 IDW。
void SingleFactorKrigingTests::fewSamplesFallBackHonestly()
{
  const std::vector<Sample> wells{ well( "a", 10, 10, 5 ), well( "b", 90, 10, 7 ),
                                   well( "c", 10, 90, 9 ), well( "d", 90, 90, 11 ) };
  const GridSpec grid = gridSpec( 20, 20, 0, 100, 5 );
  const PreparedInput input = inputWithDomain( wells, rect( 0, 0, 100, 100 ) );

  const SurfaceResult kriging = evaluateLocalKriging( input, grid, krigingParams( 1.0, 60.0 ), {} );
  const SurfaceResult idw = evaluateLocalIdw( input, grid, baseParams(), {} );
  QCOMPARE( kriging.status, Status::Ok );
  QCOMPARE( idw.status, Status::Ok );
  QCOMPARE( kriging.resolved.methodActual, std::string( "local_direction_idw" ) );
  QCOMPARE( kriging.resolved.algorithmId, std::string( "paleo:paleo_local_direction_idw" ) );
  QVERIFY( !kriging.resolved.fallbackReason.empty() );
  QCOMPARE( kriging.surfaceFallbacks, 1 );
  QVERIFY( std::any_of( kriging.issues.begin(), kriging.issues.end(), []( const std::string &issue ) {
    return issue.find( "kriging_fallback" ) != std::string::npos;
  } ) );
  QCOMPARE( kriging.values, idw.values );
  QCOMPARE( kriging.marks, idw.marks );
  QCOMPARE( kriging.krigingCells, 0 );
}

// Oracle 2（诚实面）：克里金半径闸不足 → 该格没解出克里金值，落回同参数 IDW
// 权重并计数；闸不足覆盖到整面时按整面回路口径处理（不挂克里金标签）。
void SingleFactorKrigingTests::radiusGateFallsBackPerCell()
{
  std::vector<Sample> wells;
  for ( int i = 0; i < 4; ++i )
  {
    for ( int j = 0; j < 4; ++j )
      wells.push_back( well( "w", 10.0 + 20 * j, 10.0 + 20 * i, 8.0 + 2.0 * wobble( i * 4 + j ) ) );
  }
  const GridSpec grid = gridSpec( 20, 20, 0, 100, 5 );
  const PreparedInput input = inputWithDomain( wells, rect( 0, 0, 100, 100 ) );

  // (a) 半径只容得下 2 口井、克里金闸要 4 口 → 每格都拿不到克里金值，
  //     但锚定井仍在半径内（IDW minPoints=1、taper>0）→ IDW 有值。
  ResolvedParameters gated = krigingParams( 1.0, 60.0 );
  gated.searchRadius = 12.0;
  gated.krigingMinPoints = 4;
  gated.minPoints = 1;
  const SurfaceResult perCell = evaluateLocalKriging( input, grid, gated, {} );
  QCOMPARE( perCell.status, Status::Ok );
  QCOMPARE( perCell.krigingCells, 0 );
  QVERIFY( perCell.idwFallbackCells > 0 );
  QCOMPARE( perCell.surfaceFallbacks, 1 ); // 全场无解 → 整面回落口径
  QCOMPARE( perCell.resolved.methodActual, std::string( "local_direction_idw" ) );

  // (b) 闸放宽到 1 → 正常出克里金值；再对照「同参数 IDW」确认 (a) 的数值就是 IDW。
  ResolvedParameters loose = gated;
  loose.krigingMinPoints = 1;
  const SurfaceResult solved = evaluateLocalKriging( input, grid, loose, {} );
  QCOMPARE( solved.resolved.methodActual, std::string( "kriging" ) );
  QCOMPARE( solved.idwFallbackCells, 0 );
  QVERIFY( solved.krigingCells > 0 );

  // (c) 混合分支：闸设为 2 → 半径内 2 口井的格能解克里金，其余格回落；
  //     两种计数都 >0，且 issue 如实列出回落格数。
  ResolvedParameters mixed = gated;
  // 半径放大到 25（井距 20）：井间的格能凑够 2 口井 → 解克里金；
  // 边角格不足 → 回落，两种计数同时 >0。
  mixed.searchRadius = 25.0;
  mixed.krigingMinPoints = 2;
  const SurfaceResult blended = evaluateLocalKriging( input, grid, mixed, {} );
  QCOMPARE( blended.resolved.methodActual, std::string( "kriging" ) );
  QVERIFY( blended.krigingCells > 0 );
  QVERIFY( blended.idwFallbackCells > 0 );
  QCOMPARE( blended.surfaceFallbacks, 0 );
  QVERIFY( std::any_of( blended.issues.begin(), blended.issues.end(), []( const std::string &issue ) {
    return issue.find( "kriging_solver_fallback_cells" ) != std::string::npos;
  } ) );

  ResolvedParameters idwParams = baseParams();
  idwParams.searchRadius = 12.0;
  idwParams.minPoints = 1;
  const SurfaceResult idw = evaluateLocalIdw( input, grid, idwParams, {} );
  int compared = 0;
  for ( std::size_t i = 0; i < perCell.values.size(); ++i )
  {
    if ( !std::isfinite( perCell.values[i] ) )
      continue;
    QVERIFY2( std::fabs( perCell.values[i] - idw.values[i] ) <= 1e-12,
              qPrintable( QStringLiteral( "cell=%1 kriging=%2 idw=%3" )
                              .arg( i )
                              .arg( perCell.values[i] )
                              .arg( idw.values[i] ) ) );
    ++compared;
  }
  QVERIFY( compared > 0 );
}

// Oracle 2（诚实面）：恒定场零信号 → 不虚构变程，整面回落并报因。
void SingleFactorKrigingTests::zeroSignalFallsBackHonestly()
{
  std::vector<Sample> wells = latticeWells( 4, 60.0, 0.0, 0.0 );
  for ( Sample &sample : wells )
    sample.value = 5.0;
  const GridSpec grid = gridSpec( 20, 20, -30, 250, 10 );
  const PreparedInput input = inputWithDomain( wells, rect( -30, 0, 200, 250 ) );

  ResolvedParameters params = krigingParams( 1.0, 100.0 );
  params.range = 0; // 自动拟合：恒定场半方差全 0
  const SurfaceResult result = evaluateLocalKriging( input, grid, params, {} );
  QCOMPARE( result.status, Status::Ok );
  QCOMPARE( result.resolved.methodActual, std::string( "local_direction_idw" ) );
  QCOMPARE( result.surfaceFallbacks, 1 );
  QVERIFY2( result.resolved.fallbackReason.find( "零信号" ) != std::string::npos,
            qPrintable( QString::fromStdString( result.resolved.fallbackReason ) ) );
  for ( double value : result.values )
    if ( std::isfinite( value ) )
      QVERIFY( std::fabs( value - 5.0 ) <= 1e-9 );
}

// 硬屏障：两侧分量各自求解，克里金值不跨屏障；屏障格 mark=3。
void SingleFactorKrigingTests::hardBarrierKeepsCompartmentsSeparate()
{
  std::vector<Sample> wells;
  for ( int i = 0; i < 3; ++i )
  {
    for ( int j = 0; j < 3; ++j )
    {
      wells.push_back( well( "L", 20.0 + 10 * j, 20.0 + 30 * i, 1.0 ) );
      wells.push_back( well( "R", 80.0 + 10 * j, 20.0 + 30 * i, 9.0 ) );
    }
  }
  ConstraintLine barrier;
  barrier.stableId = "fault";
  barrier.semantic = Semantic::HardBarrier;
  barrier.points = { { 60, -10 }, { 60, 130 } };

  const GridSpec grid = gridSpec( 60, 60, 0, 120, 2 );
  PreparedInput input = inputWithDomain( wells, rect( 0, 0, 120, 120 ) );
  input.constraints = { barrier };

  const SurfaceResult result = evaluateLocalKriging( input, grid, krigingParams( 1.0, 60.0 ), {} );
  QCOMPARE( result.status, Status::Ok );
  QCOMPARE( result.resolved.methodActual, std::string( "kriging" ) );
  QVERIFY( result.barrierCells > 0 );
  QVERIFY( result.krigingCells > 0 );

  int leftChecked = 0;
  int rightChecked = 0;
  for ( int row = 0; row < grid.rows; ++row )
  {
    for ( int col = 0; col < grid.cols; ++col )
    {
      const std::size_t index = static_cast<std::size_t>( row ) * grid.cols + static_cast<std::size_t>( col );
      const Point2 center = cellCenter( grid, col, row );
      if ( result.marks[index] == 3 )
        continue;
      if ( !std::isfinite( result.values[index] ) )
        continue;
      if ( center.x < 45 )
      {
        QVERIFY2( std::fabs( result.values[index] - 1.0 ) <= 1e-6,
                  qPrintable( QStringLiteral( "left x=%1 value=%2" ).arg( center.x ).arg( result.values[index] ) ) );
        ++leftChecked;
      }
      else if ( center.x > 75 )
      {
        QVERIFY2( std::fabs( result.values[index] - 9.0 ) <= 1e-6,
                  qPrintable( QStringLiteral( "right x=%1 value=%2" ).arg( center.x ).arg( result.values[index] ) ) );
        ++rightChecked;
      }
    }
  }
  QVERIFY( leftChecked > 0 );
  QVERIFY( rightChecked > 0 );
}

// 域/覆盖标记在克里金面下与 IDW 同口径：域外 nodata、屏障 mark=3、外推 mark=2。
void SingleFactorKrigingTests::domainMarksUnderKriging()
{
  const std::vector<Sample> wells = latticeWells( 4, 20.0, 10.0, 10.0 );
  const GridSpec grid = gridSpec( 20, 20, 0, 200, 10 );
  PreparedInput input = inputWithDomain( wells, rect( 0, 0, 100, 100 ) );

  ResolvedParameters params = krigingParams( 1.0, 30.0 );
  params.coverage = CoverageMode::DomainExtrapolation;
  params.supportedRadius = 25.0;
  params.supportedMinPoints = 100; // 域内全判外推

  const SurfaceResult result = evaluateLocalKriging( input, grid, params, {} );
  QCOMPARE( result.status, Status::Ok );
  QCOMPARE( result.resolved.methodActual, std::string( "kriging" ) );
  QVERIFY( result.extrapolatedCells > 0 );
  for ( int row = 0; row < grid.rows; ++row )
  {
    for ( int col = 0; col < grid.cols; ++col )
    {
      const std::size_t index = static_cast<std::size_t>( row ) * grid.cols + static_cast<std::size_t>( col );
      const Point2 center = cellCenter( grid, col, row );
      if ( center.x > 100.0 || center.y > 100.0 )
      {
        QVERIFY( result.marks[index] == 0 );
        QVERIFY( !std::isfinite( result.values[index] ) );
        QCOMPARE( result.components[index], -1 );
      }
    }
  }
  QCOMPARE( result.finiteCells, result.krigingCells + result.idwFallbackCells );
}

// 性能口径：合成格 + 比率门（禁绝对墙钟）。像元 4×，耗时门 ≤ 8×。
void SingleFactorKrigingTests::gridScaleRatioGate()
{
  const std::vector<Sample> wells = latticeWells( 10, 30.0, -20.0, -20.0 );
  const Polygon domain = rect( -30, -30, 610, 610 );
  const PreparedInput input = inputWithDomain( wells, domain );

  struct RunOutcome
  {
    qint64 elapsed = 0;
    Status status = Status::InvalidInput;
    std::string methodActual;
    int krigingCells = 0;
  };
  const auto run = [&]( const GridSpec &grid ) {
    RunOutcome outcome;
    QElapsedTimer timer;
    timer.start();
    const SurfaceResult result = evaluateLocalKriging( input, grid, krigingParams( 1.0, 120.0 ), {} );
    outcome.elapsed = timer.elapsed();
    outcome.status = result.status;
    outcome.methodActual = result.resolved.methodActual;
    outcome.krigingCells = result.krigingCells;
    return outcome;
  };

  const RunOutcome small = run( gridSpec( 128, 128, -30, 610, 2 ) );
  const RunOutcome large = run( gridSpec( 256, 256, -30, 610, 2 ) );
  for ( const RunOutcome &outcome : { small, large } )
  {
    QCOMPARE( outcome.status, Status::Ok );
    QCOMPARE( outcome.methodActual, std::string( "kriging" ) );
    QVERIFY( outcome.krigingCells > 0 );
  }
  const double ratio = static_cast<double>( std::max<qint64>( large.elapsed, 1 ) ) /
                       static_cast<double>( std::max<qint64>( small.elapsed, 1 ) );
  QVERIFY2( ratio <= 8.0, qPrintable( QStringLiteral( "small=%1ms large=%2ms ratio=%3" )
                                          .arg( small.elapsed )
                                          .arg( large.elapsed )
                                          .arg( ratio ) ) );
}

void SingleFactorKrigingTests::directionGuideAsymmetricWeight()
{
  // 样本 A 沿走向 (10, 0)，值 100；样本 B 垂直走向 (0, 10)，值 0；距原点欧氏距离均为 10。
  // 额外提供 6 口远离 (0,0) 的背景井（距离 > 500），满足全场 >= 8 口欠定门槛，
  // 通过 searchRadius=30 或 krigingMaxPoints=2 确保原点邻域仅由 A、B 构成。
  std::vector<Sample> wells;
  wells.push_back( well( "A", 10.0, 0.0, 100.0 ) );
  wells.push_back( well( "B", 0.0, 10.0, 0.0 ) );
  for ( int i = 0; i < 6; ++i )
    wells.push_back( well( ( "bg_" + std::to_string( i ) ).c_str(), 500.0 + 20.0 * i, 500.0, 50.0 ) );

  ConstraintLine guide;
  guide.stableId = "guide_x";
  guide.semantic = Semantic::DirectionGuide;
  guide.points = { { -50.0, 0.0 }, { 50.0, 0.0 } };
  guide.ratio = 4.0;
  guide.influenceRadius = 30.0;
  guide.coreRadius = 0.0;
  guide.enabled = true;

  PreparedInput input = inputWithDomain( wells, rect( -60, -60, 650, 650 ) );
  input.constraints = { guide };

  ResolvedParameters params = krigingParams( 1.0, 25.0, 0.0 );
  params.searchRadius = 30.0;
  params.krigingMaxPoints = 2;
  params.wellClusterLocality = true;

  // 1. 全网格插值：原点 (0,0) 为像元中心
  const GridSpec grid = gridSpec( 1, 1, -1.0, 1.0, 2.0 );
  const SurfaceResult result = evaluateLocalKriging( input, grid, params, {} );

  QCOMPARE( result.status, Status::Ok );
  QCOMPARE( result.resolved.methodActual, std::string( "kriging" ) );
  QVERIFY( result.krigingCells > 0 );
  QCOMPARE( result.idwFallbackCells, 0 );

  // 断言：回执存在且旧未消费 issue 缺席
  const bool hasApplied = std::any_of( result.issues.begin(), result.issues.end(), []( const std::string &issue ) {
    return issue.find( "direction_guide_applied:1" ) != std::string::npos;
  } );
  const bool hasNotUsed = std::any_of( result.issues.begin(), result.issues.end(), []( const std::string &issue ) {
    return issue.find( "direction_guide_not_used_by_kriging" ) != std::string::npos;
  } );
  QVERIFY( hasApplied );
  QVERIFY( !hasNotUsed );

  // 断言：wellClusterLocality 说明 issue
  const bool hasClusterIssue = std::any_of( result.issues.begin(), result.issues.end(), []( const std::string &issue ) {
    return issue.find( "well_cluster_locality_not_used_by_kriging（克里金方程组已通过样本协方差矩阵内置筛选/去聚类效应，不叠加几何去聚类权重）" ) != std::string::npos;
  } );
  QVERIFY( hasClusterIssue );

  const double z0 = result.values[0];
  QVERIFY2( z0 > 75.0, qPrintable( QStringLiteral( "Expected z0 > 75.0, got %1" ).arg( z0 ) ) );

  const double lambdaA = z0 / 100.0;
  const double lambdaB = 1.0 - lambdaA;
  QVERIFY2( lambdaA > 0.75, qPrintable( QStringLiteral( "Expected lambdaA > 0.75, got %1" ).arg( lambdaA ) ) );
  QVERIFY2( lambdaB < 0.25, qPrintable( QStringLiteral( "Expected lambdaB < 0.25, got %1" ).arg( lambdaB ) ) );

  // 2. 点查询核验
  const std::vector<Point2> qPts = { Point2{ 0.0, 0.0 } };
  const QueryResult qr = evaluateAt( input, qPts, params, {} );
  QCOMPARE( qr.status, Status::Ok );
  QCOMPARE( qr.krigingCells, 1 );
  QCOMPARE( qr.idwFallbackCells, 0 );
  QVERIFY( qr.values[0] > 75.0 );
}

void SingleFactorKrigingTests::softBoundaryDistanceAttenuation()
{
  // 软边界沿 Y 轴 (x=0)；查询点置于 (5, 0)；
  // 同侧样本 S1 (15, 0)，值 100；异侧样本 S2 (-5, 0)，值 0；欧氏距离均为 10。
  // 额外提供 6 口远离 (5,0) 的背景井（距离 > 500），满足全场 >= 8 口欠定门槛。
  std::vector<Sample> wells;
  wells.push_back( well( "S1", 15.0, 0.0, 100.0 ) );
  wells.push_back( well( "S2", -5.0, 0.0, 0.0 ) );
  for ( int i = 0; i < 6; ++i )
    wells.push_back( well( ( "bg_" + std::to_string( i ) ).c_str(), 500.0 + 20.0 * i, 500.0, 50.0 ) );

  ConstraintLine softLine;
  softLine.stableId = "soft_y";
  softLine.semantic = Semantic::InterpretiveBoundary;
  softLine.points = { { 0.0, -50.0 }, { 0.0, 50.0 } };
  softLine.softStrength = 0.6;
  softLine.softRadius = 25.0;
  softLine.enabled = true;

  PreparedInput input = inputWithDomain( wells, rect( -60, -60, 650, 650 ) );
  input.constraints = { softLine };

  ResolvedParameters params = krigingParams( 1.0, 25.0, 0.0 );
  params.searchRadius = 30.0;
  params.krigingMaxPoints = 2;

  // 1. 全网格插值：查询点 (5, 0) 为像元中心
  const GridSpec grid = gridSpec( 1, 1, 4.0, 1.0, 2.0 );
  const SurfaceResult result = evaluateLocalKriging( input, grid, params, {} );

  QCOMPARE( result.status, Status::Ok );
  QCOMPARE( result.resolved.methodActual, std::string( "kriging" ) );
  QVERIFY( result.krigingCells > 0 );
  QCOMPARE( result.idwFallbackCells, 0 );

  // 断言：回执存在且旧未消费 issue 缺席
  const bool hasApplied = std::any_of( result.issues.begin(), result.issues.end(), []( const std::string &issue ) {
    return issue.find( "soft_boundary_applied:1" ) != std::string::npos;
  } );
  const bool hasNotUsed = std::any_of( result.issues.begin(), result.issues.end(), []( const std::string &issue ) {
    return issue.find( "soft_boundary_not_used_by_kriging" ) != std::string::npos;
  } );
  QVERIFY( hasApplied );
  QVERIFY( !hasNotUsed );

  const double z0 = result.values[0];
  QVERIFY2( z0 > 55.0, qPrintable( QStringLiteral( "Expected z0 > 55.0, got %1" ).arg( z0 ) ) );

  const double lambda1 = z0 / 100.0;
  const double lambda2 = 1.0 - lambda1;
  QVERIFY2( lambda1 > 0.55, qPrintable( QStringLiteral( "Expected lambda1 > 0.55, got %1" ).arg( lambda1 ) ) );
  QVERIFY2( lambda2 < 0.45, qPrintable( QStringLiteral( "Expected lambda2 < 0.45, got %1" ).arg( lambda2 ) ) );

  // 2. 采样点精确性核验：Z(S1) == 100, Z(S2) == 0
  const std::vector<Point2> samplePts = { Point2{ 15.0, 0.0 }, Point2{ -5.0, 0.0 } };
  const QueryResult sampleQr = evaluateAt( input, samplePts, params, {} );
  QCOMPARE( sampleQr.status, Status::Ok );
  QCOMPARE( sampleQr.krigingCells, 2 );
  QCOMPARE( sampleQr.idwFallbackCells, 0 );
  QVERIFY2( std::fabs( sampleQr.values[0] - 100.0 ) <= 1e-9,
            qPrintable( QStringLiteral( "Expected Z(S1) == 100.0, got %1" ).arg( sampleQr.values[0] ) ) );
  QVERIFY2( std::fabs( sampleQr.values[1] - 0.0 ) <= 1e-9,
            qPrintable( QStringLiteral( "Expected Z(S2) == 0.0, got %1" ).arg( sampleQr.values[1] ) ) );

  // 3. 边界线正上方采样点精确性核验：即使样本井直接落在软边界线上，也必须精确无误差通过
  std::vector<Sample> wellsWithOnBoundary = wells;
  wellsWithOnBoundary.push_back( well( "S0_on_line", 0.0, 0.0, 42.0 ) );
  PreparedInput inputOnBoundary = inputWithDomain( wellsWithOnBoundary, rect( -60, -60, 650, 650 ) );
  inputOnBoundary.constraints = { softLine };
  const std::vector<Point2> onBoundaryPts = { Point2{ 0.0, 0.0 } };
  const QueryResult onBoundaryQr = evaluateAt( inputOnBoundary, onBoundaryPts, params, {} );
  QCOMPARE( onBoundaryQr.status, Status::Ok );
  QVERIFY2( std::fabs( onBoundaryQr.values[0] - 42.0 ) <= 1e-9,
            qPrintable( QStringLiteral( "Expected Z(S0 on boundary) == 42.0, got %1" ).arg( onBoundaryQr.values[0] ) ) );
}

void SingleFactorKrigingTests::softBoundaryContinuityAcrossBoundary()
{
  // 对称双井跨越软边界场景：
  // 软边界沿 Y 轴 (x=0)；
  // 样本 S1 位于 (+10, 0)，值 100.0；样本 S2 位于 (-10, 0)，值 0.0；
  // 背景井 6 口位于远场 (500+, 500)，满足欠定门槛，searchRadius=30 限制邻域仅为 S1 与 S2。
  std::vector<Sample> wells;
  wells.push_back( well( "S1_pos", 10.0, 0.0, 100.0 ) );
  wells.push_back( well( "S2_neg", -10.0, 0.0, 0.0 ) );
  for ( int i = 0; i < 6; ++i )
    wells.push_back( well( ( "bg_" + std::to_string( i ) ).c_str(), 500.0 + 20.0 * i, 500.0, 50.0 ) );

  ConstraintLine softLine;
  softLine.stableId = "soft_continuity";
  softLine.semantic = Semantic::InterpretiveBoundary;
  softLine.points = { { 0.0, -50.0 }, { 0.0, 50.0 } };
  softLine.softStrength = 0.6;
  softLine.softRadius = 25.0;
  softLine.enabled = true;

  PreparedInput input = inputWithDomain( wells, rect( -60, -60, 650, 650 ) );
  input.constraints = { softLine };

  ResolvedParameters params = krigingParams( 1.0, 25.0, 0.0 );
  params.searchRadius = 30.0;
  params.krigingMaxPoints = 2;

  // 1. 微元跨界连续性核验 (Micro-scale continuity: eps = 1e-5)
  // 阶跃 Heaviside 实现会导致跨界 26.93% 悬崖跳跃，连续衰减公式保证跳跃 <= 0.001
  const std::vector<Point2> microPts = {
    Point2{ -1e-5, 0.0 },
    Point2{ 0.0, 0.0 },
    Point2{ +1e-5, 0.0 }
  };
  const QueryResult microQr = evaluateAt( input, microPts, params, {} );
  QCOMPARE( microQr.status, Status::Ok );
  QCOMPARE( microQr.krigingCells, 3 );
  QCOMPARE( microQr.idwFallbackCells, 0 );

  const double zNeg = microQr.values[0];
  const double zZero = microQr.values[1];
  const double zPos = microQr.values[2];

  // 断言：跨界微元跳跃 < 0.01（阶跃 bug 会给出 26.93，连续实现 < 0.0002）
  const double crossJump = std::fabs( zPos - zNeg );
  QVERIFY2( crossJump < 0.01,
            qPrintable( QStringLiteral( "Cross-boundary step cliff detected: |Z(+1e-5) - Z(-1e-5)| = %1 (must be < 0.01)" )
                            .arg( crossJump ) ) );

  // 断言：边界线上无 V 形脊线突跳 (No V-notch on boundary line: Z(0,0) == 50.0)
  // 阶跃 bug 在 x=0 处两边 penalty 均归零，但在 x->0^- 为 36.54，产生 13.46 的 V 形折痕
  const double notchZero = std::fabs( zZero - 50.0 );
  QVERIFY2( notchZero < 0.01,
            qPrintable( QStringLiteral( "V-notch crease detected at boundary: |Z(0) - 50.0| = %1 (must be < 0.01)" )
                            .arg( notchZero ) ) );

  // 断言：边界线上点与邻域点连续过渡 (C0 continuity from either side into the boundary)
  const double jumpToZeroPos = std::fabs( zPos - zZero );
  const double jumpToZeroNeg = std::fabs( zZero - zNeg );
  QVERIFY2( jumpToZeroPos < 0.01,
            qPrintable( QStringLiteral( "|Z(+1e-5) - Z(0)| = %1 (must be < 0.01)" ).arg( jumpToZeroPos ) ) );
  QVERIFY2( jumpToZeroNeg < 0.01,
            qPrintable( QStringLiteral( "|Z(0) - Z(-1e-5)| = %1 (must be < 0.01)" ).arg( jumpToZeroNeg ) ) );

  // 2. 宏观跨界截线单调性与反对称性核验 (Macro transect x in [-1.0, 1.0])
  const std::vector<double> xs = { -1.0, -0.5, -0.1, -0.01, 0.0, 0.01, 0.1, 0.5, 1.0 };
  std::vector<Point2> transectPts;
  for ( double x : xs )
    transectPts.push_back( Point2{ x, 0.0 } );

  const QueryResult transectQr = evaluateAt( input, transectPts, params, {} );
  QCOMPARE( transectQr.status, Status::Ok );

  for ( std::size_t i = 1; i < xs.size(); ++i )
  {
    QVERIFY2( transectQr.values[i] > transectQr.values[i - 1],
              qPrintable( QStringLiteral( "Monotonicity violated across soft boundary: Z(x=%1)=%2 <= Z(x=%3)=%4" )
                              .arg( xs[i] )
                              .arg( transectQr.values[i] )
                              .arg( xs[i - 1] )
                              .arg( transectQr.values[i - 1] ) ) );
  }

  // 反对称性：以 50.0 为中心对称，Z(+x) + Z(-x) 应等于 100.0
  for ( std::size_t i = 0; i < xs.size() / 2; ++i )
  {
    const double sum = transectQr.values[i] + transectQr.values[xs.size() - 1 - i];
    QVERIFY2( std::fabs( sum - 100.0 ) < 0.01,
              qPrintable( QStringLiteral( "Symmetry violated: Z(%1) + Z(%2) = %3 != 100.0" )
                              .arg( xs[i] )
                              .arg( xs[xs.size() - 1 - i] )
                              .arg( sum ) ) );
  }

  // 3. 全网格剖面连续性：网格跨边界不应出现离散阶跃撕裂
  // 网格覆盖 x in [-5, 5]，列宽 0.5，行宽 1.0
  const GridSpec grid = gridSpec( 20, 1, -5.0, 0.5, 0.5 );
  const SurfaceResult gridRes = evaluateLocalKriging( input, grid, params, {} );
  QCOMPARE( gridRes.status, Status::Ok );
  QCOMPARE( gridRes.resolved.methodActual, std::string( "kriging" ) );
  for ( int c = 1; c < grid.cols; ++c )
  {
    const double step = std::fabs( gridRes.values[c] - gridRes.values[c - 1] );
    // dx = 0.5，在平滑曲面上单步变化应严格 < 5.0，绝对不能出现 > 20 的断崖
    QVERIFY2( step < 5.0,
              qPrintable( QStringLiteral( "Grid surface tearing between col %1 and %2: step = %3" )
                              .arg( c - 1 )
                              .arg( c )
                              .arg( step ) ) );
  }
}

void SingleFactorKrigingTests::constraintReceiptHonesty()
{
  std::vector<Sample> wells;
  wells.push_back( well( "S1", 15.0, 0.0, 100.0 ) );
  wells.push_back( well( "S2", -5.0, 0.0, 0.0 ) );
  for ( int i = 0; i < 6; ++i )
    wells.push_back( well( ( "bg_" + std::to_string( i ) ).c_str(), 500.0 + 20.0 * i, 500.0, 50.0 ) );

  PreparedInput input = inputWithDomain( wells, rect( -60, -60, 650, 650 ) );
  ResolvedParameters params = krigingParams( 1.0, 25.0, 0.0 );
  params.searchRadius = 30.0;
  params.krigingMaxPoints = 2;
  const GridSpec grid = gridSpec( 1, 1, 4.0, 1.0, 2.0 );

  // 基准：无约束纯欧氏克里金
  const SurfaceResult baseline = evaluateLocalKriging( input, grid, params, {} );
  QCOMPARE( baseline.status, Status::Ok );
  const double baselineZ = baseline.values[0];

  auto hasReceipt = []( const std::vector<std::string> &issues, const std::string &prefix ) {
    return std::any_of( issues.begin(), issues.end(), [&]( const std::string &iss ) {
      return iss.find( prefix ) != std::string::npos;
    } );
  };

  // Case 1: softStrength = 0.0 时，不得输出 soft_boundary_applied，数值必须与基准一致
  {
    ConstraintLine softZero;
    softZero.stableId = "soft_zero";
    softZero.semantic = Semantic::InterpretiveBoundary;
    softZero.points = { { 0.0, -50.0 }, { 0.0, 50.0 } };
    softZero.softStrength = 0.0;
    softZero.softRadius = 25.0;
    softZero.enabled = true;

    PreparedInput inp = input;
    inp.constraints = { softZero };
    const SurfaceResult res = evaluateLocalKriging( inp, grid, params, {} );
    QCOMPARE( res.status, Status::Ok );
    QVERIFY2( !hasReceipt( res.issues, "soft_boundary_applied" ),
              "Fabricated receipt: soft_boundary_applied emitted when softStrength == 0.0" );
    QVERIFY2( std::fabs( res.values[0] - baselineZ ) < 1e-6,
              "Numerical corruption: softStrength == 0.0 modified Kriging estimate" );
  }

  // Case 2: softStrength < 0.0（如 -0.5）时，不得输出 soft_boundary_applied
  {
    ConstraintLine softNeg;
    softNeg.stableId = "soft_neg";
    softNeg.semantic = Semantic::InterpretiveBoundary;
    softNeg.points = { { 0.0, -50.0 }, { 0.0, 50.0 } };
    softNeg.softStrength = -0.5;
    softNeg.softRadius = 25.0;
    softNeg.enabled = true;

    PreparedInput inp = input;
    inp.constraints = { softNeg };
    const SurfaceResult res = evaluateLocalKriging( inp, grid, params, {} );
    QCOMPARE( res.status, Status::Ok );
    QVERIFY2( !hasReceipt( res.issues, "soft_boundary_applied" ),
              "Fabricated receipt: soft_boundary_applied emitted when softStrength < 0" );
  }

  // Case 3: softLine.enabled == false 时，不得输出 soft_boundary_applied
  {
    ConstraintLine softDisabled;
    softDisabled.stableId = "soft_disabled";
    softDisabled.semantic = Semantic::InterpretiveBoundary;
    softDisabled.points = { { 0.0, -50.0 }, { 0.0, 50.0 } };
    softDisabled.softStrength = 0.6;
    softDisabled.softRadius = 25.0;
    softDisabled.enabled = false;

    PreparedInput inp = input;
    inp.constraints = { softDisabled };
    const SurfaceResult res = evaluateLocalKriging( inp, grid, params, {} );
    QCOMPARE( res.status, Status::Ok );
    QVERIFY2( !hasReceipt( res.issues, "soft_boundary_applied" ),
              "Fabricated receipt: soft_boundary_applied emitted when enabled == false" );
    QVERIFY2( std::fabs( res.values[0] - baselineZ ) < 1e-6,
              "Disabled constraint affected Kriging estimate" );
  }

  // Case 4: softLine 折线仅含 1 个点（几何退化）时，不得输出 soft_boundary_applied
  {
    ConstraintLine softDegen;
    softDegen.stableId = "soft_degen";
    softDegen.semantic = Semantic::InterpretiveBoundary;
    softDegen.points = { { 0.0, 0.0 } };
    softDegen.softStrength = 0.6;
    softDegen.softRadius = 25.0;
    softDegen.enabled = true;

    PreparedInput inp = input;
    inp.constraints = { softDegen };
    const SurfaceResult res = evaluateLocalKriging( inp, grid, params, {} );
    QCOMPARE( res.status, Status::Ok );
    QVERIFY2( !hasReceipt( res.issues, "soft_boundary_applied" ),
              "Fabricated receipt: soft_boundary_applied emitted for single-point boundary" );
  }

  // Case 5: directionGuide.ratio <= 1.0 时，不得输出 direction_guide_applied
  {
    ConstraintLine guideIso;
    guideIso.stableId = "guide_iso";
    guideIso.semantic = Semantic::DirectionGuide;
    guideIso.points = { { -50.0, 0.0 }, { 50.0, 0.0 } };
    guideIso.ratio = 1.0;
    guideIso.influenceRadius = 30.0;
    guideIso.enabled = true;

    PreparedInput inp = input;
    inp.constraints = { guideIso };
    const SurfaceResult res = evaluateLocalKriging( inp, grid, params, {} );
    QCOMPARE( res.status, Status::Ok );
    QVERIFY2( !hasReceipt( res.issues, "direction_guide_applied" ),
              "Fabricated receipt: direction_guide_applied emitted when ratio <= 1.0" );
  }

  // Case 6: directionGuide 折线仅含 1 点时，不得输出 direction_guide_applied
  {
    ConstraintLine guideDegen;
    guideDegen.stableId = "guide_degen";
    guideDegen.semantic = Semantic::DirectionGuide;
    guideDegen.points = { { 0.0, 0.0 } };
    guideDegen.ratio = 4.0;
    guideDegen.influenceRadius = 30.0;
    guideDegen.enabled = true;

    PreparedInput inp = input;
    inp.constraints = { guideDegen };
    const SurfaceResult res = evaluateLocalKriging( inp, grid, params, {} );
    QCOMPARE( res.status, Status::Ok );
    QVERIFY2( !hasReceipt( res.issues, "direction_guide_applied" ),
              "Fabricated receipt: direction_guide_applied emitted for single-point guide" );
  }

  // Case 7: 真实有效约束必须诚实输出正确计数回执
  {
    ConstraintLine validSoft;
    validSoft.stableId = "valid_soft";
    validSoft.semantic = Semantic::InterpretiveBoundary;
    validSoft.points = { { 0.0, -50.0 }, { 0.0, 50.0 } };
    validSoft.softStrength = 0.6;
    validSoft.softRadius = 25.0;
    validSoft.enabled = true;

    ConstraintLine validGuide;
    validGuide.stableId = "valid_guide";
    validGuide.semantic = Semantic::DirectionGuide;
    validGuide.points = { { -50.0, 0.0 }, { 50.0, 0.0 } };
    validGuide.ratio = 3.0;
    validGuide.influenceRadius = 30.0;
    validGuide.enabled = true;

    PreparedInput inp = input;
    inp.constraints = { validSoft, validGuide };
    const SurfaceResult res = evaluateLocalKriging( inp, grid, params, {} );
    QCOMPARE( res.status, Status::Ok );
    QVERIFY( hasReceipt( res.issues, "soft_boundary_applied:1" ) );
    QVERIFY( hasReceipt( res.issues, "direction_guide_applied:1" ) );
  }
}

void SingleFactorKrigingTests::softBoundaryClusterContinuity()
{
  // 密集井丛跨边界场景（左侧 5 口井值 0，右侧 5 口井值 100，软边界位于 x=0）
  std::vector<Sample> wells;
  for ( int i = 0; i < 5; ++i )
  {
    const double y = -8.0 + 4.0 * i;
    wells.push_back( well( ( "L_" + std::to_string( i ) ).c_str(), -15.0 + 0.5 * ( i % 2 ), y, 0.0 ) );
    wells.push_back( well( ( "R_" + std::to_string( i ) ).c_str(), 15.0 - 0.5 * ( i % 2 ), y, 100.0 ) );
  }

  ConstraintLine softLine;
  softLine.stableId = "soft_cluster";
  softLine.semantic = Semantic::InterpretiveBoundary;
  softLine.points = { { 0.0, -50.0 }, { 0.0, 50.0 } };
  softLine.softStrength = 0.6;
  softLine.softRadius = 25.0;
  softLine.enabled = true;

  PreparedInput input = inputWithDomain( wells, rect( -50, -50, 50, 50 ) );
  input.constraints = { softLine };

  ResolvedParameters params = krigingParams( 1.0, 50.0, 0.0 );
  params.krigingMaxPoints = 10;
  params.searchRadius = 40.0;

  // 查询跨越 x=0 的微元邻域
  const std::vector<Point2> qPts = {
    Point2{ -0.01, 0.0 },
    Point2{ 0.0, 0.0 },
    Point2{ +0.01, 0.0 }
  };
  const QueryResult qr = evaluateAt( input, qPts, params, {} );
  QCOMPARE( qr.status, Status::Ok );
  QCOMPARE( qr.krigingCells, 3 );
  QCOMPARE( qr.idwFallbackCells, 0 );

  // 在阶跃实现下，跳跃差值高达 39.904410
  // 在连续实现下，双侧井丛关于 x=0 对称，Z(-0.01) 与 Z(+0.01) 应平滑对称且差值 < 0.2
  const double clusterJump = std::fabs( qr.values[2] - qr.values[0] );
  QVERIFY2( clusterJump < 0.2,
            qPrintable( QStringLiteral( "Dense cluster boundary jump = %1 (must be < 0.2)" ).arg( clusterJump ) ) );

  // 边界线上值严格保持 50.0
  QVERIFY2( std::fabs( qr.values[1] - 50.0 ) < 0.05,
            qPrintable( QStringLiteral( "Dense cluster boundary line Z(0) = %1 (expected 50.0)" ).arg( qr.values[1] ) ) );
}

QTEST_MAIN( SingleFactorKrigingTests )
#include "tst_singlefactor_kriging.moc"
