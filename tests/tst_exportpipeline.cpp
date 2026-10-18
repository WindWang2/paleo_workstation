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
#include <QAction>
#include "../src/ui/paleomainwindow.h"
#include "../src/qgis/geologicalmaplayout.h"
#include "../src/qgis/qgisruntime.h"
#include <qgscategorizedsymbolrenderer.h>
#include <qgsfillsymbol.h>
#include "../src/workflow/geologicalexport.h"
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
  void changedInputCannotBeRegisteredAsAHistoricalMap() {
    QTemporaryDir tmp; DataCatalog cat; QVERIFY(cat.open(tmp.path())); PaleoTaskService tasks;
    const auto path = tmp.filePath("input.dat");
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("original"); file.close();
    CatalogAsset asset; asset.id = cat.nextAssetId(); asset.type = "auxiliary"; asset.format = "dat"; asset.displayName = "input.dat"; QVERIFY(cat.addAsset(asset));
    CatalogVersion version; version.id = cat.nextVersionId(); version.assetId = asset.id; version.stage = "RAW";
    version.path = path; version.managed = false; version.fileName = "input.dat"; version.sha256 = DataCatalog::sha256FileHex(path); QVERIFY(cat.addVersion(version));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); file.write("changed"); file.close();
    QgsProject project; auto *layer = points(project); PaleoGeologicalMapLayout::Settings settings;
    std::unique_ptr<QgsPrintLayout> layout(PaleoGeologicalMapLayout::build(&project, {layer}, QgsRectangle(0, 0, 1000, 1000), settings)); QVERIFY(layout);
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
  void longLegendCannotSilentlyClip() {
    QTemporaryDir tmp; DataCatalog cat; QVERIFY(cat.open(tmp.path())); PaleoTaskService tasks;
    QgsProject project; project.setCrs(QgsCoordinateReferenceSystem("EPSG:3857"));
    auto *layer = new QgsVectorLayer("Polygon?field=facies_code:integer&crs=EPSG:3857", "微相", "memory");
    QgsFeatureList features;
    QgsCategoryList categories;
    // Keep the overflow fixture independent of the project's facies catalogue.
    for (int i = 0; i < 80; ++i) {
      QgsFeature f(layer->fields()); f.setAttribute("facies_code", i);
      f.setGeometry(QgsGeometry::fromRect(QgsRectangle(i * 100, 0, (i + 1) * 100, 100))); features << f;
      categories << QgsRendererCategory(i, QgsFillSymbol::createSimple({}).release(), QStringLiteral("测试图例 %1").arg(i));
    }
    QVERIFY(layer->dataProvider()->addFeatures(features)); layer->updateExtents(); project.addMapLayer(layer);
    layer->setRenderer(new QgsCategorizedSymbolRenderer("facies_code", categories));
    PaleoGeologicalMapLayout::Settings settings;
    std::unique_ptr<QgsPrintLayout> layout(PaleoGeologicalMapLayout::build(&project, {layer}, layer->extent(), settings)); QVERIFY(layout);
    PaleoGeologicalExport::Request request; request.destination = tmp.filePath("long-legend"); request.dpi = 72;
    bool called = false; PaleoGeologicalExport::Result result;
    QVERIFY(PaleoGeologicalExport::start(layout.get(), &cat, &tasks, request, this, [&](const auto &r) { result = r; called = true; }));
    QTRY_VERIFY_WITH_TIMEOUT(called, 30000);
    QVERIFY2(result.error.contains(QStringLiteral("图例")), qPrintable(result.error));
    QVERIFY(!QFileInfo::exists(request.destination)); QVERIFY(cat.assets().isEmpty());
  }
  void mapFramesAndScaleBarsStayOnPaper() {
    QgsProject project; auto *layer = points(project);
    for (const auto kind : {PaleoStandardElements::FigureKind::WellPosition,
         PaleoStandardElements::FigureKind::SingleFactor, PaleoStandardElements::FigureKind::Facies}) {
      for (const bool landscape : {true, false}) {
        for (const auto &extent : {QgsRectangle(0, 0, 1000, 1000), QgsRectangle(450000, 3500000, 450100, 3510000)}) {
          PaleoGeologicalMapLayout::Settings settings; settings.kind = kind; settings.page.landscape = landscape;
          std::unique_ptr<QgsPrintLayout> layout(PaleoGeologicalMapLayout::build(&project, {layer}, extent, settings)); QVERIFY(layout);
          const auto paper = layout->pageCollection()->page(0)->pageSize();
          auto *map = qobject_cast<QgsLayoutItemMap *>(layout->itemById("map")); QVERIFY(map);
          const auto rect = map->mapRectToScene(map->rect());
          QVERIFY(rect.right() < paper.width()); QVERIFY(rect.bottom() < paper.height() - 18);
          auto *scale = layout->itemById("scalebar"); QVERIFY(scale);
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
  void worldFilesAndTransparentPaperKeepTheOriginalLayout() {
    QTemporaryDir tmp; QgsProject project; auto *layer = points(project);
    PaleoGeologicalMapLayout::Settings settings;
    std::unique_ptr<QgsPrintLayout> layout(PaleoGeologicalMapLayout::build(&project, {layer}, QgsRectangle(0, 0, 1000, 1000), settings));
    PaleoLayoutExport::ExportOptions options; options.transparentBackground = true; options.worldFile = true;
    const auto result = PaleoLayoutExport::exportLayout(layout.get(), tmp.filePath("transparent.png"), PaleoLayoutExport::Format::Png, 72, {}, options);
    QVERIFY2(result.ok, qPrintable(result.error));
    QVERIFY(QFileInfo::exists(tmp.filePath("transparent.pgw")));
    QVERIFY(QFileInfo::exists(tmp.filePath("transparent.prj")));
    QCOMPARE(QImage(tmp.filePath("transparent.png")).pixelColor(0, 0).alpha(), 0);
    const auto opaque = PaleoLayoutExport::exportLayout(layout.get(), tmp.filePath("opaque.png"), PaleoLayoutExport::Format::Png, 72, {});
    QVERIFY(opaque.ok); QCOMPARE(QImage(tmp.filePath("opaque.png")).pixelColor(0, 0).alpha(), 255);
  }
  void cancelledSessionDoesNotPublishToAnotherProject() {
    QTemporaryDir tmp; DataCatalog cat; QVERIFY(cat.open(tmp.path())); PaleoTaskService tasks;
    QgsProject project; auto *layer = points(project); PaleoGeologicalMapLayout::Settings settings;
    std::unique_ptr<QgsPrintLayout> layout(PaleoGeologicalMapLayout::build(&project, {layer}, QgsRectangle(0, 0, 1000, 1000), settings));
    PaleoGeologicalExport::Request request; request.destination = tmp.filePath("cancelled"); request.dpi = 72;
    bool called = false; PaleoGeologicalExport::Result result;
    QVERIFY(PaleoGeologicalExport::start(layout.get(), &cat, &tasks, request, this, [&](const auto &r) { result = r; called = true; }));
    tasks.beginNewSession(); QTRY_VERIFY_WITH_TIMEOUT(called, 10000);
    QVERIFY(result.cancelled); QVERIFY(cat.assets().isEmpty()); QVERIFY(!QFileInfo::exists(request.destination));
  }
  void batchExportsEveryPageToTheChosenDirectory() {
    QTemporaryDir tmp; DataCatalog cat; QVERIFY(cat.open(tmp.path())); PaleoTaskService tasks;
    QgsProject project; auto *layer = points(project); PaleoGeologicalMapLayout::Settings settings;
    std::unique_ptr<QgsPrintLayout> layout(PaleoGeologicalMapLayout::build(&project, {layer}, QgsRectangle(0, 0, 1000, 1000), settings));
    auto *page = new QgsLayoutItemPage(layout.get()); page->setPageSize("A4"); layout->pageCollection()->addPage(page);
    PaleoHorizonBatchExport::Request request; request.layout = layout.get(); request.catalog = &cat; request.projectDir = tmp.path();
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
    std::unique_ptr<QgsPrintLayout> layout(PaleoGeologicalMapLayout::build(&project, {layer}, QgsRectangle(0, 0, 1000, 1000), settings, &error));
    QVERIFY2(layout, qPrintable(error));
    for (const auto *id : {"map", "title", "legend", "scalebar", "northArrow", "crsCaption", "signature"}) QVERIFY2(layout->itemById(id), id);
    QCOMPARE(qobject_cast<QgsLayoutItemMap *>(layout->itemById("map"))->layers(), QList<QgsMapLayer *>{layer});
    QCOMPARE(qobject_cast<QgsLayoutItemLabel *>(layout->itemById("title"))->text(), settings.title);
    QVERIFY(qobject_cast<QgsLayoutItemLabel *>(layout->itemById("signature"))->text().contains(settings.author));
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
    std::unique_ptr<QgsPrintLayout> layout(PaleoGeologicalMapLayout::build(&project, {layer}, QgsRectangle(0, 0, 1000, 1000), settings)); QVERIFY(layout);
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
      return PaleoGeologicalMapLayout::build(&project, {layer}, QgsRectangle(0, 0, 1000, 1000), s, error);
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
    std::unique_ptr<QgsPrintLayout> layout(PaleoGeologicalMapLayout::build(&project, {layer}, QgsRectangle(0, 0, 1000, 1000), settings));
    const auto result = PaleoLayoutExport::exportLayout(layout.get(), tmp.filePath("map.tif"), PaleoLayoutExport::Format::Tiff, 72, {});
    if (PaleoLayoutExport::formatAvailable(PaleoLayoutExport::Format::Tiff)) { QVERIFY2(result.ok, qPrintable(result.error)); QVERIFY(!QImage(result.files.first()).isNull()); }
    else { QVERIFY(!result.ok); QVERIFY(!result.error.isEmpty()); }
  }
};
int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen"); if (!QgisRuntime::initialize("/usr")) return 1;
  TestExportPipeline test; const int result = QTest::qExec(&test, argc, argv); QgisRuntime::shutdown(); return result;
}
#include "tst_exportpipeline.moc"
