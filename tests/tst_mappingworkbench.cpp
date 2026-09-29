#include "../src/domain/faciescatalog.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/mappingartifactwriter.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/ui/pages/mappingworkbenchpage.h"
#include "../src/ui/pages/wellpredictionpanel.h"
#include "../src/workflow/derivedassets.h"
#include "../src/workflow/mappingworkbench.h"
#include "../src/workflow/workflows.h"
#include <QComboBox>
#include <QCryptographicHash>
#include <QFile>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QUndoStack>
#include <QtTest>
#include <gdal.h>
#include <qgsapplication.h>
#include <qgscategorizedsymbolrenderer.h>
#include <qgsgeometry.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsrasterrenderer.h>
#include <qgsvectordataprovider.h>
#include <qgsvectorlayer.h>

class DelayedPredictionService : public RemotePredictionService {
public:
  RemotePredictionRequest request;
  void start(const RemotePredictionRequest &r) override { request = r; }
  void cancel() override {}
  void deliver() {
    RemotePredictionResult result;
    result.request = request;
    result.method = "test";
    result.points = {QVariantMap{
        {"id", "wrong"}, {"x", 100}, {"y", 100}, {"facies_code", 1}}};
    emit completed(result);
  }
};

class TestMappingWorkbench : public QObject {
  Q_OBJECT
  struct Fixture {
    QTemporaryDir dir;
    DataCatalog catalog;
    QgisProjectService project;
    PaleoProjectStore store;
    LayerManifest manifest{dir.filePath("project.sqlite")};
    QgisLayerService layers{&project, &manifest};
    QgisProcessingService processing{&store};
    ConstraintWorkflow constraints{&processing, &layers};
    MappingWorkbench work{&layers, &processing, &project, &constraints};
    bool init() {
      if (!catalog.open(dir.path()) ||
          !project.createProject(dir.filePath("project.qgz")) ||
          !manifest.open())
        return false;
      store.setProjectPaths(project.projectPath(), dir.filePath("project.gpkg"),
                            dir.filePath("project.sqlite"));
      constraints.setStore(&store);
      constraints.setCatalog(&catalog, dir.path());
      project.setDeclarationProvider(
          [this](QVector<LayerDeclaration> *out, QString *error) {
            return layers.tryDeclared(out, error);
          });
      processing.setProject(project.project());
      work.bindCatalog(&catalog, dir.path());
      return true;
    }
    QString input(const QString &name, const QString &type) {
      DerivedAssetRegistrar r(&catalog, dir.path());
      auto st = r.stage(type, name, name + ".dat");
      QFile f(st.absolutePath);
      if (!f.open(QIODevice::WriteOnly))
        return {};
      if (type == "well_log")
        f.write("~V\nVERS. 2.0 : version\nWRAP. NO : wrap\n~W\nNULL. -999.25 : "
                "null\n~C\nDEPT.M : depth\nGR.API : gamma\n~A\n1000 30\n1030 "
                "50\n1060 90\n1090 45\n1120 20\n");
      else
        f.write("mock input bytes");
      f.close();
      if (!r.commit(st, {}, "fixture", {}))
        return {};
      return st.assetId;
    }
    QString well(const QString &name, double x, double y) {
      auto asset = input(name, "well_log");
      CatalogEntity e;
      e.id = name;
      e.name = name;
      e.entityType = "well";
      e.hasSurface = true;
      e.surfaceX = x;
      e.surfaceY = y;
      e.coordinateStatus = "untransformed";
      if (!catalog.addEntity(e))
        return {};
      EntityAssetLink l;
      l.entityType = "well";
      l.entityId = e.id;
      l.assetId = asset;
      l.role = "well_log";
      if (!catalog.addLink(l))
        return {};
      return e.id;
    }
    QString seismic() {
      auto asset = input("seismic", "seismic");
      CatalogEntity e;
      e.id = "survey";
      e.name = "survey";
      e.entityType = "seismic_survey";
      e.corners = {{0, 0}, {640, 0}, {640, 640}, {0, 640}};
      if (!catalog.addEntity(e))
        return {};
      EntityAssetLink l;
      l.entityId = e.id;
      l.entityType = e.entityType;
      l.assetId = asset;
      l.role = "seismic_volume";
      if (!catalog.addLink(l))
        return {};
      return asset;
    }
    QString latest(const QString &kind) {
      QString found;
      int ver = 0;
      for (const auto &row : work.products("D61")) {
        auto r = row.toMap();
        if (r.value("kind") == kind && r.value("version").toInt() > ver) {
          found = r.value("id").toString();
          ver = r.value("version").toInt();
        }
      }
      return found;
    }
  };
  static QByteArray digest(const QString &path) {
    QFile f(path.section('|', 0, 0));
    if (!f.open(QIODevice::ReadOnly))
      return {};
    return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256);
  }
private slots:
  void textureLibraryAndUnknownCategories() {
    const auto library = FaciesCatalog::library();
    QVERIFY(library.size() > 100);
    auto schema = FaciesCatalog::defaults();
    auto hierarchical = schema[0].toMap();
    hierarchical.insert("facies", "三角洲相");
    hierarchical.insert("subfacies", "三角洲前缘");
    hierarchical.insert("microfacies", "水下分流河道");
    schema[0] = hierarchical;
    QCOMPARE(schema.size(), 3);
    for (const auto &v : schema) {
      auto f = v.toMap();
      QVERIFY(!f.value("name").toString().contains("Mock"));
      QVERIFY(QFile::exists(
          FaciesCatalog::resourcePath(f.value("texture").toString())));
    }
    QgsVectorLayer legacy("Point?field=facies_code:integer&crs=EPSG:4326",
                          "legacy", "memory");
    legacy.setReadOnly(true);
    MappingArtifactWriter::applyFaciesStyle(&legacy, schema);
    QVERIFY(legacy.fields().indexOf("facies_name") >= 0);
    QVERIFY(!legacy.isEditable());
    QgsVectorLayer layer("Point?field=facies_code:integer&crs=EPSG:4326",
                         "unknown", "memory");
    QgsFeature known(layer.fields()), unknown(layer.fields()),
        empty(layer.fields());
    known.setAttributes(QgsAttributes(1, QVariant(1)));
    unknown.setAttributes(QgsAttributes(1, QVariant(77)));
    empty.setAttributes(QgsAttributes(1, QVariant()));
    for (auto *f : {&known, &unknown, &empty})
      f->setGeometry(QgsGeometry::fromWkt("POINT(1 1)"));
    QgsFeatureList features{known, unknown, empty};
    QVERIFY(layer.dataProvider()->addFeatures(features));
    QString error;
    QVERIFY2(
        MappingArtifactWriter::syncFaciesAttributes(&layer, schema, &error),
        qPrintable(error));
    MappingArtifactWriter::applyFaciesStyle(&layer, schema);
    auto *renderer =
        dynamic_cast<QgsCategorizedSymbolRenderer *>(layer.renderer());
    QVERIFY(renderer);
    QVERIFY(renderer->categories().last().value().isNull());
    QVERIFY(renderer->categories().last().symbol());
    QgsRenderContext context;
    renderer->startRender(context, layer.fields());
    auto it = layer.getFeatures();
    QgsFeature f;
    int visible = 0;
    while (it.nextFeature(f)) {
      QVERIFY(renderer->symbolForFeature(f, context));
      ++visible;
      QVERIFY(!f.attribute("facies_name").toString().isEmpty());
      if (f.attribute("facies_code").toInt() == 1) {
        QCOMPARE(f.attribute("facies_name").toString(), QString("三角洲相"));
        QCOMPARE(f.attribute("subfacies").toString(), QString("三角洲前缘"));
        QCOMPARE(f.attribute("microfacies").toString(),
                 QString("水下分流河道"));
      }
    }
    renderer->stopRender(context);
    QCOMPARE(visible, 3);
  }
  void wellIntervalsReviewUndoAndPersistence() {
    Fixture f;
    QVERIFY(f.init());
    QVERIFY(!f.well("A", 10, 20).isEmpty());
    QVERIFY(!f.well("B", 30, 40).isEmpty());
    QString error;
    QVERIFY(f.work.predict("D61", "wells", {"A", "B"}, &error));
    QTRY_VERIFY(!f.work.busy());
    const auto original = f.latest("wells_prediction");
    QVERIFY(!original.isEmpty());
    auto before = f.work.wellPredictions(original);
    QCOMPARE(before.size(), 2);
    QCOMPARE(before[0].toMap().value("intervals").toList().size(), 12);
    const auto originalHash = digest(f.work.declaration(original).source);
    const auto draft = f.work.copyForEditing(original, {}, &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft));
    QVERIFY(layer);
    auto rows = f.work.wellPredictions(draft);
    const auto well = rows[0].toMap();
    const auto wellId = well.value("id").toString();
    QVERIFY(f.work.predictionLog(well.value("log_version_id").toString()).ok);
    QVERIFY(!well.value("depth_mock").toBool());
    const auto intervals = well.value("intervals").toList();
    const int oldCode = intervals[0].toMap().value("code").toInt();
    const int code = oldCode == 1 ? 2 : 1;
    const auto version = f.work.versionForLayer(draft);
    QVERIFY(!f.work.reviseWellInterval(original, wellId, 0, code, &error));
    QVERIFY(!f.work.reviseWellInterval(draft, wellId, 999, code, &error));
    QVERIFY2(f.work.reviseWellInterval(draft, wellId, 0, code, &error),
             qPrintable(error));
    auto revised = f.work.wellPredictions(draft)[0].toMap();
    QCOMPARE(
        revised.value("intervals").toList()[0].toMap().value("code").toInt(),
        code);
    QCOMPARE(revised.value("predicted"), well.value("predicted"));
    layer->undoStack()->undo();
    QCOMPARE(
        f.work.wellPredictions(draft)[0].toMap().value("intervals").toList(),
        intervals);
    for (int i = 0; i < 12; ++i)
      QVERIFY(f.work.reviseWellInterval(draft, wellId, i, code, &error));
    auto feature = layer->getFeature(well.value("feature_id").toLongLong());
    QCOMPARE(feature.attribute("facies_code").toInt(), code);
    QCOMPARE(feature.attribute("facies_name").toString(),
             FaciesCatalog::find(f.work.facies("D61"), code)
                 .value("facies")
                 .toString());
    QVERIFY(layer->commitChanges());
    QVERIFY2(f.work.saveEditingVersion(draft, &error), qPrintable(error));
    auto saved = f.work.versionForLayer(draft);
    QCOMPARE(saved.versionNumber, version.versionNumber + 1);
    QVERIFY(saved.parentVersionIds.contains(version.id));
    QCOMPARE(digest(f.work.declaration(original).source), originalHash);
    f.work.bindCatalog(&f.catalog, f.dir.path());
    QCOMPARE(
        f.work.wellPredictions(draft)[0].toMap().value("facies_code").toInt(),
        code);
    QVERIFY2(f.work.assignFacies(draft, {well.value("feature_id").toLongLong()},
                                 oldCode, &error),
             qPrintable(error));
    for (const auto &v :
         f.work.wellPredictions(draft)[0].toMap().value("intervals").toList())
      QCOMPARE(v.toMap().value("code").toInt(), oldCode);
    WellPredictionPanel panel;
    panel.setResult(draft, f.work.wellPredictions(draft), f.work.facies("D61"));
    auto *table = panel.findChild<QTableWidget *>("predictionIntervals");
    QVERIFY(table);
    QCOMPARE(table->rowCount(), 12);
    auto *choice = panel.findChild<QComboBox *>("wellFaciesChoice");
    QVERIFY(choice);
    choice->setCurrentIndex(choice->findData(code));
    QSignalSpy revise(&panel, &WellPredictionPanel::reviseRequested);
    panel.findChild<QPushButton *>("applyWellFacies")->click();
    QCOMPARE(revise.count(), 1);
    QCOMPARE(revise[0][0].toString(), wellId);
    QCOMPARE(revise[0][1].toInt(), 0);
    QCOMPARE(revise[0][2].toInt(), code);
    panel.setLog(f.work.predictionLog(well.value("log_version_id").toString()));
    panel.resize(1250, 520);
    panel.show();
    QTest::qWait(100);
    if (const auto path = qEnvironmentVariable("PALEO_PREDICTION_CAPTURE");
        !path.isEmpty())
      QVERIFY(panel.grab().save(path));
    panel.clear();
    QCOMPARE(table->rowCount(), 0);
    QVERIFY(!panel.findChild<QPushButton *>("applyWellFacies")->isEnabled());
  }
  void seismicToPolygonToEditableVersions() {
    Fixture f;
    QVERIFY(f.init());
    const auto input = f.seismic();
    QVERIFY(!input.isEmpty());
    QString error;
    QSignalSpy ready(&f.work, &MappingWorkbench::productReady),
        errors(&f.work, &MappingWorkbench::errorOccurred);
    QVERIFY2(f.work.predict("D61", "seismic", {input}, &error),
             qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(!f.work.busy(), 5000);
    QCOMPARE(errors.count(), 0);
    const auto rasterId = f.latest("seismic_prediction");
    QVERIFY(!rasterId.isEmpty());
    auto v = f.work.versionForLayer(rasterId);
    QVERIFY(v.parentVersionIds.contains(f.catalog.currentVersion(input).id));
    QVERIFY(v.extra.value("mock").toBool());
    QCOMPARE(v.extra.value("horizon").toString(), QString("D61"));
    auto *r =
        qobject_cast<QgsRasterLayer *>(f.layers.instantiate(rasterId, &error));
    QVERIFY2(r, qPrintable(error));
    QCOMPARE(r->width(), 64);
    QCOMPARE(r->height(), 64);
    QVERIFY(r->extent().width() > 639);
    QCOMPARE(r->crs(), QgsCoordinateReferenceSystem::fromWkt(
                           DataCatalog::localGridCrsWkt()));
    const auto polygon = f.work.polygonize(rasterId, &error);
    QVERIFY2(!polygon.isEmpty(), qPrintable(error));
    auto *original =
        qobject_cast<QgsVectorLayer *>(f.layers.instantiate(polygon, &error));
    QVERIFY(original);
    QVERIFY(original->featureCount() > 0);
    for (const auto &name :
         {"facies_name", "subfacies", "microfacies", "texture"})
      QVERIFY(original->fields().indexOf(name) >= 0);
    QVERIFY(original->readOnly());
    const auto hash = digest(original->source());
    const auto draft = f.work.copyForEditing(polygon, {}, &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *edit =
        qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft, &error));
    QVERIFY(edit);
    QVERIFY(!edit->readOnly());
    const auto previous = f.work.versionForLayer(draft);
    QVERIFY(
        previous.parentVersionIds.contains(f.work.versionForLayer(polygon).id));
    QVERIFY(edit->startEditing());
    auto it = edit->getFeatures();
    QgsFeature feature;
    QVERIFY(it.nextFeature(feature));
    const int field = edit->fields().indexOf("facies_code");
    QVERIFY(field >= 0);
    int code = feature.attribute(field).toInt() == 1 ? 2 : 1;
    QVERIFY(edit->changeAttributeValue(feature.id(), field, code));
    QVERIFY(edit->commitChanges());
    QVERIFY2(f.work.saveEditingVersion(draft, &error), qPrintable(error));
    const auto next = f.work.versionForLayer(draft);
    QCOMPARE(next.versionNumber, previous.versionNumber + 1);
    QVERIFY(next.parentVersionIds.contains(previous.id));
    QCOMPARE(digest(original->source()), hash);
    QVERIFY(next.sha256 != previous.sha256);
    QgsVectorLayer snapshot(
        DataCatalog::resolvedVersionPath(f.dir.path(), next) +
            "|layername=features",
        "check", "ogr");
    QVERIFY(snapshot.isValid());
    QgsFeature saved;
    auto si = snapshot.getFeatures();
    QVERIFY(si.nextFeature(saved));
    QCOMPARE(saved.attribute("facies_code").toInt(), code);
    f.work.bindCatalog(&f.catalog, f.dir.path());
    QVERIFY(!f.work.declaration(draft).source.isEmpty());
    QVERIFY(!f.work.declaration("product." + next.id).source.isEmpty());
  }
  void wellsSingleBatchCancelAndHorizonSchemas() {
    Fixture f;
    QVERIFY(f.init());
    QVERIFY(!f.well("A", 100, 100).isEmpty());
    QVERIFY(!f.well("B", 500, 500).isEmpty());
    QString error;
    QVariantList facies{
        QVariantMap{{"code", 7}, {"name", "河道"}, {"color", "#E6C875"}},
        QVariantMap{{"code", 9}, {"name", "湖相"}, {"color", "#97B4CE"}}};
    QVERIFY(f.work.saveFacies("D61", facies, &error));
    QVERIFY(f.work.facies("D62") != facies);
    QVERIFY(f.work.predict("D61", "wells", {"A"}, &error));
    QTRY_VERIFY(!f.work.busy());
    const auto first = f.latest("wells_prediction");
    QVERIFY(!first.isEmpty());
    auto *single =
        qobject_cast<QgsVectorLayer *>(f.layers.instantiate(first, &error));
    QVERIFY(single);
    QCOMPARE(single->featureCount(), 1);
    QCOMPARE(f.work.versionForLayer(first).extra.value("facies").toList(),
             facies);
    QVERIFY(f.work.predict("D61", "wells", {"A", "B"}, &error));
    QTRY_VERIFY(!f.work.busy());
    const auto second = f.latest("wells_prediction");
    QVERIFY(first != second);
    auto *batch =
        qobject_cast<QgsVectorLayer *>(f.layers.instantiate(second, &error));
    QVERIFY(batch);
    QCOMPARE(batch->featureCount(), 2);
    const auto result = f.work.compose("D61", {second}, {}, &error);
    QVERIFY2(!result.isEmpty(), qPrintable(error));
    QVERIFY(f.work.versionForLayer(result).extra.value("mock").toBool());
    const int before = f.work.products("D61").size();
    QVERIFY(f.work.predict("D61", "wells", {"A"}, &error));
    f.work.cancelPrediction();
    QTest::qWait(350);
    QCOMPARE(f.work.products("D61").size(), before);
    QVERIFY(f.work.predict("D61", "wells", {"A"}, &error));
    f.work.bindCatalog(nullptr, {});
    QTest::qWait(350);
    QVERIFY(!f.work.busy());
    f.work.bindCatalog(&f.catalog, f.dir.path());
    QCOMPARE(f.work.facies("D61"), facies);
    QVERIFY(!f.work.predict("D61", "wells", {"missing"}, &error));
    QVERIFY(!error.isEmpty());
  }
  void constraintsFactorsContoursAndClassification() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto source = f.dir.filePath("samples.geojson");
    QVariantList points{QVariantMap{{"x", 0}, {"y", 0}, {"z", 0}},
                        QVariantMap{{"x", 100}, {"y", 0}, {"z", 20}},
                        QVariantMap{{"x", 0}, {"y", 100}, {"z", 40}},
                        QVariantMap{{"x", 100}, {"y", 100}, {"z", 60}}};
    QVERIFY(MappingArtifactWriter::points(source, points, &error));
    LayerDeclaration d;
    d.layerId = "wells.D61";
    d.horizon = "D61";
    d.type = "vector";
    d.source = source;
    d.title = "样点";
    QVERIFY(f.layers.declare(d, &error));
    QVERIFY2(f.constraints.addConstraint("D61", "LINESTRING(50 0,50 100)",
                                         "line", -1, &error),
             qPrintable(error));
    QVERIFY2(!f.latest("constraint_snapshot").isEmpty(), qPrintable(error));
    auto params = QVariantMap{
        {"pointsLayerId", d.layerId}, {"field", "z"}, {"cellSize", 10}};
    QVERIFY2(f.work.generateFactor("D61", "sandthick", params, &error),
             qPrintable(error));
    const auto factorId = f.latest("single_factor_raster");
    QVERIFY(!factorId.isEmpty());
    const auto factor = f.work.versionForLayer(factorId);
    QCOMPARE(
        f.layers.instantiate(factorId)->crs(),
        QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt()));
    QVERIFY(factor.parentVersionIds.size() >= 2);
    QVERIFY(qobject_cast<QgsRasterLayer *>(f.layers.instantiate(factorId))
                ->renderer()
                ->legendSymbologyItems()
                .size() >= 2);
    bool snapshotParent = false, inputParent = false;
    for (const auto &id : factor.parentVersionIds) {
      auto p = f.catalog.versionById(id);
      snapshotParent |= p.extra.value("kind") == "constraint_snapshot";
      inputParent |= p.extra.value("kind") == "input_snapshot";
    }
    QVERIFY(snapshotParent);
    QVERIFY(inputParent);
    QVERIFY(f.work.polygonize(factorId, &error).isEmpty());
    QVERIFY(!error.isEmpty());
    error.clear();
    QVERIFY2(f.work.generateContours("D61", factorId, 10, &error),
             qPrintable(error));
    const auto contours = f.latest("contour_lines");
    QVERIFY(!contours.isEmpty());
    QCOMPARE(
        f.layers.instantiate(contours)->crs(),
        QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt()));
    QVERIFY(
        f.work.versionForLayer(contours).parentVersionIds.contains(factor.id));
    QVERIFY(f.work.compose("D61", {factorId}, {}, &error).isEmpty());
    error.clear();
    const auto composed = f.work.compose(
        "D61", {factorId}, {{"thresholds", QVariantList{20., 40.}}}, &error);
    QVERIFY2(!composed.isEmpty(), qPrintable(error));
    auto *old = f.layers.instantiate("factor.D61.sandthick");
    QVERIFY(old);
    const auto oldPath = old->source();
    params.insert("cellSize", 20);
    QVERIFY2(f.work.generateFactor("D61", "sandthick", params, &error),
             qPrintable(error));
    QVERIFY(f.layers.instantiate("factor.D61.sandthick")->source() != oldPath);
    QVERIFY(QFileInfo::exists(oldPath));
    const auto constraint = f.latest("constraint_snapshot");
    const auto importPath = f.work.declaration(constraint).source;
    QVERIFY2(f.work.importConstraints("D62", importPath, &error),
             qPrintable(error));
    QCOMPARE(f.constraints.loadConstraints("D62").size(), 1);
  }
  void rejectsBadAndLateRemoteResponses() {
    Fixture f;
    QVERIFY(f.init());
    QVERIFY(!f.well("A", 100, 100).isEmpty());
    DelayedPredictionService service;
    f.work.setPredictionService(&service);
    QString error;
    QSignalSpy failures(&f.work, &MappingWorkbench::errorOccurred);
    QVERIFY(f.work.predict("D61", "wells", {"A"}, &error));
    service.deliver();
    QCOMPARE(failures.count(), 1);
    QVERIFY(!f.work.busy());
    QVERIFY(f.latest("wells_prediction").isEmpty());
    QVERIFY(f.work.predict("D61", "wells", {"A"}, &error));
    f.work.cancelPrediction();
    service.deliver();
    QVERIFY(f.latest("wells_prediction").isEmpty());
    QCOMPARE(failures.count(), 1);
  }
  void panelSelectionAndBusyFollowActualState() {
    Fixture f;
    QVERIFY(f.init());
    QVERIFY(!f.well("A", 100, 100).isEmpty());
    MappingWorkbenchPage page("predict", &f.work);
    page.setHorizon("D61");
    auto *kind = page.findChild<QComboBox *>("predictionKind");
    QVERIFY(kind);
    kind->setCurrentIndex(1);
    auto *list = page.findChild<QListWidget *>("workbenchInputs");
    QCOMPARE(list->count(), 1);
    QVERIFY(!page.commandButton("predict")->isEnabled());
    list->item(0)->setCheckState(Qt::Checked);
    QVERIFY(page.commandButton("predict")->isEnabled());
    QSignalSpy intent(&page, &MappingWorkbenchPage::commandRequested);
    page.commandButton("predict")->click();
    QCOMPARE(intent.count(), 1);
    QCOMPARE(intent[0][1].toMap().value("inputs").toStringList(),
             QStringList{"A"});
    QString error;
    QVERIFY(f.work.predict("D61", "wells", {"A"}, &error));
    QVERIFY(!page.commandButton("predict")->isEnabled());
    QVERIFY(page.commandButton("cancel")->isEnabled());
    page.setHorizon("D62");
    QTRY_VERIFY(!f.work.busy());
    QCOMPARE(page.horizon(), QString("D62"));
    QCOMPARE(
        page.findChild<QTreeWidget *>("workbenchResults")->topLevelItemCount(),
        0);
    page.setHorizon("D61");
    QVERIFY(
        page.findChild<QTreeWidget *>("workbenchResults")->topLevelItemCount() >
        0);
  }
};
int main(int argc, char **argv) {
  QgsApplication app(argc, argv, true);
  app.setPrefixPath("/usr", true);
  app.initQgis();
  GDALAllRegister();
  TestMappingWorkbench test;
  const int result = QTest::qExec(&test, argc, argv);
  QgsApplication::exitQgis();
  return result;
}
#include "tst_mappingworkbench.moc"
