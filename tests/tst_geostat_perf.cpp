// 层：数据（测试壳位于 tests/，被测对象为克里金/SGS 核的规模线性度）
#include <QtTest/QtTest>

#include "algorithms/geostat/kriging.h"
#include "algorithms/geostat/sgs.h"

#include <QElapsedTimer>

#include <cmath>
#include <vector>

using namespace paleo::geostat;

namespace
{

// 2000 口井：50×40 规则网 + 确定性抖动（无 RNG 依赖），值域混合频率。
std::vector<Sample> syntheticWells( int columns, int rows, double spacing )
{
  std::vector<Sample> samples;
  samples.reserve( static_cast<std::size_t>( columns * rows ) );
  for ( int row = 0; row < rows; ++row )
    for ( int column = 0; column < columns; ++column )
    {
      const double x = column * spacing + 0.3 * spacing * std::sin( 12.9898 * column + 78.233 * row );
      const double y = row * spacing + 0.3 * spacing * std::cos( 39.346 * row + 11.135 * column );
      samples.push_back( Sample{ x, y, std::sin( x / 70.0 ) + std::cos( y / 90.0 ) + 2.0 } );
    }
  return samples;
}

GridSpec gridOver( double extent, int side )
{
  GridSpec grid;
  grid.cols = side;
  grid.rows = side;
  const double cell = extent / side;
  grid.originX = -cell / 2;
  grid.originY = extent + cell / 2;
  grid.pixelWidth = cell;
  grid.pixelHeight = -cell;
  return grid;
}

VariogramModel workingModel()
{
  VariogramModel model;
  model.type = VariogramModelType::Spherical;
  model.nugget = 0.05;
  model.sill = 1.0;
  model.range = 90.0; // ~4.5 倍井距
  return model;
}

} // namespace

// 性能口径（TEST-02 教训：禁绝对毫秒墙钟断言）：
// - 断言只有规模线性度比率门（1024²/256² ≤ 像元比 16 × 1.5）；
// - 绝对耗时只打 BASELINE 行，人工誊入 docs/progress/geostat-methods.md；
// - 真工区绝对门（克里金<60s、SGS 单实现<30s）在 tst_geostat_realarea
//   （PALEO_REAL_PROJECT_AREA env 门控）里断言。
class GeostatPerfTests : public QObject
{
  Q_OBJECT
private slots:
  void krigingScalesLinearlyWithCells();
  void sgsScalesLinearlyWithCells();
};

void GeostatPerfTests::krigingScalesLinearlyWithCells()
{
  const std::vector<Sample> samples = syntheticWells( 50, 40, 20.0 ); // 2000 口
  const VariogramModel model = workingModel();
  KrigingParams params;
  params.maxPoints = 16;

  QElapsedTimer timer;
  timer.start();
  const KrigingResult small = ordinaryKriging( samples, gridOver( 1000, 256 ), model, params );
  const qint64 smallMs = timer.elapsed();
  QCOMPARE( small.status, Status::Ok );
  timer.restart();
  const KrigingResult big = ordinaryKriging( samples, gridOver( 1000, 1024 ), model, params );
  const qint64 bigMs = timer.elapsed();
  QCOMPARE( big.status, Status::Ok );
  QCOMPARE( big.finiteCells, 1024 * 1024 );
  QCOMPARE( big.solverFailures, 0 );

  const double ratio = static_cast<double>( bigMs ) / static_cast<double>( std::max<qint64>( smallMs, 1 ) );
  qWarning( "%s", qPrintable( QStringLiteral( "BASELINE geostat_kriging_ms_1024 = %1" ).arg( bigMs ) ) );
  qWarning( "%s", qPrintable( QStringLiteral( "BASELINE geostat_kriging_ms_256 = %1" ).arg( smallMs ) ) );
  qWarning( "%s", qPrintable( QStringLiteral( "BASELINE geostat_kriging_scale_ratio = %1" ).arg( ratio, 0, 'f', 2 ) ) );
  qWarning( "%s", qPrintable( QStringLiteral( "BASELINE geostat_kriging_samples = %1" ).arg( samples.size() ) ) );
  QVERIFY2( ratio <= 16.0 * 1.5,
            qPrintable( QStringLiteral( "kriging scale ratio %1 exceeds linear bound 24" ).arg( ratio ) ) );
}

void GeostatPerfTests::sgsScalesLinearlyWithCells()
{
  const std::vector<Sample> samples = syntheticWells( 50, 40, 20.0 );
  const VariogramModel model = workingModel();
  SgsParams params;
  params.nRealizations = 1;
  params.seed = 42;
  params.maxPoints = 16;

  QElapsedTimer timer;
  timer.start();
  const SgsResult small = sgs( samples, gridOver( 1000, 256 ), model, params );
  const qint64 smallMs = timer.elapsed();
  QCOMPARE( small.status, Status::Ok );
  timer.restart();
  const SgsResult big = sgs( samples, gridOver( 1000, 512 ), model, params );
  const qint64 bigMs = timer.elapsed();
  QCOMPARE( big.status, Status::Ok );
  QCOMPARE( big.solverFailures, 0 );

  const double ratio = static_cast<double>( bigMs ) / static_cast<double>( std::max<qint64>( smallMs, 1 ) );
  qWarning( "%s", qPrintable( QStringLiteral( "BASELINE geostat_sgs_ms_512 = %1" ).arg( bigMs ) ) );
  qWarning( "%s", qPrintable( QStringLiteral( "BASELINE geostat_sgs_ms_256 = %1" ).arg( smallMs ) ) );
  qWarning( "%s", qPrintable( QStringLiteral( "BASELINE geostat_sgs_scale_ratio = %1" ).arg( ratio, 0, 'f', 2 ) ) );
  QVERIFY2( ratio <= 4.0 * 1.5,
            qPrintable( QStringLiteral( "sgs scale ratio %1 exceeds linear bound 6" ).arg( ratio ) ) );
}

QTEST_MAIN( GeostatPerfTests )
#include "tst_geostat_perf.moc"
