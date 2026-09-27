#include <QtTest>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QSignalSpy>
#include <QSlider>
#include <QSpinBox>
#include <QLabel>
#include <QToolButton>

#include <glm/gtc/matrix_inverse.hpp>

#include "../src/domain/seismic/sgyvolume.h"
#include "../src/domain/seismic/sgyindexcache.h"
#include "../src/services/seismictaskservice.h"
#include "../src/services/paleotaskservice.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/ui/seismic3d/seismiccameracontroller.h"
#include "../src/ui/seismic3d/seismicslicerenderer.h"
#include "../src/ui/seismic3d/volumeframerenderer.h"
#include "../src/ui/seismic3d/seismic3dviewportwidget.h"
#include "../src/ui/seismic3d/seismic3dviewpanel.h"

using namespace seismic;

class TestSeismic3D : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cameraControllerBasics();
    void cameraControllerPresetsAndFit();
    void sliceRendererGeometryAndSlots();
    void viewPanelUiAndInteractions();
    void viewPanelAsyncSliceLoading();
    void openGLHeadlessRender();

private:
    std::shared_ptr<SgyVolume> loadFixtureVolume();
};

void TestSeismic3D::initTestCase() {
    // Ensure Qt resource system initialized
    Q_INIT_RESOURCE(seismic_shaders);
}

std::shared_ptr<SgyVolume> TestSeismic3D::loadFixtureVolume() {
    const QString sgyPath = QStringLiteral(SEGY_FIXTURE_PATH);
    if (!QFile::exists(sgyPath)) {
        return nullptr;
    }
    auto vol = std::make_shared<SgyVolume>();
    std::string err;
    if (!vol->Load(sgyPath.toStdString(), err)) {
        return nullptr;
    }
    return vol;
}

void TestSeismic3D::cameraControllerBasics() {
    SeismicCameraController cam;
    QCOMPARE(cam.Yaw(), -45.0f);
    QCOMPARE(cam.Pitch(), 35.0f);
    QCOMPARE(cam.Distance(), 9.0f);
    QCOMPARE(cam.Target().x, 0.0f);
    QCOMPARE(cam.Target().y, 0.0f);
    QCOMPARE(cam.Target().z, 0.0f);

    const glm::mat4 view = cam.BuildViewMatrix();
    const float det = glm::determinant(view);
    QVERIFY(std::abs(det) > 1e-4f);

    const glm::mat4 proj = cam.BuildProjectionMatrix(1.5f, 45.0f, 0.1f, 1000.0f);
    QVERIFY(std::abs(glm::determinant(proj)) > 1e-4f);

    // Rotate
    cam.Rotate(20.0f, -10.0f);
    QCOMPARE(cam.Yaw(), -45.0f + 20.0f * 0.25f);
    QCOMPARE(cam.Pitch(), 35.0f - 10.0f * 0.25f);

    // Pan
    const glm::vec3 prevTarget = cam.Target();
    cam.Pan(10.0f, 5.0f);
    QVERIFY(cam.Target() != prevTarget);

    // Zoom
    const float distBefore = cam.Distance();
    cam.Zoom(1.0f); // Zoom in
    QVERIFY(cam.Distance() < distBefore);
    cam.Zoom(-1.0f); // Zoom out
    QVERIFY(std::abs(cam.Distance() - distBefore) < 0.01f);
}

void TestSeismic3D::cameraControllerPresetsAndFit() {
    SeismicCameraController cam;

    cam.SetTopView();
    QCOMPARE(cam.Yaw(), 0.0f);
    QCOMPARE(cam.Pitch(), 89.9f);

    cam.SetFrontView();
    QCOMPARE(cam.Yaw(), 0.0f);
    QCOMPARE(cam.Pitch(), 0.0f);

    cam.SetSideView();
    QCOMPARE(cam.Yaw(), 90.0f);
    QCOMPARE(cam.Pitch(), 0.0f);

    cam.SetIsometricView();
    QCOMPARE(cam.Yaw(), -45.0f);
    QCOMPARE(cam.Pitch(), 35.0f);

    cam.ApplyPreset(SeismicCameraController::PresetView::Top);
    QCOMPARE(cam.Yaw(), 0.0f);
    QCOMPARE(cam.Pitch(), 89.9f);

    // Fit to bounds
    const glm::vec3 minV(-3.0f, -2.2f, -3.0f);
    const glm::vec3 maxV(3.0f, 2.2f, 3.0f);
    cam.FitToBounds(minV, maxV, 1.33f);
    QCOMPARE(cam.Target().x, 0.0f);
    QCOMPARE(cam.Target().y, 0.0f);
    QCOMPARE(cam.Target().z, 0.0f);
    QVERIFY(cam.Distance() > 5.0f);
}

void TestSeismic3D::sliceRendererGeometryAndSlots() {
    SeismicSliceRenderer renderer;
    QCOMPARE(renderer.IsInitialized(), false);
    QCOMPARE(renderer.IsVisible(), true);

    QCOMPARE(SeismicSliceRenderer::HorizontalScale(), 6.0f);
    QCOMPARE(SeismicSliceRenderer::HeightScale(), 4.4f);

    // Slot visibility
    QCOMPARE(renderer.IsSlotVisible(SeismicSliceSlot::Inline), true);
    renderer.SetSlotVisible(SeismicSliceSlot::Inline, false);
    QCOMPARE(renderer.IsSlotVisible(SeismicSliceSlot::Inline), false);
    renderer.SetSlotVisible(SeismicSliceSlot::Inline, true);
    QCOMPARE(renderer.IsSlotVisible(SeismicSliceSlot::Inline), true);
}

void TestSeismic3D::viewPanelUiAndInteractions() {
    auto vol = loadFixtureVolume();
    QVERIFY(vol != nullptr);
    QVERIFY(vol->IsLoaded());

    Seismic3DViewPanel panel;
    panel.resize(800, 600);

    auto *vp = panel.viewport();
    QVERIFY(vp != nullptr);

    auto *inlineSlider = panel.findChild<QSlider *>(QStringLiteral("inlineSlider"));
    auto *inlineSpin = panel.findChild<QSpinBox *>(QStringLiteral("inlineSpin"));
    auto *xlineSlider = panel.findChild<QSlider *>(QStringLiteral("xlineSlider"));
    auto *xlineSpin = panel.findChild<QSpinBox *>(QStringLiteral("xlineSpin"));
    auto *timeSlider = panel.findChild<QSlider *>(QStringLiteral("timeSlider"));
    auto *timeSpin = panel.findChild<QSpinBox *>(QStringLiteral("timeSpin"));
    auto *timeMsLabel = panel.findChild<QLabel *>(QStringLiteral("timeMsLabel"));

    QVERIFY(inlineSlider && inlineSpin);
    QVERIFY(xlineSlider && xlineSpin);
    QVERIFY(timeSlider && timeSpin && timeMsLabel);

    panel.setVolume(vol);
    QCOMPARE(panel.volume(), vol);

    // Ranges match volume
    QCOMPARE(inlineSlider->minimum(), vol->InlineMin());
    QCOMPARE(inlineSlider->maximum(), vol->InlineMax());
    QCOMPARE(inlineSpin->minimum(), vol->InlineMin());
    QCOMPARE(inlineSpin->maximum(), vol->InlineMax());

    QCOMPARE(xlineSlider->minimum(), vol->XlineMin());
    QCOMPARE(xlineSlider->maximum(), vol->XlineMax());
    QCOMPARE(xlineSpin->minimum(), vol->XlineMin());
    QCOMPARE(xlineSpin->maximum(), vol->XlineMax());

    QCOMPARE(timeSlider->minimum(), 0);
    QCOMPARE(timeSlider->maximum(), vol->SampleMax());
    QCOMPARE(timeSpin->minimum(), 0);
    QCOMPARE(timeSpin->maximum(), vol->SampleMax());

    // Midpoints
    const int midInl = (vol->InlineMin() + vol->InlineMax()) / 2;
    QCOMPARE(panel.currentInline(), midInl);
    QCOMPARE(inlineSpin->value(), midInl);

    // Test signal emissions on user adjustment
    QSignalSpy inlineSpy(&panel, &Seismic3DViewPanel::inlineChanged);
    panel.setInline(vol->InlineMin());
    QCOMPARE(inlineSpy.count(), 1);
    QCOMPARE(panel.currentInline(), vol->InlineMin());
    QCOMPARE(inlineSpin->value(), vol->InlineMin());

    QSignalSpy xlineSpy(&panel, &Seismic3DViewPanel::crosslineChanged);
    panel.setCrossline(vol->XlineMax());
    QCOMPARE(xlineSpy.count(), 1);
    QCOMPARE(panel.currentCrossline(), vol->XlineMax());
    QCOMPARE(xlineSpin->value(), vol->XlineMax());

    QSignalSpy timeSpy(&panel, &Seismic3DViewPanel::timeChanged);
    panel.setTimeSample(10);
    QCOMPARE(timeSpy.count(), 1);
    QCOMPARE(panel.currentTimeSample(), 10);
    QCOMPARE(timeSpin->value(), 10);
    QVERIFY(timeMsLabel->text().contains(QStringLiteral("ms")));

    // Preset view triggering
    vp->setPresetView(SeismicCameraController::PresetView::Top);
    QCOMPARE(vp->camera().Yaw(), 0.0f);
    QCOMPARE(vp->camera().Pitch(), 89.9f);

    vp->setPresetView(SeismicCameraController::PresetView::Isometric);
    QCOMPARE(vp->camera().Yaw(), -45.0f);
    QCOMPARE(vp->camera().Pitch(), 35.0f);

    // Visibility toggles
    vp->setFrameVisible(false);
    QCOMPARE(vp->isFrameVisible(), false);
    vp->setFrameVisible(true);
    QCOMPARE(vp->isFrameVisible(), true);

    vp->setSlotVisible(SeismicSliceSlot::Time, false);
    QCOMPARE(vp->isSlotVisible(SeismicSliceSlot::Time), false);
    vp->setSlotVisible(SeismicSliceSlot::Time, true);
    QCOMPARE(vp->isSlotVisible(SeismicSliceSlot::Time), true);
}

void TestSeismic3D::viewPanelAsyncSliceLoading() {
    auto vol = loadFixtureVolume();
    QVERIFY(vol != nullptr);

    PaleoProjectStore store;
    PaleoTaskService taskSvc(&store);
    SeismicTaskService seismicSvc(&taskSvc, 16 * 1024 * 1024);

    Seismic3DViewPanel panel;
    panel.setTaskService(&seismicSvc);
    panel.setVolume(vol);

    // Initial load triggered async slice extraction
    QTRY_VERIFY_WITH_TIMEOUT(!taskSvc.tasks().isEmpty(), 2000);

    // Adjusting inline triggers async extraction
    panel.setInline(vol->InlineMin());
    QTRY_VERIFY_WITH_TIMEOUT(taskSvc.tasks().size() >= 2, 2000);
}

void TestSeismic3D::openGLHeadlessRender() {
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setRenderableType(QSurfaceFormat::OpenGL);

    QOffscreenSurface surface;
    surface.setFormat(format);
    surface.create();
    QVERIFY(surface.isValid());

    QOpenGLContext context;
    context.setFormat(format);
    if (!context.create()) {
        QSKIP("OpenGL context could not be created in this environment");
    }
    if (!context.makeCurrent(&surface)) {
        QSKIP("OpenGL context makeCurrent failed");
    }

    QOpenGLFunctions_3_3_Core gl;
    if (!gl.initializeOpenGLFunctions()) {
        QSKIP("OpenGL 3.3 Core functions not available");
    }

    // 1. Test VolumeFrameRenderer
    VolumeFrameRenderer frameRenderer;
    QVERIFY(frameRenderer.Initialize(&gl));
    QVERIFY(frameRenderer.IsInitialized());

    auto vol = loadFixtureVolume();
    QVERIFY(vol != nullptr);

    frameRenderer.UpdateFromVolume(&gl, *vol);
    QCOMPARE(frameRenderer.VertexCount(), 24); // 12 edges * 2

    // Add path
    std::vector<glm::ivec2> path = {{1000, 100}, {1001, 105}, {1002, 110}};
    frameRenderer.UpdateLineSection(&gl, *vol, path);
    QCOMPARE(frameRenderer.VertexCount(), 24 + 4); // 24 frame + 2 segments * 2

    frameRenderer.ClearLineSection();
    QCOMPARE(frameRenderer.VertexCount(), 24);

    // 2. Test SeismicSliceRenderer
    SeismicSliceRenderer sliceRenderer;
    QVERIFY(sliceRenderer.Initialize(&gl));
    QVERIFY(sliceRenderer.IsInitialized());

    // Extract real slice images
    SgySliceImage inlImg;
    std::string extractErr;
    QVERIFY(vol->ExtractSlice(SgySliceType::Inline, vol->InlineMin(), inlImg, extractErr));
    QVERIFY(sliceRenderer.UpdateSlice(&gl, SeismicSliceSlot::Inline, *vol, SgySliceType::Inline, vol->InlineMin(), inlImg));
    QVERIFY(sliceRenderer.IsSlotReady(SeismicSliceSlot::Inline));

    SgySliceImage xlImg;
    QVERIFY(vol->ExtractSlice(SgySliceType::Xline, vol->XlineMin(), xlImg, extractErr));
    QVERIFY(sliceRenderer.UpdateSlice(&gl, SeismicSliceSlot::Crossline, *vol, SgySliceType::Xline, vol->XlineMin(), xlImg));
    QVERIFY(sliceRenderer.IsSlotReady(SeismicSliceSlot::Crossline));

    SgySliceImage timeImg;
    QVERIFY(vol->ExtractSlice(SgySliceType::Time, 10, timeImg, extractErr));
    QVERIFY(sliceRenderer.UpdateSlice(&gl, SeismicSliceSlot::Time, *vol, SgySliceType::Time, 10, timeImg));
    QVERIFY(sliceRenderer.IsSlotReady(SeismicSliceSlot::Time));

    // Render pass
    SeismicCameraController cam;
    cam.FitToBounds(glm::vec3(-3.0f, -2.2f, -3.0f), glm::vec3(3.0f, 2.2f, 3.0f), 1.0f);
    const glm::mat4 view = cam.BuildViewMatrix();
    const glm::mat4 proj = cam.BuildProjectionMatrix(1.0f);

    gl.glViewport(0, 0, 512, 512);
    gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    sliceRenderer.Render(&gl, view, proj);
    frameRenderer.Render(&gl, view, proj);

    const GLenum glErr = gl.glGetError();
    QCOMPARE(glErr, static_cast<GLenum>(GL_NO_ERROR));

    // Cleanup
    frameRenderer.Cleanup(&gl);
    sliceRenderer.Cleanup(&gl);
    QCOMPARE(frameRenderer.IsInitialized(), false);
    QCOMPARE(sliceRenderer.IsInitialized(), false);

    context.doneCurrent();
}

QTEST_MAIN(TestSeismic3D)
#include "tst_seismic_3d.moc"
