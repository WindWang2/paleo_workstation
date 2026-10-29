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

class TestDataServiceImportWizard : public QObject {
  Q_OBJECT
private slots:
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
