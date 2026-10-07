// 层：数据（测试壳位于 tests/，被测对象为数据层 geostat 核）
#include <QtTest/QtTest>

#include "algorithms/geostat/cokriging.h"
#include "algorithms/geostat/kriging.h"
#include "algorithms/geostat/neighborhood.h"
#include "algorithms/geostat/variogram.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace paleo::geostat;

namespace
{

// 固定系数合成场（无 RNG，逐位可复现）：S/T1/T2 相互独立的平滑场，
// Z1 = 0.9·S + 0.43589·T1、Z2 = 0.8·S − 0.6·T2 → 配置点相关 ρ = 0.9·0.8 = 0.72
// （LMC 一致构造：主/协共享 S，残差独立）。波形 ~ 数个单位，域 [0,10]²。
double fieldS( double x, double y )
{
  return std::sin( 0.9 * x + 0.4 * y ) + 0.6 * std::sin( 0.5 * x - 1.1 * y + 2.0 ) +
         0.4 * std::sin( 1.3 * x + 0.7 * y + 4.0 );
}

double fieldT1( double x, double y )
{
  return std::cos( 0.8 * x - 0.6 * y + 1.0 ) + 0.5 * std::sin( 1.1 * x + 0.9 * y + 3.0 );
}

double fieldT2( double x, double y )
{
  return std::sin( 0.7 * x + 1.2 * y + 0.5 ) + 0.7 * std::cos( 0.9 * x + 0.3 * y + 2.5 );
}

double fieldZ1( double x, double y )
{
  return 0.9 * fieldS( x, y ) + 0.43589 * fieldT1( x, y );
}

double fieldZ2( double x, double y )
{
  return 0.8 * fieldS( x, y ) - 0.6 * fieldT2( x, y );
}

constexpr double kCrossRho = 0.9 * 0.8; // 0.72

// 主变量稀疏井位（8 口，固定）；协变量按给定间距的格网采样（含共位井）。
std::vector<Sample> primarySamples()
{
  return { { 0.8, 1.1, fieldZ1( 0.8, 1.1 ) }, { 3.4, 0.6, fieldZ1( 3.4, 0.6 ) },
           { 7.1, 1.4, fieldZ1( 7.1, 1.4 ) }, { 9.3, 3.0, fieldZ1( 9.3, 3.0 ) },
           { 1.5, 5.8, fieldZ1( 1.5, 5.8 ) }, { 5.0, 6.5, fieldZ1( 5.0, 6.5 ) },
           { 8.4, 7.2, fieldZ1( 8.4, 7.2 ) }, { 3.0, 9.0, fieldZ1( 3.0, 9.0 ) } };
}

std::vector<Sample> secondarySamples( int perSide )
{
  std::vector<Sample> samples;
  const double step = 10.0 / ( perSide - 1 );
  for ( int r = 0; r < perSide; ++r )
    for ( int c = 0; c < perSide; ++c )
    {
      const double x = c * step;
      const double y = r * step;
      samples.push_back( { x, y, fieldZ2( x, y ) } );
    }
  return samples;
}

CoKrigingModel fixtureModel()
{
  CoKrigingModel model;
  model.primary.type = VariogramModelType::Spherical;
  model.primary.nugget = 0;
  model.primary.sill = 1.0;
  model.primary.range = 3.5;
  model.secondary = model.primary;
  model.crossCorrelation = kCrossRho;
  return model;
}

std::vector<Point2> checkPoints()
{
  std::vector<Point2> points;
  for ( int r = 0; r < 6; ++r )
    for ( int c = 0; c < 6; ++c )
      points.push_back( { 0.5 + 9.0 * c / 5.0, 0.5 + 9.0 * r / 5.0 } );
  return points;
}

double rmsError( const std::vector<double> &estimate, const std::vector<Point2> &where )
{
  double sum = 0;
  for ( std::size_t i = 0; i < estimate.size(); ++i )
  {
    const double error = estimate[i] - fieldZ1( where[i].x, where[i].y );
    sum += error * error;
  }
  return std::sqrt( sum / static_cast<double>( estimate.size() ) );
}

} // namespace

class CoKrigingTests : public QObject
{
  Q_OBJECT
  private slots:
    void rejectsInvalidInputs();
    void interpolatesPrimarySamplesExactly();
    void secondaryImprovesReconstruction();
    void denserSecondaryReducesError();
    void zeroCrossCorrelationMatchesOrdinaryKriging();
    void remoteSecondaryMatchesOrdinaryKriging();
    void constrainedWeightsRespondToGroupCap();
    void constraintMappingSurvivesDedupe();
};

void CoKrigingTests::rejectsInvalidInputs()
{
  const CoKrigingModel model = fixtureModel();
  CoKrigingSolver emptyPrimary( {}, secondarySamples( 5 ), model, CoKrigingParams{} );
  QVERIFY( !emptyPrimary.valid() );
  QVERIFY( !emptyPrimary.solveAt( 5, 5 ).ok );

  CoKrigingModel badRho = model;
  badRho.crossCorrelation = 1.5;
  CoKrigingSolver invalidRho( primarySamples(), secondarySamples( 5 ), badRho, CoKrigingParams{} );
  QVERIFY( !invalidRho.valid() );

  const std::vector<Sample> nonfinite = { { 0, 0, std::numeric_limits<double>::quiet_NaN() } };
  CoKrigingSolver nanSamples( nonfinite, secondarySamples( 5 ), model, CoKrigingParams{} );
  QVERIFY( !nanSamples.valid() );
}

// γ(0)=0 口径：估值点落在主变量井位上 → 估值=井值、方差=0（解析精确）。
void CoKrigingTests::interpolatesPrimarySamplesExactly()
{
  const std::vector<Sample> primary = primarySamples();
  const CoKrigingSolver solver( primary, secondarySamples( 5 ), fixtureModel(), CoKrigingParams{} );
  QVERIFY( solver.valid() );
  for ( const Sample &sample : primary )
  {
    const CoKrigingPointResult result = solver.solveAt( sample.x, sample.y );
    QVERIFY2( result.ok, "sample point must solve" );
    QCOMPARE( result.estimate, sample.value );
    QVERIFY( result.variance <= 1e-10 );
    QVERIFY( result.variance >= -1e-10 );
  }
}

// Oracle 1：两变量合成场重建——协克里金（ρ=0.72）误差 < 同井位纯 OK 误差。
void CoKrigingTests::secondaryImprovesReconstruction()
{
  const std::vector<Sample> primary = primarySamples();
  const std::vector<Sample> secondary = secondarySamples( 5 ); // 25 个协变量点
  const std::vector<Point2> points = checkPoints();
  const CoKrigingModel model = fixtureModel();

  std::vector<double> cokriging;
  for ( const Point2 &point : points )
  {
    const CoKrigingPointResult result =
        coKrigingAt( point.x, point.y, primary, secondary, model, CoKrigingParams{} );
    QVERIFY2( result.ok, "co-kriging must solve on fixture" );
    cokriging.push_back( result.estimate );
  }
  std::vector<double> ordinary;
  for ( const Point2 &point : points )
  {
    const KrigingPointResult result = ordinaryKrigingAt( point.x, point.y, primary,
                                                         model.primary, KrigingParams{} );
    QVERIFY2( result.ok, "ordinary kriging must solve on fixture" );
    ordinary.push_back( result.estimate );
  }
  const double cokrigingRms = rmsError( cokriging, points );
  const double ordinaryRms = rmsError( ordinary, points );
  QVERIFY2( cokrigingRms < 0.9 * ordinaryRms,
            qPrintable( QStringLiteral( "co-kriging RMS %1 should improve on OK RMS %2" )
                            .arg( cokrigingRms )
                            .arg( ordinaryRms ) ) );
}

// 协变量加密（25 → 81 点）重建误差单调下降（固定场，逐位可复现）。
void CoKrigingTests::denserSecondaryReducesError()
{
  const std::vector<Sample> primary = primarySamples();
  const std::vector<Point2> points = checkPoints();
  const CoKrigingModel model = fixtureModel();
  CoKrigingParams params;

  std::vector<double> sparse;
  for ( const Point2 &point : points )
    sparse.push_back(
        coKrigingAt( point.x, point.y, primary, secondarySamples( 5 ), model, params ).estimate );
  std::vector<double> dense;
  for ( const Point2 &point : points )
  {
    const CoKrigingPointResult result =
        coKrigingAt( point.x, point.y, primary, secondarySamples( 9 ), model, params );
    QVERIFY2( result.ok, "dense secondary must solve" );
    dense.push_back( result.estimate );
  }
  const double sparseRms = rmsError( sparse, points );
  const double denseRms = rmsError( dense, points );
  QVERIFY2( denseRms < sparseRms,
            qPrintable( QStringLiteral( "81 secondary points RMS %1 should beat 25 points RMS %2" )
                            .arg( denseRms )
                            .arg( sparseRms ) ) );
}

// Oracle 1：ρ=0 退化单变量 → 与 ordinaryKrigingAt 一致（交叉项消失，ν=0、μ2=0）。
void CoKrigingTests::zeroCrossCorrelationMatchesOrdinaryKriging()
{
  const std::vector<Sample> primary = primarySamples();
  const std::vector<Sample> secondary = secondarySamples( 5 );
  CoKrigingModel model = fixtureModel();
  model.crossCorrelation = 0;
  const CoKrigingSolver solver( primary, secondary, model, CoKrigingParams{} );
  QVERIFY( solver.valid() );
  for ( const Point2 &point : checkPoints() )
  {
    const CoKrigingPointResult cross = solver.solveAt( point.x, point.y );
    const KrigingPointResult plain = ordinaryKrigingAt( point.x, point.y, primary,
                                                        model.primary, KrigingParams{} );
    QVERIFY( cross.ok );
    QVERIFY( plain.ok );
    QVERIFY( std::fabs( cross.estimate - plain.estimate ) < 1e-8 );
    QVERIFY( std::fabs( cross.variance - plain.variance ) < 1e-8 );
  }
}

// 协变量全部在半径外 → 省去 μ2 行后与 OK 同系统（同布局同主元序，近逐位一致；
// 1e-8 容差兜 LU 末位噪声）。
void CoKrigingTests::remoteSecondaryMatchesOrdinaryKriging()
{
  const std::vector<Sample> primary = primarySamples();
  std::vector<Sample> far;
  for ( int i = 0; i < 5; ++i )
    far.push_back( { 100.0 + i, 100.0, fieldZ2( 100.0 + i, 100.0 ) } );
  CoKrigingParams params;
  params.searchRadius = 20.0; // 覆盖全部井距（12 会裁掉对角远井，两组邻域不同构）
  const CoKrigingSolver solver( primary, far, fixtureModel(), params );
  QVERIFY( solver.valid() );
  for ( const Point2 &point : checkPoints() )
  {
    const CoKrigingPointResult cross = solver.solveAt( point.x, point.y );
    const KrigingPointResult plain = ordinaryKrigingAt( point.x, point.y, primary,
                                                        fixtureModel().primary, KrigingParams{} );
    QVERIFY( cross.ok );
    QVERIFY( plain.ok );
    QVERIFY2( std::fabs( cross.estimate - plain.estimate ) < 1e-8,
              qPrintable( QStringLiteral( "cross=%1 plain=%2" ).arg( cross.estimate ).arg( plain.estimate ) ) );
    QVERIFY( std::fabs( cross.variance - plain.variance ) < 1e-8 );
  }
}

// Oracle 1：带约束 OK 的约束行生效——界外组 cap=0 软罚后权重趋零、估值响应。
void CoKrigingTests::constrainedWeightsRespondToGroupCap()
{
  // 6 口井：4 口在目标同侧（x<5），2 口在「软边界」另一侧（x>7）。
  std::vector<Sample> wells;
  const double nearSide[4][2] = { { 1.0, 4.0 }, { 2.5, 5.5 }, { 3.5, 3.0 }, { 2.0, 6.5 } };
  for ( const auto &well : nearSide )
    wells.push_back( { well[0], well[1], fieldZ1( well[0], well[1] ) } );
  const double farSide[2][2] = { { 6.2, 4.2 }, { 6.8, 5.4 } };
  for ( const auto &well : farSide )
    wells.push_back( { well[0], well[1], fieldZ1( well[0], well[1] ) } );

  VariogramModel model;
  model.type = VariogramModelType::Spherical;
  model.nugget = 0;
  model.sill = 2.0;
  model.range = 6.0;
  KrigingParams params; // 最近 K：默认 16 → 全部 6 口进邻域

  const double targetX = 3.0, targetY = 4.8;
  ConstrainedKrigingSolver plain( wells, model, params, {} );
  QVERIFY( plain.valid() );
  const ConstrainedKrigingPointResult plainResult = plain.solveAt( targetX, targetY );
  QVERIFY( plainResult.ok );
  double plainFarWeight = 0;
  for ( std::size_t i = 0; i < plainResult.usedSamples.size(); ++i )
    if ( plainResult.usedSamples[i] >= 4 ) // 输入下标 4/5 = 界外两口井
      plainFarWeight += plainResult.weights[i];
  QVERIFY2( plainFarWeight > 0.05,
            qPrintable( QStringLiteral( "unconstrained far-side weight %1 should be material" )
                            .arg( plainFarWeight ) ) );

  WeightGroupConstraint constraint;
  constraint.sampleIndices = { 4, 5 };
  constraint.cap = 0;
  constraint.penalty = 1000.0 * ( model.nugget + model.sill ); // 强罚：组权重拉向 0
  const ConstrainedKrigingSolver constrained( wells, model, params, { constraint } );
  QVERIFY( constrained.valid() );
  QCOMPARE( constrained.constraintCount(), 1 );
  const ConstrainedKrigingPointResult hit = constrained.solveAt( targetX, targetY );
  QVERIFY( hit.ok );
  double hitFarWeight = 0;
  double weightSum = 0;
  for ( std::size_t i = 0; i < hit.usedSamples.size(); ++i )
  {
    weightSum += hit.weights[i];
    if ( hit.usedSamples[i] >= 4 )
      hitFarWeight += hit.weights[i];
  }
  QVERIFY2( std::fabs( hitFarWeight ) < 5e-3,
            qPrintable( QStringLiteral( "capped far-side weight %1 should vanish" ).arg( hitFarWeight ) ) );
  QVERIFY( std::fabs( weightSum - 1.0 ) < 1e-6 ); // 无偏约束仍在
  QVERIFY( hit.penaltyPasses >= 1 );              // 软罚主动集确实介入
  QVERIFY( std::fabs( hit.estimate - plainResult.estimate ) > 1e-6 ); // 估值对约束有响应
}

// 约束输入下标经过重合合并仍指向正确样本（dedupeSamples 映射契约）。
void CoKrigingTests::constraintMappingSurvivesDedupe()
{
  std::vector<Sample> wells = { { 1.0, 1.0, 10.0 }, { 2.0, 2.0, 20.0 }, { 3.0, 3.0, 30.0 },
                                { 3.0, 3.0, 30.0 }, // 与上一条重合 → 合并
                                { 0, 0, std::numeric_limits<double>::quiet_NaN() } };
  wells.push_back( { 4.0, 4.0, 40.0 } ); // 输入下标 5
  VariogramModel model;
  model.type = VariogramModelType::Spherical;
  model.nugget = 0;
  model.sill = 1.0;
  model.range = 5.0;

  WeightGroupConstraint constraint; // 原始输入下标 5（合并后 3）
  constraint.sampleIndices = { 5 };
  constraint.cap = 0;
  constraint.penalty = 1000.0;
  const ConstrainedKrigingSolver solver( wells, model, KrigingParams{}, { constraint } );
  QVERIFY( solver.valid() );
  QCOMPARE( solver.sampleCount(), 4 ); // 5 条有限 + 1 条重合 - 1 条 NaN
  const ConstrainedKrigingPointResult result = solver.solveAt( 3.6, 3.6 );
  QVERIFY( result.ok );
  // 目标贴近 (4,4)：无约束时该井权重为正（cap=0 才会被触发——软罚只罚违约侧）。
  const ConstrainedKrigingSolver plain( wells, model, KrigingParams{}, {} );
  const ConstrainedKrigingPointResult plainResult = plain.solveAt( 3.6, 3.6 );
  QVERIFY( plainResult.ok );
  double plainWeightAt44 = 0;
  for ( std::size_t i = 0; i < plainResult.usedSamples.size(); ++i )
    if ( plainResult.usedSamples[i] == 3 )
      plainWeightAt44 = plainResult.weights[i];
  QVERIFY2( plainWeightAt44 > 0.02,
            qPrintable( QStringLiteral( "unconstrained (4,4) weight %1 should be positive" )
                            .arg( plainWeightAt44 ) ) );
  double weightAt44 = 0;
  for ( std::size_t i = 0; i < result.usedSamples.size(); ++i )
    if ( result.usedSamples[i] == 3 ) // 合并后下标 3 = (4,4)
      weightAt44 = result.weights[i];
  QVERIFY2( std::fabs( weightAt44 ) < 5e-3,
            qPrintable( QStringLiteral( "mapped constraint weight %1 should vanish" ).arg( weightAt44 ) ) );

  // dedupeSamples 映射出参直接契约：重合对同映射、NaN 哨兵。
  int merged = 0;
  std::vector<std::uint32_t> mapping;
  const std::vector<Sample> deduped = detail::dedupeSamples( wells, &merged, &mapping );
  QCOMPARE( merged, 1 );
  QCOMPARE( deduped.size(), static_cast<std::size_t>( 4 ) );
  QCOMPARE( mapping.size(), static_cast<std::size_t>( 6 ) );
  QCOMPARE( mapping[2], mapping[3] ); // (3,3) 两条重合 → 同一合并下标
  QCOMPARE( mapping[4], std::numeric_limits<std::uint32_t>::max() ); // NaN → 哨兵
  QVERIFY( mapping[0] != mapping[5] );
}

QTEST_MAIN( CoKrigingTests )
#include "tst_geostat_cokriging.moc"
