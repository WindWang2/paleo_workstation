#include <QtTest>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>

#include <qgsproject.h>
#include <qgsrectangle.h>
#include <qgsvectorlayer.h>

#include <gdal.h>

#include "../src/qgis/qgisruntime.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/services/paleotaskservice.h" // FE-002：真实并发上限
#include "../src/algorithms/paleoalgorithms.h" // FE-002：真实网格策略守卫
#include "perfbudget_relax.h"

// ET10 (P2) performance acceptance — §41.6 minimum NFR numbers:
//   * project open ≤ 10 s on the golden fixture
//   * PaleoTaskManager heavy-task concurrency cap = max(2, cores/4)
//   * derived rasters > 50 MB must carry GDAL overviews (GDALAddo)
//   * derived-raster default cell size = work-area extent / 2048
// These are policy/budget checks against small fixtures — the budgets are
// trivially met at this size, so each case also records the measured value
// via qDebug for the Phase-0 calibration note ("数字在 Phase 0 实测后可校准").

static QString goldenGpkg()
{
#ifdef GOLDEN_GPKG
  return QStringLiteral(GOLDEN_GPKG);
#else
  const QString testsDir = QFileInfo(QString::fromUtf8(__FILE__)).absolutePath();
  return QDir(testsDir).absoluteFilePath(QStringLiteral("../testdata/golden.gpkg"));
#endif
}

class TestPerf : public QObject
{
  Q_OBJECT
private slots:
  void initTestCase()
  {
    QVERIFY(QgisRuntime::isInitialized());
    QVERIFY2(QFile::exists(goldenGpkg()), qPrintable(goldenGpkg()));
  }

  // §41.6: project open ≤ 10 s. Fixture: real .qgz round-tripped through
  // QgisProjectService carrying the golden layers (wells + work_area).
  void projectOpenBudget()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("perf.qgz"));

    {
      QgisProjectService svc;
      QVERIFY2(svc.createProject(path), qPrintable(svc.lastErrors().join(';')));
      for (const char *name : {"work_area", "wells"})
      {
        auto *layer = new QgsVectorLayer(
          goldenGpkg() + QStringLiteral("|layername=") + QLatin1String(name),
          QLatin1String(name), QStringLiteral("ogr"));
        QVERIFY2(layer->isValid(), qPrintable(layer->error().message()));
        QVERIFY(svc.project()->addMapLayer(layer) == layer);
      }
      QVERIFY2(svc.writeProject(), qPrintable(svc.lastErrors().join(';')));
    } // first service destroyed — reopen reads from disk only

    QElapsedTimer timer;
    timer.start();
    QgisProjectService svc2;
    QVERIFY2(svc2.openProject(path), qPrintable(svc2.lastErrors().join(';')));
    const qint64 elapsedMs = timer.elapsed();
    QCOMPARE(svc2.project()->mapLayers().size(), 2);

    qDebug() << "projectOpen:" << elapsedMs << "ms (budget 10000 ms, §41.6)";
    QVERIFY2(elapsedMs < paleo::perfbudget::relaxedBudgetMs(10000),
             qPrintable(QStringLiteral("open took %1 ms > 10 s budget").arg(elapsedMs)));
  }

  // §41.6 concurrency NFR, bound to the real production value (FE-002): the
  // old body computed qMax(2, cores/4) locally and asserted it against itself —
  // a formula that exists nowhere in src/ (production is a dedicated pool with
  // maxThreadCount 4, paleotaskservice.cpp D4.5). Mutating the production cap
  // now turns this red.
  void concurrencyCap()
  {
    PaleoTaskService svc;
    // 池上限按核数取 qBound(4, idealThreadCount, 8)：须在 [4,8] 且与本机核数一致。
    const int cap = svc.workerPool()->maxThreadCount();
    QVERIFY2(cap >= 4 && cap <= 8, qPrintable(QStringLiteral("cap=%1 不在 [4,8]").arg(cap)));
    QCOMPARE(cap, qBound(4, QThread::idealThreadCount(), 8));
    qDebug() << "PaleoTaskService worker cap:" << svc.workerPool()->maxThreadCount();
  }

  // §41.6: derived rasters > 50 MB must carry overviews. Two assertions:
  // a small raster is below threshold (no overviews required by policy),
  // and the overview path itself works — GDALBuildOverviews on a read-only
  // GTiff produces the external .ovr sidecar (the gdaladdo -ro mechanism).
  void overviewPolicy()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString tif = dir.filePath(QStringLiteral("small.tif"));

    GDALAllRegister();
    GDALDriverH drv = GDALGetDriverByName("GTiff");
    QVERIFY(drv != nullptr);
    GDALDatasetH ds = GDALCreate(drv, tif.toUtf8().constData(),
                                 256, 256, 1, GDT_Byte, nullptr);
    QVERIFY(ds != nullptr);
    QCOMPARE(GDALClose(ds), CE_None);

    const qint64 size = QFileInfo(tif).size();
    qDebug() << "raster size:" << size << "bytes (overview threshold 50 MB)";
    QVERIFY2(size < 50LL * 1024 * 1024,
             "fixture raster must be under the 50 MB overview threshold");

    const QString copy = dir.filePath(QStringLiteral("copy.tif"));
    QVERIFY(QFile::copy(tif, copy));
    GDALDatasetH ro = GDALOpen(copy.toUtf8().constData(), GA_ReadOnly);
    QVERIFY(ro != nullptr);
    const int levels[] = {2};
    QCOMPARE(GDALBuildOverviews(ro, "NEAREST", 1, levels,
                                0, nullptr, nullptr, nullptr), CE_None);
    QCOMPARE(GDALClose(ro), CE_None);
    QVERIFY2(QFile::exists(copy + QStringLiteral(".ovr")),
             "read-only GTiff BuildOverviews must yield external .ovr sidecar");
  }

  // §41.6 default derived-raster grid policy, bound to the real production
  // guard (FE-002): the old body recomputed extent/2048 in-test and asserted
  // ≈0.0049 against the golden fixture — a 2048 division policy that exists
  // nowhere in src/. Production truth is PaleoAlgoGuards::gridDimsForExtent
  // (ceil(extent/cell) dimensions with the 1e8-cell budget); mutating that
  // guard now turns this red. The §41.6 "extent/2048" strategy stays unproven
  // in production and is left to policy work, not a self-asserting test.
  void defaultCellSize()
  {
    QgsVectorLayer workArea(goldenGpkg() + QStringLiteral("|layername=work_area"),
                            QStringLiteral("work_area"), QStringLiteral("ogr"));
    QVERIFY2(workArea.isValid(), qPrintable(workArea.error().message()));
    QCOMPARE(workArea.featureCount(), 1);

    const QgsRectangle ext = workArea.extent();
    QCOMPARE(ext.width(), 10.0);
    QCOMPARE(ext.height(), 10.0);

    const auto dims = PaleoAlgoGuards::gridDimsForExtent(ext, 10.0 / 2048.0);
    QCOMPARE(dims.cols, 2048);
    QCOMPARE(dims.rows, 2048);

    bool threw = false;
    try {
      // 1e9 cells > 1e8 budget — the real production rejection path.
      PaleoAlgoGuards::gridDimsForExtent(ext, 10.0 / 100000.0);
    } catch (const QgsProcessingException &) {
      threw = true;
    }
    QVERIFY2(threw, "cell budget guard must reject >1e8-cell grids");
  }
};

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
    qFatal("QgisRuntime::initialize failed");
  TestPerf tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_perf.moc"
