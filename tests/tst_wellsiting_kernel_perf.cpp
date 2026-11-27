// 层：测试壳
#include <QtTest>

#include "algorithms/wellsiting/wellsiting.h"

#include <QElapsedTimer>

#include <algorithm>
#include <vector>

// #965：从 tst_wellsiting_kernel 拆出的大工区吞吐比率门（perf 标签 + RUN_SERIAL），
// 按 #758 tst_cache_pyramid_perf 先例执行：功能 Oracle 留在 tst_wellsiting_kernel
// 进入必过车道，本套独立承担负载敏感的吞吐比率校验。
using namespace paleo::wellsiting;

namespace
{
Polygon squareDomain( double size = 10000 )
{
  Polygon domain;
  domain.exterior.points = { Point2{ 0, 0 }, Point2{ size, 0 }, Point2{ size, size },
                             Point2{ 0, size } };
  return domain;
}
} // namespace

class TestWellSitingKernelPerf : public QObject
{
  Q_OBJECT
  private slots:
    void largeFieldThroughputRatio();
};

void TestWellSitingKernelPerf::largeFieldThroughputRatio()
{
  // 抽样式比率门：同一核在大格网（500×500）与参照小格网（100×100）上的
  // 吞吐比 ≥ 0.5——桶格索引应让单格成本近似常数；比率塌方说明索引退化
  // （如环形扩张未收敛）。不设绝对毫秒阈值。
  std::vector<Point2> wells;
  for ( int i = 0; i < 200; ++i )
    wells.push_back( Point2{ double( i % 20 ) * 950.0 + 100.0,
                             double( i / 20 ) * 950.0 + 100.0 } );

  const auto throughput = [&]( double cellSize ) -> double {
    SitingOptions options;
    options.controlRadius = 2500;
    options.cellSize = cellSize;
    QElapsedTimer timer;
    timer.start();
    const SitingField field = sampleField( { squareDomain() }, wells, options );
    const qint64 elapsed = timer.elapsed();
    // QCOMPARE/QVERIFY 在 lambda 内展开为 return;（void）——状态断言放外面。
    Q_ASSERT( field.status == Status::Ok );
    return double( field.inDomain.size() ) / double( std::max<qint64>( elapsed, 1 ) );
  };

  const double reference = throughput( 100 ); // 100×100
  const double large = throughput( 20 );      // 500×500
  SitingOptions sanity;
  sanity.controlRadius = 2500;
  sanity.cellSize = 20;
  QCOMPARE( sampleField( { squareDomain() }, wells, sanity ).status, Status::Ok );
  QVERIFY2( large >= 0.5 * reference,
            qPrintable( QStringLiteral( "throughput ratio %1/%2 = %3" )
                            .arg( large )
                            .arg( reference )
                            .arg( large / reference ) ) );
}

QTEST_MAIN( TestWellSitingKernelPerf )
#include "tst_wellsiting_kernel_perf.moc"
