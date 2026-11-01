#include <QtTest>
#include <QTemporaryDir>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCheckBox>
#include <QLineEdit>
#include <QDir>
#include <QComboBox>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QLabel>
#include <QTableWidget>
#include <QAction>
#include "../src/ui/paleomainwindow.h"
#include "../src/qgis/geologicalmaplayout.h"
#include "../src/qgis/qgisruntime.h"
#include <qgscategorizedsymbolrenderer.h>
#include <qgsfillsymbol.h>
#include "../src/qgis/facieshierarchyrenderer.h"
#include "../src/domain/faciestaxonomy.h"
#include "../src/workflow/geologicalexport.h"
#include "../src/workflow/mapexport.h"
#include "../src/workflow/horizonbatchexport.h"
#include "../src/catalog/datacatalog.h"
#include "../src/services/paleotaskservice.h"
#include "../src/ui/dialogs/geologicalexportdialog.h"
#include "../src/ui/dialogs/dataexportdialog.h"
#include <qgsproject.h>
#include <qgsprintlayout.h>
#include <qgsvectorlayer.h>
#include <qgsvectordataprovider.h>
#include <qgslayoutitemlabel.h>
#include <qgslayoutitemmap.h>
#include <qgslayoutitemlegend.h>
#include <qgslayertree.h>
#include <qgslayertreemodel.h>
#include <qgslayertreemodellegendnode.h>
#include <qgslayoutpagecollection.h>
#include <qgslayoutitempage.h>
#include <memory>

class TestExportPipeline : public QObject {
  Q_OBJECT
  static QgsVectorLayer *points(QgsProject &project) {
    auto *layer = new QgsVectorLayer("Point?crs=EPSG:3857&field=value:double", "井位与因子", "memory");
    QgsFeature feature(layer->fields()); feature.setAttribute(0, 12.5);
    feature.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(500, 500)));
    QgsFeatureList features{feature}; layer->dataProvider()->addFeatures(features); layer->updateExtents();
    project.addMapLayer(layer); project.setCrs(layer->crs()); return layer;
  }
private slots:
  void externalFiguresKeepTheProjectCatalogUnchanged() {
    QTemporaryDir tmp; DataCatalog cat; QVERIFY(cat.open(tmp.path())); PaleoTaskService tasks;
    QgsProject project; auto *layer = points(project); PaleoGeologicalMapLayout::Settings settings; settings.horizon = "D61";
    auto layout = PaleoGeologicalMapLayout::buildHandle(&project, {layer}, QgsRectangle(0, 0, 1000, 1000), settings); QVERIFY(layout);
    PaleoGeologicalExport::Request request; request.destination = tmp.filePath("external"); request.dpi = 72;
    request.registerInProject = false; request.formats = {PaleoLayoutExport::Format::Png}; request.context = {{"horizon", "D61"}};
    const auto sequence = cat.mutationSeq(); bool called = false; PaleoGeologicalExport::Result result;
    QVERIFY(PaleoGeologicalExport::start(layout.get(), &cat, &tasks, request, this, [&](const auto &r) { result = r; called = true; }));
    QTRY_VERIFY_WITH_TIMEOUT(called, 20000); QVERIFY2(result.error.isEmpty(), qPrintable(result.error)); QVERIFY(!result.cancelled);
    QVERIFY(!result.files.isEmpty()); QVERIFY(result.assetIds.isEmpty()); QVERIFY(cat.assets().isEmpty()); QCOMPARE(cat.mutationSeq(), sequence);
    QFile metadata(QDir(result.directory).filePath("metadata.json")); QVERIFY(metadata.open(QIODevice::ReadOnly));
    QVERIFY(!QJsonDocument::fromJson(metadata.readAll()).object().value("registered_in_project").toBool());
  }
  void projectFiguresAreRegisteredByHorizon() {
    QTemporaryDir tmp; DataCatalog cat; QVERIFY(cat.open(tmp.path())); PaleoTaskService tasks;
    QgsProject project; auto *layer = points(project); PaleoGeologicalMapLayout::Settings settings;
    auto layout = PaleoGeologicalMapLayout::buildHandle(&project, {layer}, QgsRectangle(0, 0, 1000, 1000), settings); QVERIFY(layout);
    QStringList assets;
    for (const auto &h : {QString("D61"), QString("D62")}) {
      PaleoGeologicalExport::Request request; request.destination = tmp.filePath("exports/maps/" + h); request.dpi = 72;
      request.formats = {PaleoLayoutExport::Format::Png}; request.context = {{"horizon", h}};
      bool called = false; PaleoGeologicalExport::Result result;
      QVERIFY(PaleoGeologicalExport::start(layout.get(), &cat, &tasks, request, this, [&](const auto &r) { result = r; called = true; }));
      QTRY_VERIFY_WITH_TIMEOUT(called, 20000); QVERIFY2(result.error.isEmpty(), qPrintable(result.error)); QCOMPARE(result.assetIds.size(), 1);
      const auto version = cat.currentVersion(result.assetIds.first()); QCOMPARE(version.stage, QString("OUTPUT"));
      QCOMPARE(version.extra.value("horizon").toString(), h); QVERIFY(QFileInfo::exists(cat.versionFilePath(version))); assets << version.assetId;
    }
    QCOMPARE(cat.assets().size(), 2); QVERIFY(assets[0] != assets[1]);
  }
  void longLayerTitleWrapsWithoutLosingExportedContent() {
    QTemporaryDir tmp; DataCatalog cat; QVERIFY(cat.open(tmp.path())); PaleoTaskService tasks;
    QgsProject project; auto *layer = points(project);
    const QString name = QStringLiteral("sub-0123456789abcdef 新建古地理图 · v2");
    layer->setName(name);
    PaleoGeologicalMapLayout::Settings settings; settings.kind = PaleoStandardElements::FigureKind::Facies;
    auto layout = PaleoGeologicalMapLayout::buildHandle(&project, {layer}, QgsRectangle(0, 0, 1000, 1000), settings); QVERIFY(layout);
    PaleoGeologicalExport::Request request; request.destination = tmp.filePath("wrapped-legend"); request.dpi = 72;
    request.formats = {PaleoLayoutExport::Format::Png, PaleoLayoutExport::Format::Pdf};
    bool called = false; PaleoGeologicalExport::Result result;
    QVERIFY(PaleoGeologicalExport::start(layout.get(), &cat, &tasks, request, this, [&](const auto &r) { result = r; called = true; }));
    QTRY_VERIFY_WITH_TIMEOUT(called, 30000); QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QCOMPARE(result.assetIds.size(), 2);
    QVERIFY(!QImage(QDir(result.directory).filePath("map.png")).isNull());
    QVERIFY(QFileInfo(QDir(result.directory).filePath("map.pdf")).size() > 0);
    auto *legend = qobject_cast<QgsLayoutItemLegend *>(layout->layout()->itemById("legend")); QVERIFY(legend);
    auto *node = legend->model()->rootGroup()->findLayer(layer->id()); QVERIFY(node);
    QCOMPARE(node->name(), name); // Wrapping changes layout, never the source identity or title.
  }
  void dialogSeparatesProjectManagementFromExternalOutput() {
    QTemporaryDir tmp; QgsProject project; auto *layer = points(project); PaleoGeologicalMapLayout::Settings settings; settings.horizon = "D61";
    GeologicalExportDialog dialog(settings, [&](const auto &s, QString *error) {
      return PaleoGeologicalMapLayout::buildHandle(&project, {layer}, QgsRectangle(0, 0, 1000, 1000), s, error);
    }, false, nullptr, {}, tmp.path());
    auto *mode = dialog.findChild<QComboBox *>("mapExportDestinationMode"); QVERIFY(mode); QVERIFY(mode->currentData().toBool());
    QVERIFY(dialog.request().registerInProject); QVERIFY(dialog.request().destination.startsWith(tmp.filePath("exports/maps")));
    const auto qa = qEnvironmentVariable("PALEO_PALEOMAP_QA_DIR");
    if (!qa.isEmpty()) { dialog.show(); QTest::qWait(150); QVERIFY(dialog.grab().save(qa + "/map-export-destinations.png")); }
    mode->setCurrentIndex(1); QVERIFY(!dialog.request().registerInProject);
    QVERIFY(dialog.findChild<QLineEdit *>("mapExportDirectory")->text().isEmpty());
    QVERIFY(!dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->isEnabled());
  }
  void identicalFigureBytesAreDeduplicatedWithinTheirHorizon() {
    QTemporaryDir tmp; DataCatalog cat; QVERIFY(cat.open(tmp.path()));
    QImage image(24, 24, QImage::Format_ARGB32); image.fill(Qt::white); const auto path = tmp.filePath("figure.png"); QVERIFY(image.save(path)); QString error;
    const auto first = registerMapPdfAsset(&cat, tmp.path(), path, nullptr, nullptr, &error, "png", "D61"); QVERIFY2(!first.isEmpty(), qPrintable(error));
    const auto other = registerMapPdfAsset(&cat, tmp.path(), path, nullptr, nullptr, &error, "png", "D62"); QVERIFY2(!other.isEmpty(), qPrintable(error));
    QVERIFY(first != other); QCOMPARE(cat.currentVersion(first).sha256, cat.currentVersion(other).sha256);
    QCOMPARE(registerMapPdfAsset(&cat, tmp.path(), path, nullptr, nullptr, &error, "png", "D61"), first); QCOMPARE(cat.assets().size(), 2);
  }
  void projectLevelExportDoesNotStealHorizonScopedVersion() {
    // C-01 回归：空层位（工区级）导出不得复用带层位登记的既有同字节版本——
    // 复用+按本层位回写 extra 会把既有版本的层位归属清空，下游按 horizon
    // 归集错位。期望：新资产登记，既有版本层位保持不变。
    QTemporaryDir tmp; DataCatalog cat; QVERIFY(cat.open(tmp.path()));
    QImage image(24, 24, QImage::Format_ARGB32); image.fill(Qt::darkGray); const auto path = tmp.filePath("scoped.png"); QVERIFY(image.save(path)); QString error;
    const auto scoped = registerMapPdfAsset(&cat, tmp.path(), path, nullptr, nullptr, &error, "png", "D61"); QVERIFY2(!scoped.isEmpty(), qPrintable(error));
    QCOMPARE(cat.currentVersion(scoped).extra.value("horizon").toString(), QLatin1String("D61"));
    const auto projectLevel = registerMapPdfAsset(&cat, tmp.path(), path, nullptr, nullptr, &error, "png", QString()); QVERIFY2(!projectLevel.isEmpty(), qPrintable(error));
    QVERIFY(projectLevel != scoped); QCOMPARE(cat.assets().size(), 2);
    QCOMPARE(cat.currentVersion(scoped).extra.value("horizon").toString(), QLatin1String("D61"));
    QCOMPARE(cat.currentVersion(projectLevel).extra.value("horizon").toString(), QString());
    // 同域重导仍正确 dedup：工区级第二次注册复用工区级资产。
    QCOMPARE(registerMapPdfAsset(&cat, tmp.path(), path, nullptr, nullptr, &error, "png", QString()), projectLevel);
  }
  void changedInputCannotBeRegisteredAsAHistoricalMap() {
    QTemporaryDir tmp; DataCatalog cat; QVERIFY(cat.open(tmp.path())); PaleoTaskService tasks;
    const auto path = tmp.filePath("input.dat");
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("original"); file.close();
    CatalogAsset asset; asset.id = cat.nextAssetId(); asset.type = "auxiliary"; asset.format = "dat"; asset.displayName = "input.dat"; QVERIFY(cat.addAsset(asset));
    CatalogVersion version; version.id = cat.nextVersionId(); version.assetId = asset.id; version.stage = "RAW";
    version.path = path; version.managed = false; version.fileName = "input.dat"; version.sha256 = DataCatalog::sha256FileHex(path); QVERIFY(cat.addVersion(version));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); file.write("changed"); file.close();
    QgsProject project; auto *layer = points(project); PaleoGeologicalMapLayout::Settings settings;
    auto layout = PaleoGeologicalMapLayout::buildHandle(&project, {layer}, QgsRectangle(0, 0, 1000, 1000), settings); QVERIFY(layout);
    PaleoGeologicalExport::Request request; request.destination = tmp.filePath("changed-input"); request.dpi = 72; request.sourceVersionIds = {version.id};
    bool called = false; PaleoGeologicalExport::Result result;
    QVERIFY(PaleoGeologicalExport::start(layout.get(), &cat, &tasks, request, this, [&](const auto &r) { result = r; called = true; }));
    QTRY_VERIFY_WITH_TIMEOUT(called, 10000);
    QVERIFY(result.error.contains("SHA-256")); QVERIFY(!QFileInfo::exists(request.destination));
    QCOMPARE(cat.assets().size(), 1);
  }
  void exportHasAFileMenuEntryBeforeWorkflowsAreAttached() {
    PaleoMainWindow window(nullptr, nullptr, nullptr, nullptr, nullptr);
    auto *action = window.findChild<QAction *>("fileExportGeologicalMap");
    QVERIFY(action); QVERIFY(!action->isEnabled());
  }
  void longLegendCannotSilentlyClip_data() {
    QTest::addColumn<bool>("hierarchy");
    QTest::newRow("categorized") << false;
    QTest::newRow("facies-hierarchy") << true;
  }
  void longLegendCannotSilentlyClip() {
    QFETCH(bool, hierarchy);
    QTemporaryDir tmp; DataCatalog cat; QVERIFY(cat.open(tmp.path())); PaleoTaskService tasks;
    QgsProject project; project.setCrs(QgsCoordinateReferenceSystem("EPSG:3857"));
    auto *layer = new QgsVectorLayer("Polygon?field=facies_code:integer&crs=EPSG:3857", "微相", "memory");
    QgsFeatureList features;
    QgsCategoryList categories;
    QVariantList schema;
    // Keep the overflow fixture independent of the project's facies catalogue.
    for (int i = 0; i < 80; ++i) {
      QgsFeature f(layer->fields()); f.setAttribute("facies_code", i);
      f.setGeometry(QgsGeometry::fromRect(QgsRectangle(i * 100, 0, (i + 1) * 100, 100))); features << f;
      categories << QgsRendererCategory(i, QgsFillSymbol::createSimple({}).release(), QStringLiteral("测试图例 %1").arg(i));
      schema << QVariantMap{{"code", i + 1}, {"name", QStringLiteral("测试图例 %1").arg(i)},
          {"facies", "测试相"}, {"subfacies", "测试亚相"},
          {"microfacies", QStringLiteral("测试图例 %1").arg(i)}, {"color", "#FFE082"}};
      if (hierarchy) features.last().setAttribute("facies_code", i + 1);
    }
    QVERIFY(layer->dataProvider()->addFeatures(features)); layer->updateExtents(); project.addMapLayer(layer);
    if (hierarchy) FaciesHierarchyRenderer::apply(layer, schema, "micro_facies");
    else layer->setRenderer(new QgsCategorizedSymbolRenderer("facies_code", categories));
    PaleoGeologicalMapLayout::Settings settings;
    auto layout = PaleoGeologicalMapLayout::buildHandle(&project, {layer}, layer->extent(), settings); QVERIFY(layout);
    PaleoGeologicalExport::Request request; request.destination = tmp.filePath("long-legend"); request.dpi = 72;
    bool called = false; PaleoGeologicalExport::Result result;
    QVERIFY(PaleoGeologicalExport::start(layout.get(), &cat, &tasks, request, this, [&](const auto &r) { result = r; called = true; }));
    QTRY_VERIFY_WITH_TIMEOUT(called, 30000);
    QVERIFY2(result.error.contains(QStringLiteral("图例")), qPrintable(result.error));
    QVERIFY(!QFileInfo::exists(request.destination)); QVERIFY(cat.assets().isEmpty());
  }
  void optionalTemplatesExportThroughTheRealFaciesRenderer() {
    QTemporaryDir tmp; DataCatalog cat; QVERIFY(cat.open(tmp.path())); PaleoTaskService tasks;
    QgsProject project; project.setCrs(QgsCoordinateReferenceSystem("EPSG:3857"));
    auto *layer = new QgsVectorLayer("Polygon?field=facies_code:integer&crs=EPSG:3857", "三级相图", "memory");
    const auto templates = FaciesTaxonomy::templates();
    QVariantList schema;
    QgsFeatureList features;
    for (const auto &label : {QStringLiteral("三角洲相 / 三角洲前缘 / 水下分流河道"), QStringLiteral("河道充填"), QStringLiteral("浅湖泥")}) {
      const auto row = FaciesTaxonomy::resolve(templates, label);
      QVERIFY(!row.isEmpty());
      const int i = features.size();
      schema << row;
      QgsFeature f(layer->fields()); f.setAttribute("facies_code", row.value("code"));
      f.setGeometry(QgsGeometry::fromRect(QgsRectangle(i * 100, 0, (i + 1) * 100, 100))); features << f;
    }
    QVERIFY(layer->dataProvider()->addFeatures(features)); layer->updateExtents(); project.addMapLayer(layer);
    QCOMPARE(FaciesHierarchyRenderer::apply(layer, schema, "micro_facies").size(), 3);
    PaleoGeologicalMapLayout::Settings settings; settings.kind = PaleoStandardElements::FigureKind::Facies;
    auto layout = PaleoGeologicalMapLayout::buildHandle(&project, {layer}, layer->extent(), settings);
    QVERIFY(layout);
    PaleoGeologicalExport::Request request;
    request.destination = qEnvironmentVariable("PALEO_FACIES_EXPORT_QA_DIR", tmp.filePath("facies")); request.dpi = 72;
    request.formats = {PaleoLayoutExport::Format::Pdf, PaleoLayoutExport::Format::Png};
    bool called = false; PaleoGeologicalExport::Result result;
    QVERIFY(PaleoGeologicalExport::start(layout.get(), &cat, &tasks, request, this, [&](const auto &r) { result = r; called = true; }));
    QTRY_VERIFY_WITH_TIMEOUT(called, 30000);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QCOMPARE(result.assetIds.size(), 2);
    QVERIFY(QFileInfo::exists(QDir(request.destination).filePath("map.pdf")));
    QVERIFY(!QImage(QDir(request.destination).filePath("map.png")).isNull());
    auto *legend = qobject_cast<QgsLayoutItemLegend *>(layout->layout()->itemById("legend")); QVERIFY(legend);
    auto *node = legend->model()->rootGroup()->findLayer(layer->id()); QVERIFY(node);
    QStringList labels;
    for (auto *entry : legend->model()->layerLegendNodes(node)) labels << entry->data(Qt::DisplayRole).toString();
    for (const auto &row : schema) QVERIFY2(labels.contains(row.toMap().value("name").toString()), qPrintable(labels.join(", ")));
  }
  void mapFramesAndScaleBarsStayOnPaper() {
    QgsProject project; auto *layer = points(project);
    for (const auto kind : {PaleoStandardElements::FigureKind::WellPosition,
         PaleoStandardElements::FigureKind::SingleFactor, PaleoStandardElements::FigureKind::Facies}) {
      for (const bool landscape : {true, false}) {
        for (const auto &extent : {QgsRectangle(0, 0, 1000, 1000), QgsRectangle(450000, 3500000, 450100, 3510000)}) {
          PaleoGeologicalMapLayout::Settings settings; settings.kind = kind; settings.page.landscape = landscape;
          auto layout = PaleoGeologicalMapLayout::buildHandle(&project, {layer}, extent, settings); QVERIFY(layout);
          const auto paper = layout->layout()->pageCollection()->page(0)->pageSize();
          auto *map = qobject_cast<QgsLayoutItemMap *>(layout->layout()->itemById("map")); QVERIFY(map);
          const auto rect = map->mapRectToScene(map->rect());
          QVERIFY(rect.right() < paper.width()); QVERIFY(rect.bottom() < paper.height() - 18);
          auto *scale = layout->layout()->itemById("scalebar"); QVERIFY(scale);
          QVERIFY(scale->mapRectToScene(scale->rect()).bottom() < rect.bottom());
        }
      }
    }
  }
  void dataDialogPreflightsWithoutWritingAnExport() {
    QTemporaryDir tmp; DataCatalog cat; QVERIFY(cat.open(tmp.path())); PaleoTaskService tasks;
    CatalogAsset asset; asset.id = cat.nextAssetId(); asset.displayName = "fixture.gpkg"; asset.type = "boundary"; asset.format = "gpkg"; QVERIFY(cat.addAsset(asset));
    CatalogVersion version; version.id = cat.nextVersionId(); version.assetId = asset.id; version.stage = "RAW";
    version.managed = false; version.path = FIXTURE_GPKG; version.fileName = "fixture.gpkg"; version.sha256 = DataCatalog::sha256FileHex(version.path); QVERIFY(cat.addVersion(version));
    DataExportDialog dialog(&cat, {asset.id}, &tasks);
    auto *summary = dialog.findChild<QLabel *>("dataExportSummary"); QVERIFY(summary);
    QTRY_VERIFY_WITH_TIMEOUT(summary->text().startsWith(QStringLiteral("已检查")), 10000);
    dialog.findChild<QLineEdit *>("dataExportDirectory")->setText(tmp.path());
    QVERIFY(dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->isEnabled());
    QVERIFY(QDir(tmp.path()).entryList({"paleo_export_*"}, QDir::Dirs).isEmpty());
  }
  // L5.4：RAW scope 下某资产没有 RAW 版本时，行内下拉必须显式占位、汇总点名该
  // 资产、导出按钮禁用，且 request() 不再把空 versionId 送进 DataRequest
  // （原先下游只回一条主语为空的「来源版本缺失：」，资产还被静默排除）。
  void rawScopeWithoutRawVersionIsNamedAndBlocked() {
    QTemporaryDir tmp; DataCatalog cat; QVERIFY(cat.open(tmp.path())); PaleoTaskService tasks;
    CatalogAsset asset; asset.id = cat.nextAssetId(); asset.displayName = "fixture.gpkg"; asset.type = "boundary"; asset.format = "gpkg"; QVERIFY(cat.addAsset(asset));
    CatalogVersion version; version.id = cat.nextVersionId(); version.assetId = asset.id; version.stage = "OUTPUT";
    version.managed = false; version.path = FIXTURE_GPKG; version.fileName = "fixture.gpkg"; version.sha256 = DataCatalog::sha256FileHex(version.path); QVERIFY(cat.addVersion(version));
    DataExportDialog dialog(&cat, {asset.id}, &tasks);
    auto *scope = dialog.findChild<QComboBox *>("dataExportScope"); QVERIFY(scope);
    scope->setCurrentIndex(1); // 原始文件（RAW）
    auto *summary = dialog.findChild<QLabel *>("dataExportSummary"); QVERIFY(summary);
    auto *versions = dialog.findChild<QTableWidget *>("dataExportVersions"); QVERIFY(versions);
    auto *combo = qobject_cast<QComboBox *>(versions->cellWidget(0, 1)); QVERIFY(combo);
    QCOMPARE(combo->count(), 1);                       // 显式占位项，不留空下拉
    QVERIFY(combo->currentData().toString().isEmpty());
    QVERIFY(!combo->isEnabled());
    QVERIFY(summary->text().contains(QStringLiteral("没有 RAW")));
    QVERIFY(summary->text().contains(asset.displayName));
    QVERIFY(!dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->isEnabled());
    QVERIFY(dialog.request().versionIds.isEmpty());    // 空 id 不进请求
    // 换回「当前成果」即恢复可选：占位与提示都退场。
    scope->setCurrentIndex(0);
    QVERIFY(summary->text().contains(QStringLiteral("正在检查")));
    QCOMPARE(dialog.request().versionIds, QStringList{version.id});
  }
  void worldFilesAndTransparentPaperKeepTheOriginalLayout() {
    QTemporaryDir tmp; QgsProject project; auto *layer = points(project);
    PaleoGeologicalMapLayout::Settings settings;
    auto layout = PaleoGeologicalMapLayout::buildHandle(&project, {layer}, QgsRectangle(0, 0, 1000, 1000), settings);
    PaleoLayoutExport::ExportOptions options; options.transparentBackground = true; options.worldFile = true;
    const auto result = PaleoLayoutExport::exportLayout(layout->layout(), tmp.filePath("transparent.png"), PaleoLayoutExport::Format::Png, 72, {}, options);
    QVERIFY2(result.ok, qPrintable(result.error));
    QVERIFY(QFileInfo::exists(tmp.filePath("transparent.pgw")));
    QVERIFY(QFileInfo::exists(tmp.filePath("transparent.prj")));
    QCOMPARE(QImage(tmp.filePath("transparent.png")).pixelColor(0, 0).alpha(), 0);
    const auto opaque = PaleoLayoutExport::exportLayout(layout->layout(), tmp.filePath("opaque.png"), PaleoLayoutExport::Format::Png, 72, {});
    QVERIFY(opaque.ok); QCOMPARE(QImage(tmp.filePath("opaque.png")).pixelColor(0, 0).alpha(), 255);
  }
  void cancelledSessionDoesNotPublishToAnotherProject() {
    QTemporaryDir tmp; DataCatalog cat; QVERIFY(cat.open(tmp.path())); PaleoTaskService tasks;
    QgsProject project; auto *layer = points(project); PaleoGeologicalMapLayout::Settings settings;
    auto layout = PaleoGeologicalMapLayout::buildHandle(&project, {layer}, QgsRectangle(0, 0, 1000, 1000), settings);
    PaleoGeologicalExport::Request request; request.destination = tmp.filePath("cancelled"); request.dpi = 72;
    bool called = false; PaleoGeologicalExport::Result result;
    QVERIFY(PaleoGeologicalExport::start(layout.get(), &cat, &tasks, request, this, [&](const auto &r) { result = r; called = true; }));
    tasks.beginNewSession(); QTRY_VERIFY_WITH_TIMEOUT(called, 10000);
    QVERIFY(result.cancelled); QVERIFY(cat.assets().isEmpty()); QVERIFY(!QFileInfo::exists(request.destination));
  }
  void batchExportsEveryPageToTheChosenDirectory() {
    QTemporaryDir tmp; DataCatalog cat; QVERIFY(cat.open(tmp.path())); PaleoTaskService tasks;
    QgsProject project; auto *layer = points(project); PaleoGeologicalMapLayout::Settings settings;
    auto layout = PaleoGeologicalMapLayout::buildHandle(&project, {layer}, QgsRectangle(0, 0, 1000, 1000), settings);
    auto *page = new QgsLayoutItemPage(layout->layout()); page->setPageSize("A4"); layout->layout()->pageCollection()->addPage(page);
    PaleoHorizonBatchExport::Request request; request.layout = layout->layout(); request.catalog = &cat; request.projectDir = tmp.path();
    request.horizonLayers.insert("C6", {layer}); request.dpi = 72; request.outputDirectory = tmp.filePath("batch");
    bool called = false; PaleoHorizonBatchExport::Result result;
    QVERIFY(PaleoHorizonBatchExport::startAsync(request, &tasks, [&](const auto &r, bool) { result = r; called = true; }));
    QTRY_VERIFY_WITH_TIMEOUT(called, 30000);
    QCOMPARE(result.succeeded, 1); QCOMPARE(result.directory, request.outputDirectory);
    QCOMPARE(result.horizons.first().files.size(), 2);
    QCOMPARE(QDir(result.directory).entryList({"*.png"}, QDir::Files).size(), 2);
    QVERIFY(QFileInfo::exists(QDir(result.directory).filePath("metadata.json")));
  }
  void layoutUsesActualLayersAndMapElements() {
    QgsProject project; auto *layer = points(project);
    PaleoGeologicalMapLayout::Settings settings; settings.horizon = "C6"; settings.title = "C6 沉积相图";
    settings.version = 5; settings.author = "编图员";
    QString error;
    auto layout = PaleoGeologicalMapLayout::buildHandle(&project, {layer}, QgsRectangle(0, 0, 1000, 1000), settings, &error);
    QVERIFY2(layout, qPrintable(error));
    for (const auto *id : {"map", "title", "legend", "scalebar", "northArrow", "crsCaption", "signature"}) QVERIFY2(layout->layout()->itemById(id), id);
    QCOMPARE(qobject_cast<QgsLayoutItemMap *>(layout->layout()->itemById("map"))->layers(), QList<QgsMapLayer *>{layer});
    QCOMPARE(qobject_cast<QgsLayoutItemLabel *>(layout->layout()->itemById("title"))->text(), settings.title);
    QVERIFY(qobject_cast<QgsLayoutItemLabel *>(layout->layout()->itemById("signature"))->text().contains(settings.author));
    QVERIFY(!PaleoGeologicalMapLayout::build(&project, {}, QgsRectangle(), settings, &error));
    QVERIFY(layer->startEditing());
    QgsFeature f(layer->fields()); f.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(1, 1))); QVERIFY(layer->addFeature(f));
    QVERIFY(!PaleoGeologicalMapLayout::build(&project, {layer}, QgsRectangle(0, 0, 1000, 1000), settings, &error));
    layer->rollBack();
  }
  void formatsRegisterCorrectlyAndExportAllFiles() {
    QTemporaryDir tmp; DataCatalog cat; QVERIFY(cat.open(tmp.path())); PaleoTaskService tasks;
    QgsProject project; auto *layer = points(project);
    PaleoGeologicalMapLayout::Settings settings; settings.horizon = "C6";
    auto layout = PaleoGeologicalMapLayout::buildHandle(&project, {layer}, QgsRectangle(0, 0, 1000, 1000), settings); QVERIFY(layout);
    PaleoGeologicalExport::Request request; request.destination = qEnvironmentVariable("PALEO_EXPORT_QA_DIR", tmp.filePath("maps")); request.dpi = 72; request.includeData = true;
    request.formats = {PaleoLayoutExport::Format::Pdf, PaleoLayoutExport::Format::Png, PaleoLayoutExport::Format::Svg};
    bool called = false; PaleoGeologicalExport::Result result; QString error;
    auto *task = PaleoGeologicalExport::start(layout.get(), &cat, &tasks, request, this,
        [&](const auto &r) { result = r; called = true; }, &error);
    QVERIFY2(task, qPrintable(error)); QTRY_VERIFY_WITH_TIMEOUT(called, 30000);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error)); QVERIFY(!result.cancelled);
    QVERIFY(QFileInfo::exists(QDir(request.destination).filePath("data/layers/1/data.gpkg")));
    QCOMPARE(result.assetIds.size(), 3); QVERIFY(!result.pdfAssetId.isEmpty());
    for (const auto *name : {"map.png", "map.pdf", "map.svg", "metadata.json", "layout.qpt"}) QVERIFY(QFileInfo::exists(QDir(request.destination).filePath(name)));
    QImage image(QDir(request.destination).filePath("map.png")); QVERIFY(!image.isNull());
    QVERIFY(qAbs(image.width() - 297 * 72. / 25.4) < 2);
    QSet<QString> formats;
    for (const auto &asset : result.assetIds) formats.insert(cat.assetById(asset).format);
    QCOMPARE(formats, QSet<QString>({"png", "pdf", "svg"}));
    QCOMPARE(cat.assetById(result.pdfAssetId).format, QString("pdf"));
    QVERIFY(QFileInfo::exists(result.pdfPath));
    QFile metadata(QDir(request.destination).filePath("metadata.json")); QVERIFY(metadata.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(metadata.readAll()).object().value("schema").toString(), QString("paleo_map_export_v1"));
  }
  void dialogRequiresDestinationAndFormat() {
    QgsProject project; auto *layer = points(project); PaleoGeologicalMapLayout::Settings settings; settings.horizon = "C6";
    GeologicalExportDialog dialog(settings, [&](const auto &s, QString *error) {
      return PaleoGeologicalMapLayout::buildHandle(&project, {layer}, QgsRectangle(0, 0, 1000, 1000), s, error);
    }, false);
    QVERIFY(dialog.layout());
    for (auto *combo : dialog.findChildren<QComboBox *>()) {
      for (int i = 0; i < combo->count(); ++i) { combo->setCurrentIndex(i); QVERIFY(dialog.layout()); }
    }
    auto *buttons = dialog.findChild<QDialogButtonBox *>(); QVERIFY(buttons);
    QVERIFY(!buttons->button(QDialogButtonBox::Save)->isEnabled());
  }
  void tiffIsCapabilityGated() {
    QTemporaryDir tmp; QgsProject project; auto *layer = points(project);
    PaleoGeologicalMapLayout::Settings settings;
    auto layout = PaleoGeologicalMapLayout::buildHandle(&project, {layer}, QgsRectangle(0, 0, 1000, 1000), settings);
    const auto result = PaleoLayoutExport::exportLayout(layout->layout(), tmp.filePath("map.tif"), PaleoLayoutExport::Format::Tiff, 72, {});
    if (PaleoLayoutExport::formatAvailable(PaleoLayoutExport::Format::Tiff)) { QVERIFY2(result.ok, qPrintable(result.error)); QVERIFY(!QImage(result.files.first()).isNull()); }
    else { QVERIFY(!result.ok); QVERIFY(!result.error.isEmpty()); }
  }
  void unrelatedCatalogWriteDuringExportDoesNotDiscardResult() {
    // C-3（full-audit-20261021）：导出窗口期内的无关 catalog 写（导入/预测/
    // 另一导出发布）此前把整批判废为「工程数据已变化」。#480 P3-3 口径：
    // 输入提交时已冻结进 staging；发布撞号由 remap 换新号 + applyJournal
    // 事务兜底。此处构造最坏形状——导出启动后立刻在 live catalog 显式占用
    // staging 快照将分配的 ast-1..ast-50，断言导出仍登记成功且结果 id 有效。
    QTemporaryDir tmp; DataCatalog cat; QVERIFY(cat.open(tmp.path())); PaleoTaskService tasks;
    QgsProject project; auto *layer = points(project); PaleoGeologicalMapLayout::Settings settings; settings.horizon = "D61";
    auto layout = PaleoGeologicalMapLayout::buildHandle(&project, {layer}, QgsRectangle(0, 0, 1000, 1000), settings); QVERIFY(layout);
    PaleoGeologicalExport::Request request; request.destination = tmp.filePath("exports/maps/D61"); request.dpi = 72;
    // 双格式 = 同一 journal 含两个 AddAsset（R2-MF-1：重映射须避让同批
    // 自有 id——噪声只消耗 ast-1 时，若把撞号 ast-1 映到 journal 自有的
    // ast-2，重放即 duplicate 整批失败）。
    request.registerInProject = true;
    request.formats = {PaleoLayoutExport::Format::Png, PaleoLayoutExport::Format::Pdf};
    request.context = {{"horizon", "D61"}};
    bool called = false; PaleoGeologicalExport::Result result;
    QVERIFY(PaleoGeologicalExport::start(layout.get(), &cat, &tasks, request, this, [&](const auto &r) { result = r; called = true; }));
    // 噪声只消耗 ast-1：journal 自有的 ast-2 保持未被 live 占用——旧重映射
    // 把撞号 ast-1 映到 nextAssetId()=ast-2（journal 自有）→ 重放 duplicate。
    {
      CatalogAsset noise;
      noise.id = QStringLiteral("ast-1");
      noise.type = QStringLiteral("well_log");
      noise.format = QStringLiteral("las");
      QString err;
      QVERIFY2(cat.addAsset(noise, &err), qPrintable(err));
    }
    QTRY_VERIFY_WITH_TIMEOUT(called, 30000);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QVERIFY(!result.cancelled);
    QCOMPARE(result.assetIds.size(), 2);
    // 未撞号的 ast-2 原号保留；撞号的 ast-1 映到 live/journal 双避让的新号
    // （≥ast-3）。
    const bool keptTwo = result.assetIds.contains(QStringLiteral("ast-2"));
    const int remapped = int(result.assetIds.removeAll(QStringLiteral("ast-2")));
    QVERIFY2(keptTwo && remapped == 1, "未撞号 id 应原号保留");
    QCOMPARE(result.assetIds.size(), 1);
    QVERIFY2(result.assetIds.first() != QStringLiteral("ast-1") &&
                 result.assetIds.first() != QStringLiteral("ast-2"),
             "撞号 id 必须重映射为双避让新号（R2-MF-1）");
    result.assetIds.append(QStringLiteral("ast-2"));
    for (const QString &published : result.assetIds) {
      QVERIFY(!cat.assetById(published).id.isEmpty());
      const auto version = cat.currentVersion(published);
      QCOMPARE(version.stage, QString("OUTPUT"));
      QCOMPARE(version.extra.value("horizon").toString(), QString("D61"));
      QVERIFY(QFileInfo::exists(cat.versionFilePath(version)));
    }
  }
};
int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen"); if (!QgisRuntime::initialize("/usr")) return 1;
  TestExportPipeline test; const int result = QTest::qExec(&test, argc, argv); QgisRuntime::shutdown(); return result;
}
#include "tst_exportpipeline.moc"
