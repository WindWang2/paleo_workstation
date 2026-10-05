// 层：数据（测试壳位于 tests/，被测对象为序贯高斯模拟核）
#include <QtTest/QtTest>

#include "algorithms/geostat/sgs.h"

#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>
#include <vector>

using namespace paleo::geostat;

namespace
{

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

VariogramModel gaussianModel()
{
  VariogramModel model;
  model.type = VariogramModelType::Gaussian;
  model.nugget = 0.2;
  model.sill = 1.0;
  model.range = 40;
  return model;
}

// 对数正态偏斜样本（直方图忠实性的硬考题）：exp(0.5·N(0,1))，固定 seed。
std::vector<Sample> skewedSamples( int count, std::uint64_t seed )
{
  std::mt19937_64 rng( seed );
  auto unit = [&rng] {
    return ( static_cast<double>( rng() >> 11 ) + 0.5 ) * 0x1.0p-53;
  };
  std::vector<double> gauss( static_cast<std::size_t>( count ) );
  for ( std::size_t i = 0; i + 1 < gauss.size(); i += 2 )
  {
    const double r = std::sqrt( -2.0 * std::log( unit() ) );
    const double angle = 2.0 * std::numbers::pi * unit();
    gauss[i] = r * std::cos( angle );
    gauss[i + 1] = r * std::sin( angle );
  }
  std::vector<Sample> samples;
  samples.reserve( gauss.size() );
  const int side = static_cast<int>( std::ceil( std::sqrt( static_cast<double>( count ) ) ) );
  int placed = 0;
  for ( int row = 0; row < side && placed < count; ++row )
    for ( int column = 0; column < side && placed < count; ++column )
    {
      // 微抖动防重合（行列号混合进随机相位，保持确定性）
      const double jitterX = 0.3 * std::sin( 12.9898 * column + 78.233 * row );
      const double jitterY = 0.3 * std::sin( 39.346 * row + 11.135 * column );
      const double g = gauss[static_cast<std::size_t>( placed )];
      samples.push_back( Sample{ column * 20.0 + jitterX, row * 20.0 + jitterY,
                                std::exp( 0.5 * g ) } );
      ++placed;
    }
  return samples;
}

} // namespace

class GeostatSgsTests : public QObject
{
  Q_OBJECT
private slots:
  void reproducibleWithSameSeed();
  void differentSeedsDiffer();
  void histogramFidelity();
  void conditionedAtSampleCells();
  void singleSampleDegeneratesHonestly();
  void goldenAnchorPinsRefactoredKernel();
  void invalidAndCancelled();
};

void GeostatSgsTests::goldenAnchorPinsRefactoredKernel()
{
  // goal/prop-model-v2：机件抽到 sgs_internal.h 的重构必须算术恒等。
  // 黄金哈希 = 重构当日 master 版核与本版独立编译同场景输出的 FNV-1a
  //（逐位一致后钉入；防未来再动 sgs.cpp 时静默漂移——见
  // .goal-loop-ledger-prop-model-v2.md 批次3 纠错记录）。
  const std::vector<Sample> samples = skewedSamples( 120, 99 );
  const VariogramModel model = gaussianModel();
  SgsParams params;
  params.nRealizations = 2;
  params.seed = 20261003;
  params.maxPoints = 12;
  const GridSpec grid = makeGrid( 32, 32, -10, 250, 8 );
  const SgsResult result = sgs( samples, grid, model, params );
  QCOMPARE( result.status, Status::Ok );
  std::uint64_t hash = 1469598103934665603ULL;
  for ( const std::vector<double> &realization : result.realizations )
    for ( double value : realization )
    {
      std::uint64_t bits = 0;
      std::memcpy( &bits, &value, sizeof( bits ) );
      for ( int byte = 0; byte < 8; ++byte )
      {
        hash ^= ( bits >> ( byte * 8 ) ) & 0xFF;
        hash *= 1099511628211ULL;
      }
    }
  QCOMPARE( hash, std::uint64_t( 15131284449114955202ULL ) );
}

void GeostatSgsTests::reproducibleWithSameSeed()
{
  const std::vector<Sample> samples = skewedSamples( 120, 99 );
  const VariogramModel model = gaussianModel();
  SgsParams params;
  params.nRealizations = 2;
  params.seed = 20261003;
  params.maxPoints = 12;
  const GridSpec grid = makeGrid( 32, 32, -10, 250, 8 );

  const SgsResult first = sgs( samples, grid, model, params );
  const SgsResult second = sgs( samples, grid, model, params );
  QCOMPARE( first.status, Status::Ok );
  QCOMPARE( second.status, Status::Ok );
  QCOMPARE( first.realizations.size(), std::size_t( 2 ) );
  QCOMPARE( second.realizations.size(), std::size_t( 2 ) );
  for ( std::size_t r = 0; r < first.realizations.size(); ++r )
    for ( std::size_t cell = 0; cell < first.realizations[r].size(); ++cell )
      QVERIFY2( first.realizations[r][cell] == second.realizations[r][cell],
                QStringLiteral( "cell %1 r%2 must be bitwise equal" )
                    .arg( static_cast<double>( cell ) )
                    .arg( static_cast<double>( r ) )
                    .toUtf8()
                    .constData() );
}

void GeostatSgsTests::differentSeedsDiffer()
{
  const std::vector<Sample> samples = skewedSamples( 120, 99 );
  const VariogramModel model = gaussianModel();
  SgsParams params;
  params.seed = 1;
  params.maxPoints = 12;
  const GridSpec grid = makeGrid( 32, 32, -10, 250, 8 );
  const SgsResult first = sgs( samples, grid, model, params );
  params.seed = 2;
  const SgsResult second = sgs( samples, grid, model, params );
  QCOMPARE( first.status, Status::Ok );
  QCOMPARE( second.status, Status::Ok );
  int differing = 0;
  for ( std::size_t cell = 0; cell < first.realizations[0].size(); ++cell )
    if ( first.realizations[0][cell] != second.realizations[0][cell] )
      ++differing;
  QVERIFY2( differing > 100, "different seeds must produce different fields" );
}

void GeostatSgsTests::histogramFidelity()
{
  const std::vector<Sample> samples = skewedSamples( 150, 7 );
  const VariogramModel model = gaussianModel();
  SgsParams params;
  params.nRealizations = 1;
  params.seed = 42;
  params.maxPoints = 12;
  const GridSpec grid = makeGrid( 48, 48, -12, 364, 8 );
  const SgsResult result = sgs( samples, grid, model, params );
  QCOMPARE( result.status, Status::Ok );
  QCOMPARE( result.solverFailures, 0 );
  QVERIFY( result.finiteCells > 2000 );

  // 直方图忠实：实现场的均值/标准差贴近样本统计（归一化偏差阈值）
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
            QStringLiteral( "mean=%1 sample=%2 bias=%3σ" )
                .arg( mean )
                .arg( result.sampleMean )
                .arg( meanBias )
                .toUtf8()
                .constData() );
  QVERIFY2( stdRatio >= 0.75 && stdRatio <= 1.35,
            QStringLiteral( "std=%1 sample=%2 ratio=%3" )
                .arg( std )
                .arg( result.sampleStd )
                .arg( stdRatio )
                .toUtf8()
                .constData() );
}

void GeostatSgsTests::conditionedAtSampleCells()
{
  // 样本放在格心：SGS 是条件模拟，样本格的实现值 = 样本值
  std::vector<Sample> samples;
  for ( int row = 0; row < 6; ++row )
    for ( int column = 0; column < 6; ++column )
      samples.push_back( Sample{ 20.0 * column, 20.0 * row, std::sin( 0.4 * column ) + row } );
  const VariogramModel model = gaussianModel();
  SgsParams params;
  params.seed = 5;
  params.maxPoints = 12;
  const GridSpec grid = makeGrid( 24, 24, -10, 130, 20 ); // 格心对准 (20c, 20r)
  const SgsResult result = sgs( samples, grid, model, params );
  QCOMPARE( result.status, Status::Ok );
  for ( const Sample &sample : samples )
  {
    const int column = static_cast<int>( std::lround( ( sample.x - ( -10 + 10 ) ) / 20 ) );
    const int row = static_cast<int>( std::lround( ( sample.y - ( 130 - 10 ) ) / -20 ) );
    QVERIFY2( column >= 0 && column < 24 && row >= 0 && row < 24, "sample must fall on a cell" );
    const double value = result.realizations[0][static_cast<std::size_t>( row ) * 24 + column];
    QVERIFY2( std::fabs( value - sample.value ) <= 1e-9 * std::max( 1.0, std::fabs( sample.value ) ),
              QStringLiteral( "cell(%1,%2)=%3 z=%4" )
                  .arg( column )
                  .arg( row )
                  .arg( value )
                  .arg( sample.value )
                  .toUtf8()
                  .constData() );
  }
}

void GeostatSgsTests::singleSampleDegeneratesHonestly()
{
  const std::vector<Sample> samples = { Sample{ 30, 40, 2.5 } };
  const VariogramModel model = gaussianModel();
  SgsParams params;
  params.seed = 11;
  const GridSpec grid = makeGrid( 12, 12, 0, 120, 10 );
  const SgsResult result = sgs( samples, grid, model, params );
  QCOMPARE( result.status, Status::Ok );
  QCOMPARE( result.finiteCells, 144 );
  for ( double value : result.realizations[0] )
    QVERIFY( std::fabs( value - 2.5 ) <= 1e-12 );
}

void GeostatSgsTests::invalidAndCancelled()
{
  const std::vector<Sample> samples = skewedSamples( 100, 3 );
  const VariogramModel model = gaussianModel();
  const GridSpec grid = makeGrid( 100, 100, 0, 1000, 10 ); // 10000 格 > 2048 检查间隔
  SgsParams params;

  VariogramModel zeroSill = model;
  zeroSill.nugget = 0;
  zeroSill.sill = 0;
  QVERIFY( sgs( samples, grid, zeroSill, params ).status == Status::InvalidInput );
  QVERIFY( sgs( {}, grid, model, params ).status == Status::InvalidInput );

  Control control;
  control.cancelled = [] { return true; };
  QCOMPARE( sgs( samples, grid, model, params, control ).status, Status::Cancelled );

  // 多实现计数与口径
  params.nRealizations = 3;
  const SgsResult multi = sgs( samples, makeGrid( 20, 20, 0, 200, 10 ), model, params );
  QCOMPARE( multi.status, Status::Ok );
  QCOMPARE( multi.realizations.size(), std::size_t( 3 ) );
  QCOMPARE( multi.finiteCells, 400 );
}

QTEST_MAIN( GeostatSgsTests )
#include "tst_geostat_sgs.moc"
