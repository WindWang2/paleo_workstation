#include <QtTest>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <qgsapplication.h>
#include <qgscoordinatereferencesystem.h>
#include <qgscoordinatetransformcontext.h>
#include <qgsfeature.h>
#include <qgsfields.h>
#include <qgsgeometry.h>
#include <qgsproject.h>
#include <qgsvectorfilewriter.h>
#include <qgsvectorlayer.h>
#include <QVector>

#include "../src/catalog/datacatalog.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/mapversionstore.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/faciespartitioneditor.h"
#include "../src/qgis/mappingfeatureidentity.h"
#include "../src/qgis/qgiseditingservice.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/services/paleotaskservice.h"
#include "../src/ui/pages/mappingworkbenchpage.h"
#include "../src/workflow/workflows.h"
#include "../src/workflow/mapediteventhistory.h"
#include "../src/workflow/mappingworkbench.h"
#include "../src/workflow/mapversioncontroller.h"

namespace {
QVariantMap eventAt(const QString &id, const QString &uid, const QString &stamp) {
  return {{"event_id", id}, {"before_uid", uid}, {"after_uid", uid}, {"action", "revise_geometry"},
          {"timestamp", stamp}, {"target_object_id", uid}, {"before_geometry", stamp}};
}
CatalogVersion versionWith(const QString &id, const QVariantList &events, const QString &parent, bool delta) {
  CatalogVersion version;
  version.id = id;
  version.extra.insert("spatial_edit_events", events);
  if (delta) {
    version.extra.insert("spatial_edit_history", QVariantMap{
        {"schema", QString::fromLatin1(paleo::mapedit::kSpatialHistorySchema)},
        {"parent_version_id", parent}, {"parent_encoding", parent.isEmpty() ? "cumulative" : "delta"}});
  }
  return version;
}
}

class TestP6MapSave : public QObject {
  Q_OBJECT
  struct Fixture {
    QTemporaryDir dir;
    QgisProjectService projectSvc;
    PaleoProjectStore store;
    LayerManifest manifest{dir.filePath("project.sqlite")};
    QgisLayerService layers{&projectSvc, &manifest};
    MapVersionStore versions{dir.filePath("project.sqlite")};
    MapVersionController controller{&versions, &layers};
    bool init() {
      if (!dir.isValid() || !projectSvc.createProject(dir.filePath("proj.qgz")) || !manifest.open()) return false;
      store.setProjectPaths(dir.filePath("proj.qgz"), dir.filePath("proj.gpkg"), dir.filePath("project.sqlite"));
      controller.setProjectStore(&store);
      QString error;
      return versions.open(&error);
    }
  };
  static QString writePolygons(const QString &path, int count) {
    QgsVectorLayer mem(QStringLiteral("Polygon?crs=EPSG:4326&field=facies_code:integer"),
                       QStringLiteral("facies"), QStringLiteral("memory"));
    if (!mem.isValid() || !mem.dataProvider()) return {};
    QgsFeatureList batch;
    batch.reserve(count);
    for (int n = 0; n < count; ++n) {
      QgsFeature feature(mem.fields());
      const double x = n % 100;
      const double y = n / 100;
      feature.setAttribute(0, 1 + (n % 3));
      feature.setGeometry(QgsGeometry::fromWkt(
          QString("POLYGON((%1 %2, %3 %2, %3 %4, %1 %2))").arg(x).arg(y).arg(x + 0.4).arg(y + 0.4)));
      batch << feature;
    }
    if (!mem.dataProvider()->addFeatures(batch)) return {};
    QgsVectorFileWriter::SaveVectorOptions options;
    options.driverName = "GPKG";
    options.layerName = "facies_polygons";
    QString writeError;
    if (QgsVectorFileWriter::writeAsVectorFormatV3(
            &mem, path, QgsProject::instance()->transformContext(), options, &writeError)
        != QgsVectorFileWriter::NoError)
      return {};
    return path;
  }
  static bool declare(Fixture &f, const QString &id, const QString &path) {
    LayerDeclaration decl;
    decl.layerId = id;
    decl.horizon = "D61";
    decl.type = "vector";
    decl.source = path + "|layername=facies_polygons";
    QString error;
    return f.layers.declare(decl, &error);
  }
  static int snapshotFeatures(const QString &projectDir, const MapVersion &version) {
    const auto provenance = QJsonDocument::fromJson(version.provenance.toUtf8()).object();
    const auto layers = provenance.value("saved_layers").toArray();
    if (layers.isEmpty()) return -1;
    const auto relative = layers.first().toObject().value("path").toString();
    QgsVectorLayer layer(QDir(projectDir).filePath(relative) + "|layername=features", "snap", "ogr");
    return layer.isValid() ? int(layer.featureCount()) : -1;
  }
private slots:
  void deltaHistoryPagesAndDiagnoses() {
    QHash<QString, CatalogVersion> versions;
    QVariantList cumulative;
    for (int n = 0; n < 5; ++n) cumulative << eventAt(QString("c%1").arg(n), "feat", QString::number(n));
    versions.insert("v1", versionWith("v1", cumulative, {}, false));
    versions.insert("v2", versionWith("v2", {eventAt("d2", "feat", "20")}, "v1", true));
    versions.insert("v3", versionWith("v3", {}, "v2", true));
    versions.insert("v4", versionWith("v4", {eventAt("d4", "gone", "40")}, "v3", true));
    const auto lookup = [&](const QString &id) { return versions.value(id); };
    paleo::mapedit::SpatialEditHistoryQuery query;
    query.versionId = "v4";
    query.pageSize = 10;
    const auto all = paleo::mapedit::readSpatialEditHistory(lookup, query);
    QCOMPARE(all.value("events").toList().size(), 7);
    QVERIFY(all.value("complete").toBool());
    QStringList ids;
    for (const auto &entry : all.value("events").toList()) ids << entry.toMap().value("event_id").toString();
    QCOMPARE(ids, QStringList({"c0", "c1", "c2", "c3", "c4", "d2", "d4"}));
    query.pageSize = 2;
    QStringList paged;
    QString cursor;
    int pages = 0;
    do {
      query.cursor = cursor;
      const auto page = paleo::mapedit::readSpatialEditHistory(lookup, query);
      const auto rows = page.value("events").toList();
      QVERIFY(rows.size() <= 2);
      QVERIFY(page.value("visited_versions").toInt() > 0);
      if (pages == 0 && !rows.isEmpty())
        QCOMPARE(rows.last().toMap().value("event_id").toString(), QString("d4"));
      for (const auto &entry : rows) paged << entry.toMap().value("event_id").toString();
      cursor = page.value("next_cursor").toString();
      ++pages;
    } while (!cursor.isEmpty() && pages < 10);
    paged.removeDuplicates();
    QCOMPARE(paged.size(), ids.size());
    for (const auto &id : ids) QVERIFY2(paged.contains(id), qPrintable(id));
    QVERIFY(pages > 1);
    query.featureUid = "gone";
    query.cursor.clear();
    query.pageSize = 10;
    const auto deleted = paleo::mapedit::readSpatialEditHistory(lookup, query);
    QCOMPARE(deleted.value("events").toList().size(), 1);
    versions.insert("loop-a", versionWith("loop-a", {eventAt("la", "f", "1")}, "loop-b", true));
    versions.insert("loop-b", versionWith("loop-b", {eventAt("lb", "f", "2")}, "loop-a", true));
    query.versionId = "loop-a";
    query.featureUid.clear();
    const auto loop = paleo::mapedit::readSpatialEditHistory(lookup, query);
    QVERIFY(!loop.value("complete").toBool());
    QVERIFY(loop.value("diagnostics").toStringList().join(" ").contains("循环"));
    versions.insert("orphan", versionWith("orphan", {eventAt("o", "f", "1")}, "missing", true));
    query.versionId = "orphan";
    const auto orphan = paleo::mapedit::readSpatialEditHistory(lookup, query);
    QVERIFY(orphan.value("diagnostics").toStringList().join(" ").contains("缺少父版本"));
    CatalogVersion legacy;
    legacy.id = "legacy";
    legacy.extra.insert("spatial_edit_events", QVariantList{
        QVariantMap{{"timestamp", "t1"}, {"before_uid", "a"}, {"action", "revise_geometry"}},
        QVariantMap{{"timestamp", "t2"}, {"before_uid", "b"}, {"action", "delete_geometry"}}});
    versions.insert("legacy", legacy);
    query.versionId = "legacy";
    const auto unlabeled = paleo::mapedit::readSpatialEditHistory(lookup, query);
    QCOMPARE(unlabeled.value("events").toList().size(), 2);
    QVERIFY(unlabeled.value("diagnostics").toStringList().join(" ").contains("稳定标识") ||
            unlabeled.value("complete").toBool());
    CatalogVersion ambiguous;
    ambiguous.id = "ambiguous";
    ambiguous.extra.insert("spatial_edit_events", QVariantList{
        QVariantMap{{"event_id", "same"}, {"before_geometry", "A"}, {"timestamp", "1"}, {"action", "revise_geometry"}},
        QVariantMap{{"event_id", "same"}, {"before_geometry", "B"}, {"timestamp", "2"}, {"action", "revise_geometry"}}});
    versions.insert("ambiguous", ambiguous);
    query.versionId = "ambiguous";
    const auto clash = paleo::mapedit::readSpatialEditHistory(lookup, query);
    QCOMPARE(clash.value("events").toList().size(), 2);
    QVERIFY(!clash.value("complete").toBool());
    QVERIFY(clash.value("diagnostics").toStringList().join(" ").contains("不一致"));
  }

  void prepareFailureKeepsNativeUndo() {
    Fixture f;
    QVERIFY(f.init());
    const auto path = writePolygons(f.dir.filePath("edit.gpkg"), 1);
    QVERIFY(!path.isEmpty());
    QVERIFY(declare(f, "facies.D61", path));
    QString error;
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate("facies.D61", &error));
    QVERIFY(layer);
    QVERIFY(layer->startEditing());
    QgsFeature added(layer->fields());
    added.setAttribute("facies_code", 2);
    added.setGeometry(QgsGeometry::fromWkt("POLYGON((5 5, 6 5, 6 6, 5 5))"));
    QVERIFY(layer->addFeature(added));
    const int undo = layer->undoStack()->count();
    QVERIFY(undo > 0);
    const auto reject = [&](MapVersionController::SaveFault fault) {
      error.clear();
      f.controller.setSaveFaultForTest(fault);
      const auto saved = f.controller.saveVersion("D61", {{"saved_from", "test"}}, &error);
      QCOMPARE(saved.version, 0);
      QVERIFY(!error.isEmpty());
      QVERIFY(layer->isEditable());
      QCOMPARE(layer->undoStack()->count(), undo);
      QCOMPARE(f.versions.currentVersion("D61"), 0);
      QVERIFY(layer->undoStack()->canUndo());
      f.controller.setSaveFaultForTest(MapVersionController::SaveFault::None);
    };
    reject(MapVersionController::SaveFault::Export);
    reject(MapVersionController::SaveFault::Verify);
    reject(MapVersionController::SaveFault::Sql);
  }

  void durableSaveThenWorkCommitCanResumeWithoutNewVersion() {
    Fixture f;
    QVERIFY(f.init());
    const auto path = writePolygons(f.dir.filePath("partial.gpkg"), 1);
    QVERIFY(declare(f, "facies.D61", path));
    QString error;
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate("facies.D61", &error));
    QVERIFY(layer->startEditing());
    QgsFeature added(layer->fields());
    added.setGeometry(QgsGeometry::fromWkt("POLYGON((8 8, 9 8, 9 9, 8 8))"));
    QVERIFY(layer->addFeature(added));
    const int undo = layer->undoStack()->count();
    f.controller.setSaveFaultForTest(MapVersionController::SaveFault::WorkCommit);
    const auto saved = f.controller.saveVersion("D61", {}, &error);
    QVERIFY2(saved.version == 1, qPrintable(error));
    QVERIFY(error.contains("后续"));
    QCOMPARE(f.controller.lastSaveOutcome().kind, MapVersionController::SaveOutcome::Kind::SavedFollowUpPending);
    QCOMPARE(snapshotFeatures(f.dir.path(), saved), 2);
    QCOMPARE(layer->dataProvider()->featureCount(), qint64(1));
    QVERIFY(layer->isEditable());
    QCOMPARE(layer->undoStack()->count(), undo);
    f.controller.setSaveFaultForTest(MapVersionController::SaveFault::None);
    error.clear();
    const auto retry = f.controller.saveVersion("D61", {}, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(retry.version, 1);
    QCOMPARE(f.versions.versions("D61").size(), 1);
    QVERIFY(!layer->isEditable());
    QCOMPARE(layer->undoStack()->count(), 0);
    QCOMPARE(layer->featureCount(), qint64(2));
  }

  void replacementAndReopenDoNotResumeOldSession() {
    Fixture f;
    QVERIFY(f.init());
    const auto path = writePolygons(f.dir.filePath("session.gpkg"), 1);
    QVERIFY(declare(f, "facies.D61", path));
    QString error;
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate("facies.D61", &error));
    QVERIFY(layer->startEditing());
    QgsFeature added(layer->fields());
    added.setGeometry(QgsGeometry::fromWkt("POLYGON((3 3, 4 3, 4 4, 3 3))"));
    QVERIFY(layer->addFeature(added));
    f.controller.setSaveFaultForTest(MapVersionController::SaveFault::WorkCommit);
    const auto first = f.controller.saveVersion("D61", {}, &error);
    QVERIFY2(first.version == 1, qPrintable(error));
    PaleoProjectStore replacement;
    f.controller.setProjectStore(&replacement);
    f.controller.setSaveFaultForTest(MapVersionController::SaveFault::None);
    error.clear();
    const auto next = f.controller.saveVersion("D61", {}, &error);
    QVERIFY2(next.version == 2, qPrintable(error));
    QCOMPARE(f.versions.versions("D61").size(), 2);
    auto *editing = new QgisEditingService(&f.store, &f.controller);
    f.controller.setEditingService(editing);
    delete editing;
    QVERIFY(f.controller.saveVersion("D61", {}, &error).version == 3);
    f.controller.abandonForProjectClose();
    QVERIFY(f.controller.saveVersion("D61", {}, &error).version == 4);
  }

  void cancelBeforeDurableKeepsUndo() {
    Fixture f;
    QVERIFY(f.init());
    PaleoTaskService tasks;
    tasks.setMaxWorkerThreads(1);
    f.controller.setTaskService(&tasks);
    const auto path = writePolygons(f.dir.filePath("cancel.gpkg"), 1);
    QVERIFY(declare(f, "facies.D61", path));
    QString error;
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate("facies.D61", &error));
    QVERIFY(layer->startEditing());
    QgsFeature added(layer->fields());
    added.setGeometry(QgsGeometry::fromWkt("POLYGON((7 1, 8 1, 8 2, 7 1))"));
    QVERIFY(layer->addFeature(added));
    const int undo = layer->undoStack()->count();
    connect(&f.controller, &MapVersionController::snapshotQueryProgress, &f.controller,
            [&] { f.controller.cancelSnapshot(); });
    QSignalSpy finished(&f.controller, &MapVersionController::snapshotFinished);
    QVERIFY(f.controller.startSaveVersion("D61", {}, &error));
    QTRY_VERIFY_WITH_TIMEOUT(!f.controller.snapshotBusy(), 15000);
    QCOMPARE(finished.size(), 1);
    QVERIFY(!finished.first().at(2).toBool());
    QVERIFY(finished.first().at(3).toBool());
    QCOMPARE(f.versions.currentVersion("D61"), 0);
    QVERIFY(layer->isEditable());
    QCOMPARE(layer->undoStack()->count(), undo);
    QVERIFY(layer->undoStack()->canUndo());
    const int undoIndex = layer->undoStack()->index();
    layer->undoStack()->undo();
    QCOMPARE(layer->undoStack()->index(), undoIndex - 1);
    QVERIFY(layer->undoStack()->canRedo());
    layer->undoStack()->redo();
    QCOMPARE(layer->undoStack()->index(), undoIndex);
    QVERIFY(tasks.shutdown());
  }

  void repeatedSaveReusesUnchangedSource() {
    Fixture f;
    QVERIFY(f.init());
    const auto path = writePolygons(f.dir.filePath("repeat.gpkg"), 10000);
    QVERIFY(!path.isEmpty());
    QVERIFY(declare(f, "facies.D61", path));
    QString error;
    QVERIFY(f.layers.instantiate("facies.D61", &error));
    QElapsedTimer timer;
    timer.start();
    const auto first = f.controller.saveVersion("D61", {{"pass", "first"}}, &error);
    const auto firstMs = timer.elapsed();
    QVERIFY2(first.version == 1, qPrintable(error));
    QCOMPARE(snapshotFeatures(f.dir.path(), first), 10000);
    const auto firstExports = f.controller.lastSaveMetrics().providerExports;
    QVERIFY(firstExports >= 1);
    timer.restart();
    const auto second = f.controller.saveVersion("D61", {{"pass", "repeat"}}, &error);
    const auto secondMs = timer.elapsed();
    QVERIFY2(second.version == 2, qPrintable(error));
    QCOMPARE(snapshotFeatures(f.dir.path(), second), 10000);
    const auto repeatMetrics = f.controller.lastSaveMetrics();
    QCOMPARE(repeatMetrics.providerExports, 0);
    QVERIFY(repeatMetrics.reusedImmutableAssets >= 1);
    QVERIFY(repeatMetrics.sourceHashes >= 1);
    QVERIFY(repeatMetrics.destinationHashes >= 1);
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate("facies.D61", &error));
    QVERIFY(layer->startEditing());
    QgsFeature added(layer->fields());
    added.setGeometry(QgsGeometry::fromWkt("POLYGON((200 200, 201 200, 201 201, 200 200))"));
    QVERIFY(layer->addFeature(added));
    QVERIFY(layer->commitChanges());
    const auto edited = f.controller.saveVersion("D61", {{"pass", "edited"}}, &error);
    QVERIFY2(edited.version == 3, qPrintable(error));
    QVERIFY(f.controller.lastSaveMetrics().providerExports >= 1);
    QCOMPARE(snapshotFeatures(f.dir.path(), edited), 10001);
    qInfo().noquote() << "P6_SCALE" << "first_ms" << firstMs << "repeat_ms" << secondMs
                      << "first_exports" << firstExports
                      << "repeat_reused" << repeatMetrics.reusedImmutableAssets
                      << "repeat_bytes" << repeatMetrics.bytesCopied
                      << "repeat_source_hashes" << repeatMetrics.sourceHashes
                      << "repeat_dest_hashes" << repeatMetrics.destinationHashes
                      << "edited_exports" << f.controller.lastSaveMetrics().providerExports;
  }

  void editingVersionStoresDeltaAndKeepsOldBytes() {
    QTemporaryDir dir;
    DataCatalog catalog;
    QgisProjectService project;
    PaleoProjectStore store;
    LayerManifest manifest{dir.filePath("project.sqlite")};
    QgisLayerService layers{&project, &manifest};
    QgisProcessingService processing{&store};
    ConstraintWorkflow constraints{&processing, &layers};
    MappingWorkbench work{&layers, &processing, &project, &constraints};
    QVERIFY(catalog.open(dir.path()));
    QVERIFY(project.createProject(dir.filePath("project.qgz")));
    QVERIFY(manifest.open());
    store.setProjectPaths(project.projectPath(), dir.filePath("project.gpkg"), dir.filePath("project.sqlite"));
    constraints.setStore(&store);
    constraints.setCatalog(&catalog, dir.path());
    project.setDeclarationProvider([&layers](QVector<LayerDeclaration> *out, QString *error) {
      return layers.tryDeclared(out, error);
    });
    processing.setProject(project.project());
    work.bindCatalog(&catalog, dir.path());
    QString error;
    const auto draft = work.createBlankMap("D61", &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *layer = qobject_cast<QgsVectorLayer *>(layers.instantiate(draft, &error));
    QVERIFY(layer);
    work.styleLayer(draft);
    QVERIFY(MappingFeatureIdentity::Observer::attach(layer, &error));
    const auto schema = work.versionForLayer(draft).extra.value("facies").toList();
    QVERIFY(!schema.isEmpty());
    const int code = schema.first().toMap().value("code").toInt();
    QVERIFY2(work.setEditEvidence("D61", {{"text", "砂厚高值区与井相支持河道展布"},
                                           {"reason", "调整河道边界"}}, &error), qPrintable(error));
    QVERIFY(layer->startEditing());
    const auto outline = QgsGeometry::fromWkt("POLYGON((0 0, 20 0, 20 20, 0 20, 0 0))");
    QVERIFY2(FaciesPartitionEditor::paint(layer, outline, code, &error), qPrintable(error));
    QVERIFY2(layer->commitChanges(), qPrintable(layer->commitErrors().join(';')));
    QVERIFY(layer->startEditing());
    QgsFeature painted;
    QVERIFY(layer->getFeatures().nextFeature(painted));
    layer->selectByIds({painted.id()});
    layer->beginEditCommand(QStringLiteral("move boundary"));
    auto moved = QgsGeometry::fromWkt(QStringLiteral("POLYGON((1 0, 21 0, 21 20, 1 20, 1 0))"));
    QVERIFY(layer->changeGeometry(painted.id(), moved));
    layer->endEditCommand();
    auto *observer = MappingFeatureIdentity::Observer::of(layer);
    QVERIFY(observer);
    QCOMPARE(observer->changes().size(), 1);
    QVERIFY2(layer->commitChanges(), qPrintable(layer->commitErrors().join(';')));
    const auto previous = work.versionForLayer(draft);
    const auto previousBytes = QJsonDocument::fromVariant(previous.extra).toJson(QJsonDocument::Compact);
    QVERIFY2(work.saveEditingVersion(draft, &error), qPrintable(error));
    const auto saved = work.versionForLayer(draft);
    QCOMPARE(saved.extra.value("spatial_edit_events").toList().size(), 1);
    QCOMPARE(saved.extra.value("spatial_edit_history").toMap().value("schema").toString(),
             QString::fromLatin1(paleo::mapedit::kSpatialHistorySchema));
    QCOMPARE(saved.extra.value("spatial_edit_history").toMap().value("parent_version_id").toString(), previous.id);
    QCOMPARE(QJsonDocument::fromVariant(catalog.versionById(previous.id).extra).toJson(QJsonDocument::Compact),
             previousBytes);
    error.clear();
    QVERIFY2(work.saveEditingVersion(draft, &error), qPrintable(error));
    const auto attributeOnly = work.versionForLayer(draft);
    QVERIFY(attributeOnly.extra.value("spatial_edit_events").toList().isEmpty());
    const auto history = work.spatialEditHistory(attributeOnly.id, {}, 50, {});
    QCOMPARE(history.value("events").toList().size(), 1);
    QVERIFY(history.value("complete").toBool());
    MappingWorkbenchPage page("compose", &work);
    page.setHorizon("D61");
    page.selectLayer(draft);
    auto evidence = work.featureEvidence(draft, {});
    QVERIFY(evidence.value("spatial_edit_history").toMap().contains("events"));
    page.setEvidenceSnapshot(evidence);
    auto *historyView = page.findChild<QPlainTextEdit *>("spatialEditHistory");
    QVERIFY(historyView);
    QVERIFY(historyView->toPlainText().contains("图件空间编辑历史"));
    QVERIFY(!page.findChild<QPlainTextEdit *>("faciesEvidenceHistory")->toPlainText().contains("图件空间编辑历史"));
  }
};

int main(int argc, char *argv[]) {
  if (qgetenv("QT_QPA_PLATFORM").isEmpty()) qputenv("QT_QPA_PLATFORM", "offscreen");
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH"), true);
  app.initQgis();
  TestP6MapSave test;
  const int rc = QTest::qExec(&test, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_p6_mapsave.moc"
