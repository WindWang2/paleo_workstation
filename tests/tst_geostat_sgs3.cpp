// 层：数据（测试壳位于 tests/，被测对象为序贯高斯三维点集入口）
#include <QtTest/QtTest>

#include "algorithms/geostat/sgs3.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <random>
#include <vector>

using namespace paleo::geostat;

namespace
{

// 均匀三维格点目标（nx×ny×nz，步长 x/y/z），原点对齐 (ox,oy,oz)。
std::vector<Sgs3Target> makeTargets( int nx, int ny, int nz, double ox, double oy, double oz,
                                     double sx, double sy, double sz )
{
  std::vector<Sgs3Target> targets;
  targets.reserve( static_cast<std::size_t>( nx ) * ny * nz );
  for ( int iz = 0; iz < nz; ++iz )
    for ( int iy = 0; iy < ny; ++iy )
      for ( int ix = 0; ix < nx; ++ix )
      {
        Sgs3Target target;
        target.ix = ix;
        target.iy = iy;
        target.iz = iz;
        target.x = ox + sx * ix;
        target.y = oy + sy * iy;
        target.z = oz + sz * iz;
        targets.push_back( target );
      }
  return targets;
}

VariogramModel gaussianModel()
{
  VariogramModel model;
  model.type = VariogramModelType::Gaussian;
  model.nugget = 0.2;
  model.sill = 1.0;
  model.range = 30;
  model.verticalRangeRatio = 5; // 垂向变程 = range/5（z 步长小时纵向快速去相关）
  return model;
}

Lattice3Steps stepsFor( double sx, double sy, double sz )
{
  Lattice3Steps steps;
  steps.x = sx;
  steps.y = sy;
  steps.zMin = sz;
  return steps;
}

} // namespace

class GeostatSgs3Tests : public QObject
{
  Q_OBJECT
private slots:
  void reproducibleWithSameSeed();
  void conditionedAtSampleTargets();
  void histogramFidelity();
  void groupBarrierBlocksConditioning();
  void strongAnisotropyTallStack();
  void invalidAndCancelled();
  void variogramThreeArgMatchesTwoArgOnZeroDz();
};

void GeostatSgs3Tests::strongAnisotropyTallStack()
{
  // 地层格架典型形态：水平步长 1000 m、垂向步长 1 m（1000:1）。垂向邻居
  // 必须不被环扫剪枝截断——样本放在格点列 (0,0)/(5,5) 上叠置时，远 k 的
  // 估计仍被近垂距样本强条件化（变程 200 >> 垂距、<< 柱距 1000）。
  const std::vector<Sgs3Target> targets = makeTargets(6, 6, 40, 0, 0, 0, 1000, 1000, 1);
  std::vector<Sample3> samples;
  samples.push_back(Sample3{0.0, 0.0, 2.0, 9.0, 0});
  samples.push_back(Sample3{0.0, 0.0, 22.0, 10.0, 0});
  samples.push_back(Sample3{0.0, 0.0, 38.0, 11.0, 0});
  samples.push_back(Sample3{5000.0, 5000.0, 2.0, 0.5, 0});
  samples.push_back(Sample3{5000.0, 5000.0, 22.0, 1.0, 0});
  samples.push_back(Sample3{5000.0, 5000.0, 38.0, 1.5, 0});
  VariogramModel model;
  model.type = VariogramModelType::Spherical;
  model.nugget = 0.0;
  model.sill = 1.0;
  model.range = 200;             // 覆盖垂向全叠置，跨柱（≥1000）不相关
  model.verticalRangeRatio = 1;
  Lattice3Steps steps;
  steps.x = 1000;
  steps.y = 1000;
  steps.zMin = 1;
  Sgs3Params params;
  params.nRealizations = 8;
  params.seed = 61;
  params.maxPoints = 12;
  const Sgs3Result result = sgs3(samples, targets, steps, model, params);
  QCOMPARE(result.status, Status::Ok);
  QCOMPARE(result.solverFailures, 0);

  // 样本柱（ix=0,iy=0）中部 k：跨实现均值贴近样本柱值（强垂向条件化）；
  // 远柱（ix=5,iy=5）中部 k：贴近远柱样本（跨柱去相关后由各自静态条件化）。
  auto cellMean = [&](int ix, int iy, int iz) {
    const std::size_t index = static_cast<std::size_t>(( iz * 6 + iy ) * 6 + ix);
    double sum = 0;
    int n = 0;
    for (const std::vector<double> &realization : result.realizations)
      if (std::isfinite(realization[index]))
      {
        sum += realization[index];
        ++n;
      }
    return n > 0 ? sum / n : std::numeric_limits<double>::quiet_NaN();
  };
  const double nearColumn = cellMean(0, 0, 20);
  const double farColumn = cellMean(5, 5, 20);
  QVERIFY2(std::isfinite(nearColumn) && nearColumn > 7.5 && nearColumn < 12.5,
           QStringLiteral("near column mean %1 must track stacked samples (~10)")
               .arg(nearColumn)
               .toUtf8()
               .constData());
  QVERIFY2(std::isfinite(farColumn) && farColumn > -1.0 && farColumn < 3.0,
           QStringLiteral("far column mean %1 must track its own samples (~1)")
               .arg(farColumn)
               .toUtf8()
               .constData());
}

void GeostatSgs3Tests::reproducibleWithSameSeed()
{
  std::vector<Sample3> samples;
  for ( int iy = 0; iy < 5; ++iy )
    for ( int ix = 0; ix < 5; ++ix )
      samples.push_back( Sample3{ 24.0 * ix, 24.0 * iy, 4.0 * ( ( ix + iy ) % 4 ),
                                 std::sin( 0.3 * ix ) + 0.2 * iy, 0 } );
  const std::vector<Sgs3Target> targets = makeTargets( 14, 12, 5, 0, 0, 0, 24, 24, 4 );
  const VariogramModel model = gaussianModel();
  Sgs3Params params;
  params.nRealizations = 2;
  params.seed = 20261005;
  params.maxPoints = 12;

  const Sgs3Result first = sgs3( samples, targets, stepsFor( 24, 24, 4 ), model, params );
  const Sgs3Result second = sgs3( samples, targets, stepsFor( 24, 24, 4 ), model, params );
  QCOMPARE( first.status, Status::Ok );
  QCOMPARE( second.status, Status::Ok );
  QCOMPARE( first.realizations.size(), std::size_t( 2 ) );
  QCOMPARE( second.realizations.size(), std::size_t( 2 ) );
  for ( std::size_t r = 0; r < first.realizations.size(); ++r )
    for ( std::size_t i = 0; i < first.realizations[r].size(); ++i )
      QVERIFY2( first.realizations[r][i] == second.realizations[r][i],
                QStringLiteral( "target %1 r%2 must be bitwise equal" )
                    .arg( static_cast<double>( i ) )
                    .arg( static_cast<double>( r ) )
                    .toUtf8()
                    .constData() );

  params.seed = 7;
  const Sgs3Result third = sgs3( samples, targets, stepsFor( 24, 24, 4 ), model, params );
  QCOMPARE( third.status, Status::Ok );
  int differing = 0;
  for ( std::size_t i = 0; i < third.realizations[0].size(); ++i )
    if ( third.realizations[0][i] != first.realizations[0][i] )
      ++differing;
  QVERIFY2( differing > 100, "different seeds must produce different fields" );
}

void GeostatSgs3Tests::conditionedAtSampleTargets()
{
  // 样本放在目标点上：条件模拟在样本点精确复现（同 2D 核口径，1e-9 相对容差）
  std::vector<Sample3> samples;
  for ( int iy = 0; iy < 4; ++iy )
    for ( int ix = 0; ix < 4; ++ix )
      samples.push_back( Sample3{ 40.0 * ix, 40.0 * iy, 2.0 * ( iy % 3 ),
                                 std::sin( 0.4 * ix ) + 0.5 * iy, 0 } );
  const std::vector<Sgs3Target> targets = makeTargets( 12, 12, 4, 0, 0, 0, 20, 20, 2 );
  const VariogramModel model = gaussianModel();
  Sgs3Params params;
  params.nRealizations = 3;
  params.seed = 5;
  params.maxPoints = 12;
  const Sgs3Result result = sgs3( samples, targets, stepsFor( 20, 20, 2 ), model, params );
  QCOMPARE( result.status, Status::Ok );
  // 目标格与样本对齐：x=40ix → 目标 ix=2ix；z=2(iy%3) → iz=iy%3
  for ( const Sample3 &sample : samples )
  {
    const int tx = static_cast<int>( std::lround( sample.x / 20 ) );
    const int ty = static_cast<int>( std::lround( sample.y / 20 ) );
    const int tz = static_cast<int>( std::lround( sample.z / 2 ) );
    const std::size_t index =
        static_cast<std::size_t>( ( tz * 12 + ty ) * 12 + tx );
    for ( std::size_t r = 0; r < result.realizations.size(); ++r )
    {
      const double value = result.realizations[r][index];
      QVERIFY2( std::fabs( value - sample.value ) <=
                    1e-9 * std::max( 1.0, std::fabs( sample.value ) ),
                QStringLiteral( "target(%1,%2,%3)=%4 z=%5" )
                    .arg( tx )
                    .arg( ty )
                    .arg( tz )
                    .arg( value )
                    .arg( sample.value )
                    .toUtf8()
                    .constData() );
    }
  }
}

void GeostatSgs3Tests::histogramFidelity()
{
  std::mt19937_64 rng( 13 );
  auto unit = [&rng] {
    return ( static_cast<double>( rng() >> 11 ) + 0.5 ) * 0x1.0p-53;
  };
  std::vector<Sample3> samples;
  for ( int i = 0; i < 120; ++i )
  {
    const double gaussian = std::sqrt( -2.0 * std::log( unit() ) ) *
                            std::cos( 2.0 * std::numbers::pi * unit() );
    Sample3 sample;
    sample.x = 18.0 * ( i % 12 );
    sample.y = 18.0 * ( i / 12 );
    sample.z = 3.0 * ( i % 5 );
    sample.value = std::exp( 0.5 * gaussian ); // 对数正态偏斜样本
    samples.push_back( sample );
  }
  const std::vector<Sgs3Target> targets = makeTargets( 20, 20, 6, 0, 0, 0, 18, 18, 3 );
  const VariogramModel model = gaussianModel();
  Sgs3Params params;
  params.seed = 42;
  params.maxPoints = 12;
  const Sgs3Result result = sgs3( samples, targets, stepsFor( 18, 18, 3 ), model, params );
  QCOMPARE( result.status, Status::Ok );
  QCOMPARE( result.solverFailures, 0 );
  QVERIFY( result.finitePoints > 2000 );

  double sum = 0;
  double sumSq = 0;
  int n = 0;
  for ( double value : result.realizations[0] )
  {
    if ( std::isfinite( value ) )
    {
      sum += value;
      sumSq += value * value;
      ++n;
    }
  }
  const double mean = sum / n;
  const double std = std::sqrt( sumSq / n - mean * mean );
  const double meanBias = std::fabs( mean - result.sampleMean ) / result.sampleStd;
  const double stdRatio = std / result.sampleStd;
  QVERIFY2( meanBias <= 0.2,
            QStringLiteral( "mean=%1 sample=%2 bias=%3σ" ).arg( mean ).arg( result.sampleMean ).arg( meanBias )
                .toUtf8()
                .constData() );
  QVERIFY2( stdRatio >= 0.75 && stdRatio <= 1.35,
            QStringLiteral( "std=%1 sample=%2 ratio=%3" ).arg( std ).arg( result.sampleStd ).arg( stdRatio )
                .toUtf8()
                .constData() );
}

void GeostatSgs3Tests::groupBarrierBlocksConditioning()
{
  // 两组分：样本只在组分 0；组分 1 无样本且无同组分邻居 → 全 nodata（如实）。
  std::vector<Sample3> samples;
  samples.push_back( Sample3{ 10, 10, 1, 5.0, 0 } );
  samples.push_back( Sample3{ 30, 10, 1, 7.0, 0 } );
  std::vector<Sgs3Target> targets = makeTargets( 6, 4, 2, 0, 0, 0, 10, 10, 2 );
  for ( std::size_t i = 0; i < targets.size(); ++i )
    targets[i].group = ( targets[i].x >= 30 ) ? 1 : 0;
  const VariogramModel model = gaussianModel();
  Sgs3Params params;
  params.seed = 3;
  const Sgs3Result result = sgs3( samples, targets, stepsFor( 10, 10, 2 ), model, params );
  QCOMPARE( result.status, Status::Ok );
  for ( std::size_t i = 0; i < targets.size(); ++i )
  {
    const bool finite = std::isfinite( result.realizations[0][i] );
    if ( targets[i].group == 0 )
      QVERIFY2( finite, "group 0 has samples and must be simulated" );
    else
      QVERIFY2( !finite, "group 1 must stay nodata (no cross-group conditioning)" );
  }
}

void GeostatSgs3Tests::invalidAndCancelled()
{
  const VariogramModel model = gaussianModel();
  Sgs3Params params;
  params.seed = 1;

  std::vector<Sample3> samples = { Sample3{ 0, 0, 0, 1.0, 0 } };

  QVERIFY( sgs3( samples, {}, stepsFor( 1, 1, 1 ), model, params ).status == Status::InvalidInput );

  VariogramModel broken = model;
  broken.sill = 0;
  broken.nugget = 0;
  const std::vector<Sgs3Target> targets = makeTargets( 4, 4, 2, 0, 0, 0, 5, 5, 1 );
  QVERIFY( sgs3( samples, targets, stepsFor( 5, 5, 1 ), broken, params ).status ==
           Status::InvalidInput );

  Sgs3Target badIndex = targets.front();
  badIndex.ix = -1;
  QVERIFY( sgs3( samples, { badIndex }, stepsFor( 5, 5, 1 ), model, params ).status ==
           Status::InvalidInput );

  Sgs3Target badGroup = targets.front();
  badGroup.group = -1;
  QVERIFY( sgs3( samples, { badGroup }, stepsFor( 5, 5, 1 ), model, params ).status ==
           Status::InvalidInput );

  std::vector<Sgs3Target> dup = { targets.front(), targets.front() };
  QVERIFY( sgs3( samples, dup, stepsFor( 5, 5, 1 ), model, params ).status ==
           Status::InvalidInput );

  // 取消：第一个进度点即取消（目标数 > 2048 检查间隔）
  const std::vector<Sgs3Target> big = makeTargets( 40, 40, 2, 0, 0, 0, 5, 5, 1 );
  Control control;
  bool cancelledOnce = false;
  control.cancelled = [&cancelledOnce] {
    if ( cancelledOnce )
      return true;
    cancelledOnce = true;
    return true;
  };
  QCOMPARE( sgs3( samples, big, stepsFor( 5, 5, 1 ), model, params, control ).status,
            Status::Cancelled );
}

void GeostatSgs3Tests::variogramThreeArgMatchesTwoArgOnZeroDz()
{
  // 2D 恒等面：dz = 0 时三参重载精确等于二参重载（任意垂向比/各向异性）。
  VariogramModel model;
  model.type = VariogramModelType::Spherical;
  model.nugget = 0.1;
  model.sill = 2.0;
  model.range = 50;
  model.anisotropyRatio = 3.0;
  model.azimuthDeg = 30;
  model.verticalRangeRatio = 7;
  for ( double dx : { -80.0, -12.5, 0.0, 3.25, 77.0 } )
    for ( double dy : { -60.0, -4.5, 0.0, 9.75, 44.0 } )
      QCOMPARE( model.semivariance( dx, dy, 0.0 ), model.semivariance( dx, dy ) );
}

QTEST_MAIN( GeostatSgs3Tests )
#include "tst_geostat_sgs3.moc"
