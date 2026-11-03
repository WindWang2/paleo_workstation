#include <QtTest>
#include <QTemporaryDir>
#include "ui/dialogs/dataserviceimportwizard.h"
#include "ui/decorations/paleodecorations.h"
#include "workflow/projectsetup.h"
#include "workflow/folderimport.h"
#include "qgis/qgisprojectservice.h"
#include "qgis/qgiscanvascontroller.h"
#include "qgis/dashboardsurveymap.h"
#include "qgis/crsselector.h"
#include "io/dataimportservice.h"
#include "io/areamodel.h"
#include "metadata/paleoprojectstore.h"
#include <paleo_data/service.h>
#include <qgsapplication.h>
#include <qgsmapcanvas.h>
#include <qgsmessagelog.h>
#include <qgsrectangle.h>
#include <QComboBox>
#include <QLineEdit>
#include <QSignalSpy>
#include <QLabel>

class TestDataServiceImportWizard : public QObject {
  Q_OBJECT
private slots:
  void validatesCoordinatesOnTheirOwnPage() {
    QTemporaryDir temp; const QString input=temp.filePath("input"); QVERIFY(QDir().mkpath(input));
    DataServiceImportWizard wizard(nullptr);
    wizard.findChild<QLineEdit *>("dataServiceProjectDirectoryEdit")->setText(temp.filePath("output"));
    wizard.findChild<QLineEdit *>("dataServiceSourceRootEdit")->setText(input);
    auto *crs=wizard.findChild<PaleoCrsSelector *>("dataServiceCrsEdit"); QVERIFY(crs);
    QVERIFY(wizard.sources().projectCrs.isEmpty()); QVERIFY(wizard.sources().pointMapping.value("unit").toString().isEmpty());
    QCOMPARE(wizard.findChild<QComboBox *>("dataServiceCrsUnitCombo")->currentIndex(),-1);
    QVERIFY(crs->setDefinition("EPSG:4326"));
    wizard.restart(); QCOMPARE(wizard.currentId(),0);
    wizard.next(); QCOMPARE(wizard.currentId(),1); // First page must not validate unseen CRS fields.
    wizard.next(); QCOMPARE(wizard.currentId(),1);
    QVERIFY(!wizard.findChild<QLabel *>("dataServiceCoordinateStatus")->text().isEmpty());
    QVERIFY(wizard.findChild<QLabel *>("dataServiceWizardStatus")->text().isEmpty());
    QVERIFY(crs->setDefinition("EPSG:2263")); // Projected US feet are also unsuitable for metre calculations.
    wizard.next(); QCOMPARE(wizard.currentId(),1);
    QVERIFY(crs->setDefinition("EPSG:32649"));
    wizard.next(); QCOMPARE(wizard.currentId(),2);
    wizard.showCreationError(QStringLiteral("test creation error"));
    QCOMPARE(wizard.currentId(),2);
    QCOMPARE(wizard.findChild<QLabel *>("dataServiceCreationStatus")->text(),QString("test creation error"));
  }
  void createsModelWithDeclaredDefaultProjectCrs() {
    QTemporaryDir temp; const QString input=temp.filePath("input"); QVERIFY(QDir().mkpath(input));
    QFile file(input+"/coordinates.csv"); QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("WELL,X,Y\nSYNTH-1,0,200\n");file.close();
    QgisProjectService project;PaleoProjectStore store;DataImportService service(&store);
    connect(&project,&QgisProjectService::projectOpened,&service,[&service](const QString &path){service.setProjectDir(QFileInfo(path).absolutePath());});
    FolderImportWorkflow imports(&service,nullptr);ProjectSetupWorkflow workflow(&project,&imports);
    DataServiceImportWizard wizard(&workflow);
    wizard.findChild<QLineEdit *>("dataServiceProjectNameEdit")->setText("unknown-crs");
    wizard.findChild<QLineEdit *>("dataServiceProjectDirectoryEdit")->setText(temp.filePath("output"));
    wizard.setSourceRoot(input);
    QVERIFY(wizard.sources().projectCrs.isEmpty());
    wizard.accept();QCOMPARE(wizard.result(),int(QDialog::Accepted));
    QSignalSpy finished(&workflow,&ProjectSetupWorkflow::finished);QString error;
    QVERIFY2(workflow.create(wizard.projectPath(),wizard.sources(),wizard.preview(),&error),qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,30000);
    QVERIFY2(finished.first().at(1).toBool(),qPrintable(finished.first().at(0).toString()));
    paleo::data::DataService model;std::string detail;
    QVERIFY(model.openProjectReadOnly(paleo::area_model::directory(QFileInfo(wizard.projectPath()).absolutePath()).toStdString(),detail));
    paleo::data::ProjectCoordinates coordinates;QVERIFY(model.readProjectCoordinates(coordinates,detail));
    QCOMPARE(coordinates.coordinateSystem,std::string("EPSG:32650"));QCOMPARE(coordinates.coordinateUnit,std::string("m"));
    paleo::data::ProjectCrsInfo crs;QVERIFY(model.readProjectCrsInfo(crs,detail));QCOMPARE(crs.selectionBasis,std::string("default"));
    std::vector<paleo::data::WellInfoRecord> wells;QVERIFY(model.listWellInfos(wells,detail));QCOMPARE(wells.size(),size_t(1));
    QCOMPARE(wells.front().value.wellheadX,0.0);QCOMPARE(wells.front().value.wellheadY,200.0);
    QCOMPARE(wells.front().value.coordinateSystem,std::string("EPSG:32650"));QCOMPARE(wells.front().value.coordinateUnit,std::string("m"));
    project.closeProject();
  }
  void wizardCreatesProjectWithEpsgMetadata() {
    QTemporaryDir temp; const QString input = temp.filePath("input"); QVERIFY(QDir().mkpath(input));
    QFile file(input + "/coordinates.csv"); QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("WELL,X,Y\nSYNTH-1,300000,2360000\n"); file.close();
    QgisProjectService project; PaleoProjectStore store; DataImportService service(&store);
    connect(&project, &QgisProjectService::projectOpened, &service,
            [&service](const QString &path) { service.setProjectDir(QFileInfo(path).absolutePath()); });
    FolderImportWorkflow imports(&service, nullptr); ProjectSetupWorkflow workflow(&project, &imports);
    DataServiceImportWizard wizard(&workflow);
    wizard.findChild<QLineEdit *>("dataServiceProjectNameEdit")->setText("epsg-wizard");
    wizard.findChild<QLineEdit *>("dataServiceProjectDirectoryEdit")->setText(temp.filePath("output"));
    wizard.findChild<QLineEdit *>("dataServiceSourceRootEdit")->setText(input);
    auto *crs = wizard.findChild<PaleoCrsSelector *>("dataServiceCrsEdit"); QVERIFY(crs);
    QVERIFY(crs->setDefinition(QStringLiteral("EPSG:32649")));
    wizard.findChild<QComboBox *>("dataServiceCrsUnitCombo")->setCurrentText("m");
    wizard.accept(); QCOMPARE(wizard.result(), int(QDialog::Accepted));

    QSignalSpy finished(&workflow, &ProjectSetupWorkflow::finished); QString error;
    QVERIFY2(workflow.create(wizard.projectPath(), wizard.sources(), wizard.preview(), &error), qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 30000);
    QVERIFY2(finished.first().at(1).toBool(), qPrintable(finished.first().at(0).toString()));
    paleo::data::DataService model; std::string detail;
    const QString modelRoot = paleo::area_model::directory(QFileInfo(wizard.projectPath()).absolutePath());
    QVERIFY(model.openProjectReadOnly(modelRoot.toUtf8().toStdString(), detail));
    QCOMPARE(model.projectCrs(), std::string("EPSG:32649")); QCOMPARE(model.projectCrsUnit(), std::string("m"));
    QCOMPARE(service.catalog()->entities("well").size(), 1);
    QCOMPARE(service.catalog()->entities("well").first().surfaceX, 300000.0);
    QCOMPARE(service.catalog()->assets().size(), 1);
    project.closeProject();
  }
  void threadsComboOffersParallelSelectionAndWritesEnv() {
    const QByteArray prev = qgetenv("PALEO_IMPORT_PARSER_THREADS");
    const bool hadPrev = qEnvironmentVariableIsSet("PALEO_IMPORT_PARSER_THREADS");
    QTemporaryDir temp; const QString input = temp.filePath("input"); QVERIFY(QDir().mkpath(input));
    DataServiceImportWizard wizard(nullptr);
    wizard.findChild<QLineEdit *>("dataServiceProjectNameEdit")->setText("parallel-wizard");
    wizard.findChild<QLineEdit *>("dataServiceProjectDirectoryEdit")->setText(temp.filePath("output"));
    wizard.setSourceRoot(input);
    auto *threads = wizard.findChild<QComboBox *>("dataServiceThreadsCombo");
    QVERIFY(threads);
    QVERIFY(threads->isEnabled());
    QCOMPARE(threads->count(), 3);
    QCOMPARE(threads->itemData(0).toInt(), 1);
    QCOMPARE(threads->itemData(1).toInt(), 2);
    QCOMPARE(threads->itemData(2).toInt(), 4);
    QCOMPARE(threads->currentData().toInt(), 4);
    threads->setCurrentIndex(1);
    QVERIFY(wizard.findChild<QLabel *>("dataServiceReview")->text().contains(QStringLiteral("解析线程：2")));
    wizard.accept();
    QCOMPARE(wizard.result(), int(QDialog::Accepted));
    QCOMPARE(qEnvironmentVariableIntValue("PALEO_IMPORT_PARSER_THREADS"), 2);
    if (hadPrev) qputenv("PALEO_IMPORT_PARSER_THREADS", prev);
    else qunsetenv("PALEO_IMPORT_PARSER_THREADS");
  }
  void newWorkspaceAfterWebMercatorProjectEmitsNoTransformErrors() {
    QStringList projErrors;
    const auto conn = QObject::connect(
        QgsApplication::messageLog(),
        &QgsMessageLog::messageReceivedWithFormat,
        [&projErrors](const QString &message, const QString &, Qgis::MessageLevel, Qgis::StringFormat) {
          if (message.contains(QLatin1String("Forward transform"))
              || message.contains(QLatin1String("Point outside of projection domain")))
            projErrors << message;
        });

    QTemporaryDir temp;
    QObject context;
    QgisProjectService project(&context);
    QgisCanvasController controller(&context);
    QgsMapCanvas *mainCanvas = controller.canvas();
    mainCanvas->resize(800, 600);

    // Simulate a previously opened EPSG:3857 workspace zoomed to South China Sea (~12.87e6, 2.45e6).
    QVERIFY(project.createProject(temp.filePath("prev/prev.qgz")));
    auto prevConfig = project.mapConfiguration();
    PaleoGeoreference prevRef;
    prevRef.kind = QStringLiteral("crs");
    prevRef.sourceCrs = QStringLiteral("EPSG:32650");
    prevRef.targetCrs = QStringLiteral("EPSG:4326");
    prevConfig.georeference = prevRef;
    prevConfig.mapCrs = QStringLiteral("EPSG:3857");
    QString error;
    QVERIFY2(project.updateMapConfiguration(prevConfig, &error), qPrintable(error));
    mainCanvas->setExtent(QgsRectangle(12868023.0, 2456186.0, 12968023.0, 2536186.0));

    // Also exercise DashboardSurveyMap extreme zoom-out clamping + scale bar.
    QWidget host;
    DashboardSurveyMap::Style style;
    style.primary = QColor(QStringLiteral("#1E5EFF"));
    style.wellsLayerName = QStringLiteral("井位");
    QWidget *dashMap = DashboardSurveyMap::create(&host, style);
    auto *dashCanvas = qobject_cast<QgsMapCanvas *>(DashboardSurveyMap::canvasWidget(dashMap));
    QVERIFY(dashCanvas);
    dashCanvas->resize(640, 200);
    PaleoDecorationManager dashDecor(dashCanvas);
    dashDecor.setScaleBarEnabled(true);
    DashboardSurveyMap::update(dashMap, {}, nullptr, nullptr);
    dashCanvas->setExtent(QgsRectangle(-20332272.727273, -6067015.584416, 20332272.727273, 6067015.584416));
    QVERIFY2(dashCanvas->extent().width() < 200000.0, "dashboard survey map must clamp extreme zoom-out");

    // Now create a brand-new Data_service workspace and import UTM Zone 50N wells.
    const QString input = temp.filePath("hy_synth");
    QVERIFY(QDir().mkpath(input));
    QFile file(input + "/coordinates.csv");
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("WELL,X,Y\nHZ19-1-1,300000,2360000\nHZ25-4-1,310000,2350000\n");
    file.close();

    PaleoProjectStore store;
    DataImportService service(&store);
    connect(&project, &QgisProjectService::projectOpened, &service,
            [&service](const QString &path) { service.setProjectDir(QFileInfo(path).absolutePath()); });
    FolderImportWorkflow imports(&service, nullptr);
    ProjectSetupWorkflow workflow(&project, &imports);
    DataServiceImportWizard wizard(&workflow);
    wizard.findChild<QLineEdit *>("dataServiceProjectNameEdit")->setText("fresh-hy");
    wizard.findChild<QLineEdit *>("dataServiceProjectDirectoryEdit")->setText(temp.filePath("out"));
    wizard.setSourceRoot(input);
    wizard.accept();
    QSignalSpy finished(&workflow, &ProjectSetupWorkflow::finished);
    QVERIFY2(workflow.create(wizard.projectPath(), wizard.sources(), wizard.preview(), &error), qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 30000);
    QVERIFY2(finished.first().at(1).toBool(), qPrintable(finished.first().at(0).toString()));
    DashboardSurveyMap::update(dashMap, service.catalog()->entities("well"), nullptr, nullptr);
    QCoreApplication::processEvents();

    QObject::disconnect(conn);
    project.closeProject();
    QVERIFY2(projErrors.isEmpty(), qPrintable(projErrors.join(QLatin1Char('\n'))));
  }
  void realDatasetParallelFourThreadImport() {
    const QString root = qEnvironmentVariable("PALEO_HY_TEST_ROOT");
    if (root.isEmpty()) QSKIP("Set PALEO_HY_TEST_ROOT to run real parallel dataset import");
    QVERIFY2(QFileInfo(root).isDir(), qPrintable(root));
    QStringList projErrors;
    const auto conn = QObject::connect(
        QgsApplication::messageLog(),
        &QgsMessageLog::messageReceivedWithFormat,
        [&projErrors](const QString &message, const QString &, Qgis::MessageLevel, Qgis::StringFormat) {
          if (message.contains(QLatin1String("Forward transform"))
              || message.contains(QLatin1String("Point outside of projection domain")))
            projErrors << message;
        });
    const QByteArray prev = qgetenv("PALEO_IMPORT_PARSER_THREADS");
    const bool hadPrev = qEnvironmentVariableIsSet("PALEO_IMPORT_PARSER_THREADS");
    qputenv("PALEO_IMPORT_PARSER_THREADS", "4");

    QTemporaryDir temp;
    paleo::data::ImportReport report;
    QString error;
    const QString projDir = temp.filePath("hy_parallel");
    QVERIFY(QDir().mkpath(projDir));
    const bool ok = paleo::area_model::importDirectory(projDir, root, report, &error);
    if (hadPrev) qputenv("PALEO_IMPORT_PARSER_THREADS", prev);
    else qunsetenv("PALEO_IMPORT_PARSER_THREADS");
    QObject::disconnect(conn);

    QVERIFY2(ok || report.imported > 0 || report.review > 0, qPrintable(error));
    QCOMPARE(report.workerCount, 4);
    QVERIFY2(report.imported + report.review > 1000, "expected full HY dataset parse coverage");
    paleo::data::DataService model;
    std::string detail;
    QVERIFY2(model.openProjectReadOnly(paleo::area_model::directory(projDir).toUtf8().toStdString(), detail), detail.c_str());
    std::vector<paleo::data::WellInfoRecord> wells;
    QVERIFY(model.listWellInfos(wells, detail));
    QVERIFY2(wells.size() >= 50, "expected >= 50 wells in HY dataset");
    std::vector<paleo::data::StratigraphicSchemeSummary> schemes;
    QVERIFY(model.listStratigraphicSchemes(schemes, detail));
    QVERIFY2(!schemes.empty(), "expected auto-built seismic stratigraphic scheme");
    QVERIFY2(projErrors.isEmpty(), qPrintable(projErrors.join(QLatin1Char('\n'))));
  }
};
QTEST_MAIN(TestDataServiceImportWizard)
#include "tst_dataserviceimportwizard.moc"
