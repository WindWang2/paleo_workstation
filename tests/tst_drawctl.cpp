#include <QtTest>
#include <QDir>
#include <QTemporaryDir>
#include <QSignalSpy>

#include <qgsmapcanvas.h>

#include "../src/metadata/layermanifest.h"
#include "../src/qgis/qgiscanvascontroller.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/ui/constraintdrawcontroller.h"
#include "../src/ui/maptools/paleomaptools.h"
#include "../src/workflow/workflows.h"

// §42 acceptance — the constraint draw chain: page intent → capture tool on the
// canvas → workflow commit → teardown. Driven deterministically via
// ConstraintDrawController::onDrawn/onAborted (the map tools' own event
// semantics are covered by tst_maptools).
class TestDrawController : public QObject
{
  Q_OBJECT

  struct Stack
  {
    QgisProjectService projectSvc;
    std::unique_ptr<LayerManifest> manifest;
    std::unique_ptr<QgisLayerService> layerSvc;
    std::unique_ptr<ConstraintWorkflow> wf;
    QgisCanvasController canvasCtl;
    std::unique_ptr<ConstraintDrawController> ctl;
  };

  static std::unique_ptr<Stack> makeStack(const QString &dir)
  {
    QDir().mkpath(dir); // createProject writes proj.qgz in place at t=0
    auto s = std::make_unique<Stack>();
    if (!s->projectSvc.createProject(QDir(dir).filePath(QStringLiteral("proj.qgz"))))
      return nullptr;
    s->manifest = std::make_unique<LayerManifest>(QDir(dir).filePath(QStringLiteral("m.sqlite")));
    if (!s->manifest->open())
      return nullptr;
    s->layerSvc = std::make_unique<QgisLayerService>(&s->projectSvc, s->manifest.get());
    // proc is nullptr: addConstraint only needs the layer service.
    s->wf = std::make_unique<ConstraintWorkflow>(nullptr, s->layerSvc.get());
    s->ctl = std::make_unique<ConstraintDrawController>(&s->canvasCtl, s->wf.get());
    return s;
  }

private slots:
  void initTestCase() { QVERIFY(QgisRuntime::isInitialized()); }

  // line capture installs the right tool; onDrawn commits via the workflow
  // and tears the session down.
  void lineCaptureCommitsAndTearsDown()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto s = makeStack(dir.filePath(QStringLiteral("p")));
    QVERIFY(s != nullptr);
    auto &ctl = *s->ctl;

    QSignalSpy addedSpy(s->wf.get(), &ConstraintWorkflow::constraintAdded);
    QSignalSpy finSpy(&ctl, &ConstraintDrawController::captureFinished);
    QSignalSpy cancelSpy(&ctl, &ConstraintDrawController::captureCancelled);
    QSignalSpy failSpy(&ctl, &ConstraintDrawController::captureFailed);

    ctl.startCapture(QStringLiteral("T1"), QStringLiteral("line"), 7);
    QVERIFY(ctl.active());
    QVERIFY(qobject_cast<PaleoDrawConstraintTool *>(ctl.currentTool()));
    QCOMPARE(ctl.activeHorizon(), QStringLiteral("T1"));
    QCOMPARE(s->canvasCtl.activeTool(), ctl.currentTool());

    ctl.onDrawn(QStringLiteral("LineString (0 0, 1 1)"));

    QCOMPARE(addedSpy.count(), 1);
    QCOMPARE(finSpy.count(), 1);
    QCOMPARE(finSpy.at(0).at(0).toString(), QStringLiteral("T1"));
    QCOMPARE(finSpy.at(0).at(1).toString(), QStringLiteral("c-1"));
    QCOMPARE(cancelSpy.count(), 0);
    QCOMPARE(failSpy.count(), 0);
    QVERIFY(!ctl.active());
    QVERIFY(!s->canvasCtl.activeTool());

    // The constraint landed in the manifest declaration for horizon T1.
    const QVector<LayerDeclaration> decls = s->layerSvc->declared();
    QCOMPARE(decls.size(), 1);
    QCOMPARE(decls.at(0).layerId, QStringLiteral("constraints.T1"));
    QVERIFY(decls.at(0).source.contains(QStringLiteral("LineString")));
  }

  // shape picker maps to the right tool classes; abort tears down cleanly.
  void shapeSelectionAndAbort()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto s = makeStack(dir.filePath(QStringLiteral("p")));
    QVERIFY(s != nullptr);
    auto &ctl = *s->ctl;

    ctl.startCapture(QStringLiteral("T1"), QStringLiteral("polygon"), 1);
    QVERIFY(qobject_cast<PaleoDrawPolygonTool *>(ctl.currentTool()));

    // restarting replaces the live tool rather than stacking a second one
    QgsMapTool *first = ctl.currentTool();
    ctl.startCapture(QStringLiteral("T1"), QStringLiteral("rect"), 1);
    QVERIFY(qobject_cast<PaleoDrawRectTool *>(ctl.currentTool()));
    QVERIFY(ctl.currentTool() != first);

    QSignalSpy cancelSpy(&ctl, &ConstraintDrawController::captureCancelled);
    ctl.onAborted();
    QCOMPARE(cancelSpy.count(), 1);
    QVERIFY(!ctl.active());

    // owner-initiated cancel with no live capture still reports cancelled
    ctl.cancel();
    QCOMPARE(cancelSpy.count(), 2);
  }

  // failure modes: empty horizon, unknown shape — no tool ever installed.
  void captureFailures()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto s = makeStack(dir.filePath(QStringLiteral("p")));
    QVERIFY(s != nullptr);
    auto &ctl = *s->ctl;
    QSignalSpy failSpy(&ctl, &ConstraintDrawController::captureFailed);

    ctl.startCapture(QString(), QStringLiteral("line"), 1);
    QCOMPARE(failSpy.count(), 1);
    QVERIFY(!ctl.active());

    ctl.startCapture(QStringLiteral("T1"), QStringLiteral("helix"), 1);
    QCOMPARE(failSpy.count(), 2);
    QVERIFY(!ctl.active());
    QVERIFY(!s->canvasCtl.activeTool());
  }
};

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestDrawController tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_drawctl.moc"
