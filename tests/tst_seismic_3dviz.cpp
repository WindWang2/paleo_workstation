#include <QtTest>
#include <QAction>
#include <QMenu>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <set>

#include "../src/domain/seismic/sgyvolume.h"
#include "../src/services/seismictaskservice.h"
#include "../src/services/paleotaskservice.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/ui/seismic3d/seismic3dcolormap.h"
#include "../src/ui/seismic3d/seismic3dtf.h"
#include "../src/ui/seismic3d/seismic3dtfeditor.h"
#include "../src/ui/seismic3d/seismiccameracontroller.h"
#include "../src/ui/seismic3d/horizonsurfacerenderer.h"
#include "../src/ui/seismic3d/seismicslicerenderer.h"
#include "../src/ui/seismic3d/volumeframerenderer.h"
#include "../src/ui/seismic3d/seismic3dviewportwidget.h"
#include "../src/ui/seismic3d/seismic3dviewpanel.h"

using namespace seismic;

// wave/seismic-3d-viz：TF 传递函数 / 任意斜剖面栅栏 / 层位面井轨迹 / 动画扫掠
// GL 用例全部走 openGLHeadlessRender 的 QSKIP 先例（无 GL 环境优雅跳过）。
class TestSeismic3DViz : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    // ---- 块1 TF ----
    void tfModelBasics();
    void tfEditorDragUpdatesAlpha();
    void tfOffscreenRealtimePixels();
    void panelTfZeroRefetch();

    // ---- 块2 任意斜剖面 / 栅栏 ----
    void lineSectionOffscreenPixels();
    void viewportObliquePickCommits();
    void panelLineSectionServicePath();

    // ---- 块3 层位面 / 井轨迹 ----
    void horizonSurfaceOffscreenPixels();
    void wellTrajectoryVertexGrowth();
    void panelHorizonWellOverlayLinkage();

    // ---- 块4 扫掠动画 ----
    void sweepAnimationNonBlockingExport();
    void sweepCacheHitRatio();

    // ---- 块5 GL 护栏 ----
    void noGlTeardownClean();

    // ---- 性能实测（Oracle 6，比率门；env 门控不进常规 CI）----
    void sweepPerfBigFixture();

private:
    std::shared_ptr<SgyVolume> loadFixtureVolume();
};

void TestSeismic3DViz::initTestCase() {
    Q_INIT_RESOURCE(seismic_shaders);
}

std::shared_ptr<SgyVolume> TestSeismic3DViz::loadFixtureVolume() {
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

// ---- 块1：TF 模型（LUT/索引编码）----
void TestSeismic3DViz::tfModelBasics() {
    const QStringList names = Seismic3DTransferFunction::presetNames();
    QVERIFY(!names.isEmpty());
    std::vector<std::vector<unsigned char>> luts;
    for (const QString &name : names) {
        const Seismic3DTransferFunction tf = Seismic3DTransferFunction::preset(name);
        QVERIFY(tf.isValid());
        const auto lut = tf.buildLutRgba();
        QCOMPARE(lut.size(), std::size_t(256 * 4));
        luts.push_back(lut);
    }
    // 预设互异
    for (std::size_t i = 1; i < luts.size(); ++i)
        QVERIFY(luts[i] != luts[0]);

    // 线性分段：停靠点 alpha 端点准确落位
    Seismic3DTransferFunction tf;
    tf.setStops({{0.0f, qRgb(255, 0, 0), 0.0f},
                 {1.0f, qRgb(0, 0, 255), 1.0f}});
    const auto lut = tf.buildLutRgba();
    QCOMPARE(lut[0 * 4 + 3], static_cast<unsigned char>(0));
    QCOMPARE(lut[255 * 4 + 3], static_cast<unsigned char>(255));
    QCOMPARE(lut[255 * 4 + 0], static_cast<unsigned char>(0));   // 色彩也插值
    QCOMPARE(lut[255 * 4 + 2], static_cast<unsigned char>(255));
    // 中点 0.5 → alpha≈127/128
    QVERIFY(lut[127 * 4 + 3] > 115 && lut[127 * 4 + 3] < 141);

    // 停靠点编辑不变量：邻居区间内移动、≥2 保留
    tf.setStopPos(1, 0.5f);
    QCOMPARE(tf.stops().size(), 2);
    QVERIFY(tf.removeStop(0) == false); // 只剩 2 个——删不动
    tf.addStop(0.25f, qRgb(0, 255, 0), 0.5f);
    QCOMPARE(tf.stops().size(), 3);
    QCOMPARE(tf.stops()[1].pos, 0.25f); // 排序位

    // valueToIndex 对称归一（与 colorizeSlice 同语义）
    QCOMPARE(Seismic3DTransferFunction::valueToIndex(0.0f, 1.0f), static_cast<unsigned char>(128));
    QCOMPARE(Seismic3DTransferFunction::valueToIndex(-1.0f, 1.0f), static_cast<unsigned char>(0));
    QCOMPARE(Seismic3DTransferFunction::valueToIndex(1.0f, 1.0f), static_cast<unsigned char>(255));

    // 索引字节：NaN 掩码 G=0，正常 G=255
    SgySliceImage img;
    img.width = 2;
    img.height = 1;
    img.valueMin = -1.0f;
    img.valueMax = 1.0f;
    img.values = {0.5f, std::numeric_limits<float>::quiet_NaN()};
    const auto bytes = Seismic3DTransferFunction::buildIndexBytes(img);
    QCOMPARE(bytes.size(), std::size_t(4));
    QCOMPARE(bytes[1], static_cast<unsigned char>(255));
    QCOMPARE(bytes[3], static_cast<unsigned char>(0));
}

// ---- 块1：TF 编辑器（拖拽改不透明度 → 信号实时外发）----
void TestSeismic3DViz::tfEditorDragUpdatesAlpha() {
    Seismic3DTfEditorWidget editor;
    editor.resize(400, 200);
    editor.show();
    editor.setTransferFunction(Seismic3DTransferFunction::preset(QStringLiteral("均匀半透明")));
    QCOMPARE(editor.transferFunction().stops().size(), 3);

    // 曲线区几何（与实现同式：边距 16、色带高 max(28,h/3)）
    const int m = 16;
    const int gradH = std::max(28, editor.height() / 3);
    const int crLeft = m;
    const int crTop = m;
    const int crH = (editor.height() - m - gradH) - 2 * m;
    const int crW = editor.width() - 2 * m;
    QVERIFY(crH > 10 && crW > 10);

    // 首停靠点（pos=0，alpha≈0.28）的屏幕位
    const double alpha0 = editor.transferFunction().stops()[0].alpha;
    const QPoint stop0(crLeft,
                       static_cast<int>(crTop + crH - alpha0 * crH));
    QSignalSpy spy(&editor, &Seismic3DTfEditorWidget::transferFunctionChanged);

    // 拖到曲线区顶部（alpha→接近 1）
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, stop0);
    QTest::mouseMove(&editor, QPoint(crLeft, crTop + 2));
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(crLeft, crTop + 2));

    QVERIFY(spy.count() >= 1);
    const float dragged = editor.transferFunction().stops()[0].alpha;
    QVERIFY2(dragged > 0.85f,
             qPrintable(QStringLiteral("拖后 alpha=%1（应≈1）").arg(dragged)));

    // 双击空区插停靠点（pos 中段、无既有命中）
    const QPoint mid(crLeft + crW / 2, crTop + crH / 2);
    QTest::mouseDClick(&editor, Qt::LeftButton, Qt::NoModifier, mid);
    QCOMPARE(editor.transferFunction().stops().size(), 4);
}

// ---- 块1（Oracle 1）：TF 改不透明度曲线，3D 体实时像素响应 ----
// 断言三件：a) 两 LUT 渲染结果确有像素差；b) 差异非均匀（改动只影响半值域
// ——负峰半透明化后负振幅像素回退背景色，其余保留）；c) 全程零切片重上传
// （两次渲染之间只 SetTransferFunction，GL 无错误）。
void TestSeismic3DViz::tfOffscreenRealtimePixels() {
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QOffscreenSurface surface;
    surface.setFormat(format);
    surface.create();
    if (!surface.isValid())
        QSKIP("offscreen surface unavailable");
    QOpenGLContext context;
    context.setFormat(format);
    if (!context.create() || !context.makeCurrent(&surface))
        QSKIP("OpenGL context could not be created in this environment");
    QOpenGLFunctions_3_3_Core gl;
    if (!gl.initializeOpenGLFunctions())
        QSKIP("OpenGL 3.3 Core functions not available");

    auto vol = loadFixtureVolume();
    QVERIFY(vol != nullptr);

    SeismicSliceRenderer renderer;
    QVERIFY(renderer.Initialize(&gl));

    // 一层堆叠切片（体渲染代表；values 直传——TF 模式下不需要 rgba）
    SgySliceImage img;
    std::string err;
    QVERIFY(vol->ExtractSlice(SgySliceType::Time, vol->SampleMax() / 2, img, err));
    QVERIFY(!img.values.empty());

    Seismic3DTransferFunction tfA; // 全值域恒不透明
    tfA.setStops({{0.0f, qRgb(60, 60, 60), 1.0f},
                  {1.0f, qRgb(200, 200, 200), 1.0f}});
    QVERIFY(renderer.SetTransferFunction(&gl, tfA.buildLutRgba(), true));
    QVERIFY(renderer.IsTransferFunctionEnabled());
    QVERIFY(renderer.UpdateStackLayer(&gl, 0, *vol, vol->SampleMax() / 2, img));
    renderer.SetStackVisible(true);
    renderer.SetSliceAlpha(1.0f);

    // FBO 渲染目标（抄 tst_seismic_budgets 基线模式）
    const int W = 400, H = 300;
    GLuint fbo = 0, colorTex = 0, depthRbo = 0;
    gl.glGenFramebuffers(1, &fbo);
    gl.glGenTextures(1, &colorTex);
    gl.glBindTexture(GL_TEXTURE_2D, colorTex);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl.glGenRenderbuffers(1, &depthRbo);
    gl.glBindRenderbuffer(GL_RENDERBUFFER, depthRbo);
    gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, W, H);
    gl.glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);
    gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRbo);
    if (gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        QSKIP("baseline FBO incomplete");
    gl.glViewport(0, 0, W, H);
    gl.glClearColor(0.12f, 0.14f, 0.17f, 1.0f);
    gl.glEnable(GL_DEPTH_TEST);

    SeismicCameraController camera;
    camera.SetTopView(); // 俯视正对堆叠层——几何差最小化，只剩 TF 色差
    const glm::mat4 proj = camera.BuildProjectionMatrix(double(W) / double(H));

    const auto renderAndRead = [&](std::vector<unsigned char> &out) {
        gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderer.Render(&gl, camera.BuildViewMatrix(), proj);
        gl.glFinish();
        out.resize(std::size_t(W) * H * 4);
        gl.glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    };

    std::vector<unsigned char> pixA, pixB;
    renderAndRead(pixA);
    QVERIFY(gl.glGetError() == GL_NO_ERROR);

    // TF B：负峰半段 alpha 0（正峰半段保持）——只动 LUT，不重传切片纹理
    Seismic3DTransferFunction tfB;
    tfB.setStops({{0.0f, qRgb(60, 60, 60), 0.0f},
                  {0.5f, qRgb(60, 60, 60), 0.0f},
                  {1.0f, qRgb(200, 200, 200), 1.0f}});
    QVERIFY(renderer.SetTransferFunction(&gl, tfB.buildLutRgba(), true));
    renderAndRead(pixB);
    QVERIFY(gl.glGetError() == GL_NO_ERROR);

    // 背景色（clear color 0.12/0.14/0.17 → 31/36/43）
    const auto isBackground = [](const std::vector<unsigned char> &p, std::size_t i) {
        return std::abs(int(p[i]) - 31) <= 2 && std::abs(int(p[i + 1]) - 36) <= 2 &&
               std::abs(int(p[i + 2]) - 43) <= 2;
    };
    int drawnInA = 0, changed = 0, keptDrawn = 0;
    for (std::size_t i = 0; i < pixA.size(); i += 4) {
        const bool changedPx = pixA[i] != pixB[i] || pixA[i + 1] != pixB[i + 1] ||
                               pixA[i + 2] != pixB[i + 2];
        if (!isBackground(pixA, i)) {
            ++drawnInA;
            if (!changedPx)
                ++keptDrawn;
        }
        if (changedPx)
            ++changed;
    }
    QVERIFY2(drawnInA > 100, "TF A 渲染应有实质覆盖（堆叠层可见）");
    QVERIFY2(changed > 50, "TF 改不透明度曲线后像素应有变化");
    QVERIFY2(keptDrawn > 20, "变化应非均匀：正峰半段像素保持，负峰半段回退背景");

    renderer.SetStackVisible(false);
    gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.glDeleteFramebuffers(1, &fbo);
    gl.glDeleteTextures(1, &colorTex);
    gl.glDeleteRenderbuffers(1, &depthRbo);
    renderer.Cleanup(&gl);
    context.doneCurrent();
}

// ---- 块1：面板 TF 实时性——LUT 修改零取数（不加提取任务）----
void TestSeismic3DViz::panelTfZeroRefetch() {
    auto vol = loadFixtureVolume();
    QVERIFY(vol != nullptr);

    PaleoProjectStore store;
    PaleoTaskService taskSvc(&store);
    SeismicTaskService seismicSvc(&taskSvc, 16 * 1024 * 1024);

    Seismic3DViewPanel panel;
    panel.setTaskService(&seismicSvc);
    panel.setVolume(vol);
    panel.resize(800, 600);

    auto *vp = panel.viewport();
    // 初始切片任务已提交（offscreen 下 QOpenGLWidget 不建 GL：面板回调把
    // 切片暂存 pendingSlices_，不置 slotReady——零取数断言不受影响）
    QTRY_VERIFY_WITH_TIMEOUT(taskSvc.tasks().isEmpty() == false, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(seismicSvc.activeTaskCount() == 0, 8000);

    // 等在途任务收敛（拖尾切片全部终态）
    QTest::qWait(300);
    const int tasksBefore = taskSvc.tasks().size();

    const auto tf1 = Seismic3DTransferFunction::preset(QStringLiteral("均匀半透明"));
    panel.setTransferFunction(tf1);
    QVERIFY(panel.isTransferFunctionEnabled());
    QCOMPARE(panel.transferFunction().stops().size(), tf1.stops().size());

    // 编辑器式连续改（拖动中每步 setStopAlpha → setTransferFunction）
    Seismic3DTransferFunction tf2 = tf1;
    tf2.setStopAlpha(0, 0.9f);
    panel.setTransferFunction(tf2);
    Seismic3DTransferFunction tf3 = tf2;
    tf3.setStopAlpha(2, 0.05f);
    panel.setTransferFunction(tf3);
    QTest::qWait(200); // 若有隐藏取数，此处显形

    QCOMPARE(taskSvc.tasks().size(), tasksBefore); // 零取数
    // 值纹理重喂走暂存通道（GL 未建也不炸、不阻塞）
    QVERIFY(vp != nullptr);

    // 关 TF → 恢复预烘焙色路径（仍零取数）
    panel.setTransferFunctionEnabled(false);
    QVERIFY(!panel.isTransferFunctionEnabled());
    QTest::qWait(100);
    QCOMPARE(taskSvc.tasks().size(), tasksBefore);
}

// ---- 块2（Oracle 2a）：任意剖面渲染像素非空 ----
// BuildLineSection 取数（对角斜剖面）→ UpdateLineSlice 贴入 → 等轴视角下
// 帷幕面在渲染结果中占实质像素（非背景）且 GL 无错误。
void TestSeismic3DViz::lineSectionOffscreenPixels() {
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QOffscreenSurface surface;
    surface.setFormat(format);
    surface.create();
    if (!surface.isValid())
        QSKIP("offscreen surface unavailable");
    QOpenGLContext context;
    context.setFormat(format);
    if (!context.create() || !context.makeCurrent(&surface))
        QSKIP("OpenGL context could not be created in this environment");
    QOpenGLFunctions_3_3_Core gl;
    if (!gl.initializeOpenGLFunctions())
        QSKIP("OpenGL 3.3 Core functions not available");

    auto vol = loadFixtureVolume();
    QVERIFY(vol != nullptr);

    // 对角斜剖面（起止线）+ 栅栏（三段折线）各取一次
    const std::vector<glm::ivec2> oblique = {
        {vol->InlineMin(), vol->XlineMin()}, {vol->InlineMax(), vol->XlineMax()}};
    const std::vector<glm::ivec2> fence = {
        {vol->InlineMin(), vol->XlineMin()},
        {vol->InlineMax(), vol->XlineMin() + (vol->XlineMax() - vol->XlineMin()) / 2},
        {vol->InlineMin() + (vol->InlineMax() - vol->InlineMin()) / 2, vol->XlineMax()},
        {vol->InlineMin(), vol->XlineMax()}};

    SgySliceImage obliqueImg, fenceImg;
    SgySectionStats stats;
    std::string err;
    QVERIFY2(BuildLineSection(*vol, oblique, SgySectionOptions{}, obliqueImg, stats, err),
             err.c_str());
    QVERIFY(obliqueImg.width > 1);
    QVERIFY2(BuildLineSection(*vol, fence, SgySectionOptions{}, fenceImg, stats, err),
             err.c_str());
    QVERIFY(fenceImg.width > obliqueImg.width); // 多段折线列数更多（栅栏更长）

    SeismicSliceRenderer renderer;
    QVERIFY(renderer.Initialize(&gl));
    QVERIFY(renderer.UpdateLineSlice(&gl, *vol, oblique, obliqueImg));
    QVERIFY(renderer.IsSlotReady(SeismicSliceSlot::Line));

    const int W = 400, H = 300;
    GLuint fbo = 0, colorTex = 0, depthRbo = 0;
    gl.glGenFramebuffers(1, &fbo);
    gl.glGenTextures(1, &colorTex);
    gl.glBindTexture(GL_TEXTURE_2D, colorTex);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl.glGenRenderbuffers(1, &depthRbo);
    gl.glBindRenderbuffer(GL_RENDERBUFFER, depthRbo);
    gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, W, H);
    gl.glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);
    gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRbo);
    if (gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        QSKIP("baseline FBO incomplete");
    gl.glViewport(0, 0, W, H);
    gl.glClearColor(0.12f, 0.14f, 0.17f, 1.0f);
    gl.glEnable(GL_DEPTH_TEST);

    SeismicCameraController camera; // 默认等轴（-45°, 35°）——帷幕面斜对镜头
    const glm::mat4 proj = camera.BuildProjectionMatrix(double(W) / double(H));
    gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    renderer.Render(&gl, camera.BuildViewMatrix(), proj);
    gl.glFinish();
    QVERIFY(gl.glGetError() == GL_NO_ERROR);

    std::vector<unsigned char> pix(std::size_t(W) * H * 4);
    gl.glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pix.data());
    int drawn = 0;
    for (std::size_t i = 0; i < pix.size(); i += 4) {
        if (!(std::abs(int(pix[i]) - 31) <= 2 && std::abs(int(pix[i + 1]) - 36) <= 2 &&
              std::abs(int(pix[i + 2]) - 43) <= 2))
            ++drawn;
    }
    QVERIFY2(drawn > 200, qPrintable(QStringLiteral("剖面帷幕应有实质像素（实得 %1）").arg(drawn)));

    // 换栅栏路径重贴（动态几何重建路径）——无 GL 错误、仍就绪
    QVERIFY(renderer.UpdateLineSlice(&gl, *vol, fence, fenceImg));
    gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    renderer.Render(&gl, camera.BuildViewMatrix(), proj);
    gl.glFinish();
    QVERIFY(gl.glGetError() == GL_NO_ERROR);

    renderer.Cleanup(&gl);
    gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.glDeleteFramebuffers(1, &fbo);
    gl.glDeleteTextures(1, &colorTex);
    gl.glDeleteRenderbuffers(1, &depthRbo);
    context.doneCurrent();
}

// ---- 块2（Oracle 2b）：两点拾取 → 提交信号（纯数学路径，无需 GL）----
void TestSeismic3DViz::viewportObliquePickCommits() {
    auto vol = loadFixtureVolume();
    QVERIFY(vol != nullptr);

    Seismic3DViewportWidget viewport;
    viewport.resize(400, 300);
    viewport.setVolume(vol);
    // 俯视预设：屏幕→顶面映射确定（等轴视角下屏幕上方射线不交顶面——
    // 地平线以上不可拾取是既定行为）
    viewport.setPresetView(SeismicCameraController::PresetView::Top);

    QSignalSpy committed(&viewport, &Seismic3DViewportWidget::sectionPathCommitted);
    QSignalSpy modeChanged(&viewport, &Seismic3DViewportWidget::sectionPickModeChanged);

    viewport.setSectionPickMode(true, /*autoCommitAtTwo=*/true);
    QVERIFY(viewport.isSectionPickMode());
    QCOMPARE(modeChanged.count(), 1);

    // 两击（俯视下分落体两侧——fixture 网格 3×4，取映射后必不同格的两点）
    QTest::mousePress(&viewport, Qt::LeftButton, Qt::NoModifier, QPoint(300, 150));
    QTest::mouseRelease(&viewport, Qt::LeftButton, Qt::NoModifier, QPoint(300, 150));
    QTest::mousePress(&viewport, Qt::LeftButton, Qt::NoModifier, QPoint(100, 250));
    QTest::mouseRelease(&viewport, Qt::LeftButton, Qt::NoModifier, QPoint(100, 250));

    QTRY_COMPARE_WITH_TIMEOUT(committed.count(), 1, 1000);
    QVERIFY(!viewport.isSectionPickMode()); // 提交后自动退出拾取
    QCOMPARE(modeChanged.count(), 2);

    const QVariant arg = committed.takeFirst().at(0);
    const std::vector<glm::ivec2> path = arg.value<std::vector<glm::ivec2>>();
    QCOMPARE(path.size(), std::size_t(2));
    // 拾取点吸附在体的真实线号范围内
    for (const auto &p : path) {
        QVERIFY(p.x >= vol->InlineMin() && p.x <= vol->InlineMax());
        QVERIFY(p.y >= vol->XlineMin() && p.y <= vol->XlineMax());
    }

    // 栅栏模式：多点 + 回车提交（对角三击——fixture 网格 3×4 取必异格）
    viewport.setSectionPickMode(true, /*autoCommitAtTwo=*/false);
    QTest::mousePress(&viewport, Qt::LeftButton, Qt::NoModifier, QPoint(320, 60));
    QTest::mouseRelease(&viewport, Qt::LeftButton, Qt::NoModifier, QPoint(320, 60));
    QTest::mousePress(&viewport, Qt::LeftButton, Qt::NoModifier, QPoint(200, 150));
    QTest::mouseRelease(&viewport, Qt::LeftButton, Qt::NoModifier, QPoint(200, 150));
    QTest::mousePress(&viewport, Qt::LeftButton, Qt::NoModifier, QPoint(80, 240));
    QTest::mouseRelease(&viewport, Qt::LeftButton, Qt::NoModifier, QPoint(80, 240));
    QCOMPARE(committed.count(), 0); // 未回车不提交
    QTest::keyClick(&viewport, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(committed.count(), 1, 1000);
    const std::vector<glm::ivec2> fence =
        committed.takeFirst().at(0).value<std::vector<glm::ivec2>>();
    QCOMPARE(fence.size(), std::size_t(3));
}

// ---- 块2（Oracle 2c）：取数路径断言——服务调用被触发 → 剖面贴入 ----
void TestSeismic3DViz::panelLineSectionServicePath() {
    auto vol = loadFixtureVolume();
    QVERIFY(vol != nullptr);

    PaleoProjectStore store;
    PaleoTaskService taskSvc(&store);
    SeismicTaskService seismicSvc(&taskSvc, 16 * 1024 * 1024);

    Seismic3DViewPanel panel;
    panel.setTaskService(&seismicSvc);
    panel.setVolume(vol);

    // 等初始切片任务收敛
    QTRY_VERIFY_WITH_TIMEOUT(seismicSvc.activeTaskCount() == 0, 8000);
    QTest::qWait(200);
    const int tasksBefore = taskSvc.tasks().size();

    // 视口信号 → 面板 → 服务（两点斜剖面）
    const std::vector<glm::ivec2> path = {
        {vol->InlineMin(), vol->XlineMin()}, {vol->InlineMax(), vol->XlineMax()}};
    emit panel.viewport()->sectionPathCommitted(path);

    QTRY_VERIFY_WITH_TIMEOUT(taskSvc.tasks().size() > tasksBefore, 3000); // 服务被触发
    QTRY_VERIFY_WITH_TIMEOUT(panel.isLineSectionReady(), 8000);           // 剖面贴入（含暂存）
    QTRY_VERIFY_WITH_TIMEOUT(seismicSvc.activeTaskCount() == 0, 8000);

    // 栅栏（多段）走同一通道
    panel.clearLineSection();
    QVERIFY(!panel.isLineSectionReady());
    const std::vector<glm::ivec2> fence = {
        {vol->InlineMin(), vol->XlineMin()},
        {vol->InlineMax(), vol->XlineMax()},
        {vol->InlineMin(), vol->XlineMax()}};
    emit panel.viewport()->sectionPathCommitted(fence);
    QTRY_VERIFY_WITH_TIMEOUT(panel.isLineSectionReady(), 8000);
    QTRY_VERIFY_WITH_TIMEOUT(seismicSvc.activeTaskCount() == 0, 8000);
}

// ---- 块3（Oracle 3a）：层位面渲染——像素覆盖 / NaN 挖洞 / 逐层显隐 ----
void TestSeismic3DViz::horizonSurfaceOffscreenPixels() {
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QOffscreenSurface surface;
    surface.setFormat(format);
    surface.create();
    if (!surface.isValid())
        QSKIP("offscreen surface unavailable");
    QOpenGLContext context;
    context.setFormat(format);
    if (!context.create() || !context.makeCurrent(&surface))
        QSKIP("OpenGL context could not be created in this environment");
    QOpenGLFunctions_3_3_Core gl;
    if (!gl.initializeOpenGLFunctions())
        QSKIP("OpenGL 3.3 Core functions not available");

    auto vol = loadFixtureVolume();
    QVERIFY(vol != nullptr);
    const double maxMs = vol->SampleIntervalUs() / 1000.0 * vol->SampleMax();

    // 满网格层位面（倾斜构造：twt 随 IL/XL 缓变）
    Seismic3DHorizonSurface full;
    full.name = QStringLiteral("H-full");
    full.inlineMin = vol->InlineMin();
    full.inlineCount = vol->InlineCount();
    full.inlineStep = std::max(1, (vol->InlineMax() - vol->InlineMin()) /
                                      std::max(1, vol->InlineCount() - 1));
    full.xlineMin = vol->XlineMin();
    full.xlineCount = vol->XlineCount();
    full.xlineStep = std::max(1, (vol->XlineMax() - vol->XlineMin()) /
                                     std::max(1, vol->XlineCount() - 1));
    const auto fillGrid = [&](Seismic3DHorizonSurface &s, bool hole) {
        s.twtMs.assign(std::size_t(s.inlineCount) * std::size_t(s.xlineCount), 0.0);
        for (int i = 0; i < s.inlineCount; ++i) {
            for (int x = 0; x < s.xlineCount; ++x) {
                const bool inHole = hole && i == s.inlineCount / 2 && x == s.xlineCount / 2;
                s.twtMs[std::size_t(i) * s.xlineCount + x] =
                    inHole ? std::numeric_limits<double>::quiet_NaN()
                           : maxMs * (0.3 + 0.05 * i - 0.04 * x);
            }
        }
    };
    fillGrid(full, /*hole=*/false);
    Seismic3DHorizonSurface holed = full;
    holed.name = QStringLiteral("H-holed");
    fillGrid(holed, /*hole=*/true);

    HorizonSurfaceRenderer renderer;
    QVERIFY(renderer.Initialize(&gl));
    QVERIFY(renderer.UpdateHorizons(&gl, *vol, {full}));
    QVERIFY(renderer.TriangleCount() > 0);

    // FBO 渲染（抄 budgets 基线模式）
    const int W = 400, H = 300;
    GLuint fbo = 0, colorTex = 0, depthRbo = 0;
    gl.glGenFramebuffers(1, &fbo);
    gl.glGenTextures(1, &colorTex);
    gl.glBindTexture(GL_TEXTURE_2D, colorTex);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl.glGenRenderbuffers(1, &depthRbo);
    gl.glBindRenderbuffer(GL_RENDERBUFFER, depthRbo);
    gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, W, H);
    gl.glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);
    gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRbo);
    if (gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        QSKIP("baseline FBO incomplete");
    gl.glViewport(0, 0, W, H);
    gl.glClearColor(0.12f, 0.14f, 0.17f, 1.0f);
    gl.glEnable(GL_DEPTH_TEST);

    SeismicCameraController camera;
    camera.FitToBounds(glm::vec3(-3, -2.2, -3), glm::vec3(3, 2.2, 3), double(W) / double(H));
    const glm::mat4 proj = camera.BuildProjectionMatrix(double(W) / double(H));

    const auto renderCount = [&]() {
        gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderer.Render(&gl, camera.BuildViewMatrix(), proj);
        gl.glFinish();
        std::vector<unsigned char> pix(std::size_t(W) * H * 4);
        gl.glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pix.data());
        int drawn = 0;
        for (std::size_t i = 0; i < pix.size(); i += 4) {
            if (!(std::abs(int(pix[i]) - 31) <= 2 && std::abs(int(pix[i + 1]) - 36) <= 2 &&
                  std::abs(int(pix[i + 2]) - 43) <= 2))
                ++drawn;
        }
        return drawn;
    };

    const int drawnFull = renderCount();
    QVERIFY2(drawnFull > 500,
             qPrintable(QStringLiteral("层位面应有实质覆盖（实得 %1 px）").arg(drawnFull)));

    // 挖洞版：同覆盖规模略小（fixture 网格小，允许 >= —— 主断言是不炸+可渲）
    QVERIFY(renderer.UpdateHorizons(&gl, *vol, {holed}));
    const int drawnHoled = renderCount();
    QVERIFY(drawnHoled > 500);

    // 逐层位显隐：关 → 全背景
    renderer.SetHorizonVisible(0, false);
    const int drawnHidden = renderCount();
    QCOMPARE(drawnHidden, 0);
    renderer.SetHorizonVisible(0, true);

    QVERIFY(gl.glGetError() == GL_NO_ERROR);
    renderer.Cleanup(&gl);
    gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.glDeleteFramebuffers(1, &fbo);
    gl.glDeleteTextures(1, &colorTex);
    gl.glDeleteRenderbuffers(1, &depthRbo);
    context.doneCurrent();
}

// ---- 块3：井轨迹折线顶点计数（斜井 N 段 vs 垂直井基线）----
void TestSeismic3DViz::wellTrajectoryVertexGrowth() {
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QOffscreenSurface surface;
    surface.setFormat(format);
    surface.create();
    if (!surface.isValid())
        QSKIP("offscreen surface unavailable");
    QOpenGLContext context;
    context.setFormat(format);
    if (!context.create() || !context.makeCurrent(&surface))
        QSKIP("OpenGL context could not be created in this environment");
    QOpenGLFunctions_3_3_Core gl;
    if (!gl.initializeOpenGLFunctions())
        QSKIP("OpenGL 3.3 Core functions not available");

    auto vol = loadFixtureVolume();
    QVERIFY(vol != nullptr);

    VolumeFrameRenderer frame;
    QVERIFY(frame.Initialize(&gl));
    frame.UpdateFromVolume(&gl, *vol);
    const GLsizei base = frame.VertexCount(); // 24（12 边 × 2）

    // 垂直井：光晕+主色 2 线 = 4 顶点
    Seismic3DWell vertical;
    vertical.name = QStringLiteral("V1");
    vertical.inlineNo = vol->InlineMin();
    vertical.xlineNo = vol->XlineMin();
    vertical.bottomFrac = 1.0f;
    frame.UpdateWells(&gl, *vol, {vertical});
    QCOMPARE(frame.VertexCount(), base + 4);

    // 斜井轨迹 3 点（2 段）：垂直 4 + 轨迹 2×4 = 12 增量
    Seismic3DWell deviated;
    deviated.name = QStringLiteral("D1");
    deviated.inlineNo = vol->InlineMin();
    deviated.xlineNo = vol->XlineMin();
    deviated.bottomFrac = 1.0f;
    deviated.trajectory = {
        {float(vol->InlineMin()), float(vol->XlineMin()), 0.0f},
        {float((vol->InlineMin() + vol->InlineMax()) / 2),
         float((vol->XlineMin() + vol->XlineMax()) / 2), 0.5f},
        {float(vol->InlineMax()), float(vol->XlineMax()), 1.0f},
    };
    frame.UpdateWells(&gl, *vol, {vertical, deviated});
    QCOMPARE(frame.VertexCount(), base + 4 + 12);

    frame.Cleanup(&gl);
    context.doneCurrent();
}

// ---- 块3（Oracle 3b）：fixture 层位/井上图 + 显隐联动（层树联动信号面）----
void TestSeismic3DViz::panelHorizonWellOverlayLinkage() {
    auto vol = loadFixtureVolume();
    QVERIFY(vol != nullptr);

    // fixture 层位：真实域路径——拾取点 → IDW 网格化（SeismicTaskService::gridPicks）
    QList<SeismicPick> picks;
    int nextId = 1;
    const double maxMs = vol->SampleIntervalUs() / 1000.0 * vol->SampleMax();
    for (int il : vol->InlineValues()) {
        for (int xl : vol->XlineValues()) {
            SeismicPick p;
            p.id = nextId++;
            p.inlineNo = il;
            p.xlineNo = xl;
            p.twtMs = maxMs * 0.35;
            p.sampleIndex = int(p.twtMs / std::max(1.0, vol->SampleIntervalUs() / 1000.0));
            p.horizonName = QStringLiteral("H1");
            picks << p;
        }
    }
    const SeismicHorizonGrid grid = SeismicTaskService::gridPicks(picks);
    QVERIFY(grid.isValid());
    QVERIFY(grid.inlineCount == vol->InlineCount());
    QVERIFY(grid.xlineCount == vol->XlineCount());

    Seismic3DViewPanel panel;
    panel.resize(800, 600);
    panel.setVolume(vol);

    QSignalSpy horizonSpy(&panel, &Seismic3DViewPanel::horizonVisibilityChanged);
    QSignalSpy wellSpy(&panel, &Seismic3DViewPanel::wellVisibilityChanged);

    panel.setHorizons({QStringLiteral("H1"), QStringLiteral("H2")}, {grid, grid});
    QCOMPARE(panel.horizonNames(), QStringList({QStringLiteral("H1"), QStringLiteral("H2")}));
    QVERIFY(panel.isHorizonVisible(QStringLiteral("H1")));
    QVERIFY(panel.viewport()->horizonCount() == 2);

    // 显隐联动（层树/overlay 菜单同一入口）
    panel.setHorizonVisible(QStringLiteral("H1"), false);
    QVERIFY(!panel.isHorizonVisible(QStringLiteral("H1")));
    QVERIFY(panel.isHorizonVisible(QStringLiteral("H2"))); // 兄弟层位不受牵连
    QCOMPARE(horizonSpy.count(), 1);
    QCOMPARE(horizonSpy.takeFirst().at(0).toString(), QStringLiteral("H1"));
    panel.setHorizonVisible(QStringLiteral("H1"), true);
    QVERIFY(panel.isHorizonVisible(QStringLiteral("H1")));

    // 井轨迹上图（fixture 两口：垂直 + 斜井）+ 标注开关
    Seismic3DWell w1;
    w1.name = QStringLiteral("W-vert");
    w1.inlineNo = vol->InlineMin();
    w1.xlineNo = vol->XlineMin();
    Seismic3DWell w2;
    w2.name = QStringLiteral("W-dev");
    w2.inlineNo = vol->InlineMax();
    w2.xlineNo = vol->XlineMax();
    w2.trajectory = {{float(vol->InlineMax()), float(vol->XlineMax()), 0.0f},
                     {float(vol->InlineMin()), float(vol->XlineMin()), 1.0f}};
    panel.setWells({w1, w2});

    // overlay 菜单存在且含井/标注/逐层位动作
    const QList<QMenu *> menus = panel.findChildren<QMenu *>();
    QVERIFY(!menus.isEmpty());
    QStringList actionTexts;
    for (auto *m : menus)
        for (QAction *a : m->actions())
            actionTexts << a->text();
    QVERIFY(actionTexts.contains(QStringLiteral("井轨迹")));
    QVERIFY(actionTexts.contains(QStringLiteral("井名标注")));
    QVERIFY(actionTexts.contains(QStringLiteral("H1")));
    QVERIFY(actionTexts.contains(QStringLiteral("H2")));

    // 菜单动作拨井显隐 → 信号外发（层树联动回写面）
    for (auto *m : menus) {
        for (QAction *a : m->actions()) {
            if (a->text() == QStringLiteral("井轨迹"))
                a->setChecked(false); // setChecked 即发 toggled（联动链路单发）
        }
    }
    QCOMPARE(wellSpy.count(), 1);
    QCOMPARE(wellSpy.takeFirst().at(0).toBool(), false);
}

// ---- 块4（Oracle 4）：扫掠播放——UI 事件循环不阻塞 + 暂停/恢复 + PNG 序列 ----
void TestSeismic3DViz::sweepAnimationNonBlockingExport() {
    auto vol = loadFixtureVolume();
    QVERIFY(vol != nullptr);

    PaleoProjectStore store;
    PaleoTaskService taskSvc(&store);
    SeismicTaskService seismicSvc(&taskSvc, 16 * 1024 * 1024);

    Seismic3DViewPanel panel;
    panel.setTaskService(&seismicSvc);
    panel.setVolume(vol);
    panel.resize(800, 600);
    QTRY_VERIFY_WITH_TIMEOUT(seismicSvc.activeTaskCount() == 0, 8000);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    panel.setSweepExportDir(dir.path());

    QSignalSpy frames(&panel, &Seismic3DViewPanel::sweepFrameChanged);

    // 事件循环活性探针：1ms 定时器——播放期间持续进账 = UI 线程未被取数卡死
    QTimer probe;
    probe.setInterval(1);
    int probeTicks = 0;
    QObject::connect(&probe, &QTimer::timeout, &probe, [&probeTicks]() { ++probeTicks; });
    probe.start();

    // IL 轴（fixture 3 条线——回绕快、秒级完成多圈）
    panel.startSweep(SgySliceType::Inline, 20);
    QVERIFY(panel.isSweepRunning());
    QTest::qWait(600);
    probe.stop();

    QVERIFY2(frames.count() >= 8,
             qPrintable(QStringLiteral("帧推进不足：%1").arg(frames.count())));
    QVERIFY2(probeTicks > 30,
             qPrintable(QStringLiteral("UI 事件循环疑似阻塞：探针 %1 次/600ms").arg(probeTicks)));

    // 暂停：帧计数静止
    panel.pauseSweep();
    QVERIFY(!panel.isSweepRunning());
    const int frozen = frames.count();
    QTest::qWait(250);
    QCOMPARE(frames.count(), frozen);

    // 恢复：继续推进
    panel.resumeSweep();
    QVERIFY(panel.isSweepRunning());
    QTest::qWait(250);
    QVERIFY(frames.count() > frozen);

    // 停止 + 取数收敛（异步预取全部终态，无悬挂）
    panel.stopSweep();
    QVERIFY(!panel.isSweepRunning());
    QTRY_VERIFY_WITH_TIMEOUT(seismicSvc.activeTaskCount() == 0, 8000);

    // PNG 序列：导出计数与磁盘文件一致且非空
    QVERIFY(panel.sweepExportedCount() > 0);
    const auto pngs = QDir(dir.path()).entryList(
        {QStringLiteral("frame_*.png")}, QDir::Files, QDir::Name);
    QCOMPARE(pngs.size(), panel.sweepExportedCount());
    for (const QString &name : pngs) {
        QImage img(dir.filePath(name));
        QVERIFY2(!img.isNull(), qPrintable(name));
    }
}

// ---- 块4（Oracle 6 比率门）：扫掠取数缓存命中率（预取窗口生效）----
// 第二圈起帧与预取全走 SgyDataCache 命中——hits/请求 ≥ 0.8（fixture 门）。
void TestSeismic3DViz::sweepCacheHitRatio() {
    auto vol = loadFixtureVolume();
    QVERIFY(vol != nullptr);

    PaleoProjectStore store;
    PaleoTaskService taskSvc(&store);
    SeismicTaskService seismicSvc(&taskSvc, 16 * 1024 * 1024);

    Seismic3DViewPanel panel;
    panel.setTaskService(&seismicSvc);
    panel.setVolume(vol);
    QTRY_VERIFY_WITH_TIMEOUT(seismicSvc.activeTaskCount() == 0, 8000);

    QSignalSpy frames(&panel, &Seismic3DViewPanel::sweepFrameChanged);
    const int ilCount = int(vol->InlineValues().size());
    QVERIFY(ilCount >= 2);

    // 第一圈（冷缓存暖机）：走满 ilCount 帧
    panel.startSweep(SgySliceType::Inline, 30);
    QTRY_COMPARE_WITH_TIMEOUT(frames.count(), ilCount, 5000);

    // 第二圈起测量（全部应命中）
    const std::size_t hits0 = seismicSvc.dataCache().Hits();
    QTest::qWait(400); // 30fps ≈ 12 帧（≈4 圈）
    panel.stopSweep();
    const std::size_t hits1 = seismicSvc.dataCache().Hits();
    const int measuredFrames = frames.count() - ilCount;
    QVERIFY2(measuredFrames >= 6, "测量窗帧数不足");

    // 帧路径命中下界：每帧滑杆路径查一次缓存（预取暖过的帧即命中）。
    // 预取请求本身也计入 hits/miss，但第二圈起全命中——门 0.8 留容差。
    const std::size_t hitDelta = hits1 - hits0;
    const double ratio = double(hitDelta) / double(measuredFrames);
    qInfo("sweep hit ratio: %.2f (%zu hits / %d frames)",
          ratio, hitDelta, measuredFrames);
    QVERIFY2(ratio >= 0.8,
             qPrintable(QStringLiteral("扫掠缓存命中率 %1 < 0.8").arg(ratio)));

    QTRY_VERIFY_WITH_TIMEOUT(seismicSvc.activeTaskCount() == 0, 8000);
}

// ---- 块5（Oracle 5）：无 GL 环境优雅降级 + teardown 干净 ----
// offscreen 下 QOpenGLWidget 永不建上下文：切片/剖面/层位/TF 全走暂存路径，
// 析构不 crash 不挂死；带在途任务的先亡面板靠 QPointer 守卫安全着陆。
void TestSeismic3DViz::noGlTeardownClean() {
    auto vol = loadFixtureVolume();
    QVERIFY(vol != nullptr);

    {
        Seismic3DViewportWidget viewport;
        viewport.resize(400, 300);
        viewport.setVolume(vol);
        QVERIFY(!viewport.isGlReady());

        // 各 pending 通道灌满（GL 前暂存语义）
        SgySliceImage img;
        std::string err;
        QVERIFY(vol->ExtractSlice(SgySliceType::Time, 1, img, err));
        QVERIFY(viewport.updateSlice(SeismicSliceSlot::Time, SgySliceType::Time, 1, img));
        QVERIFY(viewport.updateLineSlice(
            {{vol->InlineMin(), vol->XlineMin()}, {vol->InlineMax(), vol->XlineMax()}}, img));
        QVERIFY(viewport.isLineSectionReady()); // pending 也算就绪

        Seismic3DHorizonSurface h;
        h.name = QStringLiteral("H");
        h.inlineMin = vol->InlineMin();
        h.inlineCount = 2;
        h.inlineStep = 1;
        h.xlineMin = vol->XlineMin();
        h.xlineCount = 2;
        h.xlineStep = 1;
        h.twtMs = {10.0, 10.0, 12.0, std::numeric_limits<double>::quiet_NaN()};
        viewport.setHorizons({h});
        QCOMPARE(viewport.horizonCount(), 1);

        viewport.setTransferFunction(std::vector<unsigned char>(256 * 4, 128), true);
        viewport.setSectionPickMode(true, true);
        QVERIFY(viewport.isSectionPickMode());
        viewport.setWellLabelsVisible(true);
    } // 无 GL 析构（cleanup 分支不进 makeCurrent）

    PaleoProjectStore store;
    PaleoTaskService taskSvc(&store);
    SeismicTaskService seismicSvc(&taskSvc, 16 * 1024 * 1024);
    {
        Seismic3DViewPanel panel;
        panel.setTaskService(&seismicSvc);
        panel.setVolume(vol);
        panel.setWells({});
        panel.startSweep(SgySliceType::Inline, 30); // 在途预取进行中
        QTest::qWait(50); // 至少一帧 + 预取入池
    } // 面板先于服务析构（回调守卫路径）
    QTRY_VERIFY_WITH_TIMEOUT(seismicSvc.activeTaskCount() == 0, 8000); // 无悬挂
}

// ---- 性能实测（Oracle 6）：大体 1 秒扫掠——帧推进/命中率/事件循环活性 ----
// env PALEO_SEISMIC_SWEEP_PERF=<sgy> 触发（220MB 生产形状夹具或真工区体）；
// 常规 CI 不设即 skip。门全比率化（帧率达标比、命中率、探针存活），
// 不卡墙钟绝对值（机器负载容差）。
void TestSeismic3DViz::sweepPerfBigFixture() {
    const QString bigPath = qEnvironmentVariable("PALEO_SEISMIC_SWEEP_PERF");
    if (bigPath.isEmpty() || !QFile::exists(bigPath))
        QSKIP("sweep perf not requested (set PALEO_SEISMIC_SWEEP_PERF=<sgy>)");

    QElapsedTimer loadClock;
    loadClock.start();
    auto vol = std::make_shared<SgyVolume>();
    std::string err;
    QVERIFY2(vol->Load(bigPath.toStdString(), err), err.c_str());
    const double loadMs = loadClock.elapsed();
    qInfo("volume load: %.0f ms (traces=%lld samples=%d)", loadMs,
          vol->TraceCount(), vol->SampleCount());

    PaleoProjectStore store;
    PaleoTaskService taskSvc(&store);
    SeismicTaskService seismicSvc(&taskSvc, 64 * 1024 * 1024);
    Seismic3DViewPanel panel;
    panel.setTaskService(&seismicSvc);
    panel.setVolume(vol);
    QTRY_VERIFY_WITH_TIMEOUT(seismicSvc.activeTaskCount() == 0, 60000);

    QSignalSpy frames(&panel, &Seismic3DViewPanel::sweepFrameChanged);
    QTimer probe;
    probe.setInterval(1);
    int probeTicks = 0;
    QObject::connect(&probe, &QTimer::timeout, &probe, [&probeTicks]() { ++probeTicks; });

    const int fps = 30;
    const std::size_t hits0 = seismicSvc.dataCache().Hits();
    panel.startSweep(SgySliceType::Time, fps);
    probe.start();
    QTest::qWait(1000);
    probe.stop();
    panel.stopSweep();
    const std::size_t hits1 = seismicSvc.dataCache().Hits();
    QTRY_VERIFY_WITH_TIMEOUT(seismicSvc.activeTaskCount() == 0, 30000);

    const int advanced = frames.count();
    const double fpsRatio = double(advanced) / double(fps); // 1s 窗口
    const std::size_t hitDelta = hits1 - hits0;
    const double hitPerFrame = double(hitDelta) / double(std::max(1, advanced));
    qInfo("sweep 1s @%dfps target: frames=%d (%.0f%%) probe=%d cacheHits=%zu (%.2f/frame)",
          fps, advanced, fpsRatio * 100.0, probeTicks, hitDelta, hitPerFrame);
    QVERIFY2(fpsRatio >= 0.6,
             qPrintable(QStringLiteral("帧推进率 %1%").arg(fpsRatio * 100.0, 0, 'f', 0)));
    // 事件循环活性：1ms 档 QTimer 受 Qt 粗粒度合并（Linux 实际 ~10ms/跳），
    // 健康循环 ≥50 跳/s；真阻塞趋零。比率门而非墙钟。
    QVERIFY2(probeTicks >= 50,
             qPrintable(QStringLiteral("事件循环探针 %1/1000ms").arg(probeTicks)));
}

QTEST_MAIN(TestSeismic3DViz)
#include "tst_seismic_3dviz.moc"
