#include <QtTest>
#include <QTemporaryDir>
#include <qgsapplication.h>
#include <qgslayertree.h>
#include <qgslayertreelayer.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>
#include "../src/metadata/layermanifest.h"
#include "../src/qgis/projectmapreference.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"

namespace {
const QString boundaryId = QStringLiteral("boundary.project_range");
LayerDeclaration boundary(const QString &path) {
  LayerDeclaration d;
  d.layerId = boundaryId; d.type = "vector"; d.source = path + "|layername=area";
  d.styleRef = path + ".qml"; d.title = QStringLiteral("工区范围（井位与地震）");
  d.group = "00_Data";
  return d;
}
QgsVectorLayer *addCopy(QgsProject *project, const LayerDeclaration &d, const QString &identity = {}) {
  auto *layer = new QgsVectorLayer(d.source, d.title, "ogr");
  if (!identity.isEmpty()) layer->setCustomProperty("paleoLayerId", identity);
  project->addMapLayer(layer);
  return layer;
}
}

class ProjectBoundaryLayerTest : public QObject {
  Q_OBJECT
private slots:
  void cleanup() { QgsProject::instance()->clear(); }
  void namedStyleRetainsStableIdentity() {
    QTemporaryDir tmp; QString error;
    const auto path = tmp.filePath("project_range.gpkg");
    QVERIFY2(paleo::mapreference::writeAreaRange(path, QgsRectangle(300000, 2300000, 310000, 2310000),
                                              QgsCoordinateReferenceSystem("EPSG:32650"), &error), qPrintable(error));
    LayerManifest manifest(tmp.filePath("manifest.sqlite")); QVERIFY(manifest.open(&error));
    QgisLayerService service(nullptr, &manifest); const auto d = boundary(path);
    QVERIFY(service.declare(d, &error)); auto *layer = service.instantiate(boundaryId, &error);
    QVERIFY2(layer, qPrintable(error)); QCOMPARE(layer->customProperty("paleoLayerId").toString(), boundaryId);
    QVERIFY(!qobject_cast<QgsVectorLayer *>(layer)->supportsEditing());
    QCOMPARE(service.instantiate(boundaryId), layer); QCOMPARE(QgsProject::instance()->mapLayers().size(), 1);
    service.setActiveHorizon("T30-T32-S1"); service.setActiveHorizon("T70-T72-S1");
    QCOMPARE(service.instantiate(boundaryId), layer); QCOMPARE(QgsProject::instance()->mapLayers().size(), 1);
    const auto qgz = tmp.filePath("project.qgz"); QVERIFY(QgsProject::instance()->write(qgz));
    QVERIFY(QgsProject::instance()->read(qgz));
    QCOMPARE(service.instantiate(boundaryId)->customProperty("paleoLayerId").toString(), boundaryId);
    QCOMPARE(QgsProject::instance()->mapLayers().size(), 1);
    QgsProject::instance()->clear();
  }
  void restoredCopiesCollapseWithoutTouchingOtherLayersOrFiles() {
    QTemporaryDir tmp; QString error; const auto path = tmp.filePath("project_range.gpkg");
    QVERIFY(paleo::mapreference::writeAreaRange(path, QgsRectangle(300000, 2300000, 310000, 2310000),
                                             QgsCoordinateReferenceSystem("EPSG:32650"), &error));
    LayerManifest manifest(tmp.filePath("manifest.sqlite")); QVERIFY(manifest.open(&error));
    QgisProjectService projectService; QgisLayerService service(&projectService, &manifest);
    auto *project = projectService.project(); const auto d = boundary(path); QVERIFY(service.declare(d, &error));
    for (int i = 0; i < 6; ++i) {
      auto *copy = addCopy(project, d); QVERIFY(copy->isValid());
      project->layerTreeRoot()->findLayer(copy->id())->setItemVisibilityChecked(i == 4);
    }
    auto *manual = addCopy(project, d, "manual.reference"); const auto manualId = manual->id();
    const auto otherPath = tmp.filePath("another.gpkg"); QVERIFY(QFile::copy(path, otherPath));
    auto *unrelated = addCopy(project, boundary(otherPath)); const auto unrelatedId = unrelated->id();
    projectService.projectOpened("test");
    auto *layer = service.layer(boundaryId); QVERIFY(layer); QCOMPARE(project->mapLayers().size(), 3);
    QVERIFY(project->mapLayer(manualId)); QVERIFY(project->mapLayer(unrelatedId));
    QVERIFY(project->layerTreeRoot()->findLayer(layer->id())->itemVisibilityChecked());
    QCOMPARE(service.instantiate(boundaryId), layer); QCOMPARE(project->mapLayers().size(), 3);
    QVERIFY(QFile::exists(path)); QVERIFY(QFile::exists(path + ".qml")); QVERIFY(QFile::exists(otherPath));
    project->clear();
  }
  void uncheckedCopiesStayUncheckedAndEditableCopiesArePreserved() {
    QTemporaryDir tmp; QString error; const auto path = tmp.filePath("project_range.gpkg");
    QVERIFY(paleo::mapreference::writeAreaRange(path, QgsRectangle(300000, 2300000, 310000, 2310000),
                                             QgsCoordinateReferenceSystem("EPSG:32650"), &error));
    LayerManifest manifest(tmp.filePath("manifest.sqlite")); QVERIFY(manifest.open(&error));
    QgisLayerService service(nullptr, &manifest); const auto d = boundary(path); QVERIFY(service.declare(d, &error));
    auto *project = QgsProject::instance();
    for (int i = 0; i < 2; ++i) {
      auto *copy = addCopy(project, d); project->layerTreeRoot()->findLayer(copy->id())->setItemVisibilityChecked(false);
    }
    auto *layer = service.instantiate(boundaryId); QVERIFY(layer); QCOMPARE(project->mapLayers().size(), 1);
    QVERIFY(!project->layerTreeRoot()->findLayer(layer->id())->itemVisibilityChecked());
    auto *editing = addCopy(project, d); QVERIFY(editing->startEditing());
    const auto editingId = editing->id();
    QCOMPARE(service.instantiate(boundaryId), layer); QVERIFY(project->mapLayer(editingId)); QVERIFY(editing->isEditable());
    QVERIFY(editing->rollBack()); QCOMPARE(service.instantiate(boundaryId), layer); QCOMPARE(project->mapLayers().size(), 1);
    // A bad replacement declaration must not remove the last usable range.
    QVERIFY(manifest.upsert(boundary(tmp.filePath("missing.gpkg")), &error));
    QVERIFY(!service.instantiate(boundaryId, &error)); QCOMPARE(project->mapLayers().size(), 1);
    QVERIFY(QFile::exists(path)); project->clear();
  }
  void registrationReentryReusesInstance() {
    QTemporaryDir tmp; QString error; const auto path = tmp.filePath("project_range.gpkg");
    QVERIFY(paleo::mapreference::writeAreaRange(path, QgsRectangle(300000, 2300000, 310000, 2310000),
                                             QgsCoordinateReferenceSystem("EPSG:32650"), &error));
    LayerManifest manifest(tmp.filePath("manifest.sqlite")); QVERIFY(manifest.open(&error));
    QgisLayerService service(nullptr, &manifest); QVERIFY(service.declare(boundary(path), &error));
    QgsMapLayer *reentered = nullptr; bool inCallback = false;
    const auto connection = connect(QgsProject::instance(), &QgsProject::layersAdded, this, [&] {
      if (inCallback) return;
      inCallback = true; reentered = service.instantiate(boundaryId); inCallback = false;
    });
    auto *layer = service.instantiate(boundaryId, &error); disconnect(connection);
    QVERIFY2(layer, qPrintable(error)); QCOMPARE(reentered, layer); QCOMPARE(QgsProject::instance()->mapLayers().size(), 1);
    QgsProject::instance()->clear();
  }
  void realSavedProjectRestoresOneRange() {
    const auto qgz = qEnvironmentVariable("PALEO_BOUNDARY_REAL_QGZ");
    const auto path = qEnvironmentVariable("PALEO_BOUNDARY_REAL_RANGE");
    if (qgz.isEmpty() || path.isEmpty()) QSKIP("Optional user-supplied project and generated range");
    QTemporaryDir tmp; QString error; LayerManifest manifest(tmp.filePath("manifest.sqlite"));
    QVERIFY(manifest.open(&error)); QgisProjectService projectService;
    QgisLayerService service(&projectService, &manifest); const auto d = boundary(path);
    QVERIFY(service.declare(d, &error)); auto *project = projectService.project(); QVERIFY(project->read(qgz));
    QSet<QString> unrelated; int copies = 0;
    for (auto *layer : project->mapLayers()) {
      if (layer->source() == d.source) ++copies;
      else unrelated.insert(layer->id());
    }
    QVERIFY(copies > 0); projectService.projectOpened(qgz);
    QVERIFY(service.layer(boundaryId)); QCOMPARE(project->mapLayers().size(), unrelated.size() + 1);
    for (const auto &id : unrelated) QVERIFY(project->mapLayer(id));
    // Save ONLY in the test directory; the live user's .qgz stays untouched.
    const auto repaired = tmp.filePath("repaired.qgz"); QVERIFY(project->write(repaired));
    QVERIFY(project->read(repaired)); projectService.projectOpened(repaired);
    QCOMPARE(project->mapLayers().size(), unrelated.size() + 1);
    QCOMPARE(service.instantiate(boundaryId), service.layer(boundaryId));
    QCOMPARE(project->mapLayers().size(), unrelated.size() + 1);
    qInfo() << "Saved project range instances" << copies << "-> 1; unrelated layers" << unrelated.size();
    QVERIFY(QFile::exists(path)); project->clear();
  }
};
int main(int argc, char **argv) {
  QTemporaryDir profile; QgsApplication app(argc, argv, true, profile.path());
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH"), true); QgsApplication::init(profile.path());
  QString error; if (!QgsApplication::createDatabase(&error)) return 1;
  app.initQgis(); QgsCoordinateReferenceSystem::invalidateCache(true);
  ProjectBoundaryLayerTest test; const int result = QTest::qExec(&test, argc, argv);
  QgsApplication::exitQgis(); return result;
}
#include "tst_projectboundarylayer.moc"
