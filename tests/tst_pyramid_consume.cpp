// tests/tst_pyramid_consume — B3（wave/deepen-perf）：栅格金字塔消费侧。
//   · DataImportService::ensureRasterPyramids（瓦片金字塔 Lazy 批量接线）；
//   · buildRasterOverviews（GDAL 外部 .ovr——受管 RAW 字节不动）；
//   · 大图「全图视口读块」改前/改后：概览在场时 GDAL provider 降采样读
//     走概览层（画布每次重渲的同一 IO 路径）。
#include <QtTest>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QTemporaryDir>

#include <memory>

#include "../src/io/dataimportservice.h"
#include "../src/io/perffixtures.h"
#include "../src/io/rasterpyramid.h"

#include "../src/qgis/qgisruntime.h"

#include <qgsrasterblock.h>
#include <qgsrasterlayer.h>
#include <qgsrasterpyramid.h>
#include <qgsrectangle.h>

class TestPyramidConsume : public QObject
{
  Q_OBJECT

  private slots:

    void initTestCase()
    {
      QVERIFY(QgisRuntime::isInitialized());
    }

    // Lazy ensure 批量接线：目录/状态表铺好、isBuilt 真、瓦片按需可取。
    void lazyEnsureBatchWired()
    {
      QTemporaryDir dir;
      const QString proj = dir.filePath(QStringLiteral("proj"));
      QDir().mkpath(proj);
      DataImportService svc(nullptr, nullptr);
      svc.setProjectDir(proj);
      QVERIFY(!svc.pyramidCacheDir().isEmpty());

      const QString tif = dir.filePath(QStringLiteral("small.tif"));
      QVERIFY(PerfFixtures::makeSyntheticGeoTiff(tif, 600, 400, false));
      QString err;
      QCOMPARE(svc.ensureRasterPyramids({tif, QStringLiteral("/nonexistent.tif")},
                                        &err),
               1); // 存在的 ensure 成功；缺文件如实失败不中断
      RasterPyramidService pyramids(svc.pyramidCacheDir());
      QVERIFY(pyramids.isBuilt(tif));
      RasterPyramidService::Tile tile;
      QVERIFY(pyramids.tile(tif, 1, 0, 0, &tile));
      QVERIFY(tile.width > 0 && tile.height > 0);
    }

    // 小图（无概览可建）：buildRasterOverviews 幂等成功。
    void smallRasterNoLevelsSucceeds()
    {
      QTemporaryDir dir;
      const QString tif = dir.filePath(QStringLiteral("tiny.tif"));
      QVERIFY(PerfFixtures::makeSyntheticGeoTiff(tif, 300, 200, false));
      DataImportService svc(nullptr, nullptr);
      QString err;
      QVERIFY(svc.buildRasterOverviews(tif, &err));
      QVERIFY(!QFile::exists(tif + QStringLiteral(".ovr"))); // 无层级 → 无边车
    }

    // 大图消费口径：全图视口读块（画布重渲的 IO 路径）改前/改后。
    void overviewSpeedsUpViewportBlockRead()
    {
      QTemporaryDir dir;
      // 4096×4096 f32 ≈ 64MB：够触发降采样读的量级，测试期可承受。
      const QString tif = dir.filePath(QStringLiteral("big.tif"));
      QVERIFY(PerfFixtures::makeSyntheticGeoTiff(tif, 4096, 4096, false));

      // 「改前」：无概览——provider 全图降采样读。
      qint64 beforeMs = -1;
      {
        QgsRasterLayer layer(tif, QStringLiteral("big"), QStringLiteral("gdal"));
        QVERIFY(layer.isValid());
        QVERIFY(!PreviewRasterAnalysisHasPyramidsFor(&layer));
        beforeMs = timedViewportBlock(&layer);
        QVERIFY(beforeMs >= 0);
      }

      // 建外部概览（.ovr）——受管 RAW 字节不动。
      DataImportService svc(nullptr, nullptr);
      QString err;
      QElapsedTimer buildClock;
      buildClock.start();
      QVERIFY(svc.buildRasterOverviews(tif, &err));
      qInfo("PERF overview-build(ms): %lld", buildClock.elapsed());
      QVERIFY(QFile::exists(tif + QStringLiteral(".ovr")));

      // 「改后」：重开层（provider 重新发现 .ovr）同一视口读块。
      qint64 afterMs = -1;
      {
        QgsRasterLayer layer(tif, QStringLiteral("big"), QStringLiteral("gdal"));
        QVERIFY(layer.isValid());
        QVERIFY(PreviewRasterAnalysisHasPyramidsFor(&layer));
        afterMs = timedViewportBlock(&layer);
      }
      qInfo("PERF viewport-block before(ms): %lld  after(ms): %lld", beforeMs,
            afterMs);
      // 机器无关断言：读块时长显著下降（概览降采样读 < 直读；CI 抖动留裕量）。
      QVERIFY2(afterMs * 3 < beforeMs + 40,
               qPrintable(QStringLiteral("before=%1ms after=%2ms — overviews "
                                         "must speed the decimated read")
                              .arg(beforeMs)
                              .arg(afterMs)));
    }

  private:
    // 画布重渲的等价 IO：按视口分辨率（全图 extent × ~900×700）读一块。
    static qint64 timedViewportBlock(QgsRasterLayer *layer)
    {
      auto *provider = layer->dataProvider();
      if (!provider)
        return -1;
      const QgsRectangle ext = layer->extent();
      QElapsedTimer t;
      t.start();
      std::unique_ptr<QgsRasterBlock> block(
          provider->block(1, ext, 900, 700));
      const qint64 ms = t.elapsed();
      if (!block || block->isEmpty())
        return -1;
      return ms;
    }

    static bool PreviewRasterAnalysisHasPyramidsFor(QgsRasterLayer *layer)
    {
      if (!layer || !layer->dataProvider())
        return false;
      const QList<QgsRasterPyramid> pyramids =
          layer->dataProvider()->buildPyramidList();
      for (const QgsRasterPyramid &p : pyramids)
        if (p.getExists())
          return true;
      return false;
    }
};

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  // QgsApplication（QgisRuntime 建）即进程 QApplication——不再叠 QApplication。
  TestPyramidConsume tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_pyramid_consume.moc"
