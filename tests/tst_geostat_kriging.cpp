// 层：数据（测试壳位于 tests/，被测对象为普通克里金核）
#include <QtTest/QtTest>

#include "algorithms/geostat/kriging.h"

#include <cmath>
#include <vector>

using namespace paleo::geostat;

namespace
{

VariogramModel sphericalModel()
{
  VariogramModel model;
  model.type = VariogramModelType::Spherical;
  model.nugget = 1;
  model.sill = 3;
  model.range = 30;
  return model;
}

GridSpec makeGrid( int cols, int rows, double x0, double y0, double cell )
{
  GridSpec grid;
  grid.cols = cols;
  grid.rows = rows;
  grid.originX = x0;
  grid.originY = y0;
  grid.pixelWidth = cell;
  grid.pixelHeight = -cell;
  return grid;
}

} // namespace

class GeostatKrigingTests : public QObject
{
  Q_OBJECT
private slots:
  void exactAtSamplePoints();
  void gridPipelineMatchesPointQueries();
  void singleSampleIsConstantWithAsymptoticVariance();
  void twoEqualDistanceMidpointIsMean();
  void pureNuggetHandCheckFourByFour();
  void asymmetricTwoSampleHandCheck();
  void varianceGrowsWithDistance();
  void radiusModeLeavesFarCellsNodata();
  void duplicatesMergeToMean();
  void cancelledAndInvalid();
};

void GeostatKrigingTests::exactAtSamplePoints()
{
  // 克里金固有性质：样本点处估值 = 样本值（精确断言），估计方差 = 0
  std::vector<Sample> samples;
  const double coords[] = { 0, 17, 33, 51, 68 };
  int index = 0;
  for ( double y : coords )
    for ( double x : coords )
    {
      const double value = std::sin( 0.11 * x ) + 0.7 * std::cos( 0.05 * y ) + 10;
      samples.push_back( Sample{ x, y, value } );
      ++index;
    }
  const VariogramModel model = sphericalModel();
  KrigingParams params;
  double scale = 1;
  for ( const Sample &sample : samples )
  {
    const KrigingPointResult result = ordinaryKrigingAt( sample.x, sample.y, samples, model, params );
    QVERIFY2( result.ok, qPrintable( QStringLiteral( "sample %1" ).arg( sample.x ) ) );
    scale = std::max( scale, std::fabs( sample.value ) );
    QVERIFY2( std::fabs( result.estimate - sample.value ) <= 1e-9 * scale,
              qPrintable( QStringLiteral( "est=%1 z=%2" ).arg( result.estimate ).arg( sample.value ) ) );
    QVERIFY2( result.variance <= 1e-8 * ( model.nugget + model.sill ),
              qPrintable( QStringLiteral( "var=%1" ).arg( result.variance ) ) );
  }
  ( void )index;
}

void GeostatKrigingTests::gridPipelineMatchesPointQueries()
{
  std::vector<Sample> samples;
  for ( int row = 0; row < 5; ++row )
    for ( int column = 0; column < 5; ++column )
      samples.push_back( Sample{ column * 20.0, row * 20.0,
                                 std::sin( 0.2 * column ) + 0.3 * row + 5 } );
  const VariogramModel model = sphericalModel();
  KrigingParams params;
  // 网格像元中心对准样本点：整场走一遍核路径，与单点 API 对拍
  const GridSpec grid = makeGrid( 5, 5, -10, 90, 20 );
  const KrigingResult result = ordinaryKriging( samples, grid, model, params );
  QCOMPARE( result.status, Status::Ok );
  QCOMPARE( result.finiteCells, 25 );
  QCOMPARE( result.nodataCells, 0 );
  QCOMPARE( result.solverFailures, 0 );
  for ( int row = 0; row < 5; ++row )
    for ( int column = 0; column < 5; ++column )
    {
      // 网格行 0 = 最大 y（north-up），样本按 y 升序生成 → 行反转映射
      const std::size_t cell = static_cast<std::size_t>( row ) * 5 + column;
      const Sample &sample = samples[static_cast<std::size_t>( 4 - row ) * 5 + column];
      QVERIFY2( std::fabs( result.estimate[cell] - sample.value ) <= 1e-9 * 10,
                qPrintable( QStringLiteral( "cell %1 est=%2 z=%3" )
                                .arg( static_cast<double>( cell ) )
                                .arg( result.estimate[cell] )
                                .arg( sample.value ) ) );
    }
}

void GeostatKrigingTests::singleSampleIsConstantWithAsymptoticVariance()
{
  const std::vector<Sample> samples = { Sample{ 0, 0, 3.25 } };
  const VariogramModel model = sphericalModel();
  KrigingParams params;
  const GridSpec grid = makeGrid( 40, 40, -1000, 1000, 50 );
  const KrigingResult result = ordinaryKriging( samples, grid, model, params );
  QCOMPARE( result.status, Status::Ok );
  QCOMPARE( result.finiteCells, 1600 );
  for ( double value : result.estimate )
    QVERIFY( std::fabs( value - 3.25 ) <= 1e-9 );

  // 单样本远点：σ² = 2γ(h) → 趋近 2×(块金+拱高)。均值未知的代价，
  // 严格 OK 下的渐近（提示词「块金+拱高」是简单克里金口径，见 progress 文档）。
  // 递增断言用变程内距离（变程外 γ 饱和，方差持平是数学事实不是 bug）。
  const KrigingPointResult nearPoint = ordinaryKrigingAt( 3, 0, samples, model, params );
  const KrigingPointResult midPoint = ordinaryKrigingAt( 15, 0, samples, model, params );
  const KrigingPointResult farPoint = ordinaryKrigingAt( 5000, 0, samples, model, params );
  const double totalSill = model.nugget + model.sill;
  QVERIFY( nearPoint.variance < midPoint.variance && midPoint.variance < farPoint.variance );
  QVERIFY2( std::fabs( farPoint.variance - 2 * totalSill ) <= 1e-6 * totalSill,
            qPrintable( QStringLiteral( "far var=%1 expect=%2" )
                            .arg( farPoint.variance ).arg( 2 * totalSill ) ) );
  QVERIFY( std::fabs( nearPoint.estimate - 3.25 ) <= 1e-9 );
}

void GeostatKrigingTests::twoEqualDistanceMidpointIsMean()
{
  const std::vector<Sample> samples = { Sample{ -10, 0, 1 }, Sample{ 10, 0, 5 } };
  const VariogramModel model = sphericalModel();
  KrigingParams params;
  const KrigingPointResult result = ordinaryKrigingAt( 0, 0, samples, model, params );
  QVERIFY( result.ok );
  QVERIFY2( std::fabs( result.estimate - 3.0 ) <= 1e-9,
            qPrintable( QStringLiteral( "est=%1" ).arg( result.estimate ) ) );
  // 对称闭式：λ=1/2，μ = γ(d) − γ(2d)/2，σ² = 2γ(d) − γ(2d)/2（d=10）
  const double expectedVariance = 2 * model.semivariance( 10 ) - 0.5 * model.semivariance( 20 );
  QVERIFY2( std::fabs( result.variance - expectedVariance ) <= 1e-9 * ( model.nugget + model.sill ),
            qPrintable( QStringLiteral( "var=%1 expect=%2" )
                            .arg( result.variance ).arg( expectedVariance ) ) );
}

void GeostatKrigingTests::pureNuggetHandCheckFourByFour()
{
  // 3 样本 + Lagrange = 4×4 克氏方程组。纯块金闭式解：
  // λᵢ = 1/n，μ = C(n−1)/n，估值 = 样本均值，σ² = C(1 + 1/n)。
  const std::vector<Sample> samples = {
    Sample{ 0, 0, 1 }, Sample{ 40, 5, 2 }, Sample{ 7, 33, 6 },
  };
  VariogramModel model;
  model.type = VariogramModelType::Spherical;
  model.nugget = 2;
  model.sill = 0;
  model.range = 1;
  KrigingParams params;
  const KrigingPointResult result = ordinaryKrigingAt( 13, 21, samples, model, params );
  QVERIFY( result.ok );
  QVERIFY2( std::fabs( result.estimate - 3.0 ) <= 1e-12 * 6,
            qPrintable( QStringLiteral( "est=%1" ).arg( result.estimate ) ) );
  const double expectedVariance = 2.0 * ( 1.0 + 1.0 / 3.0 );
  QVERIFY2( std::fabs( result.variance - expectedVariance ) <= 1e-9,
            qPrintable( QStringLiteral( "var=%1 expect=%2" )
                            .arg( result.variance ).arg( expectedVariance ) ) );
}

void GeostatKrigingTests::asymmetricTwoSampleHandCheck()
{
  // 2 样本 3×3 加边方程组的手工代数解（与核内 LU 独立推导对拍）：
  // [0 g10 1][λ1]   [g2]      λ2 = 1−λ1；g10(1−2λ1) = g2−g8
  // [g10 0 1][λ2] = [g8]  →   μ = g8 − g10·λ1
  // [1  1 0][μ ]    [1 ]
  const std::vector<Sample> samples = { Sample{ 0, 0, 2 }, Sample{ 10, 0, 6 } };
  const VariogramModel model = sphericalModel();
  const double g2 = model.semivariance( 2 );
  const double g8 = model.semivariance( 8 );
  const double g10 = model.semivariance( 10 );
  const double lambda1 = 0.5 * ( 1 - ( g2 - g8 ) / g10 );
  const double lambda2 = 1 - lambda1;
  const double mu = g8 - g10 * lambda1;
  const double expectedEstimate = lambda1 * 2 + lambda2 * 6;
  const double expectedVariance = lambda1 * g2 + lambda2 * g8 + mu;

  KrigingParams params;
  const KrigingPointResult result = ordinaryKrigingAt( 2, 0, samples, model, params );
  QVERIFY( result.ok );
  QVERIFY2( std::fabs( result.estimate - expectedEstimate ) <= 1e-10 * 6,
            qPrintable( QStringLiteral( "est=%1 expect=%2" )
                            .arg( result.estimate ).arg( expectedEstimate ) ) );
  QVERIFY2( std::fabs( result.variance - expectedVariance ) <= 1e-10 * 8,
            qPrintable( QStringLiteral( "var=%1 expect=%2" )
                            .arg( result.variance ).arg( expectedVariance ) ) );
}

void GeostatKrigingTests::varianceGrowsWithDistance()
{
  const std::vector<Sample> samples = { Sample{ 0, 0, 5 }, Sample{ 30, 0, 7 }, Sample{ 0, 30, 9 } };
  const VariogramModel model = sphericalModel();
  KrigingParams params;
  const KrigingPointResult nearResult = ordinaryKrigingAt( 5, 5, samples, model, params );
  const KrigingPointResult midResult = ordinaryKrigingAt( 15, 15, samples, model, params );
  const KrigingPointResult farResult = ordinaryKrigingAt( 300, 300, samples, model, params );
  QVERIFY( nearResult.ok && midResult.ok && farResult.ok );
  QVERIFY( nearResult.variance < midResult.variance );
  QVERIFY( midResult.variance < farResult.variance );
  // 远点趋近总基台量级（簇状样本外推渐近，上界 2×总基台）
  QVERIFY( farResult.variance > 0.9 * ( model.nugget + model.sill ) );
  QVERIFY( farResult.variance <= 2 * ( model.nugget + model.sill ) * ( 1 + 1e-6 ) );
}

void GeostatKrigingTests::radiusModeLeavesFarCellsNodata()
{
  std::vector<Sample> samples;
  for ( int i = 0; i < 10; ++i )
    samples.push_back( Sample{ 10.0 * i, 0, std::sin( 0.3 * i ) } );
  const VariogramModel model = sphericalModel();
  KrigingParams params;
  params.searchRadius = 15;
  params.minPoints = 2;
  const GridSpec grid = makeGrid( 30, 15, -10, 40, 5 );
  const KrigingResult result = ordinaryKriging( samples, grid, model, params );
  QCOMPARE( result.status, Status::Ok );
  QVERIFY( result.finiteCells > 0 );
  QVERIFY( result.nodataCells > 0 );
  QVERIFY( result.solverFailures == 0 );
}

void GeostatKrigingTests::duplicatesMergeToMean()
{
  std::vector<Sample> samples = {
    Sample{ 0, 0, 2 }, Sample{ 0, 0, 4 }, Sample{ 50, 0, 8 },
  };
  const VariogramModel model = sphericalModel();
  KrigingParams params;
  const KrigingPointResult atDuplicate = ordinaryKrigingAt( 0, 0, samples, model, params );
  QVERIFY( atDuplicate.ok );
  QVERIFY2( std::fabs( atDuplicate.estimate - 3.0 ) <= 1e-9,
            qPrintable( QStringLiteral( "est=%1（重合均值 3）" ).arg( atDuplicate.estimate ) ) );
  const GridSpec grid = makeGrid( 5, 5, -5, 5, 2.5 );
  const KrigingResult result = ordinaryKriging( samples, grid, model, params );
  QCOMPARE( result.status, Status::Ok );
  QCOMPARE( result.mergedDuplicates, 1 );
}

void GeostatKrigingTests::cancelledAndInvalid()
{
  std::vector<Sample> samples;
  for ( int row = 0; row < 8; ++row )
    for ( int column = 0; column < 8; ++column )
      samples.push_back( Sample{ column * 10.0, row * 10.0, row + column } );
  const VariogramModel model = sphericalModel();
  KrigingParams params;
  const GridSpec grid = makeGrid( 120, 120, -5, 85, 0.7 ); // 14400 格 > 2048 检查间隔
  Control control;
  control.cancelled = [] { return true; };
  const KrigingResult cancelled = ordinaryKriging( samples, grid, model, params, control );
  QCOMPARE( cancelled.status, Status::Cancelled );

  const KrigingResult invalid = ordinaryKriging( {}, grid, model, params );
  QVERIFY( invalid.status == Status::InvalidInput );
  VariogramModel broken = model;
  broken.range = 0;
  QVERIFY( ordinaryKriging( samples, grid, broken, params ).status == Status::InvalidInput );
}

QTEST_MAIN( GeostatKrigingTests )
#include "tst_geostat_kriging.moc"
