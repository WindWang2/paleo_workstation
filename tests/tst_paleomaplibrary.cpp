#include <QtTest>
#include <QEventLoop>
#include <QAction>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QComboBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QTabWidget>
#include <QScrollArea>
#include <QScrollBar>
#include <QLabel>
#include <QProgressBar>
#include "../src/ui/paleotheme.h"
#include <gdal.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>
#include <qgsvectorlayer.h>
#include <qgsmapcanvas.h>
#include <qgsrenderer.h>
#include "../src/qgis/qgisruntime.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/io/dataimportservice.h"
#include "../src/services/previewdoc.h"
#include "../src/services/storagegovernance.h"
#include "../src/services/dataexport.h"
#include "../src/services/paleotaskservice.h"
#include "../src/services/paleomapfiles.h"
#include "../src/workflow/paleomaplibrary.h"
#include "../src/workflow/projectareasummary.h"
#include "../src/workflow/mappingworkbench.h"
#include "../src/workflow/workflows.h"
#include "../src/ui/pages/datalist.h"
#include "../src/ui/paleomainwindow.h"
#include "../src/qgis/qgiscanvascontroller.h"
#include "../src/linkage/selectioncontext.h"
#include "../src/ui/pages/mappingbasemapselector.h"
#include "../src/ui/datapreview/projectstatisticspage.h"
#include "../src/ui/datapreview/datapreviewtabs.h"

class TestPaleomapLibrary : public QObject {
  Q_OBJECT
  struct Fixture {
    QTemporaryDir dir;
    QgisProjectService project;
    PaleoProjectStore store;
    DataImportService importer{&store};
    LayerManifest manifest{dir.filePath("project.sqlite")};
    QgisLayerService layers{&project, &manifest};
    QgisProcessingService processing{&store};
    ConstraintWorkflow constraints{&processing, &layers};
    MappingWorkbench work{&layers, &processing, &project, &constraints};
    PaleoTaskService tasks;
    bool init() {
      if (!project.createProject(dir.filePath("project.qgz")) || !manifest.open()) return false;
      importer.setProjectDir(dir.path()); work.bindCatalog(importer.catalog(), dir.path()); return importer.catalog()->isOpen();
    }
    PaleoMapLibrary::Result run(const PaleoMapLibrary::Request &request) {
      PaleoMapLibrary::Result result; QEventLoop loop; bool completed = false; QString error;
      auto *task = PaleoMapLibrary::importMaps(importer.catalog(), &layers, &tasks, request, &loop,
          [&](const auto &r) { result = r; completed = true; loop.quit(); }, &error);
      if (!task) { result.errors << error; return result; }
      QTimer timeout; timeout.setSingleShot(true); QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit); timeout.start(20000); loop.exec();
      if (!completed) { task->requestCancel(); result.errors << "timed out"; }
      return result;
    }
  };
  static bool shape(const QString &path, const QString &unit = "initial") {
    QDir().mkpath(QFileInfo(path).absolutePath());
    auto driver = GDALGetDriverByName("ESRI Shapefile"); if (!driver) return false;
    auto ds = GDALCreate(driver, path.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr); if (!ds) return false;
    auto srs = OSRNewSpatialReference(nullptr); OSRImportFromEPSG(srs, 3857);
    auto layer = GDALDatasetCreateLayer(ds, "map", srs, wkbPolygon, nullptr); OSRDestroySpatialReference(srs);
    if (!layer) { GDALClose(ds); return false; }
    auto field = OGR_Fld_Create("unit", OFTString); OGR_Fld_SetWidth(field, 32); const bool ok = OGR_L_CreateField(layer, field, true) == OGRERR_NONE; OGR_Fld_Destroy(field);
    auto feature = OGR_F_Create(OGR_L_GetLayerDefn(layer)); OGR_F_SetFieldString(feature, 0, unit.toUtf8().constData());
    char wkt[] = "POLYGON((0 0,1000 0,1000 1000,0 1000,0 0))"; char *ptr = wkt; OGRGeometryH geometry = nullptr;
    OGR_G_CreateFromWkt(&ptr, nullptr, &geometry); OGR_F_SetGeometryDirectly(feature, geometry);
    const bool created = ok && OGR_L_CreateFeature(layer, feature) == OGRERR_NONE; OGR_F_Destroy(feature); GDALClose(ds);
    QFile style(QFileInfo(path).absoluteDir().filePath(QFileInfo(path).completeBaseName() + ".qml"));
    if (style.open(QIODevice::WriteOnly)) style.write("<qgis version=\"4.2\"/>");
    return created;
  }
private slots:
  void initTestCase() { PaleoTheme::pinRenderEnvironment(); }
  void shapefileIsOneImmutableDatasetAndDbfChangesCreateAVersion() {
    Fixture f; QVERIFY(f.init()); QTemporaryDir source;
    const auto path = source.filePath("D61/map.shp"); QVERIFY(shape(path));
    PaleoMapLibrary::Request request; request.files = {path}; request.horizon = "C6";
    const auto first = f.run(request); QVERIFY2(first.errors.isEmpty(), qPrintable(first.errors.join("\n"))); QCOMPARE(first.assets.size(), 1);
    auto *cat = f.importer.catalog(); const auto asset = first.assets.first(); const auto original = cat->currentVersion(asset);
    QCOMPARE(original.extra.value("horizon").toString(), QString("D61")); QVERIFY(original.extra.value("bundle_members").toList().size() >= 5);
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate("paleomap." + original.id)); QVERIFY(layer && layer->isValid()); QCOMPARE(layer->featureCount(), 1);
    const auto second = f.run(request); QVERIFY(second.errors.isEmpty()); QCOMPARE(second.unchanged, 1); QCOMPARE(cat->versionsForAsset(asset).size(), 1);
    const auto sourceShapeHash = DataCatalog::sha256FileHex(path);
    auto ds = GDALOpenEx(path.toUtf8().constData(), GDAL_OF_VECTOR | GDAL_OF_UPDATE, nullptr, nullptr, nullptr); QVERIFY(ds);
    auto sourceLayer = GDALDatasetGetLayer(ds, 0); auto feature = OGR_L_GetNextFeature(sourceLayer); QVERIFY(feature);
    OGR_F_SetFieldString(feature, 0, "changed DBF only"); QCOMPARE(OGR_L_SetFeature(sourceLayer, feature), OGRERR_NONE); OGR_F_Destroy(feature); GDALClose(ds);
    QCOMPARE(DataCatalog::sha256FileHex(path), sourceShapeHash);
    const auto third = f.run(request); QVERIFY2(third.errors.isEmpty(), qPrintable(third.errors.join("\n"))); QCOMPARE(third.assets, first.assets);
    const auto current = cat->currentVersion(asset); QCOMPARE(cat->versionsForAsset(asset).size(), 2);
    QCOMPARE(current.versionNumber, 2); QCOMPARE(current.sha256, original.sha256);
    QVERIFY(current.extra.value("bundle_sha256") != original.extra.value("bundle_sha256"));
    const auto storage = paleo::storage::scan(paleo::storage::snapshot(cat));
    for (const auto &file : storage.orphans) QVERIFY2(!file.relativePath.endsWith(".qml"), qPrintable(file.relativePath));
    paleo::exporting::DataRequest exportRequest; exportRequest.versionIds = {current.id}; exportRequest.destination = source.filePath("exported");
    const auto plan = paleo::exporting::plan(*cat, f.dir.path(), exportRequest); QVERIFY2(plan.failures.isEmpty(), qPrintable(plan.failures.join("\n")));
    const auto exported = paleo::exporting::execute(plan); QVERIFY2(exported.complete(), qPrintable(exported.error + exported.failures.join("\n")));
    bool hasStyle = false; for (const auto &file : exported.files) hasStyle |= file.endsWith(".qml"); QVERIFY(hasStyle);
    request.horizon = "D62"; request.inferHorizons = false; const auto otherHorizon = f.run(request);
    QVERIFY(otherHorizon.errors.isEmpty()); QCOMPARE(cat->assets().size(), 2);
    QCOMPARE(cat->currentVersion(otherHorizon.assets.first()).extra.value("horizon").toString(), QString("D62"));
    QVERIFY(QDir(source.filePath("D61")).removeRecursively());
    QgsVectorLayer stored(cat->versionFilePath(original), "original", "ogr"); QVERIFY(stored.isValid());
    auto iterator = stored.getFeatures(); QgsFeature storedFeature; QVERIFY(iterator.nextFeature(storedFeature)); QCOMPARE(storedFeature.attribute("unit").toString(), QString("initial"));
  }
  void missingCompanionsAndCancellationNeverPublishPartialAssets() {
    Fixture f; QVERIFY(f.init()); QTemporaryDir source;
    QFile broken(source.filePath("broken.shp")); QVERIFY(broken.open(QIODevice::WriteOnly)); broken.write("incomplete"); broken.close();
    PaleoMapLibrary::Request request; request.folder = source.path(); request.horizon = "D61";
    const auto result = f.run(request); QVERIFY(!result.errors.isEmpty()); QVERIFY(f.importer.catalog()->assets().isEmpty());
    const auto valid = source.filePath("D61/map.shp"); QVERIFY(shape(valid)); request.folder.clear(); request.files = {valid};
    bool called = false; PaleoMapLibrary::Result cancelled;
    auto *task = PaleoMapLibrary::importMaps(f.importer.catalog(), &f.layers, &f.tasks, request, this, [&](const auto &r) { cancelled = r; called = true; }); QVERIFY(task);
    task->requestCancel(); QTRY_VERIFY_WITH_TIMEOUT(called, 20000); QVERIFY(cancelled.cancelled); QVERIFY(f.importer.catalog()->assets().isEmpty());
    QVERIFY(f.layers.declared().isEmpty());
  }
  void projectMutationRejectsThePreparedBatch() {
    Fixture f; QVERIFY(f.init()); QTemporaryDir source; const auto path = source.filePath("map.shp"); QVERIFY(shape(path));
    PaleoMapLibrary::Request request; request.files = {path}; request.horizon = "D61"; bool called = false; PaleoMapLibrary::Result result;
    QVERIFY(PaleoMapLibrary::importMaps(f.importer.catalog(), &f.layers, &f.tasks, request, this, [&](const auto &r) { result = r; called = true; }));
    CatalogAsset unrelated{"unrelated", "reference", "txt", "unrelated.txt"}; QVERIFY(f.importer.catalog()->addAsset(unrelated));
    QTRY_VERIFY_WITH_TIMEOUT(called, 20000); QVERIFY(!result.errors.isEmpty()); QVERIFY(result.assets.isEmpty()); QCOMPARE(f.importer.catalog()->assets().size(), 1);
    QVERIFY(f.layers.declared().isEmpty());
  }
  void dataRibbonExposesTheThreeImportEntries() {
    QObject context; Fixture f; QVERIFY(f.init()); f.project.setParent(&context);
    QgisCanvasController controller(&context); controller.setLayerResolver([&](const QString &id) { return f.layers.layer(id); });
    SelectionContext selection; selection.setActiveHorizon("D61"); PredictionWorkflow prediction(&f.processing, &f.layers);
    CompositionWorkflow composition(&f.processing, &f.layers); ValidationWorkflow validation(&f.layers, &f.store);
    PaleoMainWindow window(&controller, &f.project, &f.layers, nullptr, &selection);
    window.attachWorkflows(&prediction, &f.constraints, &composition, &validation, &f.importer, nullptr, &f.processing, &f.store);
    window.attachWorkbench(&f.work);
    auto *preview = window.findChild<DataPreviewTabs *>(); QVERIFY(preview);
    auto *tabs = preview->findChild<QTabWidget *>("dataPreviewTabs"); QVERIFY(tabs);
    QTRY_COMPARE(preview->assetIdAt(tabs->currentIndex()), QString("project_dashboard"));
    window.showPage("predict");
    const auto projectPath = f.project.projectPath(); f.project.closeProject();
    QVERIFY(f.project.openProject(projectPath));
    QTRY_COMPARE(preview->assetIdAt(tabs->currentIndex()), QString("project_dashboard"));
    QCOMPARE(window.currentPage(), QString("data"));
    QCOMPARE(window.findChild<QLabel *>("entityViewHeader")->text(), QString("工区看板"));
    QVERIFY(window.findChild<ProjectStatisticsPage *>()->isVisibleTo(&window));
    preview->openSurveyArea(); f.project.closeProject();
    QVERIFY(f.project.openProjectAsync(projectPath));
    QTRY_VERIFY_WITH_TIMEOUT(!f.project.isOpening(), 20000);
    QTRY_COMPARE(preview->assetIdAt(tabs->currentIndex()), QString("project_dashboard"));
    QCOMPARE(preview->tabCount(), 1);
    f.project.closeProject();
    QVERIFY(f.project.createProject(f.dir.filePath("new.qgz")));
    QTRY_COMPARE(preview->assetIdAt(tabs->currentIndex()), QString("project_dashboard"));
    f.project.closeProject();
    QVERIFY(f.project.openProject(projectPath)); // 尚未执行排队导航便关闭。
    f.project.closeProject(); QTest::qWait(20); QCOMPARE(preview->tabCount(), 0);
    QVERIFY(f.project.openProject(projectPath));
    QTRY_COMPARE(preview->assetIdAt(tabs->currentIndex()), QString("project_dashboard"));
    // 默认导航不能抢走打开工区后立即发出的用户意图。
    f.project.closeProject(); QVERIFY(f.project.openProject(projectPath));
    preview->openSurveyArea(); QTest::qWait(20);
    QCOMPARE(preview->assetIdAt(tabs->currentIndex()), QString("survey_area"));
    f.project.closeProject(); QVERIFY(f.project.openProject(projectPath));
    window.showPage("predict"); QTest::qWait(20);
    QCOMPARE(window.currentPage(), QString("predict")); QCOMPARE(preview->tabCount(), 0);
    f.project.closeProject(); QVERIFY(f.project.openProject(projectPath));
    QTRY_COMPARE(preview->assetIdAt(tabs->currentIndex()), QString("project_dashboard"));
    window.showPage("data");
    for (const auto &name : {"ribbonImportPaleomap", "ribbonImportPaleomapFolder", "ribbonImportFiles"}) {
      auto *action = window.findChild<QAction *>(name); QVERIFY2(action, name); QVERIFY(action->isEnabled());
    }
    const auto qa = qEnvironmentVariable("PALEO_PALEOMAP_QA_DIR");
    if (!qa.isEmpty()) { window.resize(1600, 1000); window.show(); QTest::qWait(150); QVERIFY(window.grab().save(qa + "/data-import-ribbon.png")); }
  }
  void dashboardAndMapAreIndependentOverviewEntries() {
    Fixture f; QVERIFY(f.init()); PreviewDocService doc(&f.importer); DataPreviewTabs preview;
    preview.setDocService(&doc); preview.setProject(f.project.project()); preview.openProjectDashboard();
    auto *tabs = preview.findChild<QTabWidget *>("dataPreviewTabs"); QVERIFY(tabs);
    QCOMPARE(preview.assetIdAt(tabs->currentIndex()), QString("project_dashboard"));
    QVERIFY(!tabs->tabIcon(tabs->currentIndex()).isNull());
    auto *dashboard = preview.findChild<ProjectStatisticsPage *>(); QVERIFY(dashboard);
    preview.openProjectDashboard(); QCOMPARE(preview.tabCount(), 1);
    dashboard->findChild<QPushButton *>("dashboardOpenMap")->click();
    QCOMPARE(preview.tabCount(), 2); QCOMPARE(preview.assetIdAt(tabs->currentIndex()), QString("survey_area"));
    preview.openProjectDashboard(); QCOMPARE(tabs->currentWidget(), dashboard);
    DataListPanel list; list.setDocService(&doc); list.refreshAssetTable();
    auto *tree = list.findChild<QTreeWidget *>("dataTree"); QVERIFY(tree);
    const auto roots = tree->findItems("工区概况", Qt::MatchExactly); QCOMPARE(roots.size(), 1);
    auto *root = roots.first(); QCOMPARE(root->childCount(), 2); QVERIFY(root->isExpanded());
    QVERIFY(!root->child(0)->icon(0).isNull());
    QCOMPARE(root->child(0)->text(0), QString("工区看板")); QCOMPARE(root->child(1)->text(0), QString("工区全景地图"));
    QSignalSpy requested(&list, &DataListPanel::projectDashboardActivated);
    list.selectAssetInViews("project_dashboard");
    QCOMPARE(tree->currentItem(), root->child(0));
    tree->itemActivated(root->child(0), 0); QCOMPARE(requested.size(), 1);
    QVERIFY(list.currentAssetSelection().isEmpty());
  }
  void dashboardReflowsAndRefreshesWithoutHorizontalClipping() {
    Fixture f; QVERIFY(f.init()); auto *cat = f.importer.catalog();
    CatalogAsset asset{"progress", "facies", "geojson", "progress.json"}; QVERIFY(cat->addAsset(asset));
    CatalogVersion version; version.id = "progress-v1"; version.assetId = asset.id; version.stage = "DERIVED";
    version.extra = {{"kind", "wells_prediction"}, {"horizon", "D61"}}; QVERIFY(cat->addVersion(version));
    version.id = "progress-v2"; QVERIFY(cat->addVersion(version)); // 同层位的历史版本不重复累计覆盖。
    for (const auto &kind : {QString("seismic_prediction"), QString("single_factor_raster"), QString("composed_facies")}) {
      version.id = kind; version.extra["kind"] = kind; QVERIFY(cat->addVersion(version));
    }
    for (int i = 0; i < 20; ++i) {
      CatalogEntity well; well.id = QString("well-%1").arg(i); well.name = QString("A%1").arg(i + 1); well.entityType = "well";
      well.hasSurface = true; well.surfaceX = (i % 5) * 2000; well.surfaceY = (i / 5) * 2000; QVERIFY(cat->addEntity(well));
    }
    CatalogEntity survey; survey.id = "survey"; survey.entityType = "seismic_survey"; survey.name = "S1";
    survey.corners = {{0, 0}, {10000, 0}, {10000, 15000}, {0, 15000}}; QVERIFY(cat->addEntity(survey));
    ProjectStatisticsPage page(cat); page.resize(700, 900); page.show(); QTest::qWait(100);
    auto *scroll = page.findChild<QScrollArea *>("dashboardScroll"); QVERIFY(scroll);
    QCOMPARE(page.findChild<QLabel *>("count_well")->text(), QString("1 / 8"));
    QCOMPARE(page.findChild<QProgressBar *>("coverage_well")->value(), 1);
    const auto cards = page.findChildren<QFrame *>("dashboardHorizonCard"); QCOMPARE(cards.size(), 8);
    const auto qa = qEnvironmentVariable("PALEO_PALEOMAP_QA_DIR");
    for (const auto theme : {PaleoTheme::Theme::Light, PaleoTheme::Theme::Dark}) {
      PaleoTheme::applyTheme(theme);
      for (const int width : {420, 700, 1200}) {
        page.resize(width, 1000); QTest::qWait(100);
        QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        for (auto *card : cards) {
          QVERIFY(card->geometry().left() >= 0); QVERIFY(card->geometry().right() < scroll->widget()->width());
          for (auto *label : card->findChildren<QLabel *>()) QVERIFY(label->width() >= label->minimumSizeHint().width());
        }
        if (!qa.isEmpty()) QVERIFY(page.grab().save(qa + QString("/dashboard-%1-%2.png").arg(theme == PaleoTheme::Theme::Light ? "light" : "dark").arg(width)));
      }
    }
    PaleoTheme::applyLightTheme();
    CatalogEntity well; well.id = "live-well"; well.entityType = "well"; well.name = "A1"; QVERIFY(cat->addEntity(well));
    QTRY_COMPARE(page.findChild<QLabel *>("dashboardWells")->text(), QString("21 口"));
    QCOMPARE(page.findChildren<QFrame *>("dashboardHorizonCard").size(), 8);
  }
  void statisticsCountRegisteredWorkAndUnionOverlappingSurveyAreas() {
    Fixture f; QVERIFY(f.init()); auto *cat = f.importer.catalog();
    CatalogEntity a; a.id = "well-a"; a.entityType = "well"; a.name = "A"; a.hasSurface = true; a.surfaceX = 0; a.surfaceY = 0; QVERIFY(cat->addEntity(a));
    a.id = "well-b"; a.name = "B"; a.hasSurface = false; QVERIFY(cat->addEntity(a));
    CatalogEntity survey; survey.id = "survey-a"; survey.entityType = "seismic_survey"; survey.name = "S1";
    survey.corners = {{0, 0}, {1000, 0}, {1000, 1000}, {0, 1000}}; QVERIFY(cat->addEntity(survey));
    survey.id = "survey-b"; survey.name = "S2"; survey.corners = {{500, 0}, {1500, 0}, {1500, 1000}, {500, 1000}}; QVERIFY(cat->addEntity(survey));
    const auto stats = ProjectAreaSummary::snapshot(cat); QCOMPARE(stats.value("wells").toInt(), 2); QCOMPARE(stats.value("positioned_wells").toInt(), 1);
    QCOMPARE(stats.value("survey_area_km2").toDouble(), 1.5); QCOMPARE(stats.value("seismic_surveys").toInt(), 2);
    QVERIFY(!stats.value("horizons").toList().isEmpty());
    ProjectStatisticsPage page(cat); QCOMPARE(page.findChildren<QFrame *>("dashboardHorizonCard").size(), stats.value("horizons").toList().size());
  }
  void navigationAndBaseMapChoiceRespectHorizon() {
    Fixture f; QVERIFY(f.init()); QTemporaryDir source; const auto path = source.filePath("D61/map.shp"); QVERIFY(shape(path));
    PaleoMapLibrary::Request request; request.files = {path}; request.horizon = "D61"; const auto result = f.run(request); QVERIFY(result.errors.isEmpty());
    const auto candidates = f.work.baseMapCandidates("D61"); QCOMPARE(candidates.size(), 1); QCOMPARE(candidates.first().toMap().value("category").toString(), QString("existing"));
    QVERIFY(f.work.baseMapCandidates("D62").isEmpty());
    MappingBaseMapSelector selector(&f.work); selector.setHorizon("D61");
    auto *sourceBox = selector.findChild<QComboBox *>("baseMapSource"); QVERIFY(sourceBox); QCOMPARE(sourceBox->count(), 1);
    QSignalSpy commands(&selector, &MappingBaseMapSelector::commandRequested);
    const auto buttons = selector.findChildren<QPushButton *>(); QVERIFY(buttons.first()->isEnabled()); buttons.first()->click(); QCOMPARE(commands.size(), 1);
    QCOMPARE(commands.first()[0].toString(), QString("show")); QCOMPARE(commands.first()[1].toMap().value("horizon").toString(), QString("D61"));
    selector.setHorizon("D62"); QVERIFY(!buttons.first()->isEnabled()); selector.setHorizon("D61");
    PreviewDocService doc(&f.importer); DataPreviewTabs mapPreview; mapPreview.setDocService(&doc); mapPreview.setProject(f.project.project()); mapPreview.openAsset(result.assets.first());
    bool foundSource = false;
    for (auto *canvas : mapPreview.findChildren<QgsMapCanvas *>()) for (auto *layer : canvas->layers())
      if (auto *vector = qobject_cast<QgsVectorLayer *>(layer); vector && vector->source().section('|', 0, 0) == f.importer.catalog()->versionFilePath(f.importer.catalog()->currentVersion(result.assets.first()))) {
        QVERIFY(vector->renderer()); QCOMPARE(vector->renderer()->type(), QString("singleSymbol")); foundSource = true;
      }
    QVERIFY(foundSource);
    DataListPanel list; list.setDocService(&doc); list.refreshAssetTable();
    auto *tree = list.findChild<QTreeWidget *>("dataTree"); QVERIFY(tree);
    const auto maps = tree->findItems("现有古地理图", Qt::MatchExactly | Qt::MatchRecursive); QCOMPARE(maps.size(), 1);
    QCOMPARE(maps.first()->childCount(), 1); QCOMPARE(maps.first()->child(0)->text(0), QString("D61")); QCOMPARE(maps.first()->child(0)->childCount(), 1);
    const auto qa = qEnvironmentVariable("PALEO_PALEOMAP_QA_DIR");
    if (!qa.isEmpty()) {
      tree->expandAll();
      QWidget window; auto *layout = new QVBoxLayout(&window); list.setParent(&window); selector.setParent(&window);
      layout->addWidget(&list); layout->addWidget(&selector); window.resize(1000, 1000); window.show(); QTest::qWait(150);
      QVERIFY(window.grab().save(qa + "/paleomap-library-and-base.png"));
      list.setParent(nullptr); selector.setParent(nullptr);
      ProjectStatisticsPage statistics(f.importer.catalog()); statistics.resize(1100, 500); statistics.show(); QTest::qWait(100);
      QVERIFY(statistics.grab().save(qa + "/project-statistics.png"));
    }
  }
};
int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen"); if (!QgisRuntime::initialize()) return 1; GDALAllRegister();
  TestPaleomapLibrary test; const int result = QTest::qExec(&test, argc, argv); QgisRuntime::shutdown(); return result;
}
#include "tst_paleomaplibrary.moc"
