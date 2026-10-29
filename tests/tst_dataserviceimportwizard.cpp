#include <QtTest>
#include <QTemporaryDir>
#include "ui/dialogs/dataserviceimportwizard.h"
#include "workflow/projectsetup.h"
#include "workflow/folderimport.h"
#include "qgis/qgisprojectservice.h"
#include "qgis/crsselector.h"
#include "io/dataimportservice.h"
#include "io/areamodel.h"
#include "metadata/paleoprojectstore.h"
#include <paleo_data/service.h>
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
  void createsModelWithoutInventingCoordinates() {
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
    QVERIFY(coordinates.coordinateSystem.empty());QVERIFY(coordinates.coordinateUnit.empty());
    std::vector<paleo::data::WellInfoRecord> wells;QVERIFY(model.listWellInfos(wells,detail));QCOMPARE(wells.size(),size_t(1));
    QCOMPARE(wells.front().value.wellheadX,0.0);QVERIFY(wells.front().value.coordinateSystem.empty());QVERIFY(wells.front().value.coordinateUnit.empty());
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
};
QTEST_MAIN(TestDataServiceImportWizard)
#include "tst_dataserviceimportwizard.moc"
