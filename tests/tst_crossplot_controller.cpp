#include "app/appcontext.h"
#include "app/crossplotcontroller.h"
#include "io/dataimportservice.h"
#include "io/perffixtures.h"
#include "linkage/selectioncontext.h"
#include "qgis/qgiscanvascontroller.h"
#include "qgis/qgislayerservice.h"
#include "qgis/qgisprojectservice.h"
#include "ui/crossplot/crossplotpanel.h"
#include "ui/pages/composepage.h"
#include "ui/paleomainwindow.h"
#include "workflow/faciesclassify.h"
#include "workflow/workflows.h"
#include <QAction>
#include <QComboBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTemporaryDir>
#include <QtTest>
#include <qgsapplication.h>
#include <qgsmapcanvas.h>
using namespace paleo::crossplot;
class TestController : public QObject {
  Q_OBJECT
private slots:
  void endToEnd();
};
void TestController::endToEnd() {
  QTemporaryDir dir;
  AppContext context(qEnvironmentVariable("QGIS_PREFIX_PATH", "/usr"));
  QVERIFY(context.ready());
  QVERIFY(context.projectSvc()->createProject(dir.filePath("test.qgz")));
  auto *cat = context.importSvc()->catalog();
  QVERIFY(cat->isOpen());
  for (int i = 0; i < 2; ++i) {
    const auto path = dir.filePath(QString("input%1.tif").arg(i));
    QVERIFY(PerfFixtures::makeSyntheticGeoTiff(path, 32, 32, false));
    CatalogAsset a;
    a.id = cat->nextAssetId();
    a.type = "horizon";
    a.displayName = QString("axis%1").arg(i);
    a.format = "tif";
    QVERIFY(cat->addAsset(a));
    CatalogVersion v;
    v.id = cat->nextVersionId();
    v.assetId = a.id;
    v.stage = "DERIVED";
    v.managed = false;
    v.path = path;
    QVERIFY(cat->addVersion(v));
  }
  PaleoMainWindow window(context.canvasCtl(), context.projectSvc(),
                         context.layerSvc(), context.toolSvc(),
                         context.selection());
  window.attachWorkflows(
      context.predictionWf(), context.constraintWf(), context.compositionWf(),
      context.validationWf(), context.importSvc(), context.seismicLink(),
      context.processingSvc(), context.store(), context.editingSvc(),
      context.layoutSvc(), context.taskSvc());
  new CrossplotController(&context, &window);
  window.show();
  context.selection()->setActiveHorizon("D53");
  auto *action = window.findChild<QAction *>("openCrossplot");
  QVERIFY(action);
  action->trigger();
  auto *panel = window.findChild<CrossplotPanel *>();
  QVERIFY(panel);
  auto *sources = panel->findChild<QListWidget *>("crossplotSources");
  QCOMPARE(sources->count(), 2);
  QStringList sourceIds;
  for (int i = 0; i < sources->count(); ++i) {
    sources->item(i)->setSelected(true);
    sourceIds << sources->item(i)->data(Qt::UserRole).toString();
  }
  panel->findChild<QPushButton *>("crossplotLoad")->click();
  auto *run = panel->findChild<QPushButton *>("crossplotRun");
  QTRY_VERIFY_WITH_TIMEOUT(run->isEnabled(), 10000);
  auto *workflow = window.findChild<FaciesClassifyWorkflow *>();
  QVERIFY(workflow);
  QCOMPARE(workflow->samples()->rows(), std::size_t(1024));
  panel->findChild<QComboBox *>("crossplotX")->setCurrentIndex(1);
  run->click();
  auto *write = panel->findChild<QPushButton *>("crossplotWrite");
  QTRY_VERIFY_WITH_TIMEOUT(write->isEnabled(), 10000);
  write->click();
  QVERIFY(context.layerSvc()->layer("predict.D53.crossplot"));
  auto *combo = window.findChild<QComboBox *>("faciesRasterCombo");
  QVERIFY(combo);
  QVERIFY(combo->findData("predict.D53.crossplot") >= 0);
  panel->pointRequested(
      {.5, .5}); // actual picked sample must be in the raster footprint
  auto center = context.canvasCtl()->canvas()->extent().center();
  QVERIFY(center.x() >= 500000 && center.x() <= 500800);
  QVERIFY(center.y() <= 4000000 && center.y() >= 3999200);
  panel->lassoRequested({{0, 0}, {.7, 0}, {.7, .7}, {0, .7}});
  QVERIFY(!panel->findChildren<QLabel *>().isEmpty());
  // An invalid intent leaves valid samples intact; a failed replacement clears
  // them.
  panel->samplesRequested({"missing0", "missing1"});
  QVERIFY(
      workflow
          ->samples()); // invalid intent rejected before replacing valid data
  QVERIFY(QFile::remove(dir.filePath("input0.tif")));
  panel->samplesRequested(sourceIds);
  QTRY_COMPARE_WITH_TIMEOUT(context.taskSvc()->runningCount(), 0, 10000);
  QVERIFY(!workflow->samples());
  QVERIFY(!run->isEnabled());
  QVERIFY(!write->isEnabled());
}
int main(int argc, char **argv) {
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", "/usr"), true);
  app.initQgis();
  TestController test;
  const int rc = QTest::qExec(&test, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}
#include "tst_crossplot_controller.moc"
