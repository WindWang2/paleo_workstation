// 层：测试壳
// goal/fault-surface — 3D 场景网格、相机包络、剖面交线显示挂钩。
#include <QtTest>
#include <QApplication>
#include <QtMath>

#include "../src/domain/faultset.h"
#include "../src/ui/seismic3d/faultsurfacerenderer.h"
#include "../src/ui/seismic3d/seismic3dviewportwidget.h"
#include "../src/ui/seismicsection/seismicsectioncanvas.h"
#include "../src/ui/seismicsection/seismicsectiondockwidget.h"

using namespace paleo::fault;

namespace {

FaultSurfaceMesh sampleMesh()
{
    FaultSurfaceMesh mesh;
    FaultSurfaceVertex a;
    a.x = 0;
    a.y = 0;
    a.z = 0;
    a.stickId = QStringLiteral("s-a");
    a.pointIndex = 0;
    FaultSurfaceVertex b;
    b.x = 1000;
    b.y = 0;
    b.z = 577;
    b.stickId = QStringLiteral("s-a");
    b.pointIndex = 1;
    FaultSurfaceVertex c;
    c.x = 0;
    c.y = 200;
    c.z = 0;
    c.stickId = QStringLiteral("s-b");
    c.pointIndex = 0;
    mesh.vertices = {a, b, c};
    mesh.triangles = {{0, 1, 2}};
    mesh.stickOrder = {QStringLiteral("s-a"), QStringLiteral("s-b")};
    return mesh;
}

} // namespace

class TestFaultSurfaceView : public QObject
{
    Q_OBJECT
private slots:
    void sceneFrameAndFrustumContainBounds();
    void viewportTriangleCountBeforeGl();
    void canvasAndDockStoreSectionCut();
};

void TestFaultSurfaceView::sceneFrameAndFrustumContainBounds()
{
    const seismic::FaultSceneMesh scene = seismic::makeFaultSceneMesh(sampleMesh());
    QCOMPARE(scene.triangleIndices.size(), 3u);
    QCOMPARE(scene.stickLineIndices.size(), 2u);
    // 地质 Z 向下 → 场景 Y 向上。
    QCOMPARE(scene.positions.at(0), 0.f);
    QCOMPARE(scene.positions.at(1), 0.f);
    QCOMPARE(scene.positions.at(2), 0.f);
    QVERIFY(qAbs(scene.positions.at(3) - 1000.f) < 0.1f);
    QVERIFY(qAbs(scene.positions.at(4) - (-577.f)) < 0.1f);
    QVERIFY(qAbs(scene.positions.at(5) - 0.f) < 0.1f);

    seismic::SeismicCameraController camera;
    const seismic::FaultSceneFit fit = seismic::fitFaultSceneCamera(camera, scene, 1.f);
    QVERIFY(fit.valid);
    QVERIFY(fit.zFar > 1000.f);
    QVERIFY(seismic::faultSceneBoundsInsideFrustum(camera, fit, 1.f));
}

void TestFaultSurfaceView::viewportTriangleCountBeforeGl()
{
    seismic::Seismic3DViewportWidget widget;
    QVERIFY(!widget.isGlReady());
    widget.setFaultSceneMesh(seismic::makeFaultSceneMesh(sampleMesh()));
    QCOMPARE(widget.faultSceneTriangleCount(), 1);
    widget.fitFaultSurfaces(1.f);
    QVERIFY(widget.faultSceneFit().valid);
    QVERIFY(widget.faultSceneContainsBounds(1.f));
    widget.clearFaultSceneMesh();
    QCOMPARE(widget.faultSceneTriangleCount(), 0);
    QVERIFY(!widget.faultSceneFit().valid);
}

void TestFaultSurfaceView::canvasAndDockStoreSectionCut()
{
    seismic::SeismicSectionCanvas canvas;
    seismic::SeismicSectionCanvas::FaultSurfaceCutDisplay cut;
    cut.points = {{0.25, 400.0}, {0.5, 500.0 * std::tan(qDegreesToRadians(30.0))}, {0.75, 900.0}};
    canvas.setFaultSurfaceCut(cut);
    QCOMPARE(canvas.faultSurfaceCut().points.size(), 3);
    QCOMPARE(canvas.faultSurfaceCut().points.at(0).first, 0.25);
    QCOMPARE(canvas.faultSurfaceCut().points.at(0).second, 400.0);
    QVERIFY(canvas.faultSurfaceCut().visible);

    seismic::SeismicSectionDockWidget dock;
    dock.setFaultSurfaceCut(cut);
    QCOMPARE(dock.canvas()->faultSurfaceCut().points.size(), 3);
    QCOMPARE(dock.canvas()->faultSurfaceCut().points.at(1).first, 0.5);
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    TestFaultSurfaceView tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_faultsurfaceview.moc"
