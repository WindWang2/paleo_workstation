#include "helpers/workflowfixture.h"
#include "../src/metadata/mapversionstore.h"
#include "../src/qgis/mappingartifactwriter.h"
#include "../src/services/paleotaskservice.h"
#include "../src/workflow/mapversioncontroller.h"
#include <QtTest>
#include <QJsonDocument>
#include <QSemaphore>
#include <QScopeGuard>
#include <qgsapplication.h>
#include <qgsgeometry.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>

class TestSnapshotQuery : public QObject {
  Q_OBJECT
  struct Fixture : paleo::tests::WorkflowFixture {
    MapVersionStore versions{dir.filePath("project.sqlite")};
    MapVersionController controller{&versions, &layers};
    PaleoTaskService tasks;
    bool init() {
      if (!paleo::tests::initFixture(*this) || !versions.open()) return false;
      store.setProjectPaths(dir.filePath("proj.qgz"), dir.filePath("proj.gpkg"), dir.filePath("project.sqlite"));
      controller.setProjectStore(&store); controller.setCatalog(&catalog);
      tasks.setMaxWorkerThreads(1); controller.setTaskService(&tasks);
      return true;
    }
    QgsVectorLayer *points(int count, QString *error) {
      QVariantList rows;
      for (int n = 0; n < count; ++n) rows << QVariantMap{{"id", QString::number(n)}, {"name", QString::number(n)},
        {"x", double(n)}, {"y", 0.0}, {"facies_code", 1}};
      const auto path = dir.filePath("points.geojson");
      if (!MappingArtifactWriter::points(path, rows, error)) return nullptr;
      LayerDeclaration d; d.layerId = "facies.D61"; d.horizon = "D61"; d.type = "vector"; d.source = path;
      if (!layers.declare(d, error)) return nullptr;
      auto *layer = qobject_cast<QgsVectorLayer *>(layers.instantiate(d.layerId, error));
      if (!layer || !layer->startEditing() || !layer->addAttribute(QgsField("paleo_interpretation", QMetaType::Type::QString))) return nullptr;
      // A business column forces real per-feature JSON/integrity checks even
      // when most legacy features have no registered interpretation yet.
      return layer;
    }
  };
private slots:
  void startReturnsBeforeCommittingEditableLayer() {
    Fixture f; QVERIFY(f.init()); QString error;
    auto *layer = f.points(10000, &error); QVERIFY2(layer, qPrintable(error));
    QVERIFY2(f.controller.startSaveVersion("D61", {}, &error), qPrintable(error));
    QVERIFY(layer->isEditable()); QVERIFY(layer->isModified());
    f.controller.cancelSnapshot();
    QTRY_VERIFY(!f.controller.snapshotBusy());
    QCOMPARE(f.versions.currentVersion("D61"), 0);
    QVERIFY(layer->isEditable()); QVERIFY(layer->isModified());
    QVERIFY(f.tasks.shutdown());
  }
  void partialQueryCanCancelOrRejectChangedInputs_data() {
    QTest::addColumn<QString>("action");
    for (const auto &action : {"cancel", "edit", "provider-change", "catalog-change", "manifest-change", "unload", "project-close"})
      QTest::newRow(action) << QString(action);
  }
  void partialQueryCanCancelOrRejectChangedInputs() {
    QFETCH(QString, action);
    Fixture f; QVERIFY(f.init()); QString error;
    QPointer<QgsVectorLayer> layer = f.points(10000, &error); QVERIFY2(layer, qPrintable(error));
    bool acted = false; qint64 firstChecked = 0;
    QSignalSpy finished(&f.controller, &MapVersionController::snapshotFinished);
    connect(&f.controller, &MapVersionController::snapshotQueryProgress, &f.controller,
      [&](const QString &, qint64 checked, qint64 total) {
        if (acted || checked == 0) return;
        acted = true; firstChecked = checked; QCOMPARE(total, qint64(10000)); QVERIFY(checked <= 64);
        if (action == "cancel") f.controller.cancelSnapshot();
        else if (action == "project-close") f.controller.abandonForProjectClose();
        else if (action == "unload") QgsProject::instance()->removeMapLayer(layer.data());
        else if (action == "provider-change") QVERIFY(QMetaObject::invokeMethod(layer.data(), "dataChanged", Qt::DirectConnection));
        else if (action == "catalog-change") {
          CatalogAsset asset; asset.id = "new-source"; asset.type = "reference"; QVERIFY(f.catalog.addAsset(asset));
        } else if (action == "manifest-change") {
          auto declaration = f.layers.declared().first(); declaration.title = "changed declaration";
          QVERIFY(f.layers.declare(declaration));
        }
        else {
          QgsFeature feature; QVERIFY(layer->getFeatures().nextFeature(feature));
          auto geometry = QgsGeometry::fromPointXY(QgsPointXY(8, 9));
          QVERIFY(layer->changeGeometry(feature.id(), geometry));
        }
      });
    QVERIFY2(f.controller.startSaveVersion("D61", {}, &error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(!f.controller.snapshotBusy(), 15000);
    QVERIFY(acted); QVERIFY(firstChecked > 0 && firstChecked < 10000);
    QCOMPARE(f.versions.currentVersion("D61"), 0);
    if (action == "project-close") QCOMPARE(finished.size(), 0);
    else {
      QCOMPARE(finished.size(), 1); QVERIFY(!finished[0][2].toBool());
      if (action == "cancel") QVERIFY(finished[0][3].toBool());
      else QVERIFY(!finished[0][4].toString().isEmpty());
    }
    if (layer) QVERIFY(layer->isEditable());
    QVERIFY(f.tasks.shutdown());
  }
  void malformedEvidenceFailsBeforeNativeCommit() {
    Fixture f; QVERIFY(f.init()); QString error;
    auto *layer = f.points(10, &error); QVERIFY2(layer, qPrintable(error));
    QgsFeature feature; QVERIFY(layer->getFeatures().nextFeature(feature));
    QVERIFY(layer->changeAttributeValue(feature.id(), layer->fields().indexOf("paleo_interpretation"), "broken-json"));
    QSignalSpy finished(&f.controller, &MapVersionController::snapshotFinished);
    QVERIFY(f.controller.startSaveVersion("D61", {}, &error));
    QTRY_VERIFY(!f.controller.snapshotBusy());
    QCOMPARE(finished.size(), 1); QVERIFY(!finished[0][2].toBool()); QVERIFY(!finished[0][4].toString().isEmpty());
    QVERIFY(layer->isEditable()); QVERIFY(layer->isModified()); QCOMPARE(f.versions.currentVersion("D61"), 0);
    QVERIFY(f.tasks.shutdown());
  }
  void taskAcquiringSourceDuringQueryKeepsItsLock() {
    Fixture f; QVERIFY(f.init()); QString error;
    auto *layer = f.points(1000, &error); QVERIFY2(layer, qPrintable(error));
    bool acquired = false; QSignalSpy finished(&f.controller, &MapVersionController::snapshotFinished);
    connect(&f.controller, &MapVersionController::snapshotQueryProgress, &f.controller,
      [&](const QString &, qint64 checked, qint64) {
        if (acquired || !checked) return;
        acquired = true; f.store.markLayerBusy("facies.D61", "other-computation", "other task");
      });
    QVERIFY(f.controller.startSaveVersion("D61", {}, &error)); QTRY_VERIFY(!f.controller.snapshotBusy());
    QVERIFY(acquired); QCOMPARE(finished.size(), 1); QVERIFY(!finished[0][2].toBool());
    QVERIFY(f.store.layerBusy("facies.D61")); QCOMPARE(f.store.busyLayers().first().taskId, QString("other-computation"));
    QVERIFY(layer->isEditable()); QCOMPARE(f.versions.currentVersion("D61"), 0);
    f.store.markLayerFree("facies.D61"); QVERIFY(f.tasks.shutdown());
  }
  void workerTerminationCannotReleaseReplacementOwnerLock() {
    Fixture f; QVERIFY(f.init()); QString error;
    QVERIFY2(f.points(10, &error), qPrintable(error));
    auto gate = std::make_shared<QSemaphore>(); auto entered = std::make_shared<QSemaphore>();
    f.tasks.start("hold", [gate, entered](PaleoTask *) { entered->release(); gate->acquire(); return QString(); });
    const auto release = qScopeGuard([&] { gate->release(); }); QTRY_VERIFY(entered->available());
    QVERIFY(f.controller.startSaveVersion("D61", {}, &error));
    QTRY_VERIFY(f.store.layerBusy("facies.D61"));
    f.store.markLayerBusy("facies.D61", "replacement-owner", "replacement lock");
    f.controller.cancelSnapshot(); QVERIFY(f.store.layerBusy("facies.D61"));
    gate->release(); QTRY_VERIFY(!f.controller.snapshotBusy());
    QVERIFY(f.store.layerBusy("facies.D61")); QCOMPARE(f.store.busyLayers().first().taskId, QString("replacement-owner"));
    QCOMPARE(f.versions.currentVersion("D61"), 0); f.store.markLayerFree("facies.D61");
    QVERIFY(f.tasks.shutdown());
  }
};
int main(int argc, char **argv) {
  if (qgetenv("QT_QPA_PLATFORM").isEmpty()) qputenv("QT_QPA_PLATFORM", "offscreen");
  QgsApplication app(argc, argv, false); app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", "/usr"), true);
  app.initQgis(); TestSnapshotQuery test; const auto result = QTest::qExec(&test, argc, argv);
  QgsApplication::exitQgis(); return result;
}
#include "tst_snapshotquery.moc"
