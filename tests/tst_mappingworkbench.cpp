#include "../src/domain/faciescatalog.h"
#include "../src/domain/facieshierarchy.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/facieshierarchyrenderer.h"
#include "../src/qgis/mapcanvaslink.h"
#include "../src/qgis/mappingartifactwriter.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/ui/edittools/editingtoolbar.h"
#include "../src/ui/edittools/vertexeditortools.h"
#include "../src/ui/pages/mappingworkbenchpage.h"
#include "../src/ui/pages/wellpredictionpanel.h"
#include "../src/ui/paleotheme.h"
#include "../src/ui/wellcomposite/wellcompositecanvas.h"
#include "../src/workflow/derivedassets.h"
#include "../src/workflow/mappingworkbench.h"
#include "../src/workflow/workflows.h"
#include <QAction>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QGraphicsScene>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
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
#include <qgsexpressioncontextutils.h>
#include <qgsgeometry.h>
#include <qgsmapcanvas.h>
#include <qgsmergedfeaturerenderer.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsrasterrenderer.h>
#include <qgsrubberband.h>
#include <qgsvectordataprovider.h>
#include <qgsvectorlayer.h>
#include <qgsvectorlayerlabeling.h>

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
  static QVariantList hierarchySchema() {
    QVariantList schema;
    const QStringList parents{"河流", "河流", "三角洲", "三角洲", "湖泊"};
    const QStringList children{"河道", "河道", "河道", "河道", ""};
    const QStringList leaves{"主槽", "边滩", "主槽", "边滩", ""};
    for (int i = 0; i < 5; ++i)
      schema << QVariantMap{{"code", i + 1},
                            {"name", parents[i] + leaves[i]},
                            {"facies", parents[i]},
                            {"subfacies", children[i]},
                            {"microfacies", leaves[i]},
                            {"color", i < 2 ? "#97B4CE" : "#F2D28B"}};
    return schema;
  }
  static QString hierarchyDraft(Fixture &f, QString *error) {
    if (!f.work.saveFacies("D61", hierarchySchema(), error))
      return {};
    QgsVectorLayer grid("Polygon?field=facies_code:integer", "grid", "memory");
    grid.setCrs(
        QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt()));
    QgsFeatureList features;
    for (int i = 0; i < 5; ++i) {
      QgsFeature feature(grid.fields());
      feature.setAttribute("facies_code", i + 1);
      feature.setGeometry(
          QgsGeometry::fromRect(QgsRectangle(i * 100, 0, (i + 1) * 100, 100)));
      features << feature;
    }
    if (!grid.dataProvider()->addFeatures(features))
      return {};
    const auto path = f.dir.filePath("input-grid.gpkg");
    if (!MappingArtifactWriter::vectorSnapshot(&grid, path, error))
      return {};
    LayerDeclaration d;
    d.layerId = "input.grid";
    d.horizon = "D61";
    d.type = "vector";
    d.group = "05_Composition";
    d.title = "hierarchical grid";
    d.source = path + "|layername=features";
    f.layers.declare(d);
    return f.work.copyForEditing(d.layerId, {}, error);
  }
private slots:
  void advancedSingleFactorsReachMappingWithoutReopeningProject() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto source = f.dir.filePath("advanced-samples.geojson");
    QVERIFY(MappingArtifactWriter::points(
        source,
        {QVariantMap{{"x", 0}, {"y", 0}, {"z", 10}},
         QVariantMap{{"x", 100}, {"y", 0}, {"z", 20}},
         QVariantMap{{"x", 0}, {"y", 100}, {"z", 40}},
         QVariantMap{{"x", 100}, {"y", 100}, {"z", 60}}},
        &error));
    LayerDeclaration samples;
    samples.layerId = "wells.D61";
    samples.horizon = "D61";
    samples.type = "vector";
    samples.source = source;
    samples.title = "高级工具样点";
    QVERIFY(f.layers.declare(samples, &error));
    MappingWorkbenchPage page("compose", &f.work);
    page.setHorizon("D61");
    auto *inputs = page.findChild<QListWidget *>("workbenchInputs");
    const QVariantMap options{
        {"pointsLayerId", samples.layerId}, {"field", "z"}, {"cellSize", 10}};
    // Use exactly the advanced-tools entry, bypassing MappingWorkbench.
    QVERIFY2(f.constraints.generateFactor("D61", "sandthick", options, &error),
             qPrintable(error));
    const auto first = f.work.versionForLayer("factor.D61.sandthick");
    QVERIFY(!first.id.isEmpty());
    const auto firstLayer = "product." + first.id;
    QTRY_VERIFY(!f.work.declaration(firstLayer).layerId.isEmpty());
    int checked = -1;
    for (int i = 0; i < inputs->count(); ++i)
      if (inputs->item(i)->data(Qt::UserRole).toString() == firstLayer)
        checked = i;
    QVERIFY(checked >= 0);
    inputs->item(checked)->setCheckState(Qt::Checked);
    const auto originalPath = f.work.declaration(firstLayer).source;
    const auto hash = digest(originalPath);
    QVERIFY2(f.constraints.generateFactor("D61", "sandthick", options, &error),
             qPrintable(error));
    const auto second = f.work.versionForLayer("factor.D61.sandthick");
    QVERIFY(second.id != first.id);
    QTRY_VERIFY(!f.work.declaration("product." + second.id).layerId.isEmpty());
    QCOMPARE(f.work.versionForLayer(firstLayer).id, first.id);
    QCOMPARE(f.work.declaration(firstLayer).source, originalPath);
    QCOMPARE(digest(originalPath), hash);
    int factors = 0;
    bool stillChecked = false;
    for (const auto &entry : f.work.products("D61")) {
      const auto row = entry.toMap();
      if (row.value("kind") != "single_factor_raster")
        continue;
      ++factors;
      QCOMPARE(row.value("asset_id").toString(), first.assetId);
      QVERIFY(row.value("version_id") == first.id ||
              row.value("version_id") == second.id);
    }
    QCOMPARE(factors, 2);
    for (int i = 0; i < inputs->count(); ++i)
      if (inputs->item(i)->data(Qt::UserRole).toString() == firstLayer)
        stillChecked = inputs->item(i)->checkState() == Qt::Checked;
    QVERIFY(stillChecked);
    QCOMPARE(f.work.layerForVersion(first.id, &error), firstLayer);
    auto *rendered = f.layers.instantiate(firstLayer, &error);
    QVERIFY(rendered);
    QCOMPARE(rendered->customProperty("paleoAssetId").toString(),
             first.assetId);
    QCOMPARE(rendered->customProperty("paleoVersionId").toString(), first.id);
    page.selectLayer(firstLayer);
    QVERIFY(page.commandButton("catalog")->isEnabled());
    QSignalSpy intent(&page, &MappingWorkbenchPage::commandRequested);
    page.commandButton("catalog")->click();
    QCOMPARE(intent.last()[0].toString(), QString("catalog"));
    // Old factor versions remain usable as frozen evidence sources.
    const auto draft = hierarchyDraft(f, &error);
    QVERIFY(!draft.isEmpty());
    auto *v = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft));
    auto features = v->getFeatures();
    QgsFeature feature;
    QVERIFY(features.nextFeature(feature));
    QVERIFY(f.work.addEvidence(draft, {feature.id()},
                               {{"text", "依据重算前的砂厚图"},
                                {"level", "facies"},
                                {"source_layer", firstLayer}},
                               &error));
    QCOMPARE(f.work.evidence(draft, {feature.id()})[0]
                 .toMap()
                 .value("source_version")
                 .toString(),
             first.id);
    QVERIFY(v->rollBack());
  }
  void movingCatalogAliasesDoNotReplaceHistoricalReferences() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    DerivedAssetRegistrar registrar(&f.catalog, f.dir.path());
    const QVariantMap extra{{"mapping_product", true},
                            {"layer_id", "cartographic.D61.sandthick"},
                            {"manifest_layer_id", "cartographic.D61.sandthick"},
                            {"horizon", "D61"},
                            {"layer_type", "raster"},
                            {"group", "04_SingleFactor/Cartographic"},
                            {"title", "砂厚制图工作场"},
                            {"kind", "single_factor_cartographic_work"},
                            {"value_source", "cartographic_work"}};
    QString first, second;
    for (int i = 0; i < 2; ++i) {
      const auto staged = registrar.stage("single_factor_cartographic_work",
                                          "砂厚制图工作场", "work.tif", &error);
      QVERIFY(staged.isValid());
      QVERIFY(MappingArtifactWriter::raster(staged.absolutePath, {1, 1, 2, 2},
                                            2, 2, QRectF(0, 0, 100, 100),
                                            &error));
      QVERIFY(registrar.commit(staged, {}, "fixture", extra, &error));
      (i == 0 ? first : second) = staged.versionId;
    }
    QTRY_VERIFY(!f.work.declaration("product." + second).layerId.isEmpty());
    QCOMPARE(f.work.versionForLayer("product." + first).id, first);
    QCOMPARE(f.work.versionForLayer("product." + second).id, second);
    QCOMPARE(f.work.layerForVersion(first, &error), "product." + first);
    QVERIFY(f.work.declaration("product." + first).source !=
            f.work.declaration("product." + second).source);
    QVERIFY(f.work.products("D62").isEmpty());
    QCOMPARE(f.work.products("D61").size(), 2);
    QVERIFY(f.work.compose("D61", {"product." + first}, {}, &error).isEmpty());
    QVERIFY(
        !error
             .isEmpty()); // display-only work cannot enter quantitative fusion.
    f.work.bindCatalog(&f.catalog, f.dir.path());
    QCOMPARE(f.work.versionForLayer("product." + first).id, first);
    QCOMPARE(f.work.products("D61").size(), 2);
    QVERIFY(f.work.layerForVersion("missing", &error).isEmpty());
  }
  void hierarchyScaleFallbackAndScopedNames() {
    QCOMPARE(FaciesHierarchy::resolveLevel("auto", 8000001), QString("facies"));
    QCOMPARE(FaciesHierarchy::resolveLevel("auto", 8000000), QString("sub_facies"));
    QCOMPARE(FaciesHierarchy::resolveLevel("auto", 7999999),
             QString("sub_facies"));
    QCOMPARE(FaciesHierarchy::resolveLevel("auto", 4000001),
             QString("sub_facies"));
    QCOMPARE(FaciesHierarchy::resolveLevel("auto", 4000000),
             QString("micro_facies"));
    QCOMPARE(FaciesHierarchy::resolveLevel("auto", 3999999),
             QString("micro_facies"));
    QCOMPARE(FaciesHierarchy::resolveLevel("facies", 1), QString("facies"));
    QCOMPARE(FaciesHierarchy::path({{"microfacies", "仅微相"}}),
             QStringList({"仅微相", "仅微相", "仅微相"}));
    QCOMPARE(FaciesHierarchy::path({{"facies", "相"}, {"microfacies", "微相"}}),
             QStringList({"相", "相", "微相"}));
    auto schema = hierarchySchema();
    QCOMPARE(FaciesHierarchy::legend(schema, "facies", {1, 2, 3, 4, 5}).size(),
             3);
    QCOMPARE(
        FaciesHierarchy::legend(schema, "sub_facies", {1, 2, 3, 4, 5}).size(),
        3);
    QCOMPARE(
        FaciesHierarchy::legend(schema, "micro_facies", {1, 2, 3, 4, 5}).size(),
        5);
    QVERIFY(FaciesHierarchy::key(schema[0].toMap(), "sub_facies") !=
            FaciesHierarchy::key(schema[2].toMap(), "sub_facies"));
    QCOMPARE(FaciesHierarchy::legend(schema, "facies", {1, 2}).size(), 1);
    QCOMPARE(
        FaciesHierarchy::legend(schema, "facies", {1, 77, QVariant()}).size(),
        2);
  }
  void hierarchyRenderingMergesAndUpdatesLabelsWithoutChangingGeometry() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto draft = hierarchyDraft(f, &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *v = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft));
    QVERIFY(v);
    auto it = v->getFeatures();
    QgsFeature a, b;
    QVERIFY(it.nextFeature(a));
    QVERIFY(it.nextFeature(b));
    const auto geometry = a.geometry().asWkt();
    QVERIFY(f.work.setDisplayMode(draft, "facies", &error));
    auto *merged = dynamic_cast<QgsMergedFeatureRenderer *>(v->renderer());
    QVERIFY(merged);
    auto *categorized = dynamic_cast<const QgsCategorizedSymbolRenderer *>(
        merged->embeddedRenderer());
    QVERIFY(categorized);
    QCOMPARE(categorized->categories().size(), 3);
    auto context = v->createExpressionContext();
    context.setFeature(a);
    QgsExpression expr(categorized->classAttribute());
    QVERIFY(expr.prepare(&context));
    const auto keyA = expr.evaluate(&context);
    context.setFeature(b);
    QCOMPARE(expr.evaluate(&context), keyA);
    QVERIFY(f.work.setDisplayMode(draft, "micro_facies", &error));
    QCOMPARE(f.work.displayLegend(draft).size(), 5);
    QCOMPARE(v->getFeature(a.id()).geometry().asWkt(), geometry);
    QCOMPARE(v->customProperty("paleo/faciesResolvedLevel").toString(),
             QString("micro_facies"));
    QVERIFY(f.work.setDisplayMode(draft, "auto", &error));
    f.work.updateDisplayScale(9000000);
    QCOMPARE(f.work.resolvedLevel(draft), QString("facies"));
    QCOMPARE(f.work.displayLegend(draft).size(), 3);
  }
  void hierarchyParentAssignmentIsOneUndoAndKeepsCompatibleChildren() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto draft = hierarchyDraft(f, &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *v = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft));
    QVERIFY(v);
    auto it = v->getFeatures();
    QgsFeature a, b;
    QVERIFY(it.nextFeature(a));
    QVERIFY(it.nextFeature(b));
    QVERIFY(f.work.assignHierarchy(draft, {a.id()}, 3, "facies", &error));
    QCOMPARE(v->getFeature(a.id()).attribute("facies_code").toInt(), 3);
    QCOMPARE(v->getFeature(b.id()).attribute("facies_code").toInt(), 4);
    QCOMPARE(v->getFeature(b.id()).attribute("microfacies").toString(),
             QString("边滩"));
    QCOMPARE(v->undoStack()->count(), 1);
    v->selectByIds({a.id()});
    QVERIFY(f.work.selectHierarchyMembers(draft, "facies", &error));
    QCOMPARE(v->selectedFeatureIds().size(), 4);
    v->undoStack()->undo();
    QCoreApplication::processEvents();
    QCOMPARE(v->getFeature(a.id()).attribute("facies_code").toInt(), 1);
    QCOMPARE(v->getFeature(b.id()).attribute("facies_code").toInt(), 2);
    QVERIFY(f.work.assignHierarchy(draft, {a.id()}, 3, "sub_facies", &error));
    QCOMPARE(v->getFeature(b.id()).attribute("facies_code").toInt(), 4);
    v->undoStack()->undo();
    QVERIFY(f.work.assignHierarchy(draft, {a.id()}, 3, "micro_facies", &error));
    QCOMPARE(v->getFeature(b.id()).attribute("facies_code").toInt(), 2);
    QVERIFY(v->rollBack());
  }
  void hierarchyTopologyRejectsOverlapAndAllowsSharedBoundaryChanges() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto draft = hierarchyDraft(f, &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *v = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft));
    QVERIFY(v);
    QVERIFY(FaciesHierarchyRenderer::validateTopology(v, &error));
    const auto snapshot = f.work.versionForLayer(draft);
    const auto hash =
        digest(DataCatalog::resolvedVersionPath(f.dir.path(), snapshot));
    auto it = v->getFeatures();
    QgsFeature a, b;
    QVERIFY(it.nextFeature(a));
    QVERIFY(it.nextFeature(b));
    auto expanded = QgsGeometry::fromRect(QgsRectangle(0, 0, 120, 100));
    auto narrowed = QgsGeometry::fromRect(QgsRectangle(120, 0, 200, 100));
    QVERIFY(v->startEditing());
    v->beginEditCommand("bad overlap");
    QVERIFY(v->changeGeometry(a.id(), expanded));
    v->endEditCommand();
    QVERIFY(!v->commitChanges());
    QVERIFY(v->isEditable());
    QCOMPARE(digest(DataCatalog::resolvedVersionPath(f.dir.path(), snapshot)),
             hash);
    v->undoStack()->undo();
    v->beginEditCommand("shared edge");
    QVERIFY(v->changeGeometry(a.id(), expanded));
    QVERIFY(v->changeGeometry(b.id(), narrowed));
    v->endEditCommand();
    QVERIFY2(FaciesHierarchyRenderer::validateTopology(v, &error),
             qPrintable(error));
    QVERIFY(v->commitChanges());
    QVERIFY2(f.work.saveEditingVersion(draft, &error), qPrintable(error));
    QCOMPARE(f.work.versionForLayer(draft).versionNumber, 2);
    QVERIFY(f.work.setDisplayMode(draft, "facies", &error));
    QCOMPARE(v->getFeature(a.id()).geometry().boundingBox().xMaximum(), 120.0);
    QVERIFY(f.work.setDisplayMode(draft, "micro_facies", &error));
    QCOMPARE(v->getFeature(b.id()).geometry().boundingBox().xMinimum(), 120.0);
  }
  void hierarchyDraftForcesSharedVertexTopology() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto draft = hierarchyDraft(f, &error);
    QVERIFY(!draft.isEmpty());
    auto *v = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft));
    QVERIFY(v);
    QgsMapCanvas canvas;
    PaleoEditingToolbar toolbar(&canvas);
    toolbar.setProject(f.project.project());
    toolbar.setLayers({v});
    toolbar.setCurrentLayer(v);
    QVERIFY(toolbar.actionTopological()->isChecked());
    QVERIFY(!toolbar.actionTopological()->isEnabled());
    toolbar.actionVertexEdit()->trigger();
    auto *tool = qobject_cast<PaleoVertexTool *>(canvas.mapTool());
    QVERIFY(tool);
    QVERIFY(tool->topologicalEditingEnabled());
    toolbar.actionTopological()->setChecked(false);
    QVERIFY(tool->topologicalEditingEnabled());
    QVERIFY(toolbar.actionTopological()->isChecked());
    QVERIFY(toolbar.cancelEditing());
  }
  void evidenceUndoVersionLineageAndRecovery() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto draft = hierarchyDraft(f, &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *v = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft));
    QVERIFY(v);
    auto it = v->getFeatures();
    QgsFeature feature;
    QVERIFY(it.nextFeature(feature));
    const QVariantMap entry{{"text", "井 A 砂泥比与河道解释一致"},
                            {"level", "facies"},
                            {"source_layer", "input.grid"}};
    QVERIFY(f.work.addEvidence(draft, {feature.id()}, entry, &error));
    QCOMPARE(f.work.evidence(draft, {feature.id()}).size(), 1);
    const auto evidence = f.work.evidence(draft, {feature.id()})[0].toMap();
    QVERIFY(!evidence.value("source_version").toString().isEmpty());
    v->undoStack()->undo();
    QCoreApplication::processEvents();
    QCOMPARE(f.work.evidence(draft, {feature.id()}).size(), 0);
    v->undoStack()->redo();
    QCoreApplication::processEvents();
    QCOMPARE(f.work.evidence(draft, {feature.id()}).size(), 1);
    QVERIFY(v->commitChanges());
    QVERIFY(f.work.saveEditingVersion(draft, &error));
    const auto snapshot = f.work.versionForLayer(draft);
    QCOMPARE(v->customProperty("paleoVersionId").toString(), snapshot.id);
    QVERIFY(snapshot.parentVersionIds.contains(
        evidence.value("source_version").toString()));
    QgsVectorLayer saved(
        DataCatalog::resolvedVersionPath(f.dir.path(), snapshot) +
            "|layername=features",
        "saved", "ogr");
    QVERIFY(saved.isValid());
    auto savedIt = saved.getFeatures();
    QgsFeature savedFeature;
    QVERIFY(savedIt.nextFeature(savedFeature));
    QVERIFY(
        savedFeature.attribute("facies_evidence").toString().contains("井 A"));
    f.work.bindCatalog(&f.catalog, f.dir.path());
    QCOMPARE(f.work.evidence(draft, {feature.id()}).size(), 1);
    QVERIFY(f.work.removeEvidence(draft, {feature.id()},
                                  evidence.value("id").toString(), &error));
    QCOMPARE(f.work.evidence(draft, {feature.id()}).size(), 0);
    v->undoStack()->undo();
    QCoreApplication::processEvents();
    QCOMPARE(f.work.evidence(draft, {feature.id()}).size(), 1);
    QVERIFY(v->rollBack());
    QVERIFY(f.work.assignHierarchy(draft, {feature.id()}, 3, "micro_facies",
                                   &error));
    QVERIFY(f.work.evidence(draft, {feature.id()})[0]
                .toMap()
                .value("needs_review")
                .toBool());
    QVERIFY(v->rollBack());
  }
  void linkedCanvasesFollowExtentCursorAndCanDisconnect() {
    QgsMapCanvas main, reference;
    main.resize(600, 400);
    reference.resize(600, 400);
    main.setDestinationCrs(QgsCoordinateReferenceSystem("EPSG:3857"));
    reference.setDestinationCrs(main.mapSettings().destinationCrs());
    main.setExtent(QgsRectangle(0, 0, 600, 400));
    MapCanvasLink link(&main, &reference);
    main.setExtent(QgsRectangle(100, 200, 700, 600));
    QCOMPARE(reference.extent().center(), main.extent().center());
    reference.setExtent(QgsRectangle(200, 400, 800, 800));
    QCOMPARE(main.extent().center(), reference.extent().center());
    emit main.xyCoordinates(QgsPointXY(300, 500));
    bool cursor = false;
    for (auto *item : reference.scene()->items())
      cursor |= dynamic_cast<QgsRubberBand *>(item) != nullptr;
    QVERIFY(cursor);
    link.setEnabled(false);
    const auto old = reference.extent();
    main.setExtent(QgsRectangle(500, 700, 1100, 1100));
    QCOMPARE(reference.extent(), old);
    link.setEnabled(true);
    QCOMPARE(reference.extent().center(), main.extent().center());
    main.stopRendering();
    reference.stopRendering();
  }
  void hierarchyAndEvidencePanelIssueCommands() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const auto draft = hierarchyDraft(f, &error);
    QVERIFY(!draft.isEmpty());
    MappingWorkbenchPage page("compose", &f.work);
    page.setHorizon("D61");
    page.selectLayer(draft);
    auto *level = page.findChild<QComboBox *>("faciesDisplayLevel");
    auto *edit = page.findChild<QComboBox *>("faciesEditLevel");
    QVERIFY(level && edit);
    QSignalSpy intent(&page, &MappingWorkbenchPage::commandRequested);
    level->setCurrentIndex(1);
    QCOMPARE(intent.last()[0].toString(), QString("displayLevel"));
    edit->setCurrentIndex(0);
    page.commandButton("assignFacies")->click();
    QCOMPARE(intent.last()[1].toMap().value("edit_level").toString(),
             QString("facies"));
    auto *text = page.findChild<QPlainTextEdit *>("faciesEvidenceText");
    QVERIFY(text);
    QVERIFY(!page.commandButton("addEvidence")->isEnabled());
    auto *v = qobject_cast<QgsVectorLayer *>(f.layers.layer(draft));
    auto it = v->getFeatures();
    QgsFeature feature;
    QVERIFY(it.nextFeature(feature));
    v->selectByIds({feature.id()});
    text->setPlainText("解释证据");
    QVERIFY(page.commandButton("addEvidence")->isEnabled());
    page.commandButton("addEvidence")->click();
    QCOMPARE(intent.last()[0].toString(), QString("addEvidence"));
    QCOMPARE(intent.last()[1].toMap().value("text").toString(),
             QString("解释证据"));
    QVERIFY(f.work.addEvidence(
        draft, {feature.id()},
        {{"text", "井 A 的测井砂泥比与河道解释一致"}, {"level", "facies"}},
        &error));
    const auto qa = qEnvironmentVariable("PALEO_MAPPING_QA_DIR");
    if (!qa.isEmpty()) {
      QDir().mkpath(qa);
      page.resize(680, 2200);
      page.show();
      for (auto theme : {PaleoTheme::Theme::Light, PaleoTheme::Theme::Dark}) {
        PaleoTheme::applyTheme(theme);
        QTest::qWait(60);
        const auto suffix = theme == PaleoTheme::Theme::Light ? QString("light")
                                                              : QString("dark");
        QVERIFY(page.findChild<QWidget *>("workbenchAppearance")
                    ->grab()
                    .save(qa + "/hierarchy-" + suffix + ".png"));
        QVERIFY(page.findChild<QWidget *>("workbenchEvidence")
                    ->grab()
                    .save(qa + "/evidence-" + suffix + ".png"));
      }
      PaleoTheme::applyLightTheme();
    }
    QVERIFY(v->rollBack());
  }
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
    // 测井点标记统一成同一圆点；相名只出现在文字标注里。
    QColor unified;
    for (const QgsRendererCategory &cat : renderer->categories()) {
      QVERIFY(cat.symbol());
      if (!unified.isValid())
        unified = cat.symbol()->color();
      else
        QCOMPARE(cat.symbol()->color(), unified);
    }
    QCOMPARE(unified.name().toUpper(), QStringLiteral("#24303E"));
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
    // 相图标注：点层在 QGIS 画布上标注类别名称（白底胶囊底衬）。
    QVERIFY(layer.labelsEnabled());
    auto *labeling =
        dynamic_cast<QgsVectorLayerSimpleLabeling *>(layer.labeling());
    QVERIFY(labeling);
    // 标注表达式按字段可用性回退，相名走 facies_label 通道。
    QVERIFY(labeling->settings().fieldName.contains(QStringLiteral("facies_label")));
    QVERIFY(labeling->settings().format().background().enabled());
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
    QVERIFY(f.work.setLabelMode(draft, 0, &error));
    QVERIFY(!layer->labelsEnabled());
    f.work.styleLayer(draft);
    QVERIFY(!layer->labelsEnabled());
    QCOMPARE(f.work.labelMode(draft), 0);
    QVERIFY(f.work.setLabelMode(draft, 3, &error));
    QVERIFY(layer->labelsEnabled());
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
    // 单一预测相道：预测与人工修订不再分道显示。
    auto *canvas = panel.findChild<WellComposite::WellCompositeCanvas *>();
    QVERIFY(canvas);
    int faciesTracks = 0;
    for (const auto &track : canvas->tracks())
      if (track->type() == WellComposite::TrackType::FaciesCompound)
        ++faciesTracks;
    QCOMPARE(faciesTracks, 1);
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
    // 逐笔绘制不出快照版本（避免一条线一个「约束过程·vN」刷屏）；
    // 快照在使用点 generateFactor 前置步骤产生。
    QVERIFY(f.latest("constraint_snapshot").isEmpty());
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
  void panelGuidanceSelectionAndRefreshPreserveUserIntent() {
    Fixture f;
    QVERIFY(f.init());
    MappingWorkbenchPage page("predict", &f.work);
    page.setHorizon("D61");
    auto *kind = page.findChild<QComboBox *>("predictionKind");
    auto *inputs = page.findChild<QListWidget *>("workbenchInputs");
    auto *select = page.findChild<QPushButton *>("workbenchSelectInputs");
    auto *hint = page.findChild<QLabel *>("workbenchInputHint");
    QVERIFY(kind && inputs && select && hint);
    QVERIFY(!select->isEnabled());
    QVERIFY(hint->text().contains(QStringLiteral("数据管理")));
    QVERIFY(!f.well("A", 10, 20).isEmpty());
    QVERIFY(!f.well("B", 30, 40).isEmpty());
    kind->setCurrentIndex(1);
    const int inputHeight = inputs->minimumHeight();
    inputs->item(0)->setCheckState(Qt::Checked);
    select->click(); // A partial selection becomes all, not none.
    QCOMPARE(inputs->item(0)->checkState(), Qt::Checked);
    QCOMPARE(inputs->item(1)->checkState(), Qt::Checked);
    select->click();
    QCOMPARE(inputs->item(0)->checkState(), Qt::Unchecked);
    QCOMPARE(inputs->item(1)->checkState(), Qt::Unchecked);
    QString error;
    QVERIFY(f.work.predict("D61", "wells", {"A", "B"}, &error));
    QTRY_VERIFY(!f.work.busy());
    const auto id = f.latest("wells_prediction");
    QVERIFY(!id.isEmpty());
    page.selectLayer(id);
    auto *labels = page.findChild<QComboBox *>("faciesLabelMode");
    QVERIFY(labels && labels->isEnabled());
    labels->setCurrentIndex(0);
    inputs->item(0)->setCheckState(Qt::Checked);
    page.refresh();
    QCOMPARE(labels->currentIndex(), 0); // unrelated refresh must not reset it
    QCOMPARE(inputs->minimumHeight(), inputHeight);
    QSignalSpy intent(&page, &MappingWorkbenchPage::commandRequested);
    page.commandButton("labels")->click();
    QCOMPARE(intent.last()[1].toMap().value("label_mode").toInt(), 0);
    QVERIFY(f.work.setLabelMode(id, 0, &error));
    QVERIFY(f.work.setLabelMode(id, 2, &error));
    QCOMPARE(labels->currentIndex(), 2); // saved state follows external changes

    MappingWorkbenchPage compose("compose", &f.work);
    compose.setHorizon("D61");
    auto *list = compose.findChild<QListWidget *>("workbenchInputs");
    QVERIFY(list && list->count() > 0);
    list->setCurrentRow(0);
    const auto selected = list->currentItem()->data(Qt::UserRole);
    compose.refresh();
    QVERIFY(list->currentItem());
    QCOMPARE(list->currentItem()->data(Qt::UserRole), selected);
    QVERIFY(!compose.findChild<QPushButton *>("workbenchMoveUp")->isEnabled());
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
  void constraintImportLocalCrsAndVerbatimRoundTrip() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const QString base =
        QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath(
            "fixtures/singlefactor/structural_synthetic");
    // LOCAL_CS（无大地基准的工程直角米制 CRS）与工程局部网格等价，
    // 不再被坐标门拒绝。
    QVERIFY2(f.work.importConstraints("D61", base + "/directions.shp", "auto",
                                      &error),
             qPrintable(error));
    QVERIFY2(f.work.importConstraints("D61", base + "/barriers.shp", "auto",
                                      &error),
             qPrintable(error));
    const auto rows = f.constraints.loadConstraints("D61");
    QCOMPARE(rows.size(), 8);
    QStringList types;
    QVariantList params;
    for (const auto &row : rows) {
      types << row.value("type").toString();
      params << QJsonDocument::fromJson(
                    row.value("params_json").toString().toUtf8())
                    .object()
                    .toVariantMap();
    }
    QCOMPARE(types.mid(0, 4),
             QStringList(4, QStringLiteral("direction_line")));
    QCOMPARE(types.mid(4, 4), QStringList(4, QStringLiteral("break_line")));
    QCOMPARE(params.at(0).toMap().value("semantic").toString(),
             QStringLiteral("direction_guide"));
    QCOMPARE(params.at(4).toMap().value("semantic").toString(),
             QStringLiteral("hard_barrier"));
    QCOMPARE(params.at(1).toMap().value("ratio").toDouble(), 8.0);
    QVERIFY(!params.at(3).toMap().value("enabled").toBool());
    QCOMPARE(params.at(5).toMap().value("blockMode").toString(),
             QStringLiteral("full_block"));
    QCOMPARE(params.at(6).toMap().value("blockMode").toString(),
             QStringLiteral("soft"));
    QVERIFY(!params.at(7).toMap().value("enabled").toBool());
    // generic 回导：快照里的 type+params_json 两列原样回写（不是 "line"）。
    const auto snapshot = f.latest("constraint_snapshot");
    QVERIFY(!snapshot.isEmpty());
    QVERIFY2(f.work.importConstraints(
                 "D62", f.work.declaration(snapshot).source, "auto", &error),
             qPrintable(error));
    const auto roundTrip = f.constraints.loadConstraints("D62");
    QCOMPARE(roundTrip.size(), 8);
    QCOMPARE(roundTrip.at(0).value("type").toString(),
             QStringLiteral("direction_line"));
    const auto rt = QJsonDocument::fromJson(
                        roundTrip.at(0).value("params_json").toString().toUtf8())
                        .object()
                        .toVariantMap();
    QCOMPARE(rt.value("semantic").toString(),
             QStringLiteral("direction_guide"));
    QCOMPARE(rt.value("ratio").toDouble(), 3.0);
    QCOMPARE(roundTrip.at(6).value("type").toString(),
             QStringLiteral("break_line"));
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
