#include "io/areamodel.h"
#include "services/previewdoc.h"
#include "services/paleotaskservice.h"
#include <QtTest>
#include <QThread>
#include <QSemaphore>
#include <cmath>
#include <algorithm>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTabBar>
#include <QTableWidget>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QMenu>
#include <QBuffer>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QtConcurrent>
#include "../src/io/sourceprobe.h"
#include "../src/io/welllogread.h"
#include "welllogfixturewriters.h"
#include "importuxfixtures.h"
#include "../src/ui/dialogs/importsourcedialog.h"
#include "../src/ui/dialogs/folderconfirm.h"
#include "../src/ui/pages/datalist.h"
#include "../src/io/paleomapfiles.h"
#include "../src/services/paleotaskservice.h"
#include "../src/io/welltable.h"
#include "../src/io/cuttingsdoc.h"
#include "../src/workflow/wellsectionworkflow.h"
#include "../src/domain/lithologystyle.h"
#include "../src/io/wellfileparsers.h"
#include "../src/io/ziparchive.h"
#include "../src/io/projectsetupscan.h"
#include "../src/io/importclassification.h"
#include "../src/io/dataimportservice.h"
#include "../src/io/horizonbinner.h"
#include "../src/services/projectdata.h"
#include "../src/services/welllogset.h"
#include "../src/services/previewdoc.h"
#include "../src/workflow/folderimport.h"
#include "../src/workflow/projectsetup.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/projectmapreference.h"
#include "../src/catalog/datacatalog.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/metadata/layermanifest.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/crsselector.h"
#include "../src/qgis/coordinateeditor.h"
#include "../src/workflow/projectlayerrefresh.h"
#include "helpers/visualcapture.h"
#include "../src/ui/dialogs/projectsetupdialog.h"
#include "../src/ui/dialogs/projectmapsettingsdialog.h"
#include "../src/ui/paleotheme.h"
#include "../src/ui/datapreview/datapreviewtabs.h"
#include "../src/ui/wellcomposite/wellcompositepanel.h"
#include "../src/ui/wellcomposite/wellcompositecanvas.h"
#include <qgscoordinatetransform.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>
#include <qgsrasterlayer.h>
#include <qgsmaprenderersequentialjob.h>
#include "../src/qgis/facieshierarchyrenderer.h"

class TestProjectSetup : public QObject
{
  Q_OBJECT
  static bool write(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath()); QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
  }
  static QString xlsx(const QString &path, const QString &sheet, const QVector<QStringList> &rows) {
    QByteArray xml("<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData>");
    for (int i = 0; i < rows.size(); ++i) {
      xml += "<row r=\"" + QByteArray::number(i + 1) + "\">";
      for (int c = 0; c < rows[i].size(); ++c) xml += "<c r=\"" + QByteArray(1, char('A' + c)) + QByteArray::number(i + 1) + "\" t=\"inlineStr\"><is><t>" + rows[i][c].toHtmlEscaped().toUtf8() + "</t></is></c>";
      xml += "</row>";
    }
    xml += "</sheetData></worksheet>";
    const QByteArray workbook = QString("<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><sheets><sheet name=\"%1\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>").arg(sheet).toUtf8();
    QDir().mkpath(QFileInfo(path).absolutePath()); QString error;
    return paleo::io::zipWriteArchive(path, {{"xl/workbook.xml", workbook}, {"xl/_rels/workbook.xml.rels", "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Target=\"worksheets/sheet1.xml\"/></Relationships>"}, {"xl/worksheets/sheet1.xml", xml}}, {}, &error) ? path : QString();
  }
private slots:
  void scanProbeHasHardReadBudgetAndStopsAtIdentity() {
    class CountingBuffer : public QBuffer {
    public: qint64 readBytes = 0;
    protected: qint64 readData(char *data, qint64 size) override {
      const auto result = QBuffer::readData(data, size); if (result > 0) readBytes += result; return result;
    }
    } input;
    auto xml = ImportUxFixtures::comprehensiveXml(); xml += QByteArray(2 * 1024 * 1024, 'x');
    input.setData(xml); QVERIFY(input.open(QIODevice::ReadOnly));
    const auto probe = paleo::io::probeSource(input, "comprehensive.xml");
    QCOMPARE(probe.classification.type, QString("well_log")); QCOMPARE(probe.wellName, QString("A1"));
    QVERIFY(probe.bytesRead <= 16 * 1024); QCOMPARE(input.readBytes, probe.bytesRead); QVERIFY(probe.limited);
    auto hidden = ImportUxFixtures::comprehensiveXml();
    hidden.replace("<Worksheet", (QByteArray("<!--") + QByteArray(512 * 1024, 'x') + "--><Worksheet"));
    input.close(); input.setData(hidden); input.readBytes = 0; QVERIFY(input.open(QIODevice::ReadOnly));
    const auto limited = paleo::io::probeSource(input, "hidden.xml");
    QVERIFY(limited.wellName.isEmpty()); QVERIFY(limited.limited);
    QCOMPARE(limited.bytesRead, paleo::io::kScanPrefixBytes); QCOMPARE(input.readBytes, paleo::io::kScanPrefixBytes);
    input.seek(0); input.readBytes = 0; int checks = 0;
    const auto cancelled = paleo::io::probeSource(input, "hidden.xml", [&] { return ++checks > 1; });
    QVERIFY(cancelled.cancelled); QVERIFY(input.readBytes <= 16 * 1024);
  }
  void changingWizardStepsDoesNotRestartPendingScan() {
    QTemporaryDir dir; QSemaphore started, release;
    PaleoTaskService tasks; tasks.workerPool()->setMaxThreadCount(1);
    auto blocker = QtConcurrent::run(tasks.workerPool(), [&] { started.release(); release.acquire(); });
    started.acquire();
    QgisProjectService project; PaleoProjectStore store; DataImportService service(&store);
    FolderImportWorkflow imports(&service, nullptr); ProjectSetupWorkflow workflow(&project, &imports);
    workflow.setMapImportServices(nullptr, &tasks); QSignalSpy ready(&workflow, &ProjectSetupWorkflow::previewReady);
    ProjectSetupDialog dialog(&workflow); dialog.findChild<QLineEdit *>("projectDirectoryEdit")->setText(dir.filePath("out"));
    dialog.setSourceRoot(dir.path());
    auto *steps = dialog.findChild<QTabBar *>(); steps->setCurrentIndex(1); steps->setCurrentIndex(3); steps->setCurrentIndex(1);
    const int pending = workflow.findChildren<QFutureWatcherBase *>().size();
    release.release(); blocker.waitForFinished();
    QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 20000);
    QCOMPARE(pending, 1);
  }
  void boundedProbeKeepsLegacyFormatsAndContentIdentity() {
    using namespace dlisfix;
    const auto dlis = sul() + buildFileHeaderEflr("SCAN") + buildOriginEflr("A1") +
        buildChannelEflr({{"DEPT", "m", 2, {}, {}}, {"GR", "API", 2, {}, {}}}) +
        buildFrameEflr({{"1000", {"DEPT", "GR"}, "BOREHOLE-DEPTH", "INCREASING"}}) +
        buildFdata("1000", 1, fsinglBytes(1000) + fsinglBytes(50));
    const auto lis = lisfix::buildMain();
    const QList<QPair<QByteArray, QString>> fixtures{
      {ImportUxFixtures::csv(), "A1"}, {ImportUxFixtures::xml(), "A1"},
      {ImportUxFixtures::comprehensiveXml(), "A1"},
      {"~Version\nVERS. 2.0 :\n~Well\nWELL. A1 :\n~Curve\nDEPT.M :\nGR.API :\n~A\n1000 50\n", "A1"},
      {dlis, "A1"}, {lis.raw, "HZ28-6-2"}, {lis.tif, "HZ28-6-2"}};
    for (const auto &fixture : fixtures) {
      QBuffer input; input.setData(fixture.first); QVERIFY(input.open(QIODevice::ReadOnly));
      const auto probe = paleo::io::probeSource(input, "renamed.bin");
      QCOMPARE(probe.classification.type, QString("well_log")); QCOMPARE(probe.wellName, fixture.second);
      QVERIFY(probe.bytesRead <= paleo::io::kScanPrefixBytes);
    }
  }
  void scanDoesNotValidateUnreadCurveTailOrHashWholeFile() {
    QTemporaryDir dir; const auto log = dir.filePath(QStringLiteral("测井数据/A1.xml"));
    auto bytes = ImportUxFixtures::comprehensiveXml(); bytes += QByteArray(2 * 1024 * 1024, 'x');
    QVERIFY(write(log, bytes)); const auto preview = scanProjectSources(discoverProjectSources(dir.path()));
    QCOMPARE(preview.logs, 1); QVERIFY(preview.sourceProblems.isEmpty()); QVERIFY(preview.hashes.isEmpty());
    QCOMPARE(preview.sourceSnapshots.value(log).value("prefixBytes").toLongLong(), qint64(16 * 1024));
    QString error; QVERIFY(validateProjectSourceSnapshots(preview, &error));
    LasHeaderInfo header; QList<LasCurve> curves;
    QVERIFY(!WellLogRead::parse(log, header, curves, &error)); QVERIFY(curves.isEmpty());
    PaleoProjectStore store; DataImportService service(&store); service.setProjectDir(dir.filePath("output"));
    error.clear(); QVERIFY(service.importProjectFile(log, &error).isEmpty()); QVERIFY(!error.isEmpty());
    QVERIFY(service.catalog()->assets().isEmpty());
    // Prefix replacement with unchanged size and timestamp still invalidates the preview.
    QFile file(log); QVERIFY(file.open(QIODevice::ReadWrite)); const auto modified = QFileInfo(log).lastModified();
    bytes.replace("A1", "B1"); QCOMPARE(file.write(bytes), qint64(bytes.size()));
    QVERIFY(file.setFileTime(modified, QFileDevice::FileModificationTime)); file.close();
    QVERIFY(!validateProjectSourceSnapshots(preview, &error));
  }
  void tableScanSamplesNeverDeduplicateUnseenRows() {
    QTemporaryDir dir; QByteArray first("WELL,HORIZON,MD,TVD\n"), second(first);
    for (int row = 0; row < 350; ++row) {
      const auto name = QByteArray("L") + QByteArray::number(row);
      first += "A1," + name + ',' + QByteArray::number(row + 1) + ',' + QByteArray::number(row + 1) + '\n';
      second += "A1," + (row < 200 ? name : "Other" + name) + ',' + QByteArray::number(row + 1) + ',' + QByteArray::number(row + 1) + '\n';
    }
    const auto a = dir.filePath("first.csv"), b = dir.filePath("second.csv");
    QVERIFY(write(a, first)); QVERIFY(write(b, second));
    const auto sample = paleo::io::previewWellTable(a); QVERIFY(sample.scanLimited); QCOMPARE(sample.tops.size(), 200);
    QCOMPARE(paleo::io::readWellTable(a).tops.size(), 350);
    QVector<QStringList> rows{{"WELL", "HORIZON", "MD", "TVD"}};
    for (int row = 0; row < 350; ++row) rows << QStringList{"A1", "L" + QString::number(row), QString::number(row + 1), QString::number(row + 1)};
    const auto workbook = xlsx(dir.filePath("many.xlsx"), "data", rows); QVERIFY(!workbook.isEmpty());
    const auto workbookSample = paleo::io::previewWellTable(workbook);
    QVERIFY(workbookSample.scanLimited); QCOMPARE(workbookSample.tops.size(), 200);
    QCOMPARE(paleo::io::readWellTable(workbook).tops.size(), 350);
    ProjectSetupSources sources; sources.wells = dir.path(); const auto preview = scanProjectSources(sources);
    QVERIFY(preview.paths.contains(a)); QVERIFY(preview.paths.contains(b)); QVERIFY(preview.hashes.isEmpty());
    const auto large = dir.filePath("large.xlsx"); QVERIFY(write(large, QByteArray(3 * 1024 * 1024, 'x')));
    const auto deferred = paleo::io::previewWellTable(large); QVERIFY(deferred.scanLimited); QVERIFY(deferred.error.isEmpty());
    const auto unknown = dir.filePath("unknown.xml"); QVERIFY(write(unknown, QByteArray(3 * 1024 * 1024, 'x')));
    const auto unknownSample = paleo::io::previewWellTable(unknown); QVERIFY(unknownSample.scanLimited); QVERIFY(unknownSample.error.isEmpty());
    sources.points = large; const auto pending = scanProjectSources(sources);
    QVERIFY(pending.paths.contains(large)); QVERIFY(!pending.sourceProblems.contains(large));
    sources.pointMapping = {{"crs", "EPSG:2436"}, {"well", "井号"}};
    const auto mapped = scanProjectSources(sources);
    QCOMPARE(mapped.pointSourceCrs, QString("EPSG:2436")); QCOMPARE(mapped.pointMapping.value("well").toString(), QStringLiteral("井号"));
    QVERIFY(mapped.paths.contains(large)); QVERIFY(!mapped.sourceProblems.contains(large));
    const auto compressed = dir.filePath("expanded.xlsx"); QString error;
    paleo::io::ZipBuildOptions options; options.compression = paleo::io::ZipCompression::Deflate;
    QVERIFY(paleo::io::zipWriteArchive(compressed, {{"xl/worksheets/sheet1.xml", QByteArray(5 * 1024 * 1024, 'x')}}, options, &error));
    QVERIFY(QFileInfo(compressed).size() < 2 * 1024 * 1024);
    const auto inflated = paleo::io::previewWellTable(compressed); QVERIFY(inflated.scanLimited); QVERIFY(inflated.error.isEmpty());
  }
  void additionalRootOrganizationPreservesLegacyAndMixedSources() {
    QTemporaryDir dir;
    const auto root = dir.filePath("input");
    const auto points = xlsx(root + QStringLiteral("/坐标统计.xlsx"), "data",
        {{"井号", "X（纵坐标）", "Y（横坐标）"}, {"A1", "2360000", "300000"}});
    QVERIFY(!points.isEmpty());
    const auto logs = root + QStringLiteral("/测井数据");
    QVERIFY(write(logs + "/A1.xml", ImportUxFixtures::comprehensiveXml()));
    const auto references = root + QStringLiteral("/参考资料");
    QVERIFY(write(references + "/reference.txt", "reference document"));
    auto sources = discoverProjectSources(root);
    QCOMPARE(sources.points, points); QCOMPARE(sources.wells, logs); QCOMPARE(sources.references, references);
    auto preview = scanProjectSources(sources);
    QCOMPARE(preview.logs, 1); QCOMPARE(preview.wells, 1); QCOMPARE(preview.references, 1);
    QCOMPARE(preview.paths.size(), 3); QVERIFY(preview.errors.isEmpty()); QVERIFY(preview.sourceProblems.isEmpty());
    QCOMPARE(preview.pointMapping.value("x").toString(), QStringLiteral("Y（横坐标）"));
    QCOMPARE(preview.pointMapping.value("y").toString(), QStringLiteral("X（纵坐标）"));
    // Both directory organizations coexist without importing the coordinate table twice.
    const auto legacy = root + QStringLiteral("/井");
    QVERIFY(write(legacy + "/B1/legacy.csv", ImportUxFixtures::csv("B1")));
    sources = discoverProjectSources(root); QCOMPARE(sources.wells, root);
    preview = scanProjectSources(sources); QCOMPARE(preview.logs, 2); QCOMPARE(preview.paths.size(), 4);
    QCOMPARE(preview.paths.count(points), 1); QVERIFY(preview.sourceProblems.isEmpty());
    // A loose log at the root also survives when there is a recognized directory.
    QVERIFY(write(root + "/C1.xml", ImportUxFixtures::comprehensiveXml().replace("A1", "C1")));
    preview = scanProjectSources(discoverProjectSources(root)); QCOMPARE(preview.logs, 3); QCOMPARE(preview.paths.size(), 5);
    const auto legacyPoints = root + "/wellheads.csv";
    QVERIFY(write(legacyPoints, "WELL,X,Y\nA1,300000,2360000\n"));
    QCOMPARE(discoverProjectSources(root).points, legacyPoints); // Preserve existing naming priority.
    QTemporaryDir old;
    const auto oldPoints = old.filePath(QStringLiteral("井坐标.csv"));
    QVERIFY(write(oldPoints, "WELL,X,Y\nA1,300000,2360000\n"));
    QVERIFY(write(old.filePath(QStringLiteral("井/A1/old.csv")), ImportUxFixtures::csv()));
    sources = discoverProjectSources(old.path()); QCOMPARE(sources.points, oldPoints);
    QCOMPARE(sources.wells, old.filePath(QStringLiteral("井"))); QCOMPARE(scanProjectSources(sources).logs, 1);
    QTemporaryDir flat;
    QVERIFY(write(flat.filePath("A1.xml"), ImportUxFixtures::comprehensiveXml()));
    QCOMPARE(discoverProjectSources(flat.path()).wells, flat.path());
    QCOMPARE(scanProjectSources(discoverProjectSources(flat.path())).logs, 1);
  }
  void additionalOrganizationWizardCreatesAndReopens() {
    QTemporaryDir dir; QString error;
    const auto root = dir.filePath("input");
    const auto points = xlsx(root + QStringLiteral("/坐标统计.xlsx"), "data",
        {{"井号", "X（纵坐标）", "Y（横坐标）"}, {"A1", "2360000", "300000"}, {"B1", "2361000", "301000"}});
    QVERIFY(!points.isEmpty());
    QVERIFY(write(root + QStringLiteral("/测井数据/A1.xml"), ImportUxFixtures::comprehensiveXml()));
    QVERIFY(write(root + QStringLiteral("/测井数据/B1.xml"), ImportUxFixtures::comprehensiveXml().replace("A1", "B1")));
    QVERIFY(write(root + QStringLiteral("/参考资料/reference.txt"), "reference document"));
    QgisProjectService project; PaleoProjectStore store; DataImportService service(&store);
    connect(&project, &QgisProjectService::projectOpened, &service, [&service](const QString &p) { service.setProjectDir(QFileInfo(p).absolutePath()); });
    FolderImportWorkflow imports(&service, nullptr); ProjectSetupWorkflow workflow(&project, &imports);
    ProjectSetupDialog dialog(&workflow);
    dialog.findChild<QLineEdit *>("projectDirectoryEdit")->setText(dir.filePath("output"));
    dialog.setSourceRoot(root);
    QTRY_COMPARE_WITH_TIMEOUT(dialog.preview().logs, 2, 20000);
    QCOMPARE(dialog.findChild<QLineEdit *>("projectPointsEdit")->text(), points);
    QCOMPARE(dialog.findChild<QLineEdit *>("projectWellsEdit")->text(), root + QStringLiteral("/测井数据"));
    QCOMPARE(dialog.findChild<QLineEdit *>("projectReferencesEdit")->text(), root + QStringLiteral("/参考资料"));
    QCOMPARE(dialog.findChild<QComboBox *>("pointField_x")->currentText(), QStringLiteral("Y（横坐标）"));
    QCOMPARE(dialog.findChild<QComboBox *>("pointField_y")->currentText(), QStringLiteral("X（纵坐标）"));
    QCOMPARE(dialog.findChild<QTableWidget *>("projectPointPreview")->rowCount(), 2);
    auto *crs = dialog.findChild<PaleoCrsSelector *>("pointSourceCrsEdit"); QVERIFY(crs->text().isEmpty());
    crs->setText("EPSG:2436"); dialog.findChild<QPushButton *>("projectSetupScanButton")->click();
    QTRY_COMPARE_WITH_TIMEOUT(dialog.preview().pointSourceCrs, QString("EPSG:2436"), 20000);
    QVERIFY(dialog.preview().errors.isEmpty()); QVERIFY(dialog.preview().sourceProblems.isEmpty());
    QCOMPARE(dialog.preview().paths.size(), 4); QCOMPARE(dialog.preview().wells, 2);
    const auto capture = qEnvironmentVariable("PALEO_HXN_SETUP_CAPTURE");
    if (!capture.isEmpty()) {
      dialog.resize(1100, 800); dialog.show(); QTest::qWait(100);
      QVERIFY(dialog.grab().save(capture + ".root.png"));
      dialog.findChild<QTabBar *>()->setCurrentIndex(1); QTest::qWait(100); QVERIFY(dialog.grab().save(capture));
      dialog.findChild<QTabBar *>()->setCurrentIndex(2); QTest::qWait(100); QVERIFY(dialog.grab().save(capture + ".sources.png"));
    }
    dialog.findChild<QTabBar *>()->setCurrentIndex(3);
    dialog.findChild<QPushButton *>("projectSetupNextButton")->click(); QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QSignalSpy finished(&workflow, &ProjectSetupWorkflow::finished);
    const auto target = dialog.projectPath();
    QVERIFY2(workflow.create(target, dialog.sources(), dialog.preview(), &error), qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 30000);
    QVERIFY2(finished.first()[1].toBool(), qPrintable(finished.first()[0].toString()));
    QCOMPARE(service.catalog()->entities("well").size(), 2);
    QVERIFY(QFileInfo::exists(paleo::area_model::directory(QFileInfo(target).absolutePath()) + "/data.sqlite"));
    QCOMPARE(PreviewDocService::xmlDataListAt(root + QStringLiteral("/测井数据/A1.xml")).format, QString("data_service"));
    project.closeProject(); QVERIFY2(project.openProject(target), qPrintable(project.lastErrors().join('\n')));
    QCOMPARE(service.catalog()->entities("well").size(), 2);
    QVERIFY(QFileInfo::exists(paleo::area_model::directory(QFileInfo(target).absolutePath()) + "/data.sqlite"));
    QCOMPARE(PreviewDocService::xmlDataListAt(root + QStringLiteral("/测井数据/A1.xml")).format, QString("data_service"));
    QCOMPARE(workflow.currentSources().points, points);
    QCOMPARE(workflow.currentSources().wells, root + QStringLiteral("/测井数据"));
    QVERIFY(project.georeference().has_value());
    QCOMPARE(project.georeference()->sourceCrs, QString("EPSG:2436"));
    for (const auto &well : service.catalog()->entities("well")) {
      QCOMPARE(well.surfaceX, well.name == "A1" ? 300000.0 : 301000.0);
      QCOMPARE(well.surfaceY, well.name == "A1" ? 2360000.0 : 2361000.0);
      const auto curves = WellLogSet::wellCurveIndex(service.catalog(), QFileInfo(target).absolutePath(), well.id);
      auto por = std::find_if(curves.begin(), curves.end(), [](const auto &curve) { return curve.mnemonic == "HXN_POR"; });
      QVERIFY(por != curves.end()); WellLogSet::WellCurveTvdSamples samples;
      QVERIFY2(WellLogSet::readCurveTvd(*por, nullptr, &samples, &error), qPrintable(error));
      QCOMPARE(samples.md, QVector<double>({1000, 1001})); QCOMPARE(samples.values, QVector<double>({8, 9}));
    }
    project.closeProject();
  }
  void directDirectoryCreationNeedsNoFolderConvention() {
    QTemporaryDir dir; QString error;
    const QString input = dir.filePath("arbitrary-layout");
    QVERIFY(write(input + "/anything.xml", ImportUxFixtures::comprehensiveXml()));
    QgisProjectService project; PaleoProjectStore store; DataImportService service(&store);
    connect(&project, &QgisProjectService::projectOpened, &service,
            [&service](const QString &p) { service.setProjectDir(QFileInfo(p).absolutePath()); });
    PaleoTaskService tasks;
    FolderImportWorkflow imports(&service, nullptr); ProjectSetupWorkflow workflow(&project, &imports);
    workflow.setMapImportServices(nullptr, &tasks);
    ProjectSetupDialog dialog(&workflow);
    dialog.findChild<QLineEdit *>("projectDirectoryEdit")->setText(dir.filePath("output"));
    dialog.findChild<QLineEdit *>("projectSourceRootEdit")->setText(input);
    dialog.findChild<QPushButton *>("createFromDirectoryButton")->click();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(dialog.sources().root, input); QVERIFY(dialog.preview().paths.isEmpty());
    QSignalSpy done(&workflow, &ProjectSetupWorkflow::finished);
    QVERIFY2(workflow.create(dialog.projectPath(), dialog.sources(), dialog.preview(), &error), qPrintable(error));
    const auto modelTaskVisible = [&] {
      for (auto *task : tasks.tasks()) if (task->title() == QStringLiteral("导入工区数据模型")) return !task->quiet();
      return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(modelTaskVisible(), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 30000);
    QVERIFY2(done[0][1].toBool(), qPrintable(done[0][0].toString()));
    const auto table = PreviewDocService::xmlDataListAt(input + "/anything.xml");
    QVERIFY2(table.ok, qPrintable(table.error)); QCOMPARE(table.format, QString("data_service"));
  }
  void emptyRootDoesNotClaimRecognitionSucceeded() {
    QTemporaryDir dir;
    QgisProjectService project; PaleoProjectStore store; DataImportService service(&store);
    FolderImportWorkflow imports(&service, nullptr); ProjectSetupWorkflow workflow(&project, &imports);
    ProjectSetupDialog dialog(&workflow); dialog.setSourceRoot(dir.path());
    auto *status = dialog.findChild<QLabel *>("projectSetupStatus");
    QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(QStringLiteral("未找到可导入资料")), 20000);
    QVERIFY(dialog.preview().paths.isEmpty());
  }
  void hxnRootWizardWhenExplicitlyProvided() {
    const auto root = qEnvironmentVariable("PALEO_HXN_DATA_DIR");
    if (root.isEmpty()) QSKIP("Set PALEO_HXN_DATA_DIR for read-only source directory verification");
    const auto sources = discoverProjectSources(root);
    QCOMPARE(QFileInfo(sources.points).fileName(), QStringLiteral("坐标统计.xlsx"));
    QCOMPARE(QFileInfo(sources.wells).fileName(), QStringLiteral("测井数据"));
    QCOMPARE(QFileInfo(sources.references).fileName(), QStringLiteral("参考资料"));
    QgisProjectService project; PaleoProjectStore store; DataImportService service(&store);
    FolderImportWorkflow imports(&service, nullptr); ProjectSetupWorkflow workflow(&project, &imports);
    ProjectSetupDialog dialog(&workflow); QSignalSpy ready(&workflow, &ProjectSetupWorkflow::previewReady);
    QElapsedTimer elapsed; elapsed.start(); dialog.setSourceRoot(root);
    QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 300000);
    const auto xmlFiles = QDir(sources.wells).entryInfoList({"*.xml"}, QDir::Files);
    QCOMPARE(xmlFiles.size(), 57); int auxiliary = 0;
    for (const auto &file : xmlFiles) {
      QVERIFY(dialog.preview().paths.contains(file.absoluteFilePath()));
      const bool reference = isFixedAuxiliaryPath(file.absoluteFilePath());
      if (reference) ++auxiliary;
      QCOMPARE(dialog.preview().types.value(file.absoluteFilePath()), reference ? QString("reference") : QString("well_log"));
    }
    QCOMPARE(dialog.preview().logs, xmlFiles.size() - auxiliary);
    QCOMPARE(dialog.preview().pointRows.size(), 55);
    QVERIFY(dialog.findChild<QComboBox *>("pointField_x")->currentText().contains(QStringLiteral("横")));
    QVERIFY(dialog.findChild<QComboBox *>("pointField_y")->currentText().contains(QStringLiteral("纵")));
    QVERIFY(dialog.findChild<PaleoCrsSelector *>("pointSourceCrsEdit")->text().isEmpty()); // No guessed CRS or band conversion.
    QVERIFY(dialog.preview().errors.isEmpty());
    QVERIFY(dialog.preview().hashes.isEmpty()); QVERIFY(dialog.preview().mapHashes.isEmpty());
    qint64 bytes = 0; for (const auto &snapshot : dialog.preview().sourceSnapshots)
      bytes += snapshot.value("prefixBytes").toLongLong();
    qInfo() << "HXN root wizard: 57 XML files recognized; logs=" << dialog.preview().logs
            << "existing auxiliary references=" << auxiliary << "point rows=" << dialog.preview().pointRows.size()
            << "elapsed_ms=" << elapsed.elapsed() << "identity_probe_bytes=" << bytes;
  }
  void projectLocationPreflightRejectsFilesAndLeavesNoFolders() {
    QTemporaryDir root; QString error;
    const auto target = root.filePath(QStringLiteral("nested/中文工程/中文工程.qgz"));
    QVERIFY2(ProjectSetupWorkflow::checkProjectLocation(target, &error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(root.filePath("nested"))); QVERIFY(QDir(root.path()).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
    const auto blocker = root.filePath("blocked"); QVERIFY(write(blocker, "keep existing file"));
    QVERIFY(!ProjectSetupWorkflow::checkProjectLocation(blocker + "/test17.qgz", &error));
    QVERIFY(error.contains(blocker));
    QVERIFY(QDir().mkpath(root.filePath("existing")));
    QVERIFY(write(root.filePath("existing/project.paleo"), "keep existing manifest"));
    QVERIFY(!ProjectSetupWorkflow::checkProjectLocation(root.filePath("existing/test17.qgz"), &error));
    QVERIFY(error.contains(QStringLiteral("已有工程")));
  }

  void projectCreationFailurePreservesCurrentProjectAndWizardFields() {
    QTemporaryDir root; QString error; const auto blocker = root.filePath("blocked"); QVERIFY(write(blocker, "do not remove"));
    QgisProjectService project; PaleoProjectStore store; DataImportService service(&store);
    FolderImportWorkflow imports(&service, nullptr); ProjectSetupWorkflow workflow(&project, &imports);
    const auto previous = root.filePath("existing.qgz"); QVERIFY(project.createProject(previous));
    QVERIFY(!workflow.create(blocker + "/test17/test17.qgz", {}, {}, &error)); QCOMPARE(project.projectPath(), previous);
    ProjectSetupDialog dialog(&workflow);
    auto *name = dialog.findChild<QLineEdit *>("projectNameEdit"); auto *output = dialog.findChild<QLineEdit *>("projectDirectoryEdit");
    auto *points = dialog.findChild<QLineEdit *>("projectPointsEdit");
    name->setText("test17"); output->setText(blocker); points->setText(root.filePath("source.xlsx"));
    dialog.findChild<QPushButton *>("createEmptyProjectButton")->click();
    QCOMPARE(dialog.result(), int(QDialog::Rejected)); QCOMPARE(name->text(), QString("test17")); QCOMPARE(points->text(), root.filePath("source.xlsx"));
    QVERIFY(dialog.findChild<QLabel *>("projectSetupStatus")->text().contains(blocker));
    output->setText(root.path()); dialog.findChild<QPushButton *>("createEmptyProjectButton")->click();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    dialog.showCreationError(QStringLiteral("系统错误 5：拒绝访问"));
    QCOMPARE(name->text(), QString("test17")); QCOMPARE(points->text(), root.filePath("source.xlsx"));
    QCOMPARE(dialog.findChild<QLabel *>("projectSetupStatus")->text(), QStringLiteral("系统错误 5：拒绝访问"));
    project.closeProject();
  }

  void realProjectParentCanCreateAndReopenInPrivateSubdirectory() {
    const auto parent = qEnvironmentVariable("PALEO_PROJECT_CREATE_TEST_PARENT");
    if (parent.isEmpty()) QSKIP("Set explicit project parent for a private creation probe");
    QTemporaryDir root(QDir(parent).filePath(".paleo-create-smoke-XXXXXX")); QVERIFY2(root.isValid(), qPrintable(root.errorString()));
    // Exercise the original Qt directory operation in the same process as well.
    const auto control = root.filePath("qt-control/test17"); QVERIFY(QDir().mkpath(control));
    QgisProjectService project; PaleoProjectStore store; DataImportService service(&store);
    connect(&project, &QgisProjectService::projectOpened, &service, [&service](const QString &p) { service.setProjectDir(QFileInfo(p).absolutePath()); });
    FolderImportWorkflow imports(&service, nullptr); ProjectSetupWorkflow workflow(&project, &imports); QString error;
    QSignalSpy finished(&workflow, &ProjectSetupWorkflow::finished);
    const auto target = root.filePath("test17/test17.qgz"); QVERIFY2(workflow.create(target, {}, {}, &error), qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
    QVERIFY2(finished.first()[1].toBool(), qPrintable(finished.first()[0].toString()));
    QVERIFY(QFileInfo::exists(root.filePath("test17/project.paleo")));
    project.closeProject(); QVERIFY2(project.openProject(target), qPrintable(project.lastErrors().join('\n'))); project.closeProject();
    qInfo() << "PROJECT_CREATE_PROBE parent=" << parent << "Qt_mkpath=true create=true reopen=true";
  }

  void classificationsAgreeAcrossSetupFilesFolderAndRetry() {
    QTemporaryDir dir; const auto source = dir.filePath("source"); QString error;
    const auto heads = xlsx(source + "/coordinates.xlsx", "data", {{"WELL", "X", "Y"}, {"A1", "10", "20"}});
    QVERIFY(!heads.isEmpty());
    QMap<QString, QString> expected{{heads, "well_head"}};
    const QList<QPair<QString, QByteArray>> samples{
      {"layers.csv", "WELL,HORIZON,MD,TVD\nA1,D61,100,95\n"},
      {"curve.csv", ImportUxFixtures::csv()},
      {"renamed.txt", "~Version\nVERS. 2.0 :\n~Well\nWELL. A1 :\n~Curve\nDEPT.M :\nGR.API :\n~A\n100 50\n101 60\n"},
      {"td.dat", "#TimeDepth File From SMI\n# Well : A1\n0 0 0 0\n1 -100 100 120\n"},
      {"trajectory.csv", "MD,INCL,AZIM\n0,0,0\n100,10,20\n"},
      {"grid.dat", "# Grid_size:2x2\n# P1: 1,1,0,0\n# Z_units: ms\n0 0 100 1 1\n"},
      {"volume.sgy", QByteArray(3600, '\0')}, {"notes.pdf", "%PDF-1.4\nfixture"},
      {"photo.png", "image fixture"}, {"data.sfpkg", "package fixture"},
      {QStringLiteral("参考资料/reference.xml"), ImportUxFixtures::xml()},
      {"HZ28-6-1.xml", ImportUxFixtures::xml()}, {"unknown.bin", "unrecognized fixture"},
      {"large.xml", ImportUxFixtures::paddedXml()}};
    const QStringList types{"well_stratification", "well_log", "well_log", "time_depth", "well_deviation", "horizon",
        "seismic", "document", "image_reference", "single_factor_package", "reference", "reference", "unknown", "well_log"};
    for (int i = 0; i < samples.size(); ++i) {
      const auto path = source + '/' + samples[i].first; QVERIFY(write(path, samples[i].second)); expected.insert(path, types[i]);
    }
    ProjectSetupSources sources; sources.wells = source;
    const auto setup = scanProjectSources(sources);
    QVERIFY2(setup.errors.isEmpty(), qPrintable(setup.errors.join('\n')));
    PaleoProjectStore store; DataImportService service(&store); service.setProjectDir(dir.filePath("project"));
    const auto files = service.previewFiles(expected.keys(), &error); QVERIFY2(error.isEmpty(), qPrintable(error));
    const auto folder = service.previewFolder(source, &error); QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(files.size(), expected.size()); QCOMPARE(folder.size(), expected.size());
    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
      QVERIFY2(setup.paths.contains(it.key()), qPrintable(it.key() + ": " + setup.sourceProblems.value(it.key())));
      QCOMPARE(setup.types.value(it.key()), it.value());
      for (const auto &rows : {files, folder}) for (const auto &row : rows) if (row.path == it.key()) {
        QCOMPARE(row.displayType, it.value());
        for (const auto &kind : {QString("files"), QString("wells"), QString("well_log"), QString("seismic")})
          QCOMPARE(ImportSourceDialog::preferredType(kind, row.path, row.displayType), it.value());
      }
    }
    // Retry used to classify this workbook by suffix, losing its well records.
    const auto retried = service.importFolderRow(heads, {}, &error);
    QVERIFY2(!retried.assetId.isEmpty(), qPrintable(error));
    QCOMPARE(service.catalog()->assetById(retried.assetId).type, QString("well_head"));
    QCOMPARE(service.catalog()->entities("well").size(), 1);
    const auto retriedXml = service.importFolderRow(source + "/large.xml", {}, &error);
    QVERIFY2(!retriedXml.assetId.isEmpty(), qPrintable(error));
    QCOMPARE(service.catalog()->assetById(retriedXml.assetId).type, QString("well_log"));
  }
  void addingWellDataKeepsExistingAssetsAndVersions() {
    QTemporaryDir dir; const auto heads = dir.filePath("source/heads.csv");
    const auto first = dir.filePath("source/GR.csv"), second = dir.filePath("source/RES.csv");
    QVERIFY(write(heads, "WELL,X,Y\nA1,10,20\n"));
    QVERIFY(write(first, ImportUxFixtures::csv("A1", "GR[API]")));
    QVERIFY(write(second, ImportUxFixtures::csv("A1", "RES[ohm.m]", "1000,10\n1000.5,20\n1001,30\n")));
    PaleoProjectStore store; DataImportService service(&store); service.setProjectDir(dir.filePath("project")); QString error;
    const auto initial = service.importFiles({heads, first}, &error); QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(initial.size(), 2); QCOMPARE(service.catalog()->assets().size(), 2);
    const auto previous = service.catalog()->assets(); QMap<QString, QString> versions;
    for (const auto &asset : previous) versions.insert(asset.id, service.catalog()->currentVersion(asset.id).id);
    const auto added = service.importFiles({second}, &error); QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(added.size(), 1); QVERIFY(!added.first().assetId.isEmpty());
    QCOMPARE(service.catalog()->assets().size(), 3); QCOMPARE(service.catalog()->entities("well").size(), 1);
    for (const auto &asset : previous) QCOMPARE(service.catalog()->currentVersion(asset.id).id, versions.value(asset.id));
    const auto links = service.catalog()->linksForAsset(added.first().assetId); QCOMPARE(links.size(), 1);
    QCOMPARE(links.first().role, QString("well_log")); QVERIFY(!links.first().unresolved);
  }
  void everyClassifierTypeHasAnAddEntryAndFilters() {
    DataListPanel panel; QSignalSpy requested(&panel, &DataListPanel::importRequested);
    auto *add = panel.findChild<QPushButton *>("importAddData"); QVERIFY(add); QVERIFY(add->menu());
    QCOMPARE(add->menu()->actions().size(), projectClassifierTypes().size());
    for (const auto &type : projectClassifierTypes()) {
      auto *action = add->findChild<QAction *>("addData_" + type); QVERIFY2(action, qPrintable(type));
      action->trigger(); QCOMPARE(requested.takeFirst().first().toString(), type);
      ImportSourceDialog source(type);
      for (const auto &extension : projectImportExtensions(type)) QVERIFY(source.fileFilter().contains("*." + extension));
      source.setSources({QDir::tempPath()}, true); QCOMPARE(source.folder(), QDir::tempPath());
    }
    QCOMPARE(suggestedProjectImportType("points.csv", "well_head", "well_log"), QString("well_head"));
    QCOMPARE(suggestedProjectImportType("polygon.geojson", "geojson", "boundary"), QString("boundary"));
    QCOMPARE(suggestedProjectImportType("plain.dat", "tabular", "time_depth"), QString("time_depth"));
    QCOMPARE(suggestedProjectImportType("photo.png", "image_reference", "horizon"), QString("image_reference"));
  }
  void referenceDefaultsAndExplicitChoicesAgreeOnExecution() {
    QTemporaryDir dir; const auto path = dir.filePath(QStringLiteral("参考资料/curve.xml"));
    QVERIFY(write(path, ImportUxFixtures::xml())); QString error;
    for (const auto &mode : {QString("single"), QString("batch"), QString("override")}) {
      PaleoProjectStore store; DataImportService service(&store); service.setProjectDir(dir.filePath("project-" + mode));
      const auto preview = service.previewFiles({path}, &error); QCOMPARE(preview.size(), 1);
      QCOMPARE(preview.first().classifiedType, QString("well_log")); QCOMPARE(preview.first().displayType, QString("reference"));
      QMap<QString, QString> overrides;
      if (mode == "override") {
        QTableWidget table(0, 5); QVector<QComboBox *> combos;
        PaleoFolderConfirm::populateFolderConfirmTable(&table, QFileInfo(path).absolutePath(), preview, &combos);
        combos.first()->setCurrentIndex(combos.first()->findData("well_log"));
        overrides = PaleoFolderConfirm::collectFolderTypeOverrides(&table, preview, combos);
        QCOMPARE(overrides.value(path), QString("well_log"));
      }
      QString asset;
      if (mode == "single") asset = service.importProjectFile(path, &error);
      else {
        const auto rows = service.importFiles({path}, &error, overrides); QCOMPARE(rows.size(), 1); asset = rows.first().assetId;
      }
      QVERIFY2(!asset.isEmpty(), qPrintable(error));
      QCOMPARE(service.catalog()->assetById(asset).type, mode == "override" ? QString("well_log") : QString("reference"));
      QCOMPARE(service.catalog()->entities("well").size(), mode == "override" ? 1 : 0);
    }
  }
  void interpretedTablesAreReadyOnImport() {
    QTemporaryDir source, projectDir; QString error;
    const auto heads = xlsx(source.filePath("heads.xlsx"), "data", {{"WELL", "X", "Y"}, {"A4", "10", "20"}, {"B4", "30", "40"}});
    const auto tops = xlsx(source.filePath("tops.xlsx"), "data", {{"WELL", QStringLiteral("层位"), QStringLiteral("顶界垂深(m)"), QStringLiteral("底界垂深(m)")}, {"A4", "D61", "100", "150"}, {"A4", "D62", "150", "200"}, {"B4", "D61", "80", "110"}});
    const auto cuttings = xlsx(source.filePath(QStringLiteral("A4/A4岩屑录井数据.xlsx")), "Sheet1", {{QStringLiteral("顶深(m)"), QStringLiteral("底深(m)"), QStringLiteral("岩石定名"), QStringLiteral("颜色"), QStringLiteral("岩性描述"), QStringLiteral("数据来源")}, {"120", "180", QStringLiteral("泥岩"), QStringLiteral("深灰"), QStringLiteral("保留描述，含换行\n第二行"), QStringLiteral("GR推导（合成）")}});
    const auto td = source.filePath("A4_td.dat"); QVERIFY(write(td, "# Well : A4\n0 0 0 0\n100 100 100 120\n200 200 200 240\n"));
    PaleoProjectStore store; DataImportService service(&store); service.setProjectDir(projectDir.path());
    auto session = service.beginImport(); QVERIFY(session); session->linkExternal = true;
    const auto log = source.filePath("A4.las");
    QVERIFY(write(log, "~Version\nVERS. 2.0 :\nWRAP. NO :\n~Well\nWELL. A4 :\nNULL. -9999.25 :\n# PALEO_SYNTHETIC_INPUT test source\n~Curve\nDEPT.M :\nGR.API :\n~A\n120 60\n121 70\n"));
    DataImportService::produceFiles(*session, {heads, tops, td, cuttings, log}, {{heads, "well_head"}, {tops, "well_stratification"}, {td, "time_depth"}}, {});
    QVERIFY2(session->error.isEmpty(), qPrintable(session->error));
    for (const auto &row : session->rows) QVERIFY2(row.outcome != FolderRowResult::Outcome::Failed, qPrintable(row.message));
    QVERIFY2(service.commitImport(*session, &error) == DataImportService::CommitStatus::Committed, qPrintable(error));
    auto *cat = service.catalog(); QCOMPARE(cat->entities("well").size(), 2); CatalogEntity well;
    for (const auto &candidate : cat->entities("well")) if (candidate.name == "A4") well = candidate;
    ProjectDataFacade data; data.setCatalog(cat); const auto layers = data.topsFor(well.id);
    QCOMPARE(layers.size(), 2); QCOMPARE(layers[0].md, 120.0); QCOMPARE(layers[1].md, 180.0); QCOMPARE(layers[1].tvd, 150.0);
    CatalogVersion normalized, rock, logVersion;
    for (const auto &link : cat->linksForEntity(well.id)) {
      if (link.role == "tops") normalized = cat->currentVersion(link.assetId);
      if (link.role == "cuttings") rock = cat->currentVersion(link.assetId);
      if (link.role == "well_log") logVersion = cat->currentVersion(link.assetId);
    }
    QVERIFY(normalized.managed); QVERIFY(normalized.extra.contains("depthResolution"));
    const auto bases = paleo::io::readWellTableBases(cat->versionFilePath(normalized)); QCOMPARE(bases.size(), 3); QCOMPARE(bases[1].md, 240.0); QCOMPARE(bases[1].tvd, 200.0);
    QVERIFY(rock.managed); QCOMPARE(rock.fileName, QString("cuttings.csv"));
    const auto table = paleo::io::readCuttingsFile(cat->versionFilePath(rock)); QVERIFY(table.ok); QCOMPARE(table.intervals.size(), 1);
    QCOMPARE(table.intervals[0].description, QStringLiteral("保留描述，含换行\n第二行"));
    QCOMPARE(table.intervals[0].source, QStringLiteral("GR推导（合成）"));
    QVERIFY(rock.extra.value("synthetic_input").toBool());
    QVERIFY(!logVersion.id.isEmpty());
    QVERIFY(logVersion.extra.value("synthetic_input").toBool());
    QVERIFY(!logVersion.extra.value("input_source_annotations").toStringList().isEmpty());
    QCOMPARE(WellComposite::LithologyStyle::colorFromName(table.intervals[0].litho), QColor("#616161"));
    QVERIFY(QFile::remove(tops)); QVERIFY(QFile::remove(cuttings));
    WellSectionWorkflow lithology(cat); QCOMPARE(lithology.lithologiesFor({well.id}).value(well.id).size(), 1);
    QVERIFY(lithology.lithologiesFor({well.id}).value(well.id).first().provenance.contains(QStringLiteral("GR推导（合成）")));
    PreviewDocService doc(&service); QVector<WellTopRecord> display; QHash<QString, double> displayBases;
    doc.wellTopsForLog(well.id, &display, nullptr, &displayBases);
    QCOMPARE(displayBases.size(), 2); QCOMPARE(displayBases.value("D61"), 180.0); QCOMPARE(displayBases.value("D62"), 240.0);
  }
  void pointsOptionalAndMapping() {
    QTemporaryDir dir;
    const auto path = xlsx(dir.filePath("points.xlsx"), "UTM49N", {{QStringLiteral("井号"), QStringLiteral("井口横坐标X_UTM49N(m)"), QStringLiteral("井口纵坐标Y_UTM49N(m)")}, {"A1", "233240.46", "3997621.12"}, {"A2", "bad", "1"}});
    const auto t = paleo::io::readWellTable(path);
    QCOMPARE(t.heads.size(), 1); QCOMPARE(t.report.rejectedRows, 1); QCOMPARE(t.crs, QString("EPSG:32649")); QVERIFY(std::isnan(t.heads.first().kb));
    QCOMPARE(classifyImportFile(path).type, QString("well_head"));
    const auto custom = xlsx(dir.filePath("custom.xlsx"), "data", {{"ID", "E", "N"}, {"A9", "10", "20"}});
    QVERIFY(paleo::io::readWellTable(custom).heads.isEmpty());
    const auto mapped = paleo::io::readWellTable(custom, {{"well", "ID"}, {"x", "E"}, {"y", "N"}});
    QCOMPARE(mapped.heads.first().name, QString("A9"));
  }
  void stratificationDepthAndSubsets() {
    QTemporaryDir dir;
    const QVector<QStringList> rows{{QStringLiteral("层位"), QStringLiteral("顶界垂深(m)"), QStringLiteral("底界垂深(m)")}, {"D61", "100", "120"}, {"D62", "120", "130"}};
    auto path = xlsx(dir.filePath(QStringLiteral("井/总表.xlsx")), "A1", rows);
    xlsx(dir.filePath(QStringLiteral("井/A1/分层.xlsx")), "A1", rows);
    const auto table = paleo::io::readWellTable(path);
    QCOMPARE(table.tops.size(), 2); QVERIFY(table.tops[0].hasTvd); QVERIFY(!table.tops[0].hasMd);
    const auto bases = paleo::io::readWellTableBases(path); QCOMPARE(bases.size(), 2); QCOMPARE(bases[1].tvd, 130.0); QVERIFY(!bases[1].hasMd);
    ProjectSetupSources s; s.wells = dir.filePath(QStringLiteral("井"));
    const auto p = scanProjectSources(s); QCOMPARE(p.tops, 2); QCOMPARE(p.paths.size(), 1); QVERIFY(!p.notes.isEmpty());
  }
  void multiplePointSheetsAndDepthBases() {
    QTemporaryDir dir; QString error;
    const auto path = xlsx(dir.filePath("points.xlsx"), "first", {{"WELL", "X", "Y", "KB"}, {"Well A", "10", "20", "-99999"}});
    const auto archive = paleo::io::zipReadArchive(path, &error);
    QByteArray workbook, relations, sheet;
    QVERIFY(paleo::io::zipExtractBytes(archive, "xl/workbook.xml", &workbook, &error));
    QVERIFY(paleo::io::zipExtractBytes(archive, "xl/_rels/workbook.xml.rels", &relations, &error));
    QVERIFY(paleo::io::zipExtractBytes(archive, "xl/worksheets/sheet1.xml", &sheet, &error));
    workbook.replace("</sheets>", "<sheet name=\"second\" sheetId=\"2\" r:id=\"rId2\"/></sheets>");
    relations.replace("</Relationships>", "<Relationship Id=\"rId2\" Target=\"worksheets/sheet2.xml\"/></Relationships>");
    QByteArray second = sheet; second.replace("Well A", "Well B");
    QVERIFY(paleo::io::zipWriteArchive(path, {{"xl/workbook.xml", workbook}, {"xl/_rels/workbook.xml.rels", relations}, {"xl/worksheets/sheet1.xml", sheet}, {"xl/worksheets/sheet2.xml", second}}, {}, &error));
    const auto first = paleo::io::readWellTable(path);
    QCOMPARE(first.sheets.size(), 2); QCOMPARE(first.heads.size(), 1); QCOMPARE(first.mapping.value("sheet").toString(), QString("first"));
    QCOMPARE(first.heads[0].name, QString("Well A")); QVERIFY(std::isnan(first.heads[0].kb));
    const auto roundtrip = parseWellHeadText(first.normalized); QCOMPARE(roundtrip.size(), 1); QCOMPARE(roundtrip[0].name, QString("Well A"));
    const auto selected = paleo::io::readWellTable(path, {{"sheet", "second"}});
    QCOMPARE(selected.heads.size(), 1); QCOMPARE(selected.heads[0].name, QString("Well B"));
    const auto tops = xlsx(dir.filePath("tops.xlsx"), "A1", {{"TOP", "MD", "TVD"}, {"D61", "105", "100"}, {"D62", "120", "-99999"}});
    const auto depths = paleo::io::readWellTable(tops);
    QCOMPARE(depths.tops.size(), 2); QVERIFY(depths.tops[0].hasMd); QVERIFY(depths.tops[0].hasTvd);
    QCOMPARE(depths.tops[0].md, 105.0); QCOMPARE(depths.tops[0].tvd, 100.0);
    QVERIFY(depths.tops[1].hasMd); QVERIFY(!depths.tops[1].hasTvd);
    const auto mixed = xlsx(dir.filePath("mixed.xlsx"), "A1",
                            {{"WELL", "X", "Y", "TOP", "MD"}, {"A1", "10", "20", "D61", "100"}, {"A1", "10", "20", "D62", "120"}});
    const auto mixedTable = paleo::io::readWellTable(mixed);
    QCOMPARE(mixedTable.type, QString("well_stratification"));
    QCOMPARE(mixedTable.tops.size(), 2);
    QCOMPARE(mixedTable.tops[0].topName, QString("D61"));
    const auto baseOnly = xlsx(dir.filePath("base.xlsx"), "A1", {{"WELL", "BASE_MD"}, {"A1", "80"}});
    const auto baseTable = paleo::io::readWellTable(baseOnly);
    QVERIFY(baseTable.tops.isEmpty());
  }
  // #671：表名 Sheet1、表头 WELL_NAME 仍对上井名；MD 与 BASE_MD 并存时顶深取 MD。
  void sheet1WellNameAndBaseMdKeepTopRoles() {
    QTemporaryDir dir;
    const auto named = xlsx(dir.filePath("named.xlsx"), "Sheet1", {{"WELL_NAME", "TOP", "MD"}, {"A1", "D61", "100"}});
    const auto t = paleo::io::readWellTable(named);
    QCOMPARE(t.tops.size(), 1); QCOMPARE(t.tops[0].wellName, QString("A1")); QCOMPARE(t.tops[0].topName, QString("D61"));
    QVERIFY(t.tops[0].hasMd); QCOMPARE(t.tops[0].md, 100.0);
    const auto both = xlsx(dir.filePath("both.xlsx"), "Sheet1", {{"WELL", "TOP", "MD", "BASE_MD"}, {"A1", "D61", "100", "80"}});
    const auto b = paleo::io::readWellTable(both);
    QCOMPARE(b.tops.size(), 1); QVERIFY(b.tops[0].hasMd); QCOMPARE(b.tops[0].md, 100.0);
  }
  // #670：井点槽里放带层位的井表，扫描 + 导入后井在给定坐标上，两条分层都在，不失败。
  void pointsSlotWithTopsKeepsHeadsAndTops() {
    QTemporaryDir dir; QString error;
    const auto points = xlsx(dir.filePath(QString::fromUtf8("source/井坐标.xlsx")), "data",
                             {{"WELL", "X", "Y", "TOP", "MD"}, {"A1", "1000", "2000", "D61", "100"}, {"A1", "1000", "2000", "D62", "120"}});
    QVERIFY(!points.isEmpty());
    ProjectSetupSources sources; sources.points = points;
    const auto setup = scanProjectSources(sources);
    QVERIFY2(setup.errors.isEmpty(), qPrintable(setup.errors.join('\n')));
    QVERIFY(setup.paths.contains(points));
    PaleoProjectStore store; DataImportService service(&store); service.setProjectDir(dir.filePath("project"));
    const auto rows = service.importFiles(setup.paths, &error, setup.types);
    QCOMPARE(rows.size(), 1);
    QVERIFY2(rows[0].outcome != FolderRowResult::Outcome::Failed, qPrintable(rows[0].message + error));
    QVERIFY2(!rows[0].assetId.isEmpty(), qPrintable(rows[0].message));
    ProjectDataFacade data; data.setCatalog(service.catalog(), dir.filePath("project"));
    const auto wells = data.wells(); QCOMPARE(wells.size(), 1); QCOMPARE(wells[0].name, QString("A1"));
    QCOMPARE(wells[0].surfaceX, 1000.0); QCOMPARE(wells[0].surfaceY, 2000.0);
    const auto tops = data.topsForMapping(wells[0].id);
    QStringList names; for (const auto &top : tops) names << top.horizon;
    QVERIFY2(names.contains("D61") && names.contains("D62"), qPrintable(names.join(',')));
  }
  void vectorsAndText() {
    QTemporaryDir dir;
    const auto text = dir.filePath("points.txt"); QVERIFY(write(text, "A1 10 20\nA2 30 40\n"));
    const auto t = paleo::io::readWellTable(text); QCOMPARE(t.heads.size(), 2); QVERIFY(std::isnan(t.heads[0].td));
    const auto vector = dir.filePath("points.geojson");
    QVERIFY(write(vector, R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{"name":"A1"},"geometry":{"type":"Point","coordinates":[108,36]}}]})"));
    const auto v = paleo::io::readWellTable(vector); QCOMPARE(v.heads.size(), 1); QCOMPARE(v.heads[0].x, 108.0); QVERIFY(!v.crs.isEmpty());
  }
  void noCoordinatesStillCreatesWell() {
    QTemporaryDir dir;
    const auto tops = xlsx(dir.filePath("tops.xlsx"), "A1", {{QStringLiteral("层位"), QStringLiteral("顶界垂深(m)")}, {"D61", "100"}});
    PaleoProjectStore store; DataImportService service(&store); service.setProjectDir(dir.filePath("project")); QString error;
    QVERIFY2(!service.importProjectFile(tops, &error).isEmpty(), qPrintable(error));
    const auto wells = service.catalog()->entities("well"); QCOMPARE(wells.size(), 1); QVERIFY(!wells.first().hasSurface);
  }
  void nativeHorizonAndBoundary() {
    QTemporaryDir dir; QString error;
    const QByteArray bytes("# Grid_size:2x2\n# P1: 1,1,0,0\n# P2: 1,2,20,0\n# P3: 2,2,20,40\n# Projection: UTM Zone 49N (WGS84)\n# Z_units: ms\n230000 4000000 100 1 1\n230020 4000001 101 1 2\n230002 4000040 102 2 1\n230022 4000041 103 2 2\n");
    BinnedHorizon grid; QVERIFY2(binHorizon(bytes, &grid, &error), qPrintable(error));
    QVERIFY(std::abs(grid.originX - 230002) < 1e-6); QVERIFY(std::abs(grid.originY - 4000040) < 1e-6); QVERIFY(std::abs(grid.columnY - 1) < 1e-8); QVERIFY(grid.geometryResidualM < 1e-6);
    const auto path = dir.filePath("boundary.geojson");
    QVERIFY(write(path, R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{},"geometry":{"type":"Polygon","coordinates":[[[0,0],[10,0],[5,8],[0,0]]]}}]})"));
    PaleoProjectStore store; DataImportService service(&store); service.setProjectDir(dir.filePath("project")); DataImportService::ImportOptions options; options.forceType = "boundary";
    QVERIFY2(!service.importProjectFile(path, options, &error).isEmpty(), qPrintable(error));
    const auto output = dir.filePath("survey.geojson"); QVERIFY(service.catalog()->writeSurveyGeoJson(output, &error));
    QFile f(output); QVERIFY(f.open(QIODevice::ReadOnly)); const auto coordinates = QJsonDocument::fromJson(f.readAll()).object()["features"].toArray().first().toObject()["geometry"].toObject()["coordinates"].toArray();
    QCOMPARE(coordinates.first().toArray().size(), 4); // triangle, not its bounding rectangle
  }
  void importAndReopen() {
    QTemporaryDir dir;
    const auto points = xlsx(dir.filePath("src/points.xlsx"), "UTM49N", {{QStringLiteral("井号"), "X", "Y"}, {"A1", "233240.46", "3997621.12"}});
    const auto tops = xlsx(dir.filePath("src/tops.xlsx"), "A1", {{QStringLiteral("层位"), QStringLiteral("顶界垂深(m)")}, {"D61", "100"}});
    PaleoProjectStore store; DataImportService service(&store); service.setProjectDir(dir.filePath("project")); QString error;
    const auto rows = service.importFiles({tops, points}, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error)); QCOMPARE(service.catalog()->entities("well").size(), 1);
    for (const auto &row : rows) QVERIFY2(row.outcome != FolderRowResult::Outcome::Failed, qPrintable(row.message));
    ProjectDataFacade data; data.setCatalog(service.catalog(), dir.filePath("project"));
    const auto well = service.catalog()->entities("well").first();
    const auto imported = data.topsFor(well.id); QCOMPARE(imported.size(), 1); QVERIFY(std::isnan(imported[0].md)); QCOMPARE(imported[0].tvd, 100.0);
    const auto count = service.catalog()->assets().size(); service.importFiles({points, tops}, &error); QCOMPARE(service.catalog()->assets().size(), count);
    QVERIFY(service.catalog()->writeWellsGeoJson(dir.filePath("project/artifacts/layers/wells.geojson"), &error));
    DataCatalog reopened; QVERIFY(reopened.open(dir.filePath("project"), &error)); QCOMPARE(reopened.entities("well").size(), 1); QVERIFY(std::isnan(reopened.entities("well").first().kb));
    data.setCatalog(&reopened, dir.filePath("project")); QCOMPARE(data.topsFor(well.id).size(), 1);
  }
  void georeferenceAnchorUsesQgisCoordinateEditor() {
    PaleoGeoreference g;
    g.a = 1.0;
    g.anchorLonDeg = 108.0; g.anchorLatDeg = 36.0;
    g.metersPerDegLon = 111320.0; g.metersPerDegLat = 101000.0;
    PaleoProjectFile settings; settings.georeference = g;
    ProjectMapSettingsDialog dialog(settings);
    auto *editor = dialog.findChild<PaleoCoordinateEditor *>(QStringLiteral("georeferenceAnchorEditor"));
    QVERIFY(editor);
    editor->setPoint(paleo::coordinates::point(108.037366, 36.086627));
    QCOMPARE(dialog.configuration().georeference->anchorLonDeg, 108.037366);
    QCOMPARE(dialog.configuration().georeference->anchorLatDeg, 36.086627);
    QCOMPARE(dialog.configuration().georeference->metersPerDegLon, 111320.0);
    QVERIFY(paleo::tests::captureVisual(&dialog, QStringLiteral("project-georeference-anchor"), QSize(720, 780)));
    dialog.show();
    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    QVERIFY(buttons);
    QSignalSpy saved(&dialog, &ProjectMapSettingsDialog::saveRequested);
    editor->xEdit()->setText(QStringLiteral("181"));
    buttons->button(QDialogButtonBox::Save)->click();
    QCOMPARE(saved.count(), 0);
    QVERIFY(editor->widget()->findChild<QLabel *>(QStringLiteral("coordinateError"))->isVisible());
    QCOMPARE(dialog.configuration().georeference->anchorLonDeg, 108.0);
    editor->xEdit()->setText(QStringLiteral("108.037366"));
    editor->yEdit()->setText(QStringLiteral("91"));
    buttons->button(QDialogButtonBox::Save)->click();
    QCOMPARE(saved.count(), 0);
  }
  void nativeProjectionRoundTrip() {
    PaleoGeoreference g; g.kind = "crs"; g.sourceCrs = "EPSG:32649"; g.targetCrs = "EPSG:4326";
    PaleoProjectFile settings; settings.georeference = g; ProjectMapSettingsDialog settingsDialog(settings); QCOMPARE(settingsDialog.configuration().georeference->sourceCrs, g.sourceCrs);
    auto *mapCrs = settingsDialog.findChild<PaleoCrsSelector *>(QStringLiteral("projectMapCrsSelector"));
    QVERIFY(mapCrs && mapCrs->nativeWidget());
    QVERIFY(settingsDialog.findChild<PaleoCoordinateEditor *>(QStringLiteral("georeferenceAnchorEditor"))->widget()->isHidden());
    QVERIFY(paleo::tests::captureVisual(&settingsDialog, QStringLiteral("project-coordinate-settings"), QSize(680, 760)));
    QVERIFY(g.isComplete()); double lon = 0, lat = 0; QVERIFY(applyGeoreference(g, 233240.46, 3997621.12, &lon, &lat)); QVERIFY(std::abs(lon - 108.037366) < 0.000001);
    PaleoGeoreference copy; QString error; QVERIFY(paleoGeoreferenceFromJson(paleoGeoreferenceToJson(g), &copy, &error)); QCOMPARE(copy.sourceCrs, g.sourceCrs);
    QgsProject project; QVERIFY2(paleo::mapreference::configure(&project, g, "EPSG:3857", &error), qPrintable(error));
    QgsCoordinateTransform ct(QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt()), QgsCoordinateReferenceSystem("EPSG:4326"), project.transformContext());
    const auto actual = ct.transform(QgsPointXY(233240.46, 3997621.12)); QVERIFY(std::abs(actual.x() - lon) < 1e-8); QVERIFY(std::abs(actual.y() - lat) < 1e-8);
  }
  void nativeProjectionAfterWorkerExit() {
    QgsCoordinateReferenceSystem::invalidateCache();
    QgsCoordinateTransform::invalidateCache();
    QSemaphore ready, release;
    QgsCoordinateReferenceSystem shared;
    auto worker = std::unique_ptr<QThread>(QThread::create([&] {
      const QgsCoordinateReferenceSystem source("EPSG:32649");
      shared = source; ready.release(); release.acquire();
    }));
    worker->start(); ready.acquire();
    const QgsCoordinateReferenceSystem borrowed("EPSG:32649");
    const bool validBeforeExit = !borrowed.toWkt().isEmpty();
    release.release(); worker->wait();
    QVERIFY(validBeforeExit);
    QgsCoordinateTransform transferred(shared, QgsCoordinateReferenceSystem("EPSG:4326"), QgsCoordinateTransformContext());
    const auto transferredPoint = transferred.transform(QgsPointXY(233240.46, 3997621.12));
    QVERIFY(std::abs(transferredPoint.x() - 108.037366) < .000001);
    PaleoGeoreference reference; reference.kind = "crs"; reference.sourceCrs = "EPSG:32649"; reference.targetCrs = "EPSG:4326";
    QgsProject project; QString error;
    QVERIFY2(paleo::mapreference::configure(&project, reference, "EPSG:3857", &error), qPrintable(error));
    QgsCoordinateTransform mapped(QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt()),
        QgsCoordinateReferenceSystem("EPSG:4326"), project.transformContext());
    const auto point = mapped.transform(QgsPointXY(233240.46, 3997621.12));
    QVERIFY(std::abs(point.x() - 108.037366) < .000001);
  }
  void invalidReferenceDoesNotChangeProject() {
    PaleoGeoreference reference; reference.kind = "crs"; reference.sourceCrs = "EPSG:32649"; reference.targetCrs = "EPSG:4326";
    QgsProject project; QString error;
    QVERIFY2(paleo::mapreference::configure(&project, reference, "EPSG:3857", &error), qPrintable(error));
    const auto before = project.transformContext();
    const auto targets = project.readListEntry("paleo", "localTransformTargets");
    reference.sourceCrs = "EPSG:0";
    QVERIFY(!paleo::mapreference::configure(&project, reference, "EPSG:3857", &error));
    QCOMPARE(project.crs().authid(), QString("EPSG:3857"));
    QVERIFY(project.transformContext() == before);
    QCOMPARE(project.readListEntry("paleo", "localTransformTargets"), targets);
  }
  void invalidCoordinatesDoNotCreateEmptyProject() {
    QTemporaryDir dir; QString error;
    QgisProjectService project; PaleoProjectStore store; DataImportService service(&store);
    FolderImportWorkflow imports(&service, nullptr); ProjectSetupWorkflow workflow(&project, &imports);
    const auto previous = dir.filePath("existing.qgz"); QVERIFY(project.createProject(previous));
    ProjectSetupPreview preview; preview.projectCrs = "EPSG:0";
    const auto target = dir.filePath("new/project.qgz");
    QVERIFY(!workflow.create(target, {}, preview, &error));
    QVERIFY(!error.isEmpty()); QVERIFY(!QFileInfo::exists(dir.filePath("new")));
    QCOMPARE(project.projectPath(), previous);
  }
  void emptyWizard() {
    QTemporaryDir root;
    QgisProjectService project; PaleoProjectStore store; DataImportService service(&store); FolderImportWorkflow imports(&service, nullptr); ProjectSetupWorkflow workflow(&project, &imports);
    ProjectSetupDialog dialog(&workflow); dialog.findChild<QLineEdit *>("projectDirectoryEdit")->setText(root.path());
    dialog.findChild<QPushButton *>("createEmptyProjectButton")->click(); QCOMPARE(dialog.result(), int(QDialog::Accepted)); QVERIFY(dialog.preview().paths.isEmpty());
  }
  void partialSourcesCreateReopenAndRepair() {
    QTemporaryDir dir; QString error;
    const auto heads = dir.filePath("input/wellheads.csv");
    const auto logs = dir.filePath("input/wells");
    const auto good = logs + "/B1.las", bad = logs + "/broken.las";
    QVERIFY(write(heads, "WELL,X,Y\nB1,bad,20\n"));
    QVERIFY(write(good, "~Version\nVERS. 2.0 :\nWRAP. NO :\n~Well\nWELL. B1 :\nNULL. -9999.25 :\n~Curve\nDEPT.M :\nGR.API :\n~A\n100 60\n101 70\n"));
    QVERIFY(write(bad, "broken log without curves"));
    ProjectSetupSources sources; sources.points = heads; sources.wells = logs;
    sources.references = dir.filePath("missing-references"); sources.paleomaps = dir.filePath("missing-maps");
    const auto preview = scanProjectSources(sources);
    QVERIFY(preview.errors.isEmpty()); QCOMPARE(preview.sourceProblems.size(), 3);
    QVERIFY(!preview.paths.contains(heads)); QVERIFY(preview.paths.contains(good));
    QgisProjectService project; PaleoProjectStore store; DataImportService service(&store);
    connect(&project, &QgisProjectService::projectOpened, &service, [&service](const QString &p) { service.setProjectDir(QFileInfo(p).absolutePath()); });
    FolderImportWorkflow imports(&service, nullptr); ProjectSetupWorkflow workflow(&project, &imports);
    QSignalSpy finished(&workflow, &ProjectSetupWorkflow::finished);
    const auto target = dir.filePath("output/project.qgz");
    QVERIFY2(workflow.create(target, sources, preview, &error), qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
    QVERIFY(!finished.first()[1].toBool()); QVERIFY(finished.first()[0].toString().contains(heads));
    QVERIFY(QFileInfo::exists(target)); QVERIFY(!service.catalog()->assets().isEmpty());
    QString logId;
    for (const auto &asset : service.catalog()->assets()) if (asset.type == "well_log") logId = asset.id;
    QVERIFY(!logId.isEmpty()); QCOMPARE(service.catalog()->entities("well").size(), 1);
    project.closeProject(); QVERIFY(project.openProject(target));
    QVERIFY(!service.catalog()->assetById(logId).id.isEmpty());
    QCOMPARE(project.mapConfiguration().sourceStats.value("sourceProblems").toMap().size(), 3);
    // Repair just the bad inputs; the valid log survives and is not duplicated.
    QVERIFY(write(heads, "WELL,X,Y\nB1,10,20\n")); QVERIFY(QFile::remove(bad));
    sources.references.clear(); sources.paleomaps.clear();
    const auto repaired = scanProjectSources(sources); QVERIFY(repaired.errors.isEmpty()); QVERIFY(repaired.sourceProblems.isEmpty());
    finished.clear(); QVERIFY2(workflow.applySources(sources, repaired, &error), qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
    QVERIFY2(finished.first()[1].toBool(), qPrintable(finished.first()[0].toString()));
    int count = 0; for (const auto &asset : service.catalog()->assets()) if (asset.type == "well_log") ++count;
    QCOMPARE(count, 1); QCOMPARE(service.catalog()->entities("well").size(), 1);
    QVERIFY(project.mapConfiguration().sourceStats.value("sourceProblems").toMap().isEmpty());
  }
  void unknownPointCrsDoesNotBlockOtherSources() {
    QTemporaryDir dir;
    const auto heads = dir.filePath("points.csv"); QVERIFY(write(heads, "WELL,X,Y\nB1,10,20\n"));
    ProjectSetupSources sources; sources.points = heads; sources.projectCrs = "EPSG:32649";
    const auto preview = scanProjectSources(sources);
    QVERIFY(preview.errors.isEmpty()); QVERIFY(preview.sourceProblems.contains(heads)); QVERIFY(preview.paths.isEmpty());
    QCOMPARE(preview.projectCrs, sources.projectCrs);
    sources.projectCrs = "EPSG:0";
    QVERIFY(!scanProjectSources(sources).errors.isEmpty()); // Invalid project-wide settings still block creation.
  }
  void unresolvedMapDoesNotPreventProjectCreation() {
    QTemporaryDir dir; QString error;
    QgisProjectService project; PaleoProjectStore store; DataImportService service(&store);
    connect(&project, &QgisProjectService::projectOpened, &service, [&service](const QString &p) { service.setProjectDir(QFileInfo(p).absolutePath()); });
    FolderImportWorkflow imports(&service, nullptr); ProjectSetupWorkflow workflow(&project, &imports);
    ProjectSetupPreview preview; ProjectSetupMap map; map.path = dir.filePath("map.shp"); map.included = true;
    preview.maps << map; // Selected, but its horizon has not been resolved.
    QSignalSpy finished(&workflow, &ProjectSetupWorkflow::finished);
    const auto path = dir.filePath("project/project.qgz");
    QVERIFY2(workflow.create(path, {}, preview, &error), qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000); QVERIFY(!finished.first()[1].toBool());
    QVERIFY(QFileInfo::exists(path));
    const auto stats = project.mapConfiguration().sourceStats;
    QVERIFY(stats.value("sourceProblems").toMap().contains(map.path));
    QVERIFY(stats.value("mapChoices").toMap().value(map.path).toMap().value("included").toBool());
    project.closeProject(); QVERIFY(project.openProject(path));
  }
  void geographicBoundaryNeedsExplicitMetricBaseline() {
    QTemporaryDir dir;
    const auto boundary = dir.filePath("boundary.geojson");
    QVERIFY(write(boundary, R"({"type":"FeatureCollection","crs":{"type":"name","properties":{"name":"EPSG:4326"}},"features":[{"type":"Feature","properties":{},"geometry":{"type":"Polygon","coordinates":[[[108,36],[109,36],[108,37],[108,36]]]}}]})"));
    ProjectSetupSources sources; sources.boundary = boundary;
    auto preview = scanProjectSources(sources);
    QVERIFY(preview.errors.isEmpty()); QVERIFY(preview.sourceProblems.contains(boundary));
    QVERIFY(preview.projectCrs.isEmpty()); QVERIFY(preview.paths.isEmpty());
    sources.projectCrs = "EPSG:32649"; preview = scanProjectSources(sources);
    QVERIFY(preview.errors.isEmpty()); QVERIFY(preview.sourceProblems.isEmpty());
    QCOMPARE(preview.projectCrs, sources.projectCrs); QVERIFY(preview.paths.contains(boundary));
  }
  void wizardAllowsPartialSources() {
    QTemporaryDir dir;
    const auto heads = dir.filePath("wellheads.csv"); QVERIFY(write(heads, "WELL,X,Y\nB1,bad,20\n"));
    QgisProjectService project; PaleoProjectStore store; DataImportService service(&store);
    FolderImportWorkflow imports(&service, nullptr); ProjectSetupWorkflow workflow(&project, &imports);
    ProjectSetupDialog dialog(&workflow);
    // #694 起离开第一页就做保存位置预检；默认位置（Documents/Paleo）会被
    // 本机安全软件拦截新编译的测试进程，测试改指私有临时目录保持密闭。
    dialog.findChild<QLineEdit *>("projectDirectoryEdit")->setText(dir.filePath("out"));
    dialog.setSourceRoot(dir.path());
    QTRY_VERIFY_WITH_TIMEOUT(dialog.preview().sourceProblems.contains(heads), 20000);
    auto *next = dialog.findChild<QPushButton *>("projectSetupNextButton"); QVERIFY(next); QVERIFY(next->isEnabled());
    auto *steps = dialog.findChild<QTabBar *>(); QVERIFY(steps); steps->setCurrentIndex(3);
    next->click(); QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QVERIFY(dialog.preview().sourceProblems.contains(heads));
  }
  void wizardDoesNotInheritBoundaryCrs() {
    QTemporaryDir dir;
    const auto heads = dir.filePath("points.csv"), boundary = dir.filePath("boundary.geojson");
    QVERIFY(write(heads, "WELL,X,Y\nB1,10,20\n"));
    QVERIFY(write(boundary, R"({"type":"FeatureCollection","crs":{"type":"name","properties":{"name":"EPSG:4326"}},"features":[{"type":"Feature","properties":{},"geometry":{"type":"Polygon","coordinates":[[[108,36],[109,36],[108,37],[108,36]]]}}]})"));
    ProjectSetupSources sources; sources.points = heads; sources.boundary = boundary; sources.projectCrs = "EPSG:32649";
    QgisProjectService project; PaleoProjectStore store; DataImportService service(&store);
    FolderImportWorkflow imports(&service, nullptr); ProjectSetupWorkflow workflow(&project, &imports);
    ProjectSetupDialog dialog(&workflow); dialog.setExistingProject(dir.filePath("project.qgz"), sources);
    QTRY_VERIFY_WITH_TIMEOUT(dialog.preview().sourceProblems.contains(heads), 20000);
    QVERIFY(!dialog.preview().sourceCrs.isEmpty()); QVERIFY(dialog.preview().pointSourceCrs.isEmpty());
    auto *crs = dialog.findChild<PaleoCrsSelector *>("pointSourceCrsEdit"); QVERIFY(crs); QVERIFY(crs->text().isEmpty());
    QVERIFY(dialog.sources().pointMapping.value("crs").toString().isEmpty());
  }
  void createWorkflowAndReopen() {
    QTemporaryDir dir;
    const auto path = xlsx(dir.filePath("input/points.xlsx"), "UTM49N", {{"ID", "E", "N"}, {"A1", "233240.46", "3997621.12"}});
    QgisProjectService project; PaleoProjectStore store; DataImportService service(&store);
    connect(&project, &QgisProjectService::projectOpened, &service, [&service](const QString &p) { service.setProjectDir(QFileInfo(p).absolutePath()); });
    FolderImportWorkflow imports(&service, nullptr); ProjectSetupWorkflow workflow(&project, &imports);
    ProjectSetupSources s; s.points = path; s.pointMapping = {{"well", "ID"}, {"x", "E"}, {"y", "N"}};
    const auto p = scanProjectSources(s); QVERIFY(p.errors.isEmpty()); QString error;
    QSignalSpy finished(&workflow, &ProjectSetupWorkflow::finished);
    const auto target = dir.filePath("output/project.qgz"); QVERIFY2(workflow.create(target, s, p, &error), qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000); QVERIFY2(finished.first()[1].toBool(), qPrintable(finished.first()[0].toString()));
    QCOMPARE(service.catalog()->entities("well").size(), 1);
    QCOMPARE(service.catalog()->entities("well").first().surfaceX, 233240.46);
    QCOMPARE(workflow.currentSources().pointMapping.value("well").toString(), QString("ID"));
    QCOMPARE(project.mapConfiguration().sourceStats.value("pointMapping").toMap().value("well").toString(), QString("ID"));
    project.closeProject(); QVERIFY(project.openProject(target));
    QVERIFY(project.georeference().has_value()); QCOMPARE(project.georeference()->sourceCrs, QString("EPSG:32649"));
    QCOMPARE(workflow.currentSources().pointMapping.value("well").toString(), QString("ID"));
    QVERIFY(!workflow.create(target, s, p, &error));
  }
  void realSourceImportRepro() {
    const QString src = qEnvironmentVariable("PALEO_REAL_SRC");
    if (src.isEmpty()) QSKIP("PALEO_REAL_SRC 未设置，跳过真实资料导入复现");
    ProjectSetupSources s;
    s.points = src + QStringLiteral("/井坐标.xlsx");
    s.pointMapping = {{"crs", "EPSG:32649"}, {"sheet", "EPSG32649井位"}, {"well", "井号"},
        {"x", "井口横坐标X_EPSG32649(m)"}, {"y", "井口纵坐标Y_EPSG32649(m)"},
        {"kb", "补心海拔(m)"}, {"td", "完钻井深(m)"}};
    s.wells = src + QStringLiteral("/井");
    s.seismic = src + QStringLiteral("/地震");
    s.horizons = src + QStringLiteral("/层位");
    s.references = src + QStringLiteral("/参考资料");
    s.paleomaps = src + QStringLiteral("/古地理图");
    s.projectCrs = QStringLiteral("EPSG:32649");
    const auto p = scanProjectSources(s);
    qDebug() << "scan: wells" << p.wells << "tops" << p.tops << "logs" << p.logs
             << "horizons" << p.horizons << "paths" << p.paths.size() << "errors" << p.errors;
    QVERIFY(p.errors.isEmpty());
    QTemporaryDir dir;
    QgisProjectService project; PaleoProjectStore store; DataImportService service(&store);
    connect(&project, &QgisProjectService::projectOpened, &service, [&service](const QString &pp) { service.setProjectDir(QFileInfo(pp).absolutePath()); });
    FolderImportWorkflow imports(&service, nullptr); ProjectSetupWorkflow workflow(&project, &imports);
    QString error; QSignalSpy finished(&workflow, &ProjectSetupWorkflow::finished);
    const auto target = dir.filePath("out/project.qgz");
    QVERIFY2(workflow.create(target, s, p, &error), qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 300000);
    qDebug() << "finished ok:" << finished.first()[1].toBool() << finished.first()[0].toString();
    QCOMPARE(service.catalog()->entities("well").size(), 20);
  }
  void realWizardPreview() {
    const QString root = qEnvironmentVariable("PALEO_SETUP_SAMPLE_ROOT"); if (root.isEmpty()) QSKIP("Set PALEO_SETUP_SAMPLE_ROOT");
    QgisProjectService project; PaleoProjectStore store; DataImportService service(&store); FolderImportWorkflow imports(&service, nullptr); ProjectSetupWorkflow workflow(&project, &imports);
    ProjectSetupDialog dialog(&workflow); dialog.setSourceRoot(root); dialog.show();
    QTRY_VERIFY_WITH_TIMEOUT(dialog.preview().wells == 20, 90000);
    auto *maps = dialog.findChild<QTableWidget *>("projectPaleomapChoices"); QVERIFY(maps);
    int selected = 0; for (int row = 0; row < maps->rowCount(); ++row) if (maps->item(row, 0)->checkState() == Qt::Checked) ++selected;
    QCOMPARE(selected, 3);
    maps->item(0, 0)->setCheckState(Qt::Unchecked); QVERIFY(!dialog.preview().maps[0].included);
    maps->item(0, 0)->setCheckState(Qt::Checked); QVERIFY(dialog.preview().maps[0].included);
    const QString image = qEnvironmentVariable("PALEO_SETUP_SCREENSHOT");
    if (!image.isEmpty()) {
      dialog.findChild<QTabBar *>()->setCurrentIndex(2); QTest::qWait(100); QVERIFY(dialog.grab().save(image + ".sources.png"));
      dialog.findChild<QTabBar *>()->setCurrentIndex(3); QTest::qWait(100); QVERIFY(dialog.grab().save(image));
    }
  }
  void realSample() {
    const QString root = qEnvironmentVariable("PALEO_SETUP_SAMPLE_ROOT"); if (root.isEmpty()) QSKIP("Set PALEO_SETUP_SAMPLE_ROOT for read-only real data verification");
    const auto sources = discoverProjectSources(root); const auto p = scanProjectSources(sources);
    QVERIFY2(p.errors.isEmpty(), qPrintable(p.errors.join('\n'))); QCOMPARE(p.wells, 20); QCOMPARE(p.tops, 520); QCOMPARE(p.logs, 21); QCOMPARE(p.seismic, 1); QCOMPARE(p.horizons, 8); QCOMPARE(p.sourceCrs, QString("EPSG:32649"));
    for (const auto &path : p.paths) QVERIFY(!path.contains(QStringLiteral("_旧项目工作区")));
    QCOMPARE(QFileInfo(sources.paleomaps).fileName(), QStringLiteral("古地理图"));
    int maps = 0; for (const auto &map : p.maps) if (map.included) { ++maps; QCOMPARE(QFileInfo(map.path).suffix(), QString("gpkg")); QVERIFY(!map.horizon.isEmpty()); QVERIFY(map.problem.isEmpty()); }
    QCOMPARE(maps, 3);
  }
  void mapBundlesValidateAndKeepStyles() {
    QTemporaryDir dir; QString error;
    const auto path = dir.filePath("D61_map.shp");
    QVERIFY(write(path, "shape")); QVERIFY(write(dir.filePath("D61_map.shx"), "index"));
    ProjectSetupSources s; s.paleomaps = dir.path(); auto p = scanProjectSources(s);
    QCOMPARE(p.maps.size(), 1); QVERIFY(!p.maps[0].included); QVERIFY(!p.maps[0].problem.isEmpty());
    QVERIFY(write(dir.filePath("D61_map.dbf"), "attributes")); QVERIFY(write(dir.filePath("D61_map.cpg"), "UTF-8"));
    QVERIFY(write(dir.filePath("D61_map.qml"), "style"));
    const auto members = PaleoMapFiles::members(path, &error); QCOMPARE(members.size(), 5);
    QVariantMap hashes; for (const auto &member : members) hashes.insert(member, DataCatalog::sha256FileHex(member));
    QVERIFY(write(dir.filePath("D61_map.dbf"), "changed"));
    QVERIFY(PaleoMapFiles::copyDataset(path, dir.filePath("copy"), [] { return false; }, &error, hashes).isEmpty());
    QVERIFY(error.contains(QStringLiteral("预览")));
    const auto gpkg = dir.filePath("D62_map.gpkg"); QVERIFY(write(gpkg, "package"));
    QVERIFY(write(gpkg + ".qml", "package-specific style"));
    QVERIFY(PaleoMapFiles::members(gpkg, &error).contains(gpkg + ".qml"));
  }
  void nonGeospatialJsonIsSkippedFromMaps() {
    // tools/simplify_historical_maps.py 会把「图件边界简化记录.json」留在古地理图目录里；
    // 它不是 GeoJSON 数据集，不得被当作待选图件卡住层位校验。
    QTemporaryDir dir;
    QVERIFY(write(dir.filePath("图件边界简化记录.json"), "{\"maps\":[{\"file\":\"D61_map.gpkg\"}]}"));
    const auto geojson = QByteArrayLiteral(
        "{\"type\":\"FeatureCollection\",\"features\":[{\"type\":\"Feature\",\"properties\":{},"
        "\"geometry\":{\"type\":\"Point\",\"coordinates\":[116,40]}}]}");
    QVERIFY(write(dir.filePath("D61_map.geojson"), geojson));
    ProjectSetupSources s; s.paleomaps = dir.path();
    const auto p = scanProjectSources(s);
    QCOMPARE(p.maps.size(), 1);
    QCOMPARE(QFileInfo(p.maps[0].path).fileName(), QStringLiteral("D61_map.geojson"));
    QVERIFY(p.sourceProblems.isEmpty());
    QVERIFY(!p.notes.isEmpty());
  }
  void realMapWizardImportAndReopen() {
    const QString root = qEnvironmentVariable("PALEO_SETUP_SAMPLE_ROOT"); if (root.isEmpty()) QSKIP("Set PALEO_SETUP_SAMPLE_ROOT");
    QTemporaryDir dir; QString error;
    QgisProjectService project; PaleoProjectStore store; DataImportService service(&store);
    connect(&project, &QgisProjectService::projectOpened, &service, [&service](const QString &path) { service.setProjectDir(QFileInfo(path).absolutePath()); });
    LayerManifest manifest(dir.filePath("metadata/project.sqlite"));
    QgisLayerService layers(&project, &manifest); PaleoTaskService tasks;
    FolderImportWorkflow imports(&service, nullptr); ProjectSetupWorkflow workflow(&project, &imports); workflow.setMapImportServices(&layers, &tasks);
    ProjectSetupSources s; s.paleomaps = QDir(root).filePath(QStringLiteral("古地理图"));
    s.projectCrs = qEnvironmentVariable("PALEO_SETUP_PROJECT_CRS");
    const auto p = scanProjectSources(s); QVERIFY(p.errors.isEmpty());
    ProjectSetupSources single; single.paleomaps = QDir(s.paleomaps).filePath("D61_historical_facies_map.shp");
    const auto singlePreview = scanProjectSources(single); QCOMPARE(singlePreview.maps.size(), 1); QVERIFY(singlePreview.maps[0].included);
    QCOMPARE(p.projectCrs, s.projectCrs.isEmpty() ? QString("EPSG:32649") : s.projectCrs);
    QSignalSpy finished(&workflow, &ProjectSetupWorkflow::finished);
    // Create first so the manifest parent directory exists before map publication.
    const auto path = dir.filePath("project.qgz"); QVERIFY2(workflow.create(path, s, p, &error), qPrintable(error));
    QVERIFY2(manifest.open(&error), qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 90000); QVERIFY2(finished[0][1].toBool(), qPrintable(finished[0][0].toString()));
    int maps = 0, features = 0;
    for (const auto &asset : service.catalog()->assets()) if (asset.type == "paleogeographic_map") {
      ++maps; const auto current = service.catalog()->currentVersion(asset.id); QCOMPARE(current.stage, QString("DERIVED"));
      auto *layer = qobject_cast<QgsVectorLayer *>(layers.instantiate(current.extra.value("layer_id").toString(), &error));
      QVERIFY2(layer, qPrintable(error)); features += int(layer->featureCount());
      QCOMPARE(layer->crs().authid(), p.projectCrs);
      if (p.projectCrs == QLatin1String("EPSG:3857")) QVERIFY(layer->extent().xMinimum() > 10000000.);
      for (const auto &level : {QString("facies"), QString("sub_facies"), QString("micro_facies")}) {
        FaciesHierarchyRenderer::apply(layer, current.extra.value("facies").toList(), level);
        QgsMapSettings settings; settings.setDestinationCrs(project.project()->crs());
        settings.setTransformContext(project.project()->transformContext()); settings.setLayers({layer});
        settings.setOutputSize(QSize(700, 800)); settings.setBackgroundColor(Qt::white);
        QgsCoordinateTransform transform(layer->crs(), settings.destinationCrs(), settings.transformContext());
        settings.setExtent(transform.transformBoundingBox(layer->extent()));
        QgsMapRendererSequentialJob job(settings); job.start(); job.waitForFinished();
        int painted = 0; const auto image = job.renderedImage();
        for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x)
          if (image.pixelColor(x, y) != QColor(Qt::white)) ++painted;
        qInfo() << current.id << level << "painted" << painted << "errors" << job.errors().size();
        QVERIFY2(painted > 1000, qPrintable(current.fileName + " " + level));
      }
    }
    QCOMPARE(maps, 3); QCOMPARE(features, 32); QCOMPARE(manifest.all().size(), 6);
    QVERIFY(project.writeProject()); project.closeProject(); QVERIFY(project.openProject(path));
    QCOMPARE(workflow.currentSources().paleomaps, s.paleomaps); QCOMPARE(workflow.currentSources().mapChoices.size(), p.maps.size());
    const auto before = service.catalog()->assets().size(); finished.clear();
    QVERIFY(workflow.applySources(workflow.currentSources(), p, &error));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 90000); QVERIFY2(finished[0][1].toBool(), qPrintable(finished[0][0].toString()));
    QCOMPARE(service.catalog()->assets().size(), before);
    auto shapePreview = p;
    for (auto &map : shapePreview.maps) map.included = map.horizon == "D61" && QFileInfo(map.path).suffix() == "shp";
    finished.clear(); QVERIFY(workflow.applySources(s, shapePreview, &error));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 90000); QVERIFY2(finished[0][1].toBool(), qPrintable(finished[0][0].toString()));
    QCOMPARE(service.catalog()->assets().size(), before + 1);
    bool checkedShape = false;
    for (const auto &asset : service.catalog()->assets()) if (asset.type == "paleogeographic_map" && asset.format == "shp") {
      for (const auto &raw : service.catalog()->versionsForAsset(asset.id)) if (raw.stage == "RAW") {
        const auto members = raw.extra.value("bundle_members").toList(); QCOMPARE(members.size(), 6);
        for (const auto &member : members) {
          const auto row = member.toMap(); const auto managed = QFileInfo(service.catalog()->versionFilePath(raw)).dir().filePath(row.value("file").toString());
          QVERIFY(QFileInfo::exists(managed)); QCOMPARE(DataCatalog::sha256FileHex(managed), row.value("sha256").toString());
        }
        checkedShape = true;
      }
    }
    QVERIFY(checkedShape);
  }
  void realSampleImport() {
    const QString root = qEnvironmentVariable("PALEO_SETUP_SAMPLE_ROOT");
    if (root.isEmpty() || !qEnvironmentVariableIsSet("PALEO_SETUP_IMPORT_REAL")) QSKIP("Opt in with PALEO_SETUP_IMPORT_REAL");
    QTemporaryDir dir; QString error;
    QgisProjectService project; QVERIFY(project.createProject(dir.filePath("sample.qgz")));
    LayerManifest manifest(dir.filePath("metadata/project.sqlite")); QVERIFY(manifest.open(&error));
    QgisLayerService layers(&project, &manifest);
    PaleoProjectStore store; store.setProjectPaths(project.projectPath(), dir.filePath("sample.gpkg"), dir.filePath("metadata/project.sqlite"));
    DataImportService service(&store); service.setProjectDir(dir.path());
    connect(&service, &DataImportService::layerDeclared, &layers, [&layers](const LayerDeclaration &d) { QString e; QVERIFY2(layers.declare(d, &e), qPrintable(e)); });
    const auto sources = discoverProjectSources(root); const auto p = scanProjectSources(sources);
    auto config = project.mapConfiguration(); PaleoGeoreference reference; reference.kind = "crs"; reference.sourceCrs = p.projectCrs; reference.targetCrs = "EPSG:4326"; config.georeference = reference;
    QVERIFY2(project.updateMapConfiguration(config, &error), qPrintable(error)); service.setGeoreference(reference);
    auto session = service.beginImport(); QVERIFY(session);
    session->linkExternal = true; // Current UI default: indexed RAW, managed derived layers.
    session->wellTargets = p.wellTargets;
    session->tableMappings = p.mappings;
    session->expectedSourceHashes = p.hashes;
    DataImportService::produceFiles(*session, p.paths, p.types, {});
    QVERIFY2(service.commitImport(*session, &error) == DataImportService::CommitStatus::Committed, qPrintable(error));
    const auto rows = session->rows;
    QVERIFY2(error.isEmpty(), qPrintable(error));
    int failures = 0; for (const auto &row : rows) if (row.outcome == FolderRowResult::Outcome::Failed) { qWarning() << row.path << row.message; ++failures; }
    QCOMPARE(failures, 0); QCOMPARE(service.catalog()->entities("well").size(), 20); QCOMPARE(service.catalog()->entities("seismic_survey").size(), 1);
    const auto prepared = ProjectLayerRefreshWorkflow::prepare(*service.catalog(), dir.path()); QVERIFY(prepared.wellsPrepared);
    const QString wells = dir.filePath("artifacts/layers/wells.geojson"), survey = dir.filePath("artifacts/layers/survey_area.geojson");
    QVERIFY(write(wells, prepared.wellsBytes)); QVERIFY(service.catalog()->writeSurveyGeoJson(survey, &error));
    for (const auto &pair : {qMakePair(QString("wells"), wells), qMakePair(QString("survey.area"), survey)}) {
      LayerDeclaration d; d.layerId = pair.first; d.type = "vector"; d.source = pair.second; d.group = "00_Data";
      QVERIFY2(layers.declare(d, &error), qPrintable(error));
      auto *layer = qobject_cast<QgsVectorLayer *>(layers.instantiate(d.layerId, &error)); QVERIFY2(layer, qPrintable(error));
      QCOMPARE(layer->featureCount(), pair.first == "wells" ? qint64(20) : qint64(1));
      QVERIFY(layer->extent().xMinimum() > 200000); QVERIFY(layer->extent().yMinimum() > 3900000);
    }
    QVERIFY(project.writeProject());
    DataCatalog reopened; QVERIFY(reopened.open(dir.path(), &error));
    ProjectDataFacade data; data.setCatalog(&reopened, dir.path());
    int topCount = 0, rockWells = 0; WellSectionWorkflow lithology(&reopened);
    for (const auto &well : reopened.entities("well")) {
      const auto tops = data.topsFor(well.id); topCount += tops.size();
      for (const auto &top : tops) QVERIFY2(std::isfinite(top.md), qPrintable(well.name + " " + top.horizon));
      const auto segments = lithology.lithologiesFor({well.id}).value(well.id); if (!segments.isEmpty()) ++rockWells;
      qInfo() << well.name << "tops" << tops.size() << "lithology" << segments.size();
    }
    QCOMPARE(topCount, 520); QCOMPARE(rockWells, 20);
    const auto factorRows = data.importedWellFactors("D61", &error);
    QVERIFY2(error.isEmpty(), qPrintable(error)); QCOMPARE(factorRows.size(), 20);
    int validRatios = 0;
    for (const auto &row : factorRows) if (row.toMap().contains("sand_ratio")) ++validRatios;
    QCOMPARE(validRatios, 10); // Incomplete/conflicting source intervals stay null.
    for (const auto &decl : manifest.all()) if (decl.layerId.startsWith("horizon.")) {
      auto *raster = qobject_cast<QgsRasterLayer *>(layers.instantiate(decl.layerId, &error));
      QVERIFY(raster && raster->isValid()); QCOMPARE(raster->crs().authid(), QString("EPSG:32649"));
    }

    QString a4;
    for (const auto &well : service.catalog()->entities("well")) if (well.name == "A4") a4 = well.id;
    QVERIFY(!a4.isEmpty()); QString log;
    for (const auto &link : service.catalog()->linksForEntity(a4)) if (link.role == "well_log" && link.isPrimary) log = link.assetId;
    QVERIFY(!log.isEmpty());
    DataPreviewTabs preview; preview.setImportService(&service); preview.openAsset(log);
    auto *panel = preview.findChild<WellComposite::WellCompositePanel *>("wellCompositePanel"); QVERIFY(panel);
    QTRY_COMPARE_WITH_TIMEOUT(panel->currentData().lithologyIntervals.size(), 703, 30000);
    QCOMPARE(panel->currentData().formationIntervals.size(), 28);
    for (const auto &base : paleo::io::readWellTableBases(QDir(root).filePath(QStringLiteral("井/分层数据总表.xlsx")))) {
      if (base.wellName != "A4" || !base.hasTvd) continue;
      for (const auto &layer : panel->currentData().formationIntervals)
        if (layer.name == base.topName && base.tvd > layer.topDepth) QVERIFY(std::abs(double(layer.bottomDepth) - base.tvd) < .001);
    }
    auto *canvas = panel->findChild<WellComposite::WellCompositeCanvas *>(); QVERIFY(canvas);
    bool hasRockTrack = false;
    for (const auto &track : canvas->tracks()) if (track->type() == WellComposite::TrackType::Lithology) hasRockTrack = true;
    QVERIFY(hasRockTrack);
    const auto capture = qEnvironmentVariable("PALEO_IMPORT_ROCK_CAPTURE");
    if (!capture.isEmpty()) { preview.resize(1450, 1050); preview.show(); QTest::qWait(100); canvas->setScrollDepth(1690); QTest::qWait(100); QVERIFY(panel->grab().save(capture)); }
    QCOMPARE(manifest.all().size(), 10); // eight horizons + wells + area
  }
};
int main(int argc, char **argv) {
  if (!QgisRuntime::initialize()) return 1;
  PaleoTheme::ensureApplicationFonts(); PaleoTheme::applyLightTheme();
  TestProjectSetup test; const int result = QTest::qExec(&test, argc, argv); QgisRuntime::shutdown(); return result;
}
#include "tst_projectsetup.moc"
