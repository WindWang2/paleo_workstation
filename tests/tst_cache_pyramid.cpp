// tst_cache_pyramid — wave/io-perf-cache D3：栅格瓦片金字塔的构建/瓦片 API/
// nodata 压缩/LRU/统计/并发/失效。
#include <QtTest>

#include "io/perffixtures.h"
#include "io/rasterpyramid.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <atomic>

class CachePyramidTests : public QObject
{
    Q_OBJECT

  private slots:
    void lazyEnsureCreatesMeta();
    void tileFetchMatchesSource();
    void topLevelTileCoversWholeRaster();
    void allNodataTileNotStored();
    void tileLruHitFast();
    void pinPreventsEviction();
    void eagerBuildsAllTiles();
    void statsReportTiles();
    void invalidateClears();
    void concurrentTileReads();
    void sourceChangeRebuilds();

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

void CachePyramidTests::lazyEnsureCreatesMeta()
{
  const QString tif = makeTiff(QStringLiteral("a.tif"), 600, 400, false);
  RasterPyramidService pyr(m_dir.filePath("pyr1"));
  RasterPyramidService::PyramidMeta meta;
  QString err;
  QVERIFY(pyr.ensure(tif, &meta, &err));
  QCOMPARE(meta.width, 600);
  QCOMPARE(meta.height, 400);
  QCOMPARE(meta.hasNodata, true);
  QVERIFY(meta.levels >= 2); // 600 > 256 → 至少 2 层
  QVERIFY(QFile::exists(m_dir.filePath("pyr1") + "/" + // key 哈希目录
                        QDir(m_dir.filePath("pyr1")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).at(0) +
                        "/meta.json"));
  QVERIFY(pyr.isBuilt(tif));
  // 懒建：瓦片未生成。
  QVERIFY(pyr.stats(tif).tilesStored == 0);
}

void CachePyramidTests::tileFetchMatchesSource()
{
  // z=0（原生分辨率）瓦片 = 源像元直读。
  const QString tif = makeTiff(QStringLiteral("b.tif"), 512, 300, false);
  RasterPyramidService pyr(m_dir.filePath("pyr2"));
  RasterPyramidService::PyramidMeta meta;
  QString err;
  QVERIFY(pyr.ensure(tif, &meta, &err));

  RasterPyramidService::Tile tile;
  QVERIFY(pyr.tile(tif, 0, 1, 0, &tile, &err));
  QCOMPARE(tile.width, 256);
  QCOMPARE(tile.height, 256);
  QCOMPARE(tile.samples.size(), 256 * 256);
  // 源像元 (256,0)：makeSyntheticGeoTiff 的确定性正弦面（float 存储容差）。
  const double expect = 1000.0 + 50.0 * std::sin(256 * 0.05) + 30.0 * std::cos(0 * 0.07);
  QVERIFY(qAbs(double(tile.samples.at(0)) - expect) < 0.01);
}

void CachePyramidTests::topLevelTileCoversWholeRaster()
{
  const QString tif = makeTiff(QStringLiteral("c.tif"), 512, 512, false);
  RasterPyramidService pyr(m_dir.filePath("pyr3"));
  RasterPyramidService::PyramidMeta meta;
  QString err;
  QVERIFY(pyr.ensure(tif, &meta, &err));
  const int top = meta.levels - 1;
  RasterPyramidService::Tile tile;
  QVERIFY(pyr.tile(tif, top, 0, 0, &tile, &err));
  QCOMPARE(tile.width, 256); // 512px → z1 半采样 256 → 单瓦片整幅
  QCOMPARE(tile.height, 256);
  QVERIFY(!tile.allNodata);
  // 越界瓦片拒绝。
  RasterPyramidService::Tile bad;
  QVERIFY(!pyr.tile(tif, top, 5, 5, &bad, &err));
  QVERIFY(!pyr.tile(tif, 99, 0, 0, &bad, &err));
}

void CachePyramidTests::allNodataTileNotStored()
{
  // D3.6：中央 1/3×1/3 nodata 洞——深处层级的中央瓦片全 nodata → 不落盘。
  const QString tif = makeTiff(QStringLiteral("d.tif"), 2048, 2048, true);
  RasterPyramidService pyr(m_dir.filePath("pyr4"));
  QString err;
  RasterPyramidService::PyramidMeta meta;
  QVERIFY(pyr.ensure(tif, &meta, &err, RasterPyramidService::BuildStrategy::Eager));
  const RasterPyramidService::Stats st = pyr.stats(tif);
  QVERIFY(st.tilesNodata > 0);
  QVERIFY(st.tilesStored > 0);
  RasterPyramidService::Tile tile;
  // z=0 的中央瓦片完全在洞里 → allNodata。
  QVERIFY(pyr.tile(tif, 0, 3, 3, &tile, &err));
  QVERIFY(tile.allNodata);
  QVERIFY(tile.samples.isEmpty());
}

void CachePyramidTests::tileLruHitFast()
{
  const QString tif = makeTiff(QStringLiteral("e.tif"), 1024, 1024, false);
  RasterPyramidService pyr(m_dir.filePath("pyr5"));
  QString err;
  RasterPyramidService::Tile tile;
  pyr.ensure(tif);
  QVERIFY(pyr.tile(tif, 1, 0, 0, &tile, &err)); // 首建
  QElapsedTimer t;
  t.start();
  RasterPyramidService::Tile again;
  QVERIFY(pyr.tile(tif, 1, 0, 0, &again, &err)); // LRU 命中
  QVERIFY(t.nsecsElapsed() / 1.0e6 < 2.0);
  QCOMPARE(again.samples, tile.samples);
  QVERIFY(pyr.cacheStats().hits >= 1);
}

void CachePyramidTests::pinPreventsEviction()
{
  const QString tif = makeTiff(QStringLiteral("f.tif"), 1024, 1024, false);
  RasterPyramidService pyr(m_dir.filePath("pyr6"));
  pyr.ensure(tif);
  RasterPyramidService::Tile tile;
  QVERIFY(pyr.tile(tif, 0, 0, 0, &tile));
  pyr.pinTile(tif, 0, 0, 0); // D6.3
  pyr.setTileCacheBudget(1); // 极限收缩
  RasterPyramidService::Tile probe; // 触发一次未 pin 瓦片装载后再断 pin 存活
  pyr.tile(tif, 0, 1, 0, &probe);
  QVERIFY(pyr.cacheStats().pinnedSkips > 0);
  RasterPyramidService::Tile still;
  QVERIFY(pyr.tile(tif, 0, 0, 0, &still)); // 命中（未被逐）
  pyr.unpinTile(tif, 0, 0, 0);
  pyr.setTileCacheBudget(128LL * 1024 * 1024);
}

void CachePyramidTests::eagerBuildsAllTiles()
{
  const QString tif = makeTiff(QStringLiteral("g.tif"), 700, 500, false);
  RasterPyramidService pyr(m_dir.filePath("pyr7"));
  QString err;
  QVERIFY(pyr.ensure(tif, nullptr, &err, RasterPyramidService::BuildStrategy::Eager));
  const RasterPyramidService::Stats st = pyr.stats(tif);
  QVERIFY(st.tilesStored + st.tilesNodata >= 3 * 2 + 1 + 2); // z0 3×2 + z1 2 + z2 1
  QCOMPARE(st.tilesUnbuilt, qint64(0));
}

void CachePyramidTests::statsReportTiles()
{
  const QString tif = makeTiff(QStringLiteral("h.tif"), 512, 512, false);
  RasterPyramidService pyr(m_dir.filePath("pyr8"));
  pyr.ensure(tif);
  RasterPyramidService::Tile t0;
  pyr.tile(tif, 0, 0, 0, &t0); // 只建一片
  const RasterPyramidService::Stats st = pyr.stats(tif);
  QCOMPARE(st.levels, 2);
  QCOMPARE(st.tilesStored, qint64(1));
  QVERIFY(st.tilesUnbuilt > 0);
  QVERIFY(st.bytesOnDisk > 0);
}

void CachePyramidTests::invalidateClears()
{
  const QString tif = makeTiff(QStringLiteral("i.tif"), 512, 512, false);
  RasterPyramidService pyr(m_dir.filePath("pyr9"));
  pyr.ensure(tif);
  RasterPyramidService::Tile tile;
  pyr.tile(tif, 0, 0, 0, &tile);
  pyr.invalidate(tif);
  QVERIFY(!pyr.isBuilt(tif));
  const RasterPyramidService::Stats st = pyr.stats(tif);
  QCOMPARE(st.levels, 0); // meta 没了 → 空统计
}

void CachePyramidTests::concurrentTileReads()
{
  // D3.8：4 线程并发 tile()——结果一致、无崩溃、无互相污染。
  const QString tif = makeTiff(QStringLiteral("j.tif"), 1024, 1024, true);
  RasterPyramidService pyr(m_dir.filePath("pyr10"));
  pyr.ensure(tif);
  std::atomic_bool failed{false};
  QList<QThread *> threads;
  for (int t = 0; t < 4; ++t)
  {
    auto *th = QThread::create([&pyr, &tif, &failed, t]() {
      for (int i = 0; i < 24; ++i)
      {
        RasterPyramidService::Tile tile;
        QString err;
        const int x = (i + t) % 4;
        if (!pyr.tile(tif, 0, x, 0, &tile, &err))
          failed.store(true);
        else if (tile.width != 256)
          failed.store(true);
      }
    });
    threads.append(th);
    th->start();
  }
  for (QThread *th : threads)
    th->wait();
  qDeleteAll(threads);
  QVERIFY(!failed.load());
}

void CachePyramidTests::sourceChangeRebuilds()
{
  const QString tif = makeTiff(QStringLiteral("k.tif"), 512, 512, false);
  RasterPyramidService pyr(m_dir.filePath("pyr11"));
  pyr.ensure(tif);
  // 源重写（不同尺寸）→ 身份失配 → 重建。
  const QString tif2 = makeTiff(QStringLiteral("k2.tif"), 300, 300, false);
  QFile::remove(tif);
  QVERIFY(QFile::copy(tif2, tif));
  RasterPyramidService::PyramidMeta meta;
  QString err;
  QVERIFY(pyr.ensure(tif, &meta, &err));
  QCOMPARE(meta.width, 300);
}

QTEST_MAIN(CachePyramidTests)
#include "tst_cache_pyramid.moc"
