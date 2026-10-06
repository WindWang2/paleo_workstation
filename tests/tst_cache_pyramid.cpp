// tst_cache_pyramid — wave/io-perf-cache D3：栅格瓦片金字塔的构建/瓦片 API/
// nodata 压缩/LRU/统计/并发/失效。
#include <QtTest>

#include "io/perffixtures.h"
#include "io/rasterpyramid.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>
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
    void crashResidueStateWithoutOffsetSelfHeals();

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
  QElapsedTimer cold;
  cold.start();
  QVERIFY(pyr.tile(tif, 1, 0, 0, &tile, &err)); // 首建
  const double coldMs = cold.nsecsElapsed() / 1.0e6;
  QElapsedTimer t;
  t.start();
  RasterPyramidService::Tile again;
  QVERIFY(pyr.tile(tif, 1, 0, 0, &again, &err)); // LRU 命中
  const double warmMs = t.nsecsElapsed() / 1.0e6;
  // TEST-02：原 <2ms 绝对预算在多 worktree 并行争用下偶发超（内存拷贝被
  // 抢占）——比率门（LRU 命中须显著快于首建）随负载同侧伸缩，缓存失效
  // （回退磁盘/重建）时仍红。
  QVERIFY2(warmMs < coldMs / 2.0,
           qPrintable(QStringLiteral("lru hit %1ms >= cold/2 %2ms").arg(warmMs).arg(coldMs / 2.0)));
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

void CachePyramidTests::crashResidueStateWithoutOffsetSelfHeals()
{
  // #215：崩溃残片 state=1 而 offset 仍为 0——读路径不得把文件头当像素，
  // 应判非法偏移并重建该瓦片（永久自愈）。
  const QString tif = makeTiff(QStringLiteral("residue.tif"), 512, 300, false);
  const QString root = m_dir.filePath("pyr_residue");
  {
    RasterPyramidService pyr(root);
    QString err;
    QVERIFY(pyr.ensure(tif, nullptr, &err));
    // 先建两个别的瓦片，让 z0.bin 足够大：offset=0 时整块 read 能成功，
    // 修复前会把文件头 + 别的瓦片 payload 当像素静默返回。
    RasterPyramidService::Tile other;
    QVERIFY(pyr.tile(tif, 0, 0, 0, &other, &err));
    QVERIFY(pyr.tile(tif, 0, 0, 1, &other, &err));
  }
  const QStringList dirs = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
  QCOMPARE(dirs.size(), 1);
  QFile z0(root + "/" + dirs.at(0) + "/z0.bin");
  QVERIFY(z0.open(QIODevice::ReadWrite));
  const int tilesX = 2; // 512px / 256
  const qint64 statePos = 28 + 0 * tilesX + 1; // 瓦片 (1,0)
  QVERIFY(z0.seek(statePos));
  QVERIFY(z0.putChar('\1')); // state 先于 payload/offset 落盘的残片
  z0.close();

  RasterPyramidService pyr(root); // 新实例：LRU 空，必走磁盘层
  RasterPyramidService::Tile tile;
  QString err;
  QVERIFY2(pyr.tile(tif, 0, 1, 0, &tile, &err), qPrintable(err));
  QCOMPARE(tile.samples.size(), 256 * 256);
  const double expect = 1000.0 + 50.0 * std::sin(256 * 0.05) + 30.0 * std::cos(0 * 0.07);
  QVERIFY2(qAbs(double(tile.samples.at(0)) - expect) < 0.01,
           qPrintable(QString::number(tile.samples.at(0))));

  // 重建后 offset 已回填（非 0）——残片被永久修复而非每次重算。
  {
    QFile check(root + "/" + dirs.at(0) + "/z0.bin");
    QVERIFY(check.open(QIODevice::ReadOnly));
    const int n = tilesX * 2; // 512×300 → 2×2 瓦片
    QVERIFY(check.seek(28 + n + 1 * 8));
    const QByteArray off = check.read(8);
    QCOMPARE(off.size(), 8);
    QVERIFY(qFromLittleEndian<quint64>(reinterpret_cast<const uchar *>(off.constData())) != 0);
  }
  // 重建后落盘有效：再开新实例直读磁盘层仍正确。
  RasterPyramidService again(root);
  RasterPyramidService::Tile t2;
  QVERIFY(again.tile(tif, 0, 1, 0, &t2, &err));
  QVERIFY(qAbs(double(t2.samples.at(0)) - expect) < 0.01);
}

QTEST_MAIN(CachePyramidTests)
#include "tst_cache_pyramid.moc"
