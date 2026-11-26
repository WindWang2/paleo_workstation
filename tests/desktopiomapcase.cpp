#include "../src/app/appcontext.h"
#include "../src/catalog/datacatalog.h"
#include "../src/io/dataimportservice.h"
#include "../src/linkage/selectioncontext.h"
#include "../src/qgis/qgiscanvascontroller.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/services/paleotaskservice.h"
#include "../src/ui/dialogs/dataserviceimportwizard.h"
#include "../src/ui/dialogs/importsourcedialog.h"
#include "../src/ui/paleomainwindow.h"
#include "../src/ui/pages/mappingworkbenchpage.h"
#include "../src/workflow/mappingworkbench.h"

#include <QtTest>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QFileDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <qgsmapcanvas.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>

namespace {
// Install a one-shot driver before the real user action opens a modal event loop.
template<class Dialog, class Trigger, class Drive>
void driveDialog(Trigger trigger, Drive drive, const QString &name = {})
{
  bool visited = false;
  QTimer driver;
  QElapsedTimer elapsed;
  elapsed.start();
  QObject::connect(&driver, &QTimer::timeout, [&] {
    auto *visible = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    auto *dialog = qobject_cast<Dialog *>(visible);
    if ((!dialog || (!name.isEmpty() && dialog->objectName() != name)) && elapsed.elapsed() < 10000)
      return;
    driver.stop();
    auto cleanup = qScopeGuard([&] { if (visible && visible->isVisible()) visible->reject(); });
    if (!dialog || (!name.isEmpty() && dialog->objectName() != name)) {
      if (visible) qWarning() << "Unexpected dialog" << visible->windowTitle();
      QFAIL("The action did not open the expected dialog");
    }
    visited = true;
    drive(*dialog);
  });
  driver.start(100);
  trigger();
  QVERIFY2(visited, "The user action did not open its dialog");
}

void pickFile(const QString &path, const std::function<void()> &open)
{
  driveDialog<QFileDialog>(open, [&](QFileDialog &dialog) {
    auto *fileName = dialog.findChild<QLineEdit *>("fileNameEdit");
    QVERIFY(fileName);
    fileName->setFocus();
    fileName->setText(QDir::toNativeSeparators(path));
    QTRY_VERIFY(dialog.selectedFiles().contains(QDir::fromNativeSeparators(path)));
    QTest::keyClick(fileName, Qt::Key_Return);
    QTRY_VERIFY(!dialog.isVisible());
  });
}

QMap<QString, int> faciesCounts(QgsVectorLayer &layer)
{
  QMap<QString, int> counts;
  QgsFeatureRequest request;
  request.setFlags(Qgis::FeatureRequestFlag::NoGeometry);
  request.setSubsetOfAttributes(QStringList{QStringLiteral("microfacies")}, layer.fields());
  auto features = layer.getFeatures(request);
  QgsFeature feature;
  while (features.nextFeature(feature))
    ++counts[feature.attribute(QStringLiteral("microfacies")).toString()];
  return counts;
}

QgsVectorLayer *visibleMap(QgsMapCanvas *canvas, const QString &id)
{
  for (auto *layer : canvas->layers())
    if (layer->customProperty("paleoLayerId").toString() == id)
      return qobject_cast<QgsVectorLayer *>(layer);
  return nullptr;
}

QString findOutput(const QDir &directory, const QString &pattern)
{
  QDirIterator it(directory.path(), QStringList{pattern}, QDir::Files, QDirIterator::Subdirectories);
  return it.hasNext() ? it.next() : QString();
}

void showImportedMap(PaleoMainWindow &window, const QString &horizon, const QString &layerId)
{
  window.showPage("compose");
  auto *chip = window.findChild<QToolButton *>(QStringLiteral("chip_") + horizon);
  QVERIFY(chip);
  chip->click();
  auto *page = window.findChild<MappingWorkbenchPage *>("mappingWorkbench.compose");
  QVERIFY(page);
  auto *category = page->findChild<QComboBox *>("baseMapCategory");
  auto *sources = page->findChild<QComboBox *>("baseMapSource");
  QVERIFY(category && sources);
  QTRY_VERIFY(category->findData(QStringLiteral("existing")) >= 0);
  category->setCurrentIndex(category->findData(QStringLiteral("existing")));
  const auto matchingSource = [&] {
    for (int i = 0; i < sources->count(); ++i)
      if (sources->itemData(i).toMap().value("id").toString() == layerId) return i;
    return -1;
  };
  QTRY_VERIFY(matchingSource() >= 0);
  sources->setCurrentIndex(matchingSource());
  auto *show = page->findChild<QPushButton *>("baseMapShow");
  QVERIFY(show && show->isEnabled());
  show->click();
}

int colouredSamples(const QImage &image)
{
  int samples = 0;
  for (int y = image.height() / 4; y < 3 * image.height() / 4; y += 12)
    for (int x = image.width() / 4; x < 2 * image.width() / 3; x += 12)
      if (image.pixelColor(x, y).hsvSaturation() > 24) ++samples;
  return samples;
}
}

void runDesktopIoMapCase(AppContext &ctx, PaleoMainWindow &window)
{
  const QString source = qEnvironmentVariable("PALEO_DESKTOP_IO_SOURCE");
  const QString previous = qEnvironmentVariable("PALEO_DESKTOP_IO_MAP");
  const QDir out(qEnvironmentVariable("PALEO_DESKTOP_IO_OUTPUT"));
  QVERIFY(QFileInfo(source).isDir() && QFileInfo(previous).isFile());
  QVERIFY(QDir().mkpath(out.absolutePath()));
  const QString name = QStringLiteral("HY-map-") + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmsszzz");
  const QDir project(out.filePath(name));
  const QString projectPath = project.filePath(name + ".qgz");
  qInfo().noquote() << "Acceptance project" << projectPath;
  const auto shot = [&](QWidget &widget, const QString &stage) {
    QCoreApplication::processEvents();
    QVERIFY(widget.grab().save(out.filePath(stage + ".png")));
  };
  const QString originalHash = DataCatalog::sha256FileHex(previous);
  QgsVectorLayer original(previous, "historical facies", "ogr");
  QVERIFY(original.isValid() && !originalHash.isEmpty());
  const auto expected = faciesCounts(original);
  QVERIFY(expected.size() > 1 && !expected.contains(QString()));
  window.resize(1360, 900);
  window.show();
  QTest::qWait(100);
  QTimer notices;
  QObject::connect(&notices, &QTimer::timeout, [&] {
    auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
    if (box && box->standardButtons() == QMessageBox::Ok) {
      qInfo().noquote() << box->windowTitle() << box->text();
      box->button(QMessageBox::Ok)->click();
    }
  });
  notices.start(150);
  auto *create = window.findChild<QAction *>("fileNewProjectAction");
  QVERIFY(create && create->isEnabled());
  driveDialog<DataServiceImportWizard>([&] { create->trigger(); }, [&](DataServiceImportWizard &wizard) {
    wizard.findChild<QLineEdit *>("dataServiceProjectNameEdit")->setText(name);
    wizard.findChild<QLineEdit *>("dataServiceProjectDirectoryEdit")->setText(out.absolutePath());
    wizard.findChild<QLineEdit *>("dataServiceSourceRootEdit")->setText(source);
    shot(wizard, "01-sources");
    wizard.button(QWizard::NextButton)->click();
    QCOMPARE(wizard.currentId(), 1);
    wizard.button(QWizard::NextButton)->click();
    QCOMPARE(wizard.currentId(), 2);
    shot(wizard, "02-wizard-review");
    wizard.button(QWizard::FinishButton)->click();
  });
  QVERIFY(!QTest::currentTestFailed());
  QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(projectPath).isFile() &&
      QFileInfo(ctx.projectSvc()->projectPath()).canonicalFilePath() == QFileInfo(projectPath).canonicalFilePath(), 90000);
  QTRY_VERIFY_WITH_TIMEOUT(ctx.importSvc()->catalog()->assets().size() > 0 && ctx.taskSvc()->runningCount() == 0, 180000);
  qInfo() << "Model import assets" << ctx.importSvc()->catalog()->assets().size();
  shot(window, "03-model");
  window.showPage("data");
  auto *import = window.findChild<QAction *>("ribbonImportPaleomap");
  QVERIFY(import && import->isEnabled());
  QString horizon;
  // The source dialog is followed immediately by the layer attribution dialog.
  driveDialog<QDialog>([&] {
    driveDialog<ImportSourceDialog>([&] { import->trigger(); }, [&](ImportSourceDialog &sourceDialog) {
      auto *select = sourceDialog.findChild<QPushButton *>("importSourceAddFiles");
      QVERIFY(select);
      pickFile(previous, [&] { select->click(); });
      QCOMPARE(sourceDialog.files().size(), 1);
      shot(sourceDialog, "04-map-source");
      auto *review = sourceDialog.findChild<QPushButton *>("importSourceConfirm");
      QVERIFY(review && review->isEnabled());
      review->click();
    });
  }, [&](QDialog &settings) {
    QCOMPARE(settings.objectName(), QStringLiteral("paleomapImportDialog"));
    auto *selector = settings.findChild<QComboBox *>("paleomapImportHorizon");
    QVERIFY(selector && selector->count());
    const QString requested = qEnvironmentVariable("PALEO_DESKTOP_IO_HORIZON");
    if (!requested.isEmpty()) {
      QVERIFY2(selector->findText(requested) >= 0, qPrintable(requested));
      selector->setCurrentText(requested);
    }
    horizon = selector->currentText();
    QVERIFY(!horizon.isEmpty());
    settings.findChild<QLineEdit *>("paleomapMicrofaciesField")->setText("microfacies");
    QVERIFY(settings.findChild<QCheckBox *>("paleomapNormalizeFacies")->isChecked());
    shot(settings, "05-map-classification");
    settings.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
  }, QStringLiteral("paleomapImportDialog"));
  QVERIFY(!QTest::currentTestFailed());
  const auto normalized = [&] {
    for (const auto &v : ctx.importSvc()->catalog()->versions())
      if (v.extra.value("method") == QStringLiteral("existing-paleomap-hierarchy-v2")) return v;
    return CatalogVersion{};
  };
  QTRY_VERIFY_WITH_TIMEOUT(!normalized().id.isEmpty(), 60000);
  const auto version = normalized();
  const QString layerId = version.extra.value("layer_id").toString();
  QVERIFY(!version.parentVersionIds.isEmpty() && !layerId.isEmpty());
  showImportedMap(window, horizon, layerId);
  QCOMPARE(ctx.selection()->activeHorizon(), horizon);
  auto *canvas = ctx.canvasCtl()->canvas();
  QTRY_VERIFY(visibleMap(canvas, layerId));
  const QString originalLayerId = ctx.importSvc()->catalog()->versionById(
      version.extra.value("source_raw_version_id").toString()).extra.value("layer_id").toString();
  QVERIFY(!originalLayerId.isEmpty());
  QTRY_VERIFY(!visibleMap(canvas, originalLayerId));
  const auto controls = window.findChildren<QComboBox *>("ribbonFaciesDisplayMode");
  QComboBox *mode = nullptr;
  for (auto *control : controls) if (control->isEnabled()) { mode = control; break; }
  QVERIFY(mode);
  const auto detail = mode->findData(QStringLiteral("micro_facies"));
  QVERIFY(detail >= 0);
  mode->setCurrentIndex(detail);
  emit mode->activated(detail);
  QTRY_COMPARE(ctx.mappingWorkbench()->resolvedLevel(layerId), QStringLiteral("micro_facies"));
  QCOMPARE(faciesCounts(*visibleMap(canvas, layerId)), expected);
  auto *spatial = visibleMap(canvas, layerId);
  QTRY_VERIFY(canvas->extent().intersects(canvas->mapSettings().layerExtentToOutputExtent(spatial, spatial->extent())));
  QTRY_VERIFY(!canvas->isDrawing());
  shot(window, "06-main-map");
  auto *menu = window.findChild<QMenu *>("fileMenu");
  auto *output = window.findChild<QAction *>("fileExportGeologicalMap");
  QVERIFY(menu && output);
  menu->popup(window.mapToGlobal(QPoint(16, 45)));
  QTRY_VERIFY(menu->isVisible() && output->isEnabled());
  driveDialog<QDialog>([&] {
    QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(output).center());
  }, [&](QDialog &dialog) {
    QCOMPARE(dialog.objectName(), QStringLiteral("geologicalExportDialog"));
    dialog.findChild<QLineEdit *>("mapExportTitle")->setText(QStringLiteral("HY 历史沉积相复核图"));
    auto *coordinates = dialog.findChild<QComboBox *>("mapExportCrs");
    QVERIFY(coordinates && coordinates->count() == 2);
    coordinates->setCurrentIndex(1);
    auto *labels = dialog.findChild<QCheckBox *>("mapExportPatchLabels");
    QVERIFY(labels && labels->isVisible() && labels->isChecked());
    dialog.findChild<QCheckBox *>("mapExportPng")->setChecked(true);
    QVERIFY(dialog.findChild<QCheckBox *>("mapExportPdf")->isChecked());
    auto *save = dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save);
    auto *preview = dialog.findChild<QWidget *>("mapExportPreview");
    QVERIFY(preview);
    QElapsedTimer renderTime;
    renderTime.start();
    int previewColours = 0;
    do {
      QTest::qWait(600);
      previewColours = colouredSamples(preview->grab().toImage());
    } while ((!save->isEnabled() || previewColours <= 10) && renderTime.elapsed() < 30000);
    shot(dialog, "07-export-preview");
    if (!save->isEnabled() || previewColours <= 10) {
      qWarning() << "Export preview status" << save->isEnabled() << previewColours << preview->size();
      for (const auto *label : dialog.findChildren<QLabel *>())
        if (label->wordWrap()) qWarning().noquote() << label->text();
    }
    QVERIFY(save->isEnabled() && previewColours > 10);
    save->click();
  });
  QVERIFY(!QTest::currentTestFailed());
  const QDir exports(project.filePath("exports/maps"));
  QTRY_VERIFY_WITH_TIMEOUT(!findOutput(exports, "*.pdf").isEmpty() && !findOutput(exports, "*.png").isEmpty() &&
                           ctx.taskSvc()->runningCount() == 0, 90000);
  const QString pdf = findOutput(exports, "*.pdf"), png = findOutput(exports, "*.png");
  QFile pageFile(pdf);
  QVERIFY(pageFile.open(QIODevice::ReadOnly) && pageFile.read(5) == "%PDF-");
  const QImage raster(png);
  QVERIFY(!raster.isNull() && raster.width() > 1000);
  const int coloured = colouredSamples(raster);
  QVERIFY2(coloured > 10, "No mapped colours in the centre of the exported page");
  QFile metadata(findOutput(exports, "metadata.json"));
  QVERIFY(metadata.open(QIODevice::ReadOnly));
  const auto manifest = QJsonDocument::fromJson(metadata.readAll()).object();
  const auto inputVersions = manifest.value("input_versions").toArray();
  QVERIFY(inputVersions.contains(version.id));
  QVERIFY(!inputVersions.contains(version.extra.value("source_raw_version_id").toString()));
  const auto context = manifest.value("context").toObject();
  QCOMPARE(context.value("horizon").toString(), horizon);
  QCOMPARE(context.value("map_crs").toString(), spatial->crs().authid());
  const QString pdfHash = DataCatalog::sha256FileHex(pdf), pngHash = DataCatalog::sha256FileHex(png);
  const auto registered = [&] {
    QSet<QString> hashes;
    for (const auto &v : ctx.importSvc()->catalog()->versions()) if (v.stage == "OUTPUT") hashes.insert(v.sha256);
    return hashes.contains(pdfHash) && hashes.contains(pngHash);
  };
  QTRY_VERIFY_WITH_TIMEOUT(registered(), 10000);
  auto *save = window.findChild<QAction *>("saveProjectAction");
  QVERIFY(save && save->isEnabled());
  save->trigger();
  ctx.projectSvc()->closeProject();
  auto *open = window.findChild<QAction *>("fileOpenProjectAction");
  QVERIFY(open && open->isEnabled());
  pickFile(projectPath, [&] { open->trigger(); });
  QTRY_COMPARE_WITH_TIMEOUT(QFileInfo(ctx.projectSvc()->projectPath()).canonicalFilePath(),
                            QFileInfo(projectPath).canonicalFilePath(), 60000);
  QTRY_VERIFY(registered());
  QCOMPARE(normalized().id, version.id);
  showImportedMap(window, horizon, layerId);
  QVERIFY(!QTest::currentTestFailed());
  QTRY_VERIFY(visibleMap(canvas, layerId));
  QTRY_VERIFY(!visibleMap(canvas, originalLayerId));
  QCOMPARE(faciesCounts(*visibleMap(canvas, layerId)), expected);
  QCOMPARE(DataCatalog::sha256FileHex(previous), originalHash);
  QTRY_VERIFY(!canvas->isDrawing());
  shot(window, "08-reopened");
  menu->popup(window.mapToGlobal(QPoint(16, 45)));
  QTRY_VERIFY(menu->isVisible() && output->isEnabled());
  driveDialog<QDialog>([&] {
    QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(output).center());
  }, [&](QDialog &dialog) {
    auto *title = dialog.findChild<QLineEdit *>("mapExportTitle");
    QVERIFY(title && title->text().startsWith(horizon + QLatin1Char(' ')));
    dialog.reject();
  }, QStringLiteral("geologicalExportDialog"));
  qInfo().noquote() << "Map version" << version.id << "facies" << expected << "coloured samples" << coloured
                    << "PDF" << pdf << "PNG" << png;
}
