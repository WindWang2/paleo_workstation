#include <QtTest>
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

QTEST_MAIN(TestSeismic3DViz)
#include "tst_seismic_3dviz.moc"
