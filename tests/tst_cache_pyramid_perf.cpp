// tst_cache_pyramid_perf — #758：栅格瓦片金字塔的墙钟比率门（perf 标签 + RUN_SERIAL）。
//
// 从 tst_cache_pyramid::tileLruHitFast 拆出。原处只有「LRU 命中须显著快于首建」
// 这一条零余量比率断言（warmMs < coldMs/2.0）：-j8 并行车道下内存拷贝被抢占即假红，
// 而 tst_cache_pyramid 其余用例（构建/瓦片 API/nodata/LRU/pin/统计/并发/失效）是
// 功能覆盖——按 cmake/paleo-test-labels.cmake 对 tst_panels / tst_correlation_full
// 的同类裁决，不因个别计时断言把整个功能测试挂 perf（-LE perf 不该丢功能覆盖）。
// 故计时断言独立成 perf 标签测试（由 paleo-test-labels.cmake 统一登记），功能测试
// 只留字节一致性与缓存命中计数等语义断言。
#include <QtTest>

#include "io/perffixtures.h"
#include "io/rasterpyramid.h"

#include <QDir>
#include <QElapsedTimer>
#include <QTemporaryDir>

class CachePyramidPerfTests : public QObject
{
    Q_OBJECT

  private slots:
    void lruHitIsFasterThanColdBuild();
    void cacheHitCountsAreSemantic();

  private:
    QTemporaryDir m_dir;
    QString makeTiff(const QString &name, int w, int h, bool hole)
    {
      const QString p = m_dir.filePath(name);
      QString err;
      if (!PerfFixtures::makeSyntheticGeoTiff(p, w, h, hole, &err))
        qWarning() << err;
      return p;
    }
};

void CachePyramidPerfTests::lruHitIsFasterThanColdBuild()
{
  const QString tif = makeTiff(QStringLiteral("perf.tif"), 1024, 1024, false);
  RasterPyramidService pyr(m_dir.filePath("pyr-perf"));
  QString err;
  RasterPyramidService::Tile tile;
  pyr.ensure(tif);
  QElapsedTimer cold;
  cold.start();
  QVERIFY(pyr.tile(tif, 1, 0, 0, &tile, &err)); // 首建（冷）
  const double coldMs = cold.nsecsElapsed() / 1.0e6;
  QElapsedTimer warm;
  warm.start();
  RasterPyramidService::Tile again;
  QVERIFY(pyr.tile(tif, 1, 0, 0, &again, &err)); // LRU 命中
  const double warmMs = warm.nsecsElapsed() / 1.0e6;
  // 比率门（随负载同侧伸缩，无绝对毫秒）：缓存失效回退磁盘/重建时仍红。
  QVERIFY2(warmMs < coldMs / 2.0,
           qPrintable(QStringLiteral("lru hit %1ms >= cold/2 %2ms").arg(warmMs).arg(coldMs / 2.0)));
  QCOMPARE(again.samples, tile.samples); // 命中必须字节一致（防「快但错」）
}

void CachePyramidPerfTests::cacheHitCountsAreSemantic()
{
  // 语义断言（零墙钟）：同一次 fetch 的第二次必须记 LRU 命中。
  const QString tif = makeTiff(QStringLiteral("sem.tif"), 512, 512, false);
  RasterPyramidService pyr(m_dir.filePath("pyr-sem"));
  QString err;
  QVERIFY(pyr.ensure(tif, nullptr, &err));
  RasterPyramidService::Tile a;
  RasterPyramidService::Tile b;
  QVERIFY(pyr.tile(tif, 0, 0, 0, &a, &err));
  const qint64 hitsBefore = pyr.cacheStats().hits;
  QVERIFY(pyr.tile(tif, 0, 0, 0, &b, &err));
  QCOMPARE(pyr.cacheStats().hits, hitsBefore + 1);
  QCOMPARE(b.samples, a.samples);
}

QTEST_MAIN(CachePyramidPerfTests)
#include "tst_cache_pyramid_perf.moc"
