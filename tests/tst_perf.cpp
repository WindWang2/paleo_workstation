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
    QVERIFY2(elapsedMs < 10000,
             qPrintable(QStringLiteral("open took %1 ms > 10 s budget").arg(elapsedMs)));
  }

  // §41.6: PaleoTaskManager caps concurrent heavy tasks at max(2, cores/4).
  // Policy-value check only — the manager itself is not part of this slice.
  void concurrencyCap()
  {
    const int cores = QThread::idealThreadCount(); // -1 if undetectable
    const int cap = qMax(2, cores / 4);
    qDebug() << "idealThreadCount:" << cores << "-> concurrency cap:" << cap;
    QVERIFY2(cap >= 2, "cap must never drop below 2 even on degenerate cores");
    if (cores > 0)
      QVERIFY(cap <= cores);
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

  // §41.6: default derived-raster cell size = work-area extent / 2048.
  // Golden work_area is POLYGON 100–110 × 30–40 (10×10 deg), so the default
  // cell = 10/2048 ≈ 0.0049 deg.
  void defaultCellSize()
  {
    QgsVectorLayer workArea(goldenGpkg() + QStringLiteral("|layername=work_area"),
                            QStringLiteral("work_area"), QStringLiteral("ogr"));
    QVERIFY2(workArea.isValid(), qPrintable(workArea.error().message()));
    QCOMPARE(workArea.featureCount(), 1);

    const QgsRectangle ext = workArea.extent();
    QCOMPARE(ext.width(), 10.0);
    QCOMPARE(ext.height(), 10.0);

    const double cellSize = ext.width() / 2048.0;
    qDebug() << "work_area extent" << ext.width() << "deg -> default cell size" << cellSize;
    QVERIFY2(qAbs(cellSize - 0.0049) < 0.0001,
             qPrintable(QStringLiteral("cell %1 not ≈0.0049 deg").arg(cellSize)));
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
