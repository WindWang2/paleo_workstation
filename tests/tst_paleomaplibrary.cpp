#include <QtTest>
#include <QEventLoop>
#include <QAction>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QComboBox>
#include <QPushButton>
#include <QToolButton>
#include <QDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QMenu>
#include "../src/workflow/assetops.h"
#include <QTreeWidget>
#include <QTableWidget>
#include "../src/domain/projectclassifier.h"
#include <QTimer>
#include <QVBoxLayout>
#include <QTabWidget>
#include <QScrollArea>
#include <QScrollBar>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QIcon>
#include <qgssymbollayer.h>
#include <qgssymbol.h>
#include "../src/domain/faciescatalog.h"
#include "../src/domain/faciestaxonomy.h"
#include "../src/domain/paleomapfacies.h"
#include "../src/qgis/facieshierarchyrenderer.h"
#include "../src/qgis/faciesmaplegend.h"
#include "../src/ui/paleotheme.h"
#include <gdal.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>
#include <qgsvectorlayer.h>
#include <qgsmapcanvas.h>
#include <qgsmaprenderersequentialjob.h>
#include <qgsproject.h>
#include <qgslayertree.h>
#include <qgslayertreemodel.h>
#include <qgssinglesymbolrenderer.h>
#include <qgsfillsymbol.h>
#include "../src/ui/layers/layertreepanel.h"
#include "../src/ui/pages/mappingworkbenchpage.h"
#include <qgsrenderer.h>
#include "../src/qgis/qgisruntime.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/paleomapsource.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/io/dataimportservice.h"
#include "../src/io/ingestplan.h"
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
#include "../src/ui/pages/datapage.h"
#include "../src/ui/dialogs/importsourcedialog.h"
#include "../src/ui/dialogs/offlinebasemapdialog.h"
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
  static bool microShape(const QString &path, const QVariantList &rows, const QString &microField = "microfacies") {
    QDir().mkpath(QFileInfo(path).absolutePath());
    auto ds = GDALCreate(GDALGetDriverByName("ESRI Shapefile"), path.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr);
    if (!ds) return false;
    auto srs = OSRNewSpatialReference(nullptr); OSRImportFromEPSG(srs, 3857);
    char encoding[] = "ENCODING=UTF-8"; char *options[] = {encoding, nullptr};
    auto layer = GDALDatasetCreateLayer(ds, "map", srs, wkbPolygon, options); OSRDestroySpatialReference(srs);
    if (!layer) { GDALClose(ds); return false; }
    for (const auto &name : QStringList{microField, "facies_name", "subfacies", "src_note"}) {
      auto field = OGR_Fld_Create(name.toUtf8().constData(), OFTString); OGR_Fld_SetWidth(field, 128);
      const auto err = OGR_L_CreateField(layer, field, true); OGR_Fld_Destroy(field); if (err != OGRERR_NONE) { GDALClose(ds); return false; }
    }
    for (int i = 0; i < rows.size(); ++i) {
      const auto row = rows[i].toMap(); auto feature = OGR_F_Create(OGR_L_GetLayerDefn(layer));
      OGR_F_SetFieldString(feature, 0, row.value("microfacies").toString().toUtf8().constData());
      OGR_F_SetFieldString(feature, 1, row.value("facies").toString().toUtf8().constData());
      OGR_F_SetFieldString(feature, 2, row.value("subfacies").toString().toUtf8().constData()); OGR_F_SetFieldString(feature, 3, "preserved");
      const auto wkt = QString("POLYGON((%1 0,%2 0,%2 1000,%1 1000,%1 0))").arg(i * 1000).arg((i + 1) * 1000).toUtf8();
      char *ptr = const_cast<char *>(wkt.constData()); OGRGeometryH geometry = nullptr; OGR_G_CreateFromWkt(&ptr, nullptr, &geometry); OGR_F_SetGeometryDirectly(feature, geometry);
      const auto err = OGR_L_CreateFeature(layer, feature); OGR_F_Destroy(feature); if (err != OGRERR_NONE) { GDALClose(ds); return false; }
    }
    GDALClose(ds); return true;
  }
private slots:
  void checkingLayerRepairsStaleDrawOrder() {
    QObject context; Fixture f; QVERIFY(f.init()); f.project.setParent(&context);
    QTemporaryDir source; const auto path = source.filePath("map.shp"); QVERIFY(microShape(path, {QVariantMap{{"microfacies", QStringLiteral("分流河道")}}}));
    PaleoMapLibrary::Request request; request.files = {path}; request.horizon = "D62";
    const auto result = f.run(request); QVERIFY(result.errors.isEmpty());
    const auto version = f.importer.catalog()->currentVersion(result.assets.first());
    auto *map = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(version.extra.value("layer_id").toString())); QVERIFY(map);
    auto *mask = map->clone(); mask->setRenderer(new QgsSingleSymbolRenderer(QgsFillSymbol::createSimple({{"color", "255,255,255,255"}, {"outline_style", "no"}}).release()));
    auto *root = f.project.project()->layerTreeRoot(); f.project.project()->addMapLayer(mask, false); root->addLayer(mask);
    root->setCustomLayerOrder(QList<QgsMapLayer *>{mask, map}); root->setHasCustomLayerOrder(true);
    QgisCanvasController controller(&context); auto *canvas = controller.canvas();
    LayerTreePanel panel(f.project.project(), canvas, &f.layers);
    auto *model = panel.layerTreeModel(); auto *node = root->findLayer(map->id()); QVERIFY(node);
    const auto index = model->node2index(node);
    QVERIFY(model->setData(index, Qt::Unchecked, Qt::CheckStateRole));
    QVERIFY(model->setData(index, Qt::Checked, Qt::CheckStateRole));
    QVERIFY(root->customLayerOrder().indexOf(map) < root->customLayerOrder().indexOf(mask));
    QTRY_VERIFY(!canvas->layers().isEmpty()); QTRY_COMPARE(canvas->layers().first(), map);
  }
  void initTestCase() { PaleoTheme::pinRenderEnvironment(); }
  void offlineBasemapButtonAndFileMenuOpenProjectImportDialog() {
    Fixture f; QVERIFY(f.init()); QgisCanvasController controller; SelectionContext selection;
    PredictionWorkflow prediction(&f.processing, &f.layers); CompositionWorkflow composition(&f.processing, &f.layers);
    ValidationWorkflow validation(&f.layers, &f.store);
    PaleoMainWindow window(&controller, &f.project, &f.layers, nullptr, &selection);
    window.attachWorkflows(&prediction, &f.constraints, &composition, &validation, &f.importer, nullptr, &f.processing, &f.store, nullptr, nullptr, &f.tasks);
    auto *button = window.findChild<QPushButton *>("importOfflineBasemap"); QVERIFY(button); QVERIFY(button->isEnabled()); button->click();
    auto *dialog = window.findChild<OfflineBasemapDialog *>(); QVERIFY(dialog); QVERIFY(dialog->isVisible());
    auto *confirm = dialog->findChild<QPushButton *>("offlineBasemapConfirm"); QVERIFY(confirm); QVERIFY(!confirm->isEnabled());
    const auto qa = qEnvironmentVariable("PALEO_PALEOMAP_QA_DIR");
    if (!qa.isEmpty()) dialog->grab().save(qa + "/offline-basemap-import.png");
    dialog->findChild<QLineEdit *>("offlineBasemapPath")->setText(f.dir.filePath("missing.tif"));
    confirm->click(); QVERIFY(!dialog->findChild<QLabel *>("offlineBasemapError")->text().isEmpty());
    QVERIFY(f.importer.catalog()->assets().isEmpty()); dialog->reject(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    auto *action = window.findChild<QAction *>("projectOfflineBasemapAction"); QVERIFY(action); action->trigger();
    dialog = window.findChild<OfflineBasemapDialog *>(); QVERIFY(dialog); QVERIFY(dialog->isVisible()); dialog->reject();
    QVERIFY(f.project.mapConfiguration().basemapTopo.isEmpty());
  }
  void sourceDialogValidatesAndKeepsSelectionUntilConfirmation() {
    QTemporaryDir source; const auto path = source.filePath("points.xlsx");
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("fixture"); file.close();
    ImportSourceDialog dialog("wells");
    auto *confirm = dialog.findChild<QPushButton *>("importSourceConfirm"); QVERIFY(confirm);
    QVERIFY(!confirm->isEnabled()); QVERIFY(dialog.fileFilter().contains("*.xlsx"));
    QSignalSpy accepted(&dialog, &QDialog::accepted);
    dialog.accept(); QCOMPARE(accepted.count(), 0);
    dialog.setSources({path, path}); QCOMPARE(dialog.files().size(), 1); QVERIFY(confirm->isEnabled());
    dialog.setSources({source.filePath("missing.xlsx")}); dialog.accept(); QCOMPARE(accepted.count(), 0);
    QVERIFY(!dialog.findChild<QLabel *>("importSourceError")->text().isEmpty());
    dialog.setSources({path});
    auto *list = dialog.findChild<QListWidget *>("importSourceFileList"); list->item(0)->setSelected(true);
    dialog.findChild<QPushButton *>("importSourceRemoveFiles")->click(); QVERIFY(dialog.files().isEmpty()); QVERIFY(!confirm->isEnabled());
    dialog.setSources({path}, true); dialog.accept(); QCOMPARE(accepted.count(), 0); // 文件不能当目录
    dialog.setSources({source.path()}, true); QVERIFY(dialog.files().isEmpty()); dialog.accept(); QCOMPARE(accepted.count(), 1);
    QCOMPARE(dialog.folder(), source.path());
  }
  void everyImportButtonOpensSourceDialogAndCancelLeavesCatalogUntouched() {
    Fixture f; QVERIFY(f.init()); QgisCanvasController controller; SelectionContext selection;
    PredictionWorkflow prediction(&f.processing, &f.layers); CompositionWorkflow composition(&f.processing, &f.layers);
    ValidationWorkflow validation(&f.layers, &f.store);
    PaleoMainWindow window(&controller, &f.project, &f.layers, nullptr, &selection);
    window.attachWorkflows(&prediction, &f.constraints, &composition, &validation, &f.importer, nullptr, &f.processing, &f.store, nullptr, nullptr, &f.tasks);
    const QList<QPair<QString, QString>> buttons{{"importWells", "wells"}, {"importWellLogs", "well_log"},
        {"importSeismic", "seismic"}, {"importBoundary", "boundary"}, {"importFolder", "folder"},
        {"importFiles", "files"}, {"importPaleomap", "paleomap"}, {"importPaleomapFolder", "paleomap_folder"}};
    for (const auto &[name, kind] : buttons) {
      auto *button = window.findChild<QPushButton *>(name); QVERIFY2(button, qPrintable(name)); QVERIFY2(button->isEnabled(), qPrintable(name));
      bool shown = false, correctMode = false;
      QTimer::singleShot(0, &window, [&] {
        auto *dialog = window.findChild<ImportSourceDialog *>("dataImportSourceDialog");
        if (!dialog) { if (auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget())) modal->reject(); return; }
        shown = dialog->property("importKind").toString() == kind;
        correctMode = dialog->findChild<QComboBox *>("importSourceMode")->currentIndex() == (kind.endsWith("folder") ? 1 : 0);
        const auto qa = qEnvironmentVariable("PALEO_PALEOMAP_QA_DIR");
        if (!qa.isEmpty()) dialog->grab().save(qa + "/" + kind + "-import-source.png");
        dialog->reject();
      });
      button->click(); QVERIFY2(shown, qPrintable(name)); QVERIFY2(correctMode, qPrintable(name));
      QVERIFY(f.importer.catalog()->assets().isEmpty());
    }
    for (const auto &kind : projectClassifierTypes()) {
      auto *action = window.findChild<QAction *>("addData_" + kind); QVERIFY2(action, qPrintable(kind));
      bool shown = false;
      QTimer::singleShot(0, &window, [&] {
        if (auto *dialog = window.findChild<ImportSourceDialog *>("dataImportSourceDialog")) {
          shown = dialog->property("importKind").toString() == kind; dialog->reject();
        }
      });
      action->trigger(); QVERIFY2(shown, qPrintable(kind)); QVERIFY(f.importer.catalog()->assets().isEmpty());
    }
  }
  void boundaryButtonConfirmsAsSurveyAreaAndImports() {
    Fixture f; QVERIFY(f.init()); QTemporaryDir source; const auto path = source.filePath("polygon.geojson");
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{},"geometry":{"type":"Polygon","coordinates":[[[108,36],[108.1,36],[108.1,36.1],[108,36.1],[108,36]]]}}]})"); file.close();
    QgisCanvasController controller; SelectionContext selection;
    PredictionWorkflow prediction(&f.processing, &f.layers); CompositionWorkflow composition(&f.processing, &f.layers);
    ValidationWorkflow validation(&f.layers, &f.store);
    PaleoMainWindow window(&controller, &f.project, &f.layers, nullptr, &selection);
    window.attachWorkflows(&prediction, &f.constraints, &composition, &validation, &f.importer, nullptr, &f.processing, &f.store, nullptr, nullptr, &f.tasks);
    bool reviewed = false;
    QTimer poll; poll.setInterval(10); connect(&poll, &QTimer::timeout, &window, [&] {
      if (auto *dialog = window.findChild<ImportSourceDialog *>(); dialog && dialog->isVisible()) {
        dialog->setSources({path}); dialog->accept();
      }
      if (auto *dialog = window.findChild<QDialog *>("folderImportDialog"); dialog && dialog->isVisible()) {
        if (!reviewed) {
          auto *type = dialog->findChild<QComboBox *>("folderType0");
          reviewed = type && type->currentData().toString() == QStringLiteral("boundary");
          dialog->findChild<QPushButton *>("folderConfirmButton")->click();
        }
        if (auto *table = dialog->findChild<QTableWidget *>("folderTable"); reviewed && table && table->isEnabled() && !f.importer.catalog()->assets().isEmpty()) dialog->reject();
      }
    }); poll.start();
    window.findChild<QPushButton *>("importBoundary")->click();
    QTRY_VERIFY_WITH_TIMEOUT(reviewed, 20000); poll.stop();
    QTRY_COMPARE_WITH_TIMEOUT(f.importer.catalog()->assets().size(), 1, 20000);
    QCOMPARE(f.importer.catalog()->assets().first().type, QString("boundary"));
    QTRY_VERIFY_WITH_TIMEOUT(!f.importer.catalog()->entities("study_area").isEmpty(), 20000);
  }
  void selectedShapefileExpandsAndCopiesItsDataset() {
    Fixture f; QVERIFY(f.init()); QTemporaryDir source; const auto path = source.filePath("area.shp"); QVERIFY(shape(path));
    QFile fullStyle(source.filePath("area.shp.qml")); QVERIFY(fullStyle.open(QIODevice::WriteOnly)); fullStyle.write("<qgis/>"); fullStyle.close();
    const auto plan = buildIngestPlanForFiles({path, path}, *f.importer.catalog());
    QCOMPARE(plan.items.size(), 1); QCOMPARE(plan.items.first().members.size(), 6); // SHP/SHX/DBF/PRJ/QML/全名 QML
    auto selected = plan; selected.items[0].type = "boundary";
    const auto result = executeIngestPlan(selected, f.importer);
    QCOMPARE(result.size(), 1); QVERIFY2(!result.first().assetId.isEmpty(), qPrintable(result.first().message));
    const auto asset = f.importer.catalog()->assets().first();
    const auto managed = DataCatalog::resolvedVersionPath(f.dir.path(), f.importer.catalog()->currentVersion(asset.id));
    QVERIFY(QFileInfo::exists(QFileInfo(managed).absoluteDir().filePath("area.qml")));
    QVERIFY(QFileInfo::exists(QFileInfo(managed).absoluteDir().filePath("area.shp.qml")));
    QgsVectorLayer copied(managed, "copied", "ogr"); QVERIFY(copied.isValid()); QCOMPARE(copied.featureCount(), 1);
  }
  void boundaryFileAndFolderReachReviewAndCanCancel() {
    for (bool directory : {false, true}) {
      Fixture f; QVERIFY(f.init()); QTemporaryDir source; const auto path = source.filePath("area.shp"); QVERIFY(shape(path));
      QgisCanvasController controller; SelectionContext selection;
      PredictionWorkflow prediction(&f.processing, &f.layers); CompositionWorkflow composition(&f.processing, &f.layers);
      ValidationWorkflow validation(&f.layers, &f.store);
      PaleoMainWindow window(&controller, &f.project, &f.layers, nullptr, &selection);
      window.attachWorkflows(&prediction, &f.constraints, &composition, &validation, &f.importer, nullptr, &f.processing, &f.store, nullptr, nullptr, &f.tasks);
      bool reviewed = false, boundaryDefault = false;
      QTimer poll; poll.setInterval(10); connect(&poll, &QTimer::timeout, &window, [&] {
        if (auto *dialog = window.findChild<ImportSourceDialog *>(); dialog && dialog->isVisible()) {
          dialog->setSources({directory ? source.path() : path}, directory); dialog->accept();
        }
        if (auto *dialog = window.findChild<QDialog *>("folderImportDialog"); dialog && dialog->isVisible()) {
          reviewed = true; auto *type = dialog->findChild<QComboBox *>("folderType0");
          boundaryDefault = type && type->currentData().toString() == QStringLiteral("boundary");
          dialog->reject();
        }
      }); poll.start();
      window.findChild<QPushButton *>("importBoundary")->click();
      QTRY_VERIFY_WITH_TIMEOUT(reviewed, 20000); poll.stop(); QVERIFY(boundaryDefault);
      QVERIFY(f.importer.catalog()->assets().isEmpty());
    }
  }
  void paleomapSourceContinuesToHorizonDialogAndCancelDoesNotImport() {
    Fixture f; QVERIFY(f.init()); QTemporaryDir source; const auto path = source.filePath("D61.shp"); QVERIFY(shape(path));
    QgisCanvasController controller; SelectionContext selection;
    PredictionWorkflow prediction(&f.processing, &f.layers); CompositionWorkflow composition(&f.processing, &f.layers);
    ValidationWorkflow validation(&f.layers, &f.store);
    PaleoMainWindow window(&controller, &f.project, &f.layers, nullptr, &selection);
    window.attachWorkflows(&prediction, &f.constraints, &composition, &validation, &f.importer, nullptr, &f.processing, &f.store, nullptr, nullptr, &f.tasks);
    bool reviewed = false;
    QTimer::singleShot(0, &window, [&] {
      auto *dialog = window.findChild<ImportSourceDialog *>(); if (!dialog) return;
      dialog->setSources({path}); dialog->accept();
      QTimer::singleShot(0, &window, [&] {
        auto *settings = window.findChild<QDialog *>("paleomapImportDialog"); if (!settings) return;
        reviewed = settings->findChild<QComboBox *>("paleomapImportHorizon") != nullptr; settings->reject();
      });
    });
    window.findChild<QPushButton *>("importPaleomap")->click(); QVERIFY(reviewed); QVERIFY(f.importer.catalog()->assets().isEmpty());
  }
  void existingMapNormalizationDialogRunsTheRegisteredMapFlow() {
    Fixture f; QVERIFY(f.init()); QTemporaryDir source; const auto path = source.filePath("D61/map.shp");
    QVERIFY(microShape(path, {QVariantMap{{"microfacies", "河口坝"}}}, "SEDCLASS"));
    PaleoMapLibrary::Request request; request.files = {path}; request.horizon = "D61"; request.normalizeFacies = false;
    const auto imported = f.run(request); QVERIFY(imported.errors.isEmpty()); const auto id = imported.assets.first();
    QgisCanvasController controller; controller.setLayerResolver([&](const QString &layer) { return f.layers.layer(layer); });
    SelectionContext selection; selection.setActiveHorizon("D61"); PredictionWorkflow prediction(&f.processing, &f.layers);
    CompositionWorkflow composition(&f.processing, &f.layers); ValidationWorkflow validation(&f.layers, &f.store);
    PaleoMainWindow window(&controller, &f.project, &f.layers, nullptr, &selection);
    window.attachWorkflows(&prediction, &f.constraints, &composition, &validation, &f.importer, nullptr, &f.processing, &f.store, nullptr, nullptr, &f.tasks);
    auto *list = window.findChild<DataListPanel *>(); QVERIFY(list); list->refreshAssetTable(); list->selectAssetInViews(id);
    bool shown = false;
    QTimer::singleShot(0, [&] {
      auto *dialog = window.findChild<QDialog *>("paleomapNormalizeDialog");
      if (!dialog) return;
      auto *field = dialog->findChild<QLineEdit *>("paleomapNormalizeField");
      if (!field) { dialog->reject(); return; }
      shown = true; field->setText("SEDCLASS");
      const auto qa = qEnvironmentVariable("PALEO_PALEOMAP_QA_DIR");
      if (!qa.isEmpty()) dialog->grab().save(qa + "/normalize-existing-map.png");
      dialog->accept();
    });
    list->normalizeSelectedPaleomaps(); QVERIFY(shown);
    QTRY_COMPARE_WITH_TIMEOUT(f.importer.catalog()->currentVersion(id).stage, QString("DERIVED"), 20000);
    const auto qa = qEnvironmentVariable("PALEO_PALEOMAP_QA_DIR");
    if (!qa.isEmpty()) {
      list->refreshAssetTable(); list->selectAssetInViews(id);
      auto *tree = list->findChild<QTreeWidget *>("dataTree"); QVERIFY(tree); tree->expandAll();
      const auto theme = PaleoTheme::currentTheme();
      list->resize(520, 700); list->show(); QTest::qWait(100); list->grab().save(qa + "/data-map-actions.png");
      auto *panels = window.findChild<QToolButton *>("panelsMenuButton"); QVERIFY(panels); panels->click();
      auto *toggle = window.findChild<QAction *>("themeToggleAction"); QVERIFY(toggle);
      auto *menu = qobject_cast<QMenu *>(toggle->parent()); QVERIFY(menu); menu->hide();
      toggle->setChecked(true); QTest::qWait(100); list->grab().save(qa + "/data-map-actions-dark.png");
      toggle->setChecked(theme == PaleoTheme::Theme::Dark); menu->close();
    }
  }
  void existingMapsNormalizeFromStoredOriginalWithoutDuplicatingAssets() {
    Fixture f; QVERIFY(f.init()); QTemporaryDir source;
    const auto path = source.filePath("D61/legacy.shp");
    QVERIFY(microShape(path, {QVariantMap{{"microfacies", "河口坝"}}}, "SEDCLASS"));
    PaleoMapLibrary::Request request; request.files = {path}; request.horizon = "D61"; request.normalizeFacies = false;
    const auto imported = f.run(request); QVERIFY(imported.errors.isEmpty()); QCOMPARE(imported.assets.size(), 1);
    auto *cat = f.importer.catalog(); const auto raw = cat->currentVersion(imported.assets.first());
    QCOMPARE(raw.stage, QString("RAW")); const auto sha = DataCatalog::sha256FileHex(cat->versionFilePath(raw));
    QVERIFY(QDir(source.filePath("D61")).removeRecursively());
    request = {}; request.assetIds = {raw.assetId, raw.assetId}; request.microfaciesField = "SEDCLASS";
    const auto processed = f.run(request); QVERIFY2(processed.errors.isEmpty(), qPrintable(processed.errors.join("\n")));
    QCOMPARE(processed.assets, QStringList{raw.assetId}); QCOMPARE(cat->assets().size(), 1);
    const auto current = cat->currentVersion(raw.assetId);
    QCOMPARE(current.stage, QString("DERIVED")); QCOMPARE(current.extra.value("horizon").toString(), QString("D61"));
    QCOMPARE(current.sourceUri, path); QCOMPARE(current.parentVersionIds, QStringList{raw.id});
    QCOMPARE(cat->versionsForAsset(raw.assetId).size(), 2); QCOMPARE(DataCatalog::sha256FileHex(cat->versionFilePath(raw)), sha);
    const auto id = current.extra.value("layer_id").toString();
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(id)); QVERIFY(layer);
    auto features = layer->getFeatures(); QgsFeature feature; QVERIFY(features.nextFeature(feature));
    QCOMPARE(feature.attribute("microfacies").toString(), QString("河口坝"));
    QCOMPARE(feature.attribute("facies_name").toString(), QString("三角洲相"));
    QCOMPARE(feature.attribute("subfacies").toString(), QString("三角洲前缘"));
    QVERIFY(!FaciesCatalog::fillPath(feature.attribute("texture").toString()).isEmpty());
    const auto again = f.run(request); QVERIFY(again.errors.isEmpty()); QCOMPARE(again.unchanged, 1);
    QCOMPARE(cat->currentVersion(raw.assetId).id, current.id);
    for (const auto &mode : {QString("facies"), QString("sub_facies"), QString("micro_facies")})
      QVERIFY(f.work.setDisplayMode(id, mode, nullptr));
    QVERIFY(!f.work.copyForEditing(id, {}, nullptr).isEmpty());
  }
  void batchNormalizationKeepsEachMapsHorizon() {
    Fixture f; QVERIFY(f.init()); QTemporaryDir source;
    PaleoMapLibrary::Request request; request.horizon = "D61"; request.normalizeFacies = false;
    for (const auto &h : {QString("D61"), QString("D62")}) {
      const auto path = source.filePath(h + "/map.shp");
      QVERIFY(microShape(path, {QVariantMap{{"microfacies", "河口坝"}}})); request.files << path;
    }
    const auto imported = f.run(request); QVERIFY(imported.errors.isEmpty()); QCOMPARE(imported.assets.size(), 2);
    QMap<QString, QString> horizons;
    for (const auto &id : imported.assets) horizons[id] = f.importer.catalog()->currentVersion(id).extra.value("horizon").toString();
    request = {}; request.assetIds = imported.assets;
    const auto processed = f.run(request); QVERIFY2(processed.errors.isEmpty(), qPrintable(processed.errors.join("\n"))); QCOMPARE(processed.assets.size(), 2);
    for (const auto &id : imported.assets) {
      const auto version = f.importer.catalog()->currentVersion(id); QCOMPARE(version.stage, QString("DERIVED"));
      QCOMPARE(version.extra.value("horizon").toString(), horizons.value(id));
    }
  }
  void existingMapWithoutMicroFieldReportsActionableFailure() {
    Fixture f; QVERIFY(f.init()); QTemporaryDir source; const auto path = source.filePath("map.shp");
    QVERIFY(microShape(path, {QVariantMap{{"microfacies", "河口坝"}}}, "SEDCLASS"));
    PaleoMapLibrary::Request request; request.files = {path}; request.horizon = "D61";
    const auto imported = f.run(request); QVERIFY(imported.errors.isEmpty()); auto *cat = f.importer.catalog();
    const auto raw = cat->currentVersion(imported.assets.first()); request = {}; request.assetIds = {raw.assetId};
    const auto rejected = f.run(request); QVERIFY(rejected.assets.isEmpty()); QVERIFY(!rejected.errors.isEmpty());
    QVERIFY(rejected.errors.join("\n").contains(QString::fromUtf8("微相字段")));
    QCOMPARE(cat->currentVersion(raw.assetId).id, raw.id);
  }
  void recyclingDuringNormalizationRejectsPublication() {
    Fixture f; QVERIFY(f.init()); QTemporaryDir source; const auto path = source.filePath("map.shp");
    QVERIFY(microShape(path, {QVariantMap{{"microfacies", "河口坝"}}}));
    PaleoMapLibrary::Request request; request.files = {path}; request.horizon = "D61"; request.normalizeFacies = false;
    const auto imported = f.run(request); QVERIFY(imported.errors.isEmpty()); auto *cat = f.importer.catalog();
    const auto raw = cat->currentVersion(imported.assets.first()); request = {}; request.assetIds = {raw.assetId};
    bool called = false; PaleoMapLibrary::Result result;
    QVERIFY(PaleoMapLibrary::importMaps(cat, &f.layers, &f.tasks, request, this, [&](const auto &r) { result = r; called = true; }));
    paleo::dataops::RecycleBin bin; bin.load(cat); bin.remove(raw.assetId, "paleogeographic_map", "map", "test"); QVERIFY(bin.save());
    QTRY_VERIFY_WITH_TIMEOUT(called, 20000); QVERIFY(!result.errors.isEmpty()); QVERIFY(result.assets.isEmpty());
    QCOMPARE(cat->currentVersion(raw.assetId).id, raw.id); QCOMPARE(cat->versionsForAsset(raw.assetId).size(), 1);
    QString error; QVERIFY(!PaleoMapLibrary::importMaps(cat, &f.layers, &f.tasks, request, this, {}, &error)); QVERIFY(!error.isEmpty());
  }
  // C-10（full-audit-20261020）：#579 的回收防线只盖 assetIds 路径——文件重导入
  // 命中软删资产必须拒绝（不得「导入成功但数据树仍隐藏」，也不得在恢复时产生
  // 同源双资产）；declareMaps 不得为删除态资产声明图层（画布/数据树可见性一致）。
  void fileReimportOfSoftDeletedMapRejectedAndDeclareMapsSkipsRecycled() {
    Fixture f; QVERIFY(f.init()); QTemporaryDir source; const auto path = source.filePath("D61/map.shp");
    QVERIFY(microShape(path, {QVariantMap{{"microfacies", "河口坝"}}}));
    PaleoMapLibrary::Request request; request.files = {path}; request.horizon = "D61"; request.normalizeFacies = false;
    const auto imported = f.run(request); QVERIFY(imported.errors.isEmpty()); auto *cat = f.importer.catalog();
    const auto id = imported.assets.first();
    const auto versionsBefore = cat->versionsForAsset(id).size();
    paleo::dataops::RecycleBin bin; bin.load(cat);
    bin.remove(id, "paleogeographic_map", "map", "test"); QVERIFY(bin.save());

    // 同文件同层位重导入：命中软删资产 → 拒绝并提示，catalog 不动
    const auto reimported = f.run(request);
    QVERIFY(!reimported.errors.isEmpty());
    QVERIFY(reimported.assets.isEmpty());
    QVERIFY2(reimported.errors.join("\n").contains(QString::fromUtf8("可回收清单")),
             qPrintable(reimported.errors.join("\n")));
    QCOMPARE(cat->versionsForAsset(id).size(), versionsBefore);

    // declareMaps：删除态资产不进图层声明；恢复后重新声明回归
    QTemporaryDir declareDir;
    QgisProjectService project2; LayerManifest manifest2{declareDir.filePath("m.sqlite")};
    QgisLayerService layers2{&project2, &manifest2};
    QVERIFY(project2.createProject(declareDir.filePath("p.qgz"))); QVERIFY(manifest2.open());
    QVERIFY(PaleoMapLibrary::declareMaps(cat, &layers2).isEmpty());
    const auto current = cat->currentVersion(id);
    const QString removedLayerId = current.extra.value("layer_id", "paleomap." + current.id).toString();
    QVERIFY(!removedLayerId.isEmpty());
    QVector<LayerDeclaration> rows; QString manifestError;
    QVERIFY(manifest2.readAll(&rows, &manifestError));
    QStringList declaredIds; for (const auto &row : rows) declaredIds << row.layerId;
    QVERIFY2(!declaredIds.contains(removedLayerId), qPrintable(removedLayerId));
    QVERIFY(bin.restore(id)); QVERIFY(bin.save());
    QVERIFY(PaleoMapLibrary::declareMaps(cat, &layers2).isEmpty());
    rows.clear(); QVERIFY(manifest2.readAll(&rows, &manifestError));
    declaredIds.clear(); for (const auto &row : rows) declaredIds << row.layerId;
    QVERIFY(declaredIds.contains(removedLayerId));
  }
  // C-14（full-audit-20261020）：copyDataset 失败自清——第二个成员 copy 失败
  //（目标已存在，QFile::copy 拒绝覆写）时整目录回收，不留半拷孤儿字节。
  void copyDatasetFailureCleansDirectory() {
    QTemporaryDir source, target;
    const auto path = source.filePath("half.shp");
    QVERIFY(shape(path));
    QDir().mkpath(target.path());
    // 预置目标目录内同名 .shx：拷贝 .shp 成功、拷 .shx 时 QFile::copy 失败。
    QFile preexisting(target.filePath("half.shx"));
    QVERIFY(preexisting.open(QIODevice::WriteOnly)); preexisting.close();
    QString error;
    const auto copied = PaleoMapFiles::copyDataset(path, target.path(), {}, &error);
    QVERIFY(copied.isEmpty());
    QVERIFY2(!error.isEmpty(), "copy 失败必须报错");
    QVERIFY2(!QFile::exists(target.filePath("half.shp")), "失败后目录必须自清");
    QVERIFY(QFile::exists(source.filePath("half.shp"))); // 源不动
  }
  // C-11（full-audit-20261020）：角点是 SEG-Y 道头原值（单位随工区 CRS）——
  // 度制坐标（|x|≤360 且 |y|≤90）下 /1e6 是量级失真的伪面积，surveyArea 如实
  // 返回 -1（消费端按「未登记边界」兜底）；米制照常计算。
  void surveyAreaRejectsDegreeLikeCorners() {
    QVector<QVector<QPair<double, double>>> degreePoly;
    degreePoly << QVector<QPair<double, double>>{{1.0, 1.0}, {2.0, 1.0}, {2.0, 2.0}, {1.0, 2.0}};
    QCOMPARE(PaleoMapSource::surveyArea(degreePoly), -1.0);
    QVector<QVector<QPair<double, double>>> meterPoly;
    meterPoly << QVector<QPair<double, double>>{{1000.0, 2000.0}, {6000.0, 2000.0},
                                                {6000.0, 7000.0}, {1000.0, 7000.0}};
    QCOMPARE(PaleoMapSource::surveyArea(meterPoly), 25.0); // 5km × 5km
  }
  // B-03/A-14 导入侧（full-audit-20261020）：members() 消费 domain/sidecarvocab
  // 单一词表——dbf 属性索引（ain/aih）与统计文件随主件拷贝（此前导入不全）。
  void paleomapMembersIncludeDbfIndexSidecars() {
    QTemporaryDir source;
    const auto path = source.filePath("idx.shp");
    QVERIFY(shape(path));
    const QDir dir = QFileInfo(path).absoluteDir();
    for (const char *ext : {"ain", "aih", "atx"})
      QVERIFY(QFile(dir.filePath(QStringLiteral("idx.%1").arg(QLatin1String(ext)))).open(QIODevice::WriteOnly));
    QString err;
    const auto got = PaleoMapFiles::members(path, &err);
    QVERIFY2(!got.isEmpty(), qPrintable(err));
    QVERIFY(got.contains(dir.filePath(QStringLiteral("idx.ain"))));
    QVERIFY(got.contains(dir.filePath(QStringLiteral("idx.aih"))));
    QVERIFY(got.contains(dir.filePath(QStringLiteral("idx.atx"))));
    QVERIFY(got.contains(dir.filePath(QStringLiteral("idx.qml")))); // shape() 附带样式文件同卷
  }
  void existingMapMissingCompanionDoesNotPublish() {
    Fixture f; QVERIFY(f.init()); QTemporaryDir source; const auto path = source.filePath("map.shp");
    QVERIFY(microShape(path, {QVariantMap{{"microfacies", "河口坝"}}}));
    PaleoMapLibrary::Request request; request.files = {path}; request.horizon = "D61"; request.normalizeFacies = false;
    const auto imported = f.run(request); QVERIFY(imported.errors.isEmpty()); auto *cat = f.importer.catalog();
    const auto raw = cat->currentVersion(imported.assets.first());
    const auto companion = QFileInfo(cat->versionFilePath(raw)).absoluteDir().filePath("map.dbf");
    QVERIFY(QFile::setPermissions(companion, QFile::permissions(companion) | QFileDevice::WriteOwner)); QVERIFY(QFile::remove(companion));
    request = {}; request.assetIds = {raw.assetId}; const auto rejected = f.run(request);
    QVERIFY(!rejected.errors.isEmpty()); QVERIFY(rejected.assets.isEmpty()); QCOMPARE(cat->currentVersion(raw.assetId).id, raw.id);
    QCOMPARE(cat->versionsForAsset(raw.assetId).size(), 1);
  }
  void purgingCompoundDatasetProtectsLegacySharedReferences() {
    Fixture f; QVERIFY(f.init()); QTemporaryDir source; const auto path = source.filePath("map.shp");
    QVERIFY(shape(path)); PaleoMapLibrary::Request request; request.files = {path}; request.horizon = "D61";
    const auto imported = f.run(request); QVERIFY(imported.errors.isEmpty()); auto *cat = f.importer.catalog();
    const auto raw = cat->currentVersion(imported.assets.first());
    CatalogAsset alias{cat->nextAssetId(), "reference", "shp", "shared"}; QVERIFY(cat->addAsset(alias));
    CatalogVersion version; version.id = cat->nextVersionId(); version.assetId = alias.id; version.stage = "RAW";
    version.versionNumber = 1; version.managed = false; version.path = cat->versionFilePath(raw); version.sha256 = raw.sha256;
    QVERIFY(cat->addVersion(version));
    const auto purged = paleo::assetops::purgeAssets(cat, f.dir.path(), {raw.assetId});
    QVERIFY(purged.failedAssets.isEmpty()); QCOMPARE(purged.purgedAssetIds, QStringList{raw.assetId});
    for (const auto &value : raw.extra.value("bundle_members").toList())
      QVERIFY(QFile::exists(QFileInfo(version.path).absoluteDir().filePath(value.toMap().value("file").toString())));
  }
  void dataManagementDeleteIsVisibleRecoverableAndCompoundPurgeKeepsExternalOriginal() {
    Fixture f; QVERIFY(f.init()); QTemporaryDir source; const auto path = source.filePath("D61/map.shp");
    QVERIFY(microShape(path, {QVariantMap{{"microfacies", "河口坝"}}}));
    PaleoMapLibrary::Request request; request.files = {path}; request.horizon = "D61";
    const auto imported = f.run(request); QVERIFY(imported.errors.isEmpty()); const auto id = imported.assets.first(); auto *cat = f.importer.catalog();
    PreviewDocService doc(&f.importer); DataListPanel list; list.setDocService(&doc); list.refreshAssetTable();
    auto *remove = list.findChild<QToolButton *>("dataDeleteButton"); QVERIFY(remove); QVERIFY(!remove->isEnabled());
    // §35/D-11：禁用必须带 reason tooltip；启用后还原动作说明。
    QVERIFY2(remove->toolTip().contains(QString::fromUtf8("请先在列表中选择数据")), qPrintable(remove->toolTip()));
    list.selectAssetInViews(id); QVERIFY(remove->isEnabled());
    QVERIFY2(!remove->toolTip().contains(QString::fromUtf8("请先")), qPrintable(remove->toolTip()));
    auto *normalize = list.findChild<QToolButton *>("dataNormalizePaleomapButton"); QVERIFY(normalize); QVERIFY(normalize->isEnabled());
    QSignalSpy intent(&list, &DataListPanel::normalizePaleomapsRequested); normalize->click(); QCOMPARE(intent.size(), 1);
    QCOMPARE(intent.first().first().toStringList(), QStringList{id});
    QTimer::singleShot(0, [] { auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()); if (dialog) dialog->done(QMessageBox::No); });
    remove->click(); QVERIFY(!list.opsContext().recycle->isRemoved(id));
    QTimer::singleShot(0, [] { auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()); if (dialog) dialog->done(QMessageBox::Yes); });
    remove->click(); QVERIFY(list.opsContext().recycle->isRemoved(id)); QVERIFY(list.currentAssetSelection().isEmpty());
    QVERIFY(!remove->isEnabled()); QVERIFY(!normalize->isEnabled());
    list.undoOp(); QVERIFY(!list.opsContext().recycle->isRemoved(id));
    list.selectAssetInViews("project_dashboard"); QVERIFY(list.currentAssetSelection().isEmpty());
    QVERIFY(!remove->isEnabled()); QVERIFY(!normalize->isEnabled());
    QStringList files;
    for (const auto &version : cat->versionsForAsset(id)) {
      files << cat->versionFilePath(version);
      for (const auto &value : version.extra.value("bundle_members").toList())
        files << QFileInfo(cat->versionFilePath(version)).absoluteDir().filePath(value.toMap().value("file").toString());
    }
    files.removeDuplicates(); for (const auto &file : files) QVERIFY(QFile::exists(file));
    const auto purged = paleo::assetops::purgeAssets(cat, f.dir.path(), {id});
    QVERIFY2(purged.failedAssets.isEmpty(), qPrintable(purged.failedAssets.join("\n"))); QCOMPARE(purged.purgedAssetIds, QStringList{id});
    for (const auto &file : files) QVERIFY2(!QFile::exists(file), qPrintable(file));
    QVERIFY(QFile::exists(path)); QVERIFY(QFile::exists(source.filePath("D61/map.dbf")));
  }
  void legacyMicrofacFieldUsesProjectHierarchy() {
    Fixture f; QVERIFY(f.init()); QString error;
    auto schema = FaciesCatalog::defaults();
    auto channel = schema[4].toMap(); channel["code"] = 77; schema[4] = channel;
    QVERIFY(f.work.saveFacies("D61", schema, &error));
    QTemporaryDir source;
    const auto path = source.filePath("D61/historical.shp");
    QVERIFY(microShape(path, {QVariantMap{{"microfacies", "分流河道"},
        {"facies", "三角洲相"}, {"subfacies", "三角洲平原"}}}, "microfac"));
    PaleoMapLibrary::Request request; request.files = {path}; request.horizon = "D61";
    const auto imported = f.run(request);
    QVERIFY2(imported.errors.isEmpty(), qPrintable(imported.errors.join("\n")));
    QCOMPARE(imported.assets.size(), 1);
    const auto version = f.importer.catalog()->currentVersion(imported.assets.first());
    QCOMPARE(version.stage, QString("DERIVED"));
    QCOMPARE(version.extra.value("source_microfacies_field").toString(), QString("microfac"));
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(version.extra.value("layer_id").toString(), &error));
    QVERIFY2(layer, qPrintable(error));
    const auto feature = layer->getFeature(1);
    QCOMPARE(feature.attribute("facies_code").toInt(), 77);
    QCOMPARE(feature.attribute("facies_name").toString(), QString("三角洲相"));
    QCOMPARE(feature.attribute("subfacies").toString(), QString("三角洲平原"));
    QCOMPARE(feature.attribute("microfacies").toString(), QString("分流河道"));
    QCOMPARE(feature.attribute("texture").toString(), channel.value("texture").toString());
  }
  void importedMicrofaciesUseProjectTexturesAndThreeLevelEditing() {
    Fixture f; QVERIFY(f.init()); QString error;
    auto projectSchema = FaciesCatalog::defaults(); auto mouth = projectSchema[1].toMap(); mouth["code"] = 77; projectSchema[1] = mouth;
    QVERIFY2(f.work.saveFacies("D61", projectSchema, &error), qPrintable(error));
    QTemporaryDir source; const auto path = source.filePath("D61/micro.shp");
    QVERIFY(microShape(path, {QVariantMap{{"microfacies", "水下河道"}}, QVariantMap{{"microfacies", "河口坝"}}}));
    const auto originalHash = DataCatalog::sha256FileHex(path);
    PaleoMapLibrary::Request request; request.files = {path}; request.horizon = "D61";
    const auto result = f.run(request); QVERIFY2(result.errors.isEmpty(), qPrintable(result.errors.join("\n"))); QCOMPARE(result.assets.size(), 1);
    auto *cat = f.importer.catalog(); const auto version = cat->currentVersion(result.assets.first());
    QCOMPARE(version.stage, QString("DERIVED")); QVERIFY(version.path.endsWith(".gpkg"));
    const auto raw = cat->versionById(version.extra.value("source_raw_version_id").toString());
    QCOMPARE(raw.stage, QString("RAW")); QCOMPARE(version.parentVersionIds, QStringList{raw.id});
    QCOMPARE(cat->versionsForAsset(version.assetId).size(), 2); QCOMPARE(DataCatalog::sha256FileHex(path), originalHash);
    const auto id = version.extra.value("layer_id").toString();
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(id, &error)); QVERIFY2(layer, qPrintable(error));
    QVERIFY(f.work.supportsFaciesDisplay(id)); QCOMPARE(layer->featureCount(), 2); QVERIFY(layer->readOnly());
    auto features = layer->getFeatures(); QgsFeature first, second; QVERIFY(features.nextFeature(first)); QVERIFY(features.nextFeature(second));
    QCOMPARE(first.attribute("facies_code").toInt(), 1); QCOMPARE(second.attribute("facies_code").toInt(), 77);
    QCOMPARE(first.attribute("facies_name").toString(), QString("三角洲相")); QCOMPARE(first.attribute("subfacies").toString(), QString("三角洲前缘"));
    QCOMPARE(first.attribute("microfacies").toString(), QString("水下分流河道")); QCOMPARE(first.attribute("src_note").toString(), QString("preserved"));
    QVERIFY(!FaciesCatalog::fillPath(first.attribute("texture").toString()).isEmpty());
    for (const auto &level : {QString("facies"), QString("sub_facies"), QString("micro_facies")}) {
      QVERIFY2(f.work.setDisplayMode(id, level, &error), qPrintable(error));
      const auto entries = FaciesHierarchyRenderer::legend(layer, version.extra.value("facies").toList(), level);
      QCOMPARE(entries.size(), level == "micro_facies" ? 2 : 1);
      const auto symbols = FaciesMapLegend::symbolEntries(layer, entries);
      for (const auto &row : symbols) { QVERIFY(!row.toMap().value("texture").toString().isEmpty()); QVERIFY(!row.toMap().value("legend_icon").value<QIcon>().isNull()); }
    }
    const auto draft = f.work.copyForEditing(id, {}, &error); QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *working = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft, &error)); QVERIFY2(working, qPrintable(error));
    auto iterator = working->getFeatures(); QgsFeature feature; QVERIFY(iterator.nextFeature(feature)); const auto geometry = feature.geometry().asWkt();
    QVERIFY2(f.work.assignHierarchy(draft, {feature.id()}, 77, "micro_facies", &error), qPrintable(error));
    QCOMPARE(working->getFeature(feature.id()).attribute("microfacies").toString(), QString("河口坝"));
    QCOMPARE(working->getFeature(feature.id()).attribute("facies_name").toString(), QString("三角洲相"));
    QCOMPARE(working->getFeature(feature.id()).geometry().asWkt(), geometry);
    QCOMPARE(layer->getFeature(first.id()).attribute("microfacies").toString(), QString("水下分流河道"));
    QCOMPARE(DataCatalog::sha256FileHex(path), originalHash);
    const auto repeated = f.run(request); QVERIFY(repeated.errors.isEmpty()); QCOMPARE(repeated.unchanged, 1); QCOMPARE(cat->currentVersion(version.assetId).id, version.id);
    PreviewDocService doc(&f.importer); DataPreviewTabs preview; preview.setDocService(&doc); preview.setProject(f.project.project()); preview.openAsset(version.assetId);
    bool styled = false;
    for (auto *canvas : preview.findChildren<QgsMapCanvas *>()) for (auto *map : canvas->layers())
      if (auto *vector = qobject_cast<QgsVectorLayer *>(map); vector && vector->fields().indexOf("facies_code") >= 0) {
        styled = true; QCOMPARE(vector->customProperty("paleo/faciesDisplayLegend").toList().size(), 2); QVERIFY(vector->legend());
      }
    QVERIFY(styled);
    const auto qa = qEnvironmentVariable("PALEO_PALEOMAP_QA_DIR");
    if (!qa.isEmpty()) { preview.resize(1000, 700); preview.show(); QTest::qWait(200); QVERIFY(preview.grab().save(qa + "/imported-microfacies.png")); }
    paleo::exporting::DataRequest exportRequest; exportRequest.versionIds = {raw.id}; exportRequest.destination = source.filePath("original-export");
    const auto plan = paleo::exporting::plan(*cat, f.dir.path(), exportRequest); QVERIFY2(plan.failures.isEmpty(), qPrintable(plan.failures.join("\n")));
    const auto exported = paleo::exporting::execute(plan); QVERIFY2(exported.complete(), qPrintable(exported.error));
    bool exportedOriginal = false; for (const auto &file : exported.files) if (file.endsWith(".shp")) { exportedOriginal = true; QCOMPARE(DataCatalog::sha256FileHex(file), originalHash); }
    QVERIFY(exportedOriginal);
  }
  void explicitMicrofaciesFieldAndReferenceFallback() {
    Fixture f; QVERIFY(f.init()); QTemporaryDir source;
    const auto path = source.filePath("custom.shp"); QVERIFY(microShape(path, {QVariantMap{{"microfacies", "河口坝"}}}, "SEDCLASS"));
    PaleoMapLibrary::Request request; request.files = {path}; request.horizon = "D61";
    const auto reference = f.run(request); QVERIFY(reference.errors.isEmpty()); QCOMPARE(reference.assets.size(), 1);
    auto *cat = f.importer.catalog(); const auto raw = cat->currentVersion(reference.assets.first()); QCOMPARE(raw.stage, QString("RAW"));
    request.microfaciesField = "SEDCLASS";
    const auto converted = f.run(request); QVERIFY2(converted.errors.isEmpty(), qPrintable(converted.errors.join("\n"))); QCOMPARE(converted.assets, reference.assets);
    const auto current = cat->currentVersion(raw.assetId); QCOMPARE(current.stage, QString("DERIVED")); QCOMPARE(cat->versionsForAsset(raw.assetId).size(), 2);
    QCOMPARE(current.extra.value("source_raw_version_id").toString(), raw.id);
    const auto unknown = source.filePath("unknown.shp"); QVERIFY(microShape(unknown, {QVariantMap{{"microfacies", "陌生微相"}}}));
    request.microfaciesField.clear(); request.files = {unknown}; const auto rejected = f.run(request);
    QVERIFY(!rejected.errors.isEmpty()); QCOMPARE(rejected.assets.size(), 1);
    const auto unclassified = cat->currentVersion(rejected.assets.first()); QCOMPARE(unclassified.stage, QString("RAW")); QVERIFY(!unclassified.extra.value("mapping_product").toBool());
    const auto complete = source.filePath("complete.shp"); QVERIFY(microShape(complete, {QVariantMap{{"microfacies", "自定义微相"}, {"facies", "自定义相"}, {"subfacies", "自定义亚相"}}}));
    request.files = {complete}; const auto custom = f.run(request); QVERIFY2(custom.errors.isEmpty(), qPrintable(custom.errors.join("\n")));
    QCOMPARE(cat->currentVersion(custom.assets.first()).stage, QString("DERIVED"));
    const auto disabled = source.filePath("disabled.shp"); QVERIFY(microShape(disabled, {QVariantMap{{"microfacies", "河口坝"}}}));
    request.files = {disabled}; request.normalizeFacies = false; const auto plain = f.run(request); QVERIFY(plain.errors.isEmpty());
    QCOMPARE(cat->currentVersion(plain.assets.first()).stage, QString("RAW"));
  }
  void schemaChangeRegeneratesFromRawAndAmbiguousNamesRequireParents() {
    Fixture f; QVERIFY(f.init()); QTemporaryDir source; QString error;
    const auto path = source.filePath("micro.shp"); QVERIFY(microShape(path, {QVariantMap{{"microfacies", "河口坝"}}}));
    PaleoMapLibrary::Request request; request.files = {path}; request.horizon = "D61";
    const auto imported = f.run(request); QVERIFY(imported.errors.isEmpty()); const auto asset = imported.assets.first(); auto *cat = f.importer.catalog();
    const auto first = cat->currentVersion(asset); const auto rawId = first.extra.value("source_raw_version_id").toString();
    auto schema = FaciesCatalog::defaults(); auto mouth = schema[1].toMap(); mouth["code"] = 99; mouth["texture"] = "strata/micro_sheet_sand_fill.svg"; schema[1] = mouth;
    QVERIFY(f.work.saveFacies("D61", schema, &error)); const auto changed = f.run(request); QVERIFY2(changed.errors.isEmpty(), qPrintable(changed.errors.join("\n")));
    const auto second = cat->currentVersion(asset); QVERIFY(first.id != second.id); QCOMPARE(second.extra.value("source_raw_version_id").toString(), rawId);
    QCOMPARE(cat->versionsForAsset(asset).size(), 3); const auto list = second.extra.value("facies").toList(); QCOMPARE(FaciesTaxonomy::resolve(list, "河口坝").value("code").toInt(), 99);
    QFile::setPermissions(cat->versionFilePath(second), QFileDevice::ReadOwner | QFileDevice::WriteOwner); QVERIFY(QFile::remove(cat->versionFilePath(second)));
    const auto restored = f.run(request); QVERIFY(restored.errors.isEmpty()); QVERIFY(QFile::exists(cat->versionFilePath(cat->currentVersion(asset)))); QCOMPARE(cat->versionsForAsset(asset).size(), 4);
    const QVariantList ambiguous{QVariantMap{{"code", 1}, {"name", "同名微相"}, {"facies", "相甲"}, {"subfacies", "亚相甲"}, {"microfacies", "同名微相"}},
      QVariantMap{{"code", 2}, {"name", "同名微相"}, {"facies", "相乙"}, {"subfacies", "亚相乙"}, {"microfacies", "同名微相"}}};
    QVERIFY(PaleoMapFacies::resolve(ambiguous, {QVariantMap{{"microfacies", "同名微相"}}}, &error).isEmpty()); QVERIFY(error.contains("多个归属"));
    const auto exact = PaleoMapFacies::resolve(ambiguous, {QVariantMap{{"microfacies", "同名微相"}, {"facies", "相乙"}, {"subfacies", "亚相乙"}}}, &error);
    QCOMPARE(exact.value("codes").toList(), QVariantList{2});
  }
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
    QCOMPARE(page.findChild<QLabel *>("count_well")->text(), QString("1 / 1")); // 只列本工程出现过的层位（D61）
    QCOMPARE(page.findChild<QProgressBar *>("coverage_well")->value(), 1);
    const auto cards = page.findChildren<QFrame *>("dashboardHorizonCard"); QCOMPARE(cards.size(), 1);
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
    QCOMPARE(page.findChildren<QFrame *>("dashboardHorizonCard").size(), 1);
  }
  void dashboardSurveyMapSurvivesImportStorm() {
    // 回归（2026-10-24 创建工程即崩 0xc0000374）：导入期间 DataCatalog::changed
    // 密集触发防抖刷新，井位内存层 truncate/重灌与在飞渲染作业并发即堆损坏。
    // 现在刷新先同步 stopRendering 再改层；页面隐藏时跳过地图刷新，显示时补刷。
    Fixture f; QVERIFY(f.init()); auto *cat = f.importer.catalog();
    CatalogEntity survey; survey.id = "survey"; survey.entityType = "seismic_survey"; survey.name = "S1";
    survey.corners = {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}}; QVERIFY(cat->addEntity(survey));
    ProjectStatisticsPage page(cat); page.resize(700, 900); page.show(); QTest::qWait(150);
    auto *wells = page.findChild<QgsVectorLayer *>("dashboardWellsLayer"); QVERIFY(wells);
    for (int round = 0; round < 6; ++round) {
      for (int i = 0; i < 6; ++i) {
        CatalogEntity well; well.id = QString("storm-%1-%2").arg(round).arg(i);
        well.entityType = "well"; well.name = QString("W%1%2").arg(round).arg(i);
        well.hasSurface = true; well.surfaceX = 500.0 * i + round * 37; well.surfaceY = 400.0 * i + round * 53;
        QVERIFY(cat->addEntity(well));
      }
      QTest::qWait(160); // 越过 100ms 防抖窗口：本轮刷新与上一轮渲染作业并发。
    }
    QTRY_COMPARE(wells->featureCount(), 36);
    page.hide();
    CatalogEntity extra; extra.id = "hidden-well"; extra.entityType = "well"; extra.name = "H1";
    extra.hasSurface = true; extra.surfaceX = 1; extra.surfaceY = 1; QVERIFY(cat->addEntity(extra));
    QTest::qWait(160);
    QCOMPARE(wells->featureCount(), 36); // 隐藏期间地图层不动。
    page.show();
    QTRY_COMPARE(wells->featureCount(), 37); // 重新显示补刷。
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
    QVERIFY(stats.value("horizons").toList().isEmpty()); // 无层位资料 → 不显示内置默认层位
    ProjectStatisticsPage page(cat); QCOMPARE(page.findChildren<QFrame *>("dashboardHorizonCard").size(), stats.value("horizons").toList().size());
  }
  void emptyProjectHasNoDatasetHorizons() {
    // 新建空工程：内置默认层序界面名单（竞赛数据集 C3…D72）不得出现在分层位进度。
    Fixture f; QVERIFY(f.init()); auto *cat = f.importer.catalog();
    const auto stats = ProjectAreaSummary::snapshot(cat);
    QVERIFY(stats.value("horizons").toList().isEmpty());
    for (const auto &v : stats.value("stage_counts").toMap()) QCOMPARE(v.toInt(), 0);
    ProjectStatisticsPage page(cat);
    QVERIFY(page.findChildren<QFrame *>("dashboardHorizonCard").isEmpty());
    QCOMPARE(page.findChild<QLabel *>("count_well")->text(), QString("0 / 0"));
    // 导入层位界面后（竞赛数据集导入路径）该层位正常出现。
    CatalogEntity sb; sb.id = "sb-D61"; sb.entityType = "sequence_boundary"; sb.name = "D61"; QVERIFY(cat->addEntity(sb));
    const auto after = ProjectAreaSummary::snapshot(cat).value("horizons").toList();
    QCOMPARE(after.size(), 1); QCOMPARE(after.first().toMap().value("horizon").toString(), QString("D61"));
  }
  void navigationAndBaseMapChoiceRespectHorizon() {
    Fixture f; QVERIFY(f.init()); QTemporaryDir source; const auto path = source.filePath("D61/map.shp"); QVERIFY(shape(path));
    PaleoMapLibrary::Request request; request.files = {path}; request.horizon = "D61"; const auto result = f.run(request); QVERIFY(result.errors.isEmpty());
    const auto candidates = f.work.baseMapCandidates("D61"); QCOMPARE(candidates.size(), 1); QCOMPARE(candidates.first().toMap().value("category").toString(), QString("existing"));
    QVERIFY(f.work.baseMapCandidates("D62").isEmpty());
    MappingBaseMapSelector selector(&f.work); selector.setHorizon("D61");
    auto *category = selector.findChild<QComboBox *>("baseMapCategory"); QVERIFY(category);
    category->setCurrentIndex(category->findData("existing"));
    auto *sourceBox = selector.findChild<QComboBox *>("baseMapSource"); QVERIFY(sourceBox); QCOMPARE(sourceBox->count(), 1);
    QSignalSpy commands(&selector, &MappingBaseMapSelector::commandRequested);
    auto *show = selector.findChild<QPushButton *>("baseMapShow"); QVERIFY(show); QVERIFY(show->isEnabled());
    auto *start = selector.findChild<QPushButton *>("baseMapStart"); QVERIFY(start); QVERIFY(!start->isEnabled());
    show->click(); QCOMPARE(commands.size(), 1);
    QCOMPARE(commands.first()[0].toString(), QString("show")); QCOMPARE(commands.first()[1].toMap().value("horizon").toString(), QString("D61"));
    selector.setHorizon("D62"); QVERIFY(!show->isEnabled()); QVERIFY(start->isEnabled());
    QCOMPARE(category->currentData().toString(), QString("new"));
    selector.setHorizon("D61"); category->setCurrentIndex(category->findData("existing"));
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
  void realImportedMapMainCanvas() {
    const QString root = qEnvironmentVariable("PALEO_SETUP_SAMPLE_ROOT"); if (root.isEmpty()) QSKIP("Set PALEO_SETUP_SAMPLE_ROOT");
    QObject context; Fixture f; QVERIFY(f.init());
    auto config = f.project.mapConfiguration(); PaleoGeoreference ref; ref.kind = "crs"; ref.sourceCrs = "EPSG:32649"; ref.targetCrs = "EPSG:4326"; config.georeference = ref;
    QString error; QVERIFY2(f.project.updateMapConfiguration(config, &error), qPrintable(error));
    PaleoMapLibrary::Request request; request.files = {QDir(root).filePath(QStringLiteral("古地理图/D62_historical_facies_map.gpkg"))}; request.horizon = "D62";
    auto result = f.run(request); QVERIFY2(result.errors.isEmpty(), qPrintable(result.errors.join('\n'))); QCOMPARE(result.assets.size(), 1);
    const auto snapshot = qEnvironmentVariable("PALEO_MAP_PROJECT_SNAPSHOT");
    if (!snapshot.isEmpty()) {
      QVERIFY(f.project.openProject(QDir(snapshot).filePath("comp.qgz")));
      f.importer.setProjectDir(snapshot); f.work.bindCatalog(f.importer.catalog(), snapshot);
      QVERIFY(PaleoMapLibrary::declareMaps(f.importer.catalog(), &f.layers).isEmpty()); result.assets.clear();
      for (const auto &asset : f.importer.catalog()->assets())
        if (asset.type == "paleogeographic_map" && f.importer.catalog()->currentVersion(asset.id).extra.value("horizon") == "D62") result.assets << asset.id;
      QCOMPARE(result.assets.size(), 1);
      qInfo() << "existing project configuration" << f.project.project()->crs().authid();
    }
    f.project.setParent(&context); QgisCanvasController controller(&context);
    controller.setLayerResolver([&](const QString &id) { return f.layers.layer(id); });
    SelectionContext selection; selection.setActiveHorizon("D62");
    PredictionWorkflow prediction(&f.processing, &f.layers); CompositionWorkflow composition(&f.processing, &f.layers); ValidationWorkflow validation(&f.layers, &f.store);
    PaleoMainWindow window(&controller, &f.project, &f.layers, nullptr, &selection);
    window.attachWorkflows(&prediction, &f.constraints, &composition, &validation, &f.importer, nullptr, &f.processing, &f.store); window.attachWorkbench(&f.work);
    window.resize(1400, 1000); window.show();
    const auto v = f.importer.catalog()->currentVersion(result.assets.first()); const auto id = v.extra.value("layer_id").toString();
    const auto raw = f.importer.catalog()->versionById(v.parentVersionIds.first());
    for (const auto &shownId : {id, raw.extra.value("layer_id").toString()})
    for (const auto &mode : {QString("constraint"), QString("compose")}) {
      window.showPage(mode); QTest::qWait(150);
      auto *page = window.findChild<MappingWorkbenchPage *>("mappingWorkbench." + mode); QVERIFY(page);
      page->commandRequested("show", {{"horizon", "D62"}, {"layer", shownId}}); QTest::qWait(150);
      auto *canvas = controller.canvas(); QVERIFY(canvas->layers().contains(f.layers.layer(shownId)));
      QgsMapRendererSequentialJob job(canvas->mapSettings()); job.start(); job.waitForFinished();
      const auto image = job.renderedImage(); int painted = 0;
      for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x)
        if (image.pixelColor(x, y) != QColor(Qt::white)) ++painted;
      qInfo() << shownId << mode << "painted" << painted << "extent" << canvas->extent().toString() << "crs" << canvas->mapSettings().destinationCrs().authid();
      QVERIFY(painted > 1000);
      const auto qa = qEnvironmentVariable("PALEO_PALEOMAP_QA_DIR"); if (!qa.isEmpty()) { QDir().mkpath(qa); QVERIFY(window.grab().save(qa + "/imported-map-" + mode + ".png")); }
    }
  }
};
int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen"); if (!QgisRuntime::initialize()) return 1; GDALAllRegister();
  TestPaleomapLibrary test; const int result = QTest::qExec(&test, argc, argv); QgisRuntime::shutdown(); return result;
}
#include "tst_paleomaplibrary.moc"
