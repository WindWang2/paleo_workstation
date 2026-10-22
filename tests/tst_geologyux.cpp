// 层：测试壳
#include <QtTest>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QTableWidget>
#include <QToolButton>
#include "helpers/workflowfixture.h"
#include "workflow/workflows.h"
#include "qgis/qgisruntime.h"
#include "ui/paleotheme.h"
#include "ui/pages/constraintpage.h"
#include "ui/wellcomposite/wellcompositepanel.h"

class TestGeologyUx : public QObject {
  Q_OBJECT
private slots:
  void cellSizeFollowsSamplesUntilUserOverridesIt() {
    paleo::tests::WorkflowFixture fixture; QVERIFY(paleo::tests::initFixture(fixture));
    const auto path = fixture.dir.filePath("wells.geojson"); QString error;
    QVERIFY(paleo::tests::writePointsGeoJson(path));
    LayerDeclaration wells; wells.layerId = "wells.T1"; wells.horizon = "T1";
    wells.type = "vector"; wells.source = path;
    QVERIFY2(fixture.layers.declare(wells, &error), qPrintable(error));
    ConstraintWorkflow workflow(&fixture.proc, &fixture.layers);
    workflow.setCatalog(&fixture.catalog, fixture.dir.path());
    ConstraintPage page(&workflow); page.setHorizons({"T1"});
    auto *cell = page.findChild<QDoubleSpinBox *>("factorCellSizeSpin"); QVERIFY(cell);
    const QVariantMap parameters{{"factorMode", "direct"}, {"valueField", "z"}};
    QVERIFY2(workflow.extractWellFactors("T1", "sandthick", parameters, &error), qPrintable(error));
    QCOMPARE(cell->value(), .01); // 4 metre fixture must not become a single 50 metre cell.
    cell->setValue(2.);
    QVERIFY2(workflow.extractWellFactors("T1", "sandthick", parameters, &error), qPrintable(error));
    QCOMPARE(cell->value(), 2.);
  }
  void previewFrequentActionsFitWithoutHorizontalScrolling() {
    WellComposite::WellCompositePanel panel;
    panel.resize(900, 600); panel.show(); QTest::qWait(10);
    auto *bar = panel.findChild<QWidget *>("wellCompositeTopBar"); QVERIFY(bar);
    auto *options = panel.findChild<QToolButton *>("wellPreviewOptions"); QVERIFY(options && options->menu());
    QVERIFY(bar->minimumSizeHint().width() <= 900);
    // Hiding a control must not break the shell's findChild bindings.
    auto *attributes = panel.findChild<QToolButton *>("btnWellAttributes"); QVERIFY(attributes);
    auto *models = panel.findChild<QComboBox *>("faciesPredictionModel"); QVERIFY(models);
    models->addItem("test model", "test");
    auto *fit = panel.findChild<QToolButton *>("btnCompResetZoom"); QVERIFY(fit && fit->isVisibleTo(&panel));
    const auto capture = qEnvironmentVariable("PALEO_GEOLOGY_QA_DIR");
    if (!capture.isEmpty()) QVERIFY(panel.grab().save(capture + "/compact-well.png"));
  }
  void factorPageUsesOnePrimaryActionAndOnlySupportedFactors() {
    ConstraintPage page(nullptr); page.resize(420, 850); page.show(); QTest::qWait(10);
    auto *table = page.findChild<QTableWidget *>("factorTable"); QVERIFY(table);
    for (int row = 0; row < table->rowCount(); ++row) {
      const auto id = table->item(row, 0)->data(Qt::UserRole).toString();
      QVERIFY(id != "confidence");
      if (id == "poro" || id == "perm") QVERIFY(table->isRowHidden(row));
    }
    QCOMPARE(table->item(0, 0)->checkState(), Qt::Checked);
    auto *run = page.findChild<QPushButton *>("factorOneClickButton"); QVERIFY(run && run->isVisibleTo(&page));
    auto *manual = page.findChild<QPushButton *>("generateFactorButton"); QVERIFY(manual && !manual->isVisibleTo(&page));
    QCOMPARE(page.findChild<QDoubleSpinBox *>("factorCellSizeSpin")->value(), 50.);
    QCOMPARE(page.findChild<QComboBox *>("factorMethodCombo")->currentData().toString(), QString("surfer_idw"));
    const auto capture = qEnvironmentVariable("PALEO_GEOLOGY_QA_DIR");
    if (!capture.isEmpty()) QVERIFY(page.grab().save(capture + "/simple-factor.png"));
    page.setWellFactorFields({QVariantMap{{"id", "poro"}, {"label", "porosity"}}});
    for (int row = 0; row < table->rowCount(); ++row)
      if (table->item(row, 0)->data(Qt::UserRole).toString() == "poro") QVERIFY(!table->isRowHidden(row));
  }
};
int main(int argc, char **argv) {
  if (!QgisRuntime::initialize()) return 1;
  PaleoTheme::ensureApplicationFonts(); PaleoTheme::applyLightTheme();
  TestGeologyUx test; const auto result = QTest::qExec(&test, argc, argv);
  QgisRuntime::shutdown(); return result;
}
#include "tst_geologyux.moc"
