#include "catalog/datacatalog.h"
#include "metadata/paleoprojectstore.h"
#include "qgis/crossplotmaplink.h"
#include "qgis/qgiscanvascontroller.h"
#include "qgis/qgislayerservice.h"
#include "qgis/qgisprocessingservice.h"
#include "qgis/qgisprojectservice.h"
#include "services/crossplotsamples.h"
#include "services/faciesclassificationservice.h"
#include "workflow/faciesclassify.h"
#include "workflow/workflows.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <gdal.h>
#include <qgsapplication.h>
#include <qgsmapcanvas.h>
#include <qgsrasterlayer.h>
using namespace paleo::crossplot;
class TestFacies : public QObject {
  Q_OBJECT
private slots:
  void rasterAndCompose();
  void wellIntervals();
  void taskLifecycle();
  void manualRules();
  void mapLocation();
};
static SampleSet rasterSamples() {
  Grid grid;
  grid.rows = 4;
  grid.cols = 4;
  grid.spatial = true;
  Plane a{"A",
          "raw-a",
          grid,
          {1, 1, 1, 1, 1, 1, 1, 1, 9, 9, 9, 9, 9, 9, 9, qQNaN()}},
      b = a;
  b.name = "B";
  b.versionId = "raw-b";
  auto r = CrossplotSamples::planes({a, b});
  return r.samples;
}
void TestFacies::rasterAndCompose() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  PaleoProjectStore store;
  PaleoTaskService tasks(&store);
  QgisProjectService project;
  QVERIFY(project.createProject(dir.filePath("test.qgz")));
  LayerManifest manifest(dir.filePath("metadata.sqlite"));
  QVERIFY(manifest.open());
  QgisLayerService layers(&project, &manifest);
  QgisProcessingService proc(&store);
  auto samples = std::make_shared<SampleSet>(rasterSamples());
  FaciesClassifyWorkflow wf(&tasks, &store, &layers);
  wf.setCatalog(&cat, dir.path());
  wf.setSamples(samples);
  ClassificationOptions options;
  options.k = 2;
  auto *task = wf.classify(options);
  QVERIFY(task);
  QSignalSpy finished(task, &PaleoTask::finished);
  if (!task->isFinished())
    QVERIFY(finished.wait(10000));
  QVERIFY(wf.classification().ok);
  QCOMPARE(wf.classification().counts.size(), 2);
  auto product = wf.write("D53");
  QVERIFY2(product.ok, qPrintable(product.error));
  QVERIFY(QFile::exists(product.path));
  auto version = cat.currentVersion(product.assetId);
  QCOMPARE(version.stage, QString("DERIVED"));
  QCOMPARE(version.parentVersionIds, QStringList({"raw-a", "raw-b"}));
  QCOMPARE(version.extra.value("parameterHash").toString().size(), 64);
  QVERIFY(!version.sha256.isEmpty());
  auto ds = GDALOpen(product.path.toUtf8().constData(), GA_ReadOnly);
  QVERIFY(ds);
  auto band = GDALGetRasterBand(ds, 1);
  unsigned char pixels[16];
  QCOMPARE(
      GDALRasterIO(band, GF_Read, 0, 0, 4, 4, pixels, 4, 4, GDT_Byte, 0, 0),
      CE_None);
  QCOMPARE(pixels[15], static_cast<unsigned char>(255));
  QVector<qint64> counts(2, 0);
  for (int i = 0; i < 15; ++i) {
    QVERIFY(pixels[i] < 2);
    ++counts[pixels[i]];
  }
  QCOMPARE(counts, product.counts);
  auto colors = GDALGetRasterColorTable(band);
  QVERIFY(colors);
  auto c = classColor(0);
  QCOMPARE(GDALGetColorEntry(colors, 0)->c1, short(c.red));
  GDALClose(ds);
  auto *raster =
      qobject_cast<QgsRasterLayer *>(layers.instantiate(product.layerId));
  QVERIFY(raster);
  QVERIFY(raster->isValid());
  CompositionWorkflow compose(&proc, &layers);
  compose.setCatalog(&cat, dir.path());
  QString error;
  QVERIFY2(compose.deriveFaciesPolygons("D53", product.layerId,
                                        {{"MIN_AREA", 0}, {"SIMPLIFY", 0}},
                                        &error),
           qPrintable(error));
  DataCatalog reopened;
  QVERIFY(reopened.open(dir.path()));
  QCOMPARE(
      reopened.currentVersion(product.assetId).extra.value("parameterHash"),
      version.extra.value("parameterHash"));
  cat.setLockedReadOnly(true);
  QVERIFY(!wf.write("D53").ok);
}
void TestFacies::wellIntervals() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));
  CatalogEntity well;
  well.id = "well-A";
  well.entityType = "well";
  well.name = "A";
  QVERIFY(cat.addEntity(well));
  PaleoProjectStore store;
  PaleoTaskService tasks(&store);
  FaciesClassifyWorkflow wf(&tasks, &store, nullptr);
  wf.setCatalog(&cat, dir.path());
  Channel a{"A", "", "well-A", "va", {0, 1, 2, 3, 4}, {1, 1, qQNaN(), 1, 9}},
      b = a;
  b.name = "B";
  auto samples =
      std::make_shared<SampleSet>(CrossplotSamples::well({a, b}).samples);
  wf.setSamples(samples);
  ClassificationOptions o;
  o.k = 2;
  auto *t = wf.classify(o);
  QSignalSpy done(t, &PaleoTask::finished);
  if (!t->isFinished())
    QVERIFY(done.wait(10000));
  auto intervals =
      FaciesClassificationService::intervals(*samples, wf.classification());
  QCOMPARE(intervals.size(), 3);
  QCOMPARE(intervals[0].top, 0.);
  QCOMPARE(intervals[0].base, 1.);
  QCOMPARE(intervals[1].top, 3.);
  QCOMPARE(intervals[1].base, 3.);
  auto product = wf.write("D53");
  QVERIFY2(product.ok, qPrintable(product.error));
  QFile file(product.path);
  QVERIFY(file.open(QIODevice::ReadOnly));
  auto rows = QJsonDocument::fromJson(file.readAll())
                  .object()
                  .value("intervals")
                  .toArray();
  QCOMPARE(rows.size(), 3);
  const auto links = cat.links();
  QVERIFY(
      std::any_of(links.begin(), links.end(), [&](const EntityAssetLink &l) {
        return l.assetId == product.assetId && l.role == "interpretation" &&
               l.entityId == "well-A";
      }));
}
void TestFacies::taskLifecycle() {
  PaleoProjectStore store;
  PaleoTaskService tasks(&store);
  tasks.setMaxWorkerThreads(1);
  FaciesClassifyWorkflow wf(&tasks, &store, nullptr);
  auto samples = std::make_shared<SampleSet>(rasterSamples());
  wf.setSamples(samples);
  auto *blocker = tasks.start("block", [](PaleoTask *t) {
    while (!t->cancelRequested())
      QThread::msleep(1);
    return QString();
  });
  auto *t = wf.classify({});
  QVERIFY(t);
  wf.cancel();
  blocker->requestCancel();
  QSignalSpy done(t, &PaleoTask::finished);
  if (!t->isFinished())
    QVERIFY(done.wait(10000));
  QCOMPARE(t->state(), PaleoTask::State::Cancelled);
  QVERIFY(!wf.classification().ok);
  wf.setSamples(std::make_shared<SampleSet>());
  t = wf.classify({});
  QVERIFY(t);
  QSignalSpy failed(t, &PaleoTask::finished);
  if (!t->isFinished())
    QVERIFY(failed.wait(10000));
  QCOMPARE(t->state(), PaleoTask::State::Failed);
}
void TestFacies::manualRules() {
  auto s = rasterSamples();
  ClassificationOptions o;
  o.method = Classifier::Hull;
  o.selection = {{0, 0}, {.2, 0}, {.2, .2}, {0, .2}};
  o.manualClass = 4;
  auto r = FaciesClassificationService::classify(s, o);
  QVERIFY(r.ok);
  QCOMPARE(r.counts[4], 8);
  QCOMPARE(r.labels.back(), -1);
  o.method = Classifier::Box;
  r = FaciesClassificationService::classify(s, o);
  QVERIFY(r.ok);
  QCOMPARE(r.counts[4], 8);
  o.selection.clear();
  QVERIFY(!FaciesClassificationService::classify(s, o).ok);
}
void TestFacies::mapLocation() {
  QgisCanvasController ctl;
  auto *canvas = ctl.canvas();
  CrossplotMapLink link(canvas);
  SampleSet s = rasterSamples();
  s.grid.crs = DataCatalog::localGridCrsWkt();
  QString error;
  QVERIFY2(link.locate(s, 0, &error), qPrintable(error));
  const auto center = canvas->extent().center();
  QVERIFY(std::abs(center.x() - s.locations[0].x) < 1e-8);
  QVERIFY(std::abs(center.y() - s.locations[0].y) < 1e-8);
  QVERIFY(link.highlight(s, {0, 3, 8}, &error));
  link.clear();
  s.locations[0].hasXY = false;
  QVERIFY(!link.locate(s, 0, &error));
  QVERIFY(!error.isEmpty());
}
int main(int argc, char **argv) {
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", "/usr"), true);
  app.initQgis();
  GDALAllRegister();
  TestFacies test;
  int rc = QTest::qExec(&test, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}
#include "tst_faciesclassify.moc"
