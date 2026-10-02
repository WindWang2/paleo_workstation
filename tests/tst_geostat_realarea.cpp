// 层：数据（测试壳位于 tests/，被测对象为克里金/SGS 核；真机数据门控）
#include <QtTest/QtTest>

#include "algorithms/geostat/kriging.h"
#include "algorithms/geostat/sgs.h"
#include "algorithms/geostat/variogram.h"
#include "algorithms/gridsolver.h"
#include "io/horizonbinner.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>

#include <cmath>
#include <vector>

using namespace paleo::geostat;

// 真工区克里金/SGS 实测（方向18 Oracle 7）：PALEO_REAL_PROJECT_AREA 指向
// 真目录时执行；未设置时 QSKIP（CI 不红）。全程只读，不写源目录。
// 绝对门（env 门控实测口径，不受 TEST-02 CI 墙钟禁令约束）：
//   克里金 < 60s；SGS 单实现 < 30s。
// 输出行钉死 BASELINE geostat_* = <value>，人工誊入
// docs/progress/geostat-methods.md。取散点最多的层位做代表（样本量贴近
// 验收口径 ~2k）。
class GeostatRealAreaTests : public QObject
{
  Q_OBJECT
private slots:
  void krigingAndSgsOnLargestHorizon();
};

void GeostatRealAreaTests::krigingAndSgsOnLargestHorizon()
{
  const QString area = qEnvironmentVariable( "PALEO_REAL_PROJECT_AREA" );
  if ( area.isEmpty() )
    QSKIP( "PALEO_REAL_PROJECT_AREA not set — real-data geostat skipped" );

  const QDir horizonDir( QDir( area ).absoluteFilePath( QStringLiteral( "层位" ) ) );
  const QStringList files = horizonDir.entryList(
      QStringList() << QStringLiteral( "*.dat" ), QDir::Files, QDir::Name );
  QVERIFY2( files.size() >= 8,
            qPrintable( QStringLiteral( "expected >=8 horizon files in %1" )
                            .arg( horizonDir.absolutePath() ) ) );

  QString chosen;
  int chosenPoints = -1;
  HorizonScatter scatter;
  HorizonHeader header;
  for ( const QString &name : files )
  {
    QFile f( horizonDir.absoluteFilePath( name ) );
    if ( !f.open( QIODevice::ReadOnly ) )
      continue;
    const QByteArray text = f.readAll();
    const HorizonScatter candidate = parseHorizonScatter( text );
    if ( candidate.points.size() > chosenPoints )
    {
      HorizonHeader hh;
      if ( !parseHorizonHeader( text, &hh ) )
        continue;
      chosen = name;
      chosenPoints = candidate.points.size();
      scatter = candidate;
      header = hh;
    }
  }
  QVERIFY2( !chosen.isEmpty(), "no parseable horizon" );
  qWarning( "%s", qPrintable( QStringLiteral( "BASELINE geostat_real_horizon = %1" ).arg( chosen ) ) );
  qWarning( "%s", qPrintable( QStringLiteral( "BASELINE geostat_real_samples = %1" ).arg( chosenPoints ) ) );

  std::vector<Sample> samples;
  samples.reserve( static_cast<std::size_t>( scatter.points.size() ) );
  double sampleMin = 1e300, sampleMax = -1e300;
  double minX = 1e300, maxX = -1e300, minY = 1e300, maxY = -1e300;
  for ( const HorizonScatterPoint &p : scatter.points )
  {
    if ( !std::isfinite( p.x ) || !std::isfinite( p.y ) || !std::isfinite( p.z ) )
      continue;
    samples.push_back( Sample{ p.x, p.y, p.z } );
    sampleMin = std::min( sampleMin, p.z );
    sampleMax = std::max( sampleMax, p.z );
    minX = std::min( minX, p.x );
    maxX = std::max( maxX, p.x );
    minY = std::min( minY, p.y );
    maxY = std::max( maxY, p.y );
  }
  QVERIFY( samples.size() >= 8 );

  const double cell = ( header.p2x - header.p1x ) / ( header.gridCols - 1 );
  QVERIFY2( cell > 0 && std::isfinite( cell ), "bad header cell" );
  QString geomErr;
  const paleo::gridsolver::GridGeometry geom =
      paleo::gridsolver::geometryForExtent( minX, maxX, minY, maxY, cell, &geomErr );
  QVERIFY2( geom.isValid(), qPrintable( geomErr ) );

  GridSpec grid;
  grid.cols = geom.cols;
  grid.rows = geom.rows;
  grid.originX = geom.originX;
  grid.originY = geom.originY;
  grid.pixelWidth = geom.dx;
  grid.pixelHeight = -geom.dy;
  qWarning( "%s", qPrintable( QStringLiteral( "BASELINE geostat_real_grid = %1x%2" )
                                  .arg( grid.cols )
                                  .arg( grid.rows ) ) );

  // 自动拟合（全向球状）：lag = sqrt(面积/样本数)
  const double lag = std::max( cell, std::sqrt( ( maxX - minX ) * ( maxY - minY ) / samples.size() ) );
  const VariogramFit fit = fitVariogram( experimentalVariogram( samples, lag, 12 ),
                                         VariogramModelType::Spherical );
  QVERIFY2( fit.status == Status::Ok, qPrintable( QString::fromStdString( fit.message ) ) );
  qWarning( "%s", qPrintable( QStringLiteral( "BASELINE geostat_real_fit_range = %1" )
                                  .arg( fit.model.range, 0, 'f', 1 ) ) );
  qWarning( "%s", qPrintable( QStringLiteral( "BASELINE geostat_real_fit_sill = %1" )
                                  .arg( fit.model.sill, 0, 'f', 3 ) ) );
  qWarning( "%s", qPrintable( QStringLiteral( "BASELINE geostat_real_fit_nugget = %1" )
                                  .arg( fit.model.nugget, 0, 'f', 4 ) ) );
  qWarning( "%s", qPrintable( QStringLiteral( "BASELINE geostat_real_fit_r2 = %1" )
                                  .arg( fit.r2, 0, 'f', 4 ) ) );

  KrigingParams krigingParams;
  krigingParams.maxPoints = 16;
  QElapsedTimer timer;
  timer.start();
  const KrigingResult kriged = ordinaryKriging( samples, grid, fit.model, krigingParams );
  const qint64 krigingMs = timer.elapsed();
  QCOMPARE( kriged.status, Status::Ok );
  qWarning( "%s", qPrintable( QStringLiteral( "BASELINE geostat_real_kriging_ms = %1" ).arg( krigingMs ) ) );
  qWarning( "%s", qPrintable( QStringLiteral( "BASELINE geostat_real_kriging_finite = %1" )
                                  .arg( kriged.finiteCells ) ) );
  QVERIFY2( kriged.solverFailures == 0,
            qPrintable( QStringLiteral( "solver failures: %1" ).arg( kriged.solverFailures ) ) );
  QVERIFY2( kriged.finiteCells > static_cast<int>( kriged.estimate.size() ) / 2,
            "majority of cells must be filled" );
  // 克里金估值不外冲样本值域（±5% 极差裕度：块金下的小幅越界是合法的）
  const double span = std::max( sampleMax - sampleMin, 1e-12 );
  double zMin = 1e300, zMax = -1e300;
  for ( double value : kriged.estimate )
  {
    if ( std::isfinite( value ) )
    {
      zMin = std::min( zMin, value );
      zMax = std::max( zMax, value );
    }
  }
  QVERIFY2( zMin >= sampleMin - 0.05 * span && zMax <= sampleMax + 0.05 * span,
            qPrintable( QStringLiteral( "estimates [%1,%2] vs samples [%3,%4]" )
                            .arg( zMin )
                            .arg( zMax )
                            .arg( sampleMin )
                            .arg( sampleMax ) ) );
  // 绝对门（真机 env 门控口径）
  QVERIFY2( krigingMs < 60000,
            qPrintable( QStringLiteral( "kriging %1 ms exceeds 60 s gate" ).arg( krigingMs ) ) );

  SgsParams sgsParams;
  sgsParams.nRealizations = 1;
  sgsParams.seed = 20261003;
  sgsParams.maxPoints = 16;
  timer.restart();
  const SgsResult simulated = sgs( samples, grid, fit.model, sgsParams );
  const qint64 sgsMs = timer.elapsed();
  QCOMPARE( simulated.status, Status::Ok );
  qWarning( "%s", qPrintable( QStringLiteral( "BASELINE geostat_real_sgs_ms = %1" ).arg( sgsMs ) ) );
  QVERIFY2( sgsMs < 30000,
            qPrintable( QStringLiteral( "sgs %1 ms exceeds 30 s gate" ).arg( sgsMs ) ) );
  QVERIFY2( simulated.solverFailures == 0,
            qPrintable( QStringLiteral( "sgs solver failures: %1" ).arg( simulated.solverFailures ) ) );
}

QTEST_MAIN( GeostatRealAreaTests )
#include "tst_geostat_realarea.moc"
