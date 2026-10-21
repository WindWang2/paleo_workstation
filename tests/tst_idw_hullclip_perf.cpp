// 层：测试壳（被测对象 src/algorithms/paleoalgorithms.h 的 PreparedHull）
// perf 基准：IDW 凸包裁剪逐格点判定，旧路径 QgsGeometry::contains( x, y )
// （每格点重建未 prepare 的 GEOS 几何）对比 PreparedHull（prepare 一次）。
// 打 perf 标签（cmake/paleo-test-labels.cmake 的 _paleo_perf_tests），RUN_SERIAL；
// 墙钟只进 perf，不进功能门禁。掩码逐格一致是硬断言，与计时无关。
#include <QtTest/QtTest>

#include <qgsapplication.h>
#include <qgsgeometry.h>
#include <qgspointxy.h>

#include <QElapsedTimer>

#include <cmath>
#include <numbers>
#include <vector>

#include "../src/algorithms/paleoalgorithms.h"

class TestIdwHullClipPerf : public QObject
{
    Q_OBJECT

  private slots:
    void preparedVsUnprepared()
    {
      // 256 顶点近圆凸包（真实约束线并集凸包的量级上限附近），400x400 网格 = 16 万格点。
      QgsMultiPointXY pts;
      for ( int i = 0; i < 256; ++i )
      {
        const double a = 2.0 * std::numbers::pi * i / 256.0;
        pts << QgsPointXY( 500.0 + 450.0 * std::cos( a ), 500.0 + 430.0 * std::sin( a ) );
      }
      const QgsGeometry hull = QgsGeometry::fromMultiPointXY( pts ).convexHull();
      QVERIFY( !hull.isNull() );
      const int n = 400;
      const double xMin = 0.0, yMax = 1000.0, cell = 1000.0 / n;

      std::vector<char> ref( static_cast<size_t>( n ) * n ), got( ref.size() );
      QElapsedTimer t;
      t.start();
      for ( int r = 0; r < n; ++r )
      {
        const double y = yMax - ( r + 0.5 ) * cell;
        for ( int c = 0; c < n; ++c )
          ref[static_cast<size_t>( r ) * n + c] = hull.contains( xMin + ( c + 0.5 ) * cell, y ) ? 1 : 0;
      }
      const qint64 unpreparedNs = t.nsecsElapsed();

      t.restart();
      const paleo::algorithms::PreparedHull prepared( hull );
      for ( int r = 0; r < n; ++r )
      {
        const double y = yMax - ( r + 0.5 ) * cell;
        for ( int c = 0; c < n; ++c )
          got[static_cast<size_t>( r ) * n + c] = prepared.contains( xMin + ( c + 0.5 ) * cell, y ) ? 1 : 0;
      }
      const qint64 preparedNs = t.nsecsElapsed();

      QVERIFY( ref == got ); // 逐格一致（硬断言）
      const double speedup = preparedNs > 0 ? static_cast<double>( unpreparedNs ) / preparedNs : 0.0;
      qInfo().noquote() << QStringLiteral( "PERF idw_hullclip cells=%1 hullVertices=256 unprepared_ms=%2 prepared_ms=%3 speedup=%4x" )
                               .arg( n * n )
                               .arg( unpreparedNs / 1e6, 0, 'f', 1 )
                               .arg( preparedNs / 1e6, 0, 'f', 1 )
                               .arg( speedup, 0, 'f', 1 );
      // 只做方向性断言（prepared 不得比旧路径慢），不设紧阈值。
      QVERIFY2( preparedNs <= unpreparedNs, qPrintable( QStringLiteral( "prepared %1 ns > unprepared %2 ns" ).arg( preparedNs ).arg( unpreparedNs ) ) );
    }
};

int main( int argc, char *argv[] )
{
  QgsApplication app( argc, argv, false );
  app.setPrefixPath( qEnvironmentVariable( "QGIS_PREFIX_PATH", QStringLiteral( "/usr" ) ), true );
  app.initQgis();
  TestIdwHullClipPerf tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_idw_hullclip_perf.moc"
