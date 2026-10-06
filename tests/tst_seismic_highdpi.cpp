// 层：测试壳
// goal/highdpi-20261007（DESIGN.md「High DPI」）：高 DPI 口径验收。
//   1. dpr=1 基线：QT_SCALE_FACTOR 未设 → devicePixelRatioF()==1，
//      physicalViewportSize 恒等式（dpr=1 不回归的行为红线）。
//   2. dpr=2 尺寸：QT_SCALE_FACTOR 只在 QGuiApplication 构造时读取——
//      dpr=2 断言以子进程重入自身实现（父槽 QProcess + 环境变量）。
//      grab() 物理尺寸 = 逻辑×2；2D 拼接回退件满幅（四角+中心已着色、
//      三 cell 切片内容就位）；拾取坐标 dpr 无关（中心点击在 dpr=1 与
//      dpr=2 下提交同一网格点——拾取错位是 dpr 修复的经典次生 bug）。
//   3. GL 物理视口满幅：offscreen 下 QOpenGLWidget 不建 GL（既有结论），
//      走 QOffscreenSurface+FBO 直渲——视口尺寸用 widget 的
//      physicalViewportSize 同一口径；对照实验（逻辑视口画进物理 FBO）
//      复现缺陷形态（右上背景化），自证测试能抓到该 bug。
#include <QtTest>
#include <QApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>

#include <cstdio>

#if defined(Q_OS_WINDOWS)
#include <windows.h>
#endif

#include "../src/domain/seismic/sgyvolume.h"
#include "../src/ui/seismic3d/seismic3dfallback.h"
#include "../src/ui/seismic3d/seismic3dtf.h"
#include "../src/ui/seismic3d/seismiccameracontroller.h"
#include "../src/ui/seismic3d/seismicslicerenderer.h"
#include "../src/ui/seismic3d/seismic3dviewportwidget.h"

using namespace seismic;

static void initSeismicResources() {
    Q_INIT_RESOURCE(seismic_shaders);
}

namespace {

// 子进程槽名（QTest 按名运行单槽；父进程运行时这些槽 QSKIP 让位）。
constexpr const char *kChildGrab = "dpr2ChildGrabSize";
constexpr const char *kChildFallback = "dpr2ChildFallbackFullFrame";
constexpr const char *kChildPick = "dpr2ChildPickCenter";

// 子进程标记行解析（防 vacuous green：QT_SCALE_FACTOR 失效时子槽全 skip
// 退出码仍 0——父槽必须见到标记行才认账）。返回 (宽, 高)。
std::pair<int, int> parseMarker(const QByteArray &out, const char *tag) {
    const QByteArray prefix = QByteArray(tag) + ' ';
    for (const QByteArray &line : out.split('\n')) {
        if (line.startsWith(prefix)) {
            const QList<QByteArray> parts = line.mid(prefix.size()).split(' ');
            if (parts.size() == 2)
                return {parts[0].simplified().toInt(), parts[1].simplified().toInt()};
        }
    }
    return {-1, -1};
}

// 本进程 exe 路径。QCoreApplication::applicationFilePath() 在 Windows 混链
// 环境（exe 按 Qt 6.8 编、运行时加载 qgis-deps 的 6.11）实测返回空串，
// Windows 下用 GetModuleFileNameW 直取；其余平台走 Qt 口径。
QString selfExePath() {
#if defined(Q_OS_WINDOWS)
    wchar_t buf[MAX_PATH + 1] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
        return QString::fromWCharArray(buf, int(n));
#endif
    return QCoreApplication::applicationFilePath();
}

// 以 QT_SCALE_FACTOR=2 重入测试自身跑单个槽；返回退出码，out 收 stdout。
int runChild(const char *slot, QByteArray *out = nullptr) {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("QT_SCALE_FACTOR"), QStringLiteral("2"));
    QProcess proc;
    proc.setProcessEnvironment(env);
    proc.start(selfExePath(), {QString::fromLatin1(slot)});
    if (!proc.waitForStarted(10000)) {
        if (out) *out = proc.errorString().toUtf8();
        return -1;
    }
    if (!proc.waitForFinished(120000)) {
        proc.kill();
        proc.waitForFinished(5000);
        if (out) *out = proc.readAllStandardOutput();
        return -2;
    }
    const QByteArray so = proc.readAllStandardOutput();
    if (out) *out = so;
    return proc.exitCode();
}

std::shared_ptr<SgyVolume> loadFixtureVolume() {
    const QString sgyPath = QStringLiteral(SEGY_FIXTURE_PATH);
    if (!QFile::exists(sgyPath))
        return nullptr;
    auto vol = std::make_shared<SgyVolume>();
    std::string err;
    if (!vol->Load(sgyPath.toStdString(), err))
        return nullptr;
    return vol;
}

// 当前进程的 dpr（QWidget::devicePixelRatioF——QGuiApplication 无此实例方法）。
qreal envDpr() {
    QWidget probe;
    return probe.devicePixelRatioF();
}

// 顶视预设下点击视口中心，把自动提交的拾取路径首点（=中心网格）写入 out。
// dpr=1 父进程与 dpr=2 子进程各跑一遍，提交点必须一致。
// 返回 bool 而非在体内用 QVERIFY/QTRY（其展开是裸 `return;`，非 void 函数编不过）。
bool pickCenterGrid(const std::shared_ptr<SgyVolume> &vol, glm::ivec2 &out) {
    Seismic3DViewportWidget viewport;
    viewport.resize(400, 300);
    viewport.setVolume(vol);
    viewport.setPresetView(SeismicCameraController::PresetView::Top);

    QSignalSpy committed(&viewport, &Seismic3DViewportWidget::sectionPathCommitted);
    viewport.setSectionPickMode(true, /*autoCommitAtTwo=*/true);
    // 首击=视口中心；次击取既有用例证实必异格的点（小夹具 3×4 网格上
    // 中心附近两点可能同格被去重，两点自动提交就不触发）。
    QTest::mousePress(&viewport, Qt::LeftButton, Qt::NoModifier, QPoint(200, 150));
    QTest::mouseRelease(&viewport, Qt::LeftButton, Qt::NoModifier, QPoint(200, 150));
    QTest::mousePress(&viewport, Qt::LeftButton, Qt::NoModifier, QPoint(100, 250));
    QTest::mouseRelease(&viewport, Qt::LeftButton, Qt::NoModifier, QPoint(100, 250));
    if (!QTest::qWaitFor([&]() { return committed.count() == 1; }, 5000))
        return false;
    out = committed.takeFirst().at(0).value<std::vector<glm::ivec2>>().front();
    return true;
}

} // namespace

class TestSeismicHighDpi : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { initSeismicResources(); }

    // ---- dpr=1 基线（行为红线：修复不影响 dpr=1）----
    void dpr1BaselineIdentity();

    // ---- dpr=2 验收（父槽起子进程；子槽见下）----
    void dpr2GrabSizeSpawnsChild();
    void dpr2FallbackFullFrameSpawnsChild();
    void dpr2PickCenterMatchesBaseline();

    // ---- GL：物理视口满幅 + 逻辑视口缺陷对照 ----
    void glPhysicalViewportFullFrame();

    // ---- 子进程槽（父进程 dpr=1 环境下 QSKIP；只在 QT_SCALE_FACTOR=2 生效）----
    void dpr2ChildGrabSize();
    void dpr2ChildFallbackFullFrame();
    void dpr2ChildPickCenter();
};

void TestSeismicHighDpi::dpr1BaselineIdentity() {
    // ctest 沙箱不设 QT_SCALE_FACTOR → offscreen 平台 dpr 必为 1（既有
    // golden/快照测试全部在该基线跑，是 dpr=1 不回归的实证面）。
    QWidget probe;
    probe.resize(300, 200);
    QVERIFY2(qFuzzyCompare(probe.devicePixelRatioF(), qreal(1.0)),
             qPrintable(QStringLiteral("dpr=%1（QT_SCALE_FACTOR 泄漏？）")
                            .arg(probe.devicePixelRatioF())));

    // 换算口径恒等式：dpr=1 逐值不变；整数 dpr 精确倍乘；分数 dpr 四舍五入。
    QCOMPARE(Seismic3DViewportWidget::physicalViewportSize(400, 300, 1.0), QSize(400, 300));
    QCOMPARE(Seismic3DViewportWidget::physicalViewportSize(399, 299, 1.0), QSize(399, 299));
    QCOMPARE(Seismic3DViewportWidget::physicalViewportSize(400, 300, 2.0), QSize(800, 600));
    QCOMPARE(Seismic3DViewportWidget::physicalViewportSize(400, 300, 1.5), QSize(600, 450));
    QCOMPARE(Seismic3DViewportWidget::physicalViewportSize(401, 301, 1.25), QSize(501, 376));
    QCOMPARE(Seismic3DViewportWidget::physicalViewportSize(0, 0, 2.0), QSize(0, 0));
}

void TestSeismicHighDpi::dpr2GrabSizeSpawnsChild() {
    QByteArray out;
    const int rc = runChild(kChildGrab, &out);
    QVERIFY2(rc == 0, QByteArray("dpr=2 子进程（grab 尺寸）失败：\n" + out).constData());
    // 标记行必须出现且为物理尺寸——防子进程静默 skip（vacuous green）。
    const auto size = parseMarker(out, "PALEO_HIDPI_GRAB");
    QVERIFY2(size.first == 1200 && size.second == 600,
             qPrintable(QStringLiteral("dpr=2 grab 物理尺寸 (%1,%2) 应为 (1200,600)——"
                                       "QT_SCALE_FACTOR 未生效？")
                            .arg(size.first).arg(size.second)));
}

void TestSeismicHighDpi::dpr2FallbackFullFrameSpawnsChild() {
    QByteArray out;
    const int rc = runChild(kChildFallback, &out);
    QVERIFY2(rc == 0, QByteArray("dpr=2 子进程（2D 拼接满幅）失败：\n" + out).constData());
    const auto size = parseMarker(out, "PALEO_HIDPI_FB");
    QVERIFY2(size.first == 1200 && size.second == 600,
             qPrintable(QStringLiteral("dpr=2 拼接帧物理尺寸 (%1,%2) 应为 (1200,600)——"
                                       "QT_SCALE_FACTOR 未生效？")
                            .arg(size.first).arg(size.second)));
}

void TestSeismicHighDpi::dpr2PickCenterMatchesBaseline() {
    auto vol = loadFixtureVolume();
    QVERIFY2(vol != nullptr, "地震夹具缺失：" SEGY_FIXTURE_PATH);

    // dpr=1 基线：中心点击的拾取网格。
    glm::ivec2 baseline(-1, -1);
    QVERIFY2(pickCenterGrid(vol, baseline), "dpr=1 中心拾取未提交（基线自检失败）");
    QVERIFY2(baseline.x >= vol->InlineMin() && baseline.x <= vol->InlineMax() &&
                 baseline.y >= vol->XlineMin() && baseline.y <= vol->XlineMax(),
             qPrintable(QStringLiteral("中心拾取应在体线号范围内（il=%1 xl=%2，范围 %3-%4/%5-%6）")
                            .arg(baseline.x).arg(baseline.y)
                            .arg(vol->InlineMin()).arg(vol->InlineMax())
                            .arg(vol->XlineMin()).arg(vol->XlineMax())));

    QByteArray out;
    const int rc = runChild(kChildPick, &out);
    QVERIFY2(rc == 0, QByteArray("dpr=2 子进程（拾取）失败：\n" + out).constData());

    // 子进程把拾取点打成标记行，父进程对拍。
    glm::ivec2 child(-1, -1);
    bool parsed = false;
    for (const QByteArray &line : out.split('\n')) {
        if (line.startsWith("PALEO_HIDPI_PICK ")) {
            const QList<QByteArray> parts = line.mid(qstrlen("PALEO_HIDPI_PICK ")).split(' ');
            if (parts.size() == 2) {
                child = glm::ivec2(parts[0].toInt(), parts[1].toInt());
                parsed = true;
            }
        }
    }
    QVERIFY2(parsed, QByteArray("未解析到 PALEO_HIDPI_PICK 标记行：\n" + out).constData());
    QVERIFY2(child == baseline,
             qPrintable(QStringLiteral("拾取错位：dpr=1 得 (il=%1 xl=%2)，dpr=2 得 (il=%3 xl=%4)")
                            .arg(baseline.x).arg(baseline.y).arg(child.x).arg(child.y)));
}

// ---- 子进程槽 ----

void TestSeismicHighDpi::dpr2ChildGrabSize() {
    if (qFuzzyCompare(envDpr(), qreal(1.0))) {
        QSKIP("child-only 槽：由 dpr2GrabSizeSpawnsChild 以 QT_SCALE_FACTOR=2 重入运行");
    }
    QCOMPARE(qRound(envDpr() * 1000), 2000);

    // 2D 拼接回退件（GL 看门狗失败路径的渲染面）：逻辑 600×300。
    Seismic3DFallbackWidget fallback;
    fallback.resize(600, 300);
    QCOMPARE(fallback.width(), 600);   // dpr 不改变逻辑尺寸
    QCOMPARE(fallback.height(), 300);
    QCOMPARE(qRound(fallback.devicePixelRatioF() * 1000), 2000);

    const QPixmap pm = fallback.grab();
    QCOMPARE(pm.size(), QSize(1200, 600));            // 物理像素 = 逻辑×2
    QCOMPARE(qRound(pm.devicePixelRatio() * 1000), 2000);
    QCOMPARE(pm.deviceIndependentSize(), QSizeF(600, 300));
    std::printf("PALEO_HIDPI_GRAB %d %d\n", pm.width(), pm.height());
    std::fflush(stdout);
}

void TestSeismicHighDpi::dpr2ChildFallbackFullFrame() {
    if (qFuzzyCompare(envDpr(), qreal(1.0))) {
        QSKIP("child-only 槽：由 dpr2FallbackFullFrameSpawnsChild 以 QT_SCALE_FACTOR=2 重入运行");
    }

    Seismic3DFallbackWidget fallback;
    fallback.resize(600, 300);
    // 三槽纯色切片：与 paintEvent 的 cell 几何（gap=8、top=50）对照采样。
    const QRgb colors[3] = {qRgb(255, 0, 0), qRgb(0, 255, 0), qRgb(0, 0, 255)};
    const char *names[3] = {"IL 1002", "XL 3010", "T 512"};
    for (int i = 0; i < 3; ++i) {
        QImage slice(64, 64, QImage::Format_RGBA8888);
        slice.fill(QColor(colors[i])); // QColor 重载按格式正确落色（QRgb 裸填按字节序解释）
        fallback.setSlice(i, slice, QString::fromLatin1(names[i]));
    }

    const QImage img = fallback.grab().toImage();
    QCOMPARE(img.size(), QSize(1200, 600));

    // 满幅：物理四角 + 中心全部被画过（不透明），拼接图不留未渲染区。
    const QPoint probes[] = {{2, 2}, {1197, 2}, {2, 597}, {1197, 597}, {600, 300}};
    for (const QPoint &pt : probes)
        QCOMPARE(img.pixelColor(pt).alpha(), 255);

    // 三 cell 中心（逻辑 (102,171)/(299,171)/(496,171) × dpr）各就各位：
    // 纯色切片按 KeepAspectRatio 缩放，cell 中心必然落在切片内容上。
    const QPoint cellCenters[3] = {{204, 342}, {598, 342}, {992, 342}};
    for (int i = 0; i < 3; ++i) {
        const QColor c = img.pixelColor(cellCenters[i]);
        const QRgb want = colors[i];
        QVERIFY2(std::abs(int(c.red()) - qRed(want)) <= 2 &&
                     std::abs(int(c.green()) - qGreen(want)) <= 2 &&
                     std::abs(int(c.blue()) - qBlue(want)) <= 2,
                 qPrintable(QStringLiteral("cell %1 中心色 (%2,%3,%4) 应为切片色 (%5,%6,%7)")
                                .arg(i).arg(c.red()).arg(c.green()).arg(c.blue())
                                .arg(qRed(want)).arg(qGreen(want)).arg(qBlue(want))));
    }
    std::printf("PALEO_HIDPI_FB %d %d\n", img.width(), img.height());
    std::fflush(stdout);
}

void TestSeismicHighDpi::dpr2ChildPickCenter() {
    if (qFuzzyCompare(envDpr(), qreal(1.0))) {
        QSKIP("child-only 槽：由 dpr2PickCenterMatchesBaseline 以 QT_SCALE_FACTOR=2 重入运行");
    }
    auto vol = loadFixtureVolume();
    QVERIFY2(vol != nullptr, "地震夹具缺失：" SEGY_FIXTURE_PATH);

    glm::ivec2 picked(-1, -1);
    QVERIFY(pickCenterGrid(vol, picked));
    std::printf("PALEO_HIDPI_PICK %d %d\n", picked.x, picked.y);
    std::fflush(stdout);
    QVERIFY(picked.x >= vol->InlineMin() && picked.x <= vol->InlineMax());
    QVERIFY(picked.y >= vol->XlineMin() && picked.y <= vol->XlineMax());
}

// ---- GL：物理视口满幅（widget 换算口径）+ 逻辑视口缺陷对照 ----
void TestSeismicHighDpi::glPhysicalViewportFullFrame() {
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

    // 一层堆叠时间切片铺满水平面（TF 全不透明）——内容覆盖面。
    SeismicSliceRenderer renderer;
    QVERIFY(renderer.Initialize(&gl));
    SgySliceImage img;
    std::string err;
    QVERIFY(vol->ExtractSlice(SgySliceType::Time, vol->SampleMax() / 2, img, err));
    QVERIFY(!img.values.empty());
    Seismic3DTransferFunction tf;
    tf.setStops({{0.0f, qRgb(60, 60, 60), 1.0f},
                 {1.0f, qRgb(200, 200, 200), 1.0f}});
    QVERIFY(renderer.SetTransferFunction(&gl, tf.buildLutRgba(), true));
    QVERIFY(renderer.UpdateStackLayer(&gl, 0, *vol, vol->SampleMax() / 2, img));
    renderer.SetStackVisible(true);
    renderer.SetSliceAlpha(1.0f);

    // 物理帧buffer = 逻辑 400×300 × dpr2（widget 同一口径）。
    const QSize phys = Seismic3DViewportWidget::physicalViewportSize(400, 300, 2.0);
    QCOMPARE(phys, QSize(800, 600));

    GLuint fbo = 0, colorTex = 0, depthRbo = 0;
    gl.glGenFramebuffers(1, &fbo);
    gl.glGenTextures(1, &colorTex);
    gl.glBindTexture(GL_TEXTURE_2D, colorTex);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, phys.width(), phys.height(), 0, GL_RGBA,
                    GL_UNSIGNED_BYTE, nullptr);
    gl.glGenRenderbuffers(1, &depthRbo);
    gl.glBindRenderbuffer(GL_RENDERBUFFER, depthRbo);
    gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, phys.width(), phys.height());
    gl.glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);
    gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRbo);
    if (gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        QSKIP("baseline FBO incomplete");
    gl.glClearColor(0.12f, 0.14f, 0.17f, 1.0f);
    gl.glEnable(GL_DEPTH_TEST);

    // 俯视 + 距离收紧到平面半幅（3）> 视场半宽（d·tan22.5°·aspect）——
    // 切片面在两轴都溢出视场，物理视口正确时全帧有内容。
    SeismicCameraController camera;
    camera.SetTopView();
    camera.SetTarget(glm::vec3(0.0f));
    camera.SetDistance(5.0f);
    const glm::mat4 proj =
        camera.BuildProjectionMatrix(double(phys.width()) / double(phys.height()));

    const int PW = phys.width(), PH = phys.height();
    const auto renderInto = [&](int vw, int vh, std::vector<unsigned char> &out) {
        gl.glViewport(0, 0, vw, vh);
        gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderer.Render(&gl, camera.BuildViewMatrix(), proj);
        gl.glFinish();
        out.resize(std::size_t(PW) * PH * 4);
        gl.glReadPixels(0, 0, PW, PH, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    };
    // 背景色（0.12/0.14/0.17 → 31/36/43）
    const auto isBg = [&](const std::vector<unsigned char> &p, int x, int y) {
        const std::size_t i = (std::size_t(PH - 1 - y) * PW + x) * 4; // GL 左下原点
        return std::abs(int(p[i]) - 31) <= 2 && std::abs(int(p[i + 1]) - 36) <= 2 &&
               std::abs(int(p[i + 2]) - 43) <= 2;
    };

    // 1) 修复态：物理视口 → 四象限中心 + 帧中心全部有内容，覆盖占大半。
    std::vector<unsigned char> pix;
    renderInto(PW, PH, pix);
    QVERIFY(gl.glGetError() == GL_NO_ERROR);
    const QPoint probes[] = {{200, 150}, {600, 150}, {200, 450}, {600, 450}, {400, 300}};
    for (const QPoint &pt : probes)
        QVERIFY2(!isBg(pix, pt.x(), pt.y()),
                 qPrintable(QStringLiteral("物理视口下 (%1,%2) 仍为背景——内容不满幅")
                                .arg(pt.x()).arg(pt.y())));
    int drawn = 0;
    for (std::size_t i = 0; i < pix.size(); i += 4)
        if (!(std::abs(int(pix[i]) - 31) <= 2 && std::abs(int(pix[i + 1]) - 36) <= 2 &&
              std::abs(int(pix[i + 2]) - 43) <= 2))
            ++drawn;
    QVERIFY2(drawn > PW * PH * 9 / 10,
             qPrintable(QStringLiteral("物理视口覆盖 %1/%2 像素，应 >90%%").arg(drawn).arg(PW * PH)));

    // 2) 缺陷对照（测试自证）：逻辑视口画进物理 FBO —— 修复前的形态，
    //    右上大半必为背景。此断言若翻红说明 GL 视口语义变化，需重审口径。
    std::vector<unsigned char> pixBug;
    renderInto(400, 300, pixBug);
    QVERIFY(gl.glGetError() == GL_NO_ERROR);
    QVERIFY2(isBg(pixBug, 600, 450), "逻辑视口对照组右上应为背景（缺陷形态漂移）");
    int drawnBug = 0;
    for (std::size_t i = 0; i < pixBug.size(); i += 4)
        if (!(std::abs(int(pixBug[i]) - 31) <= 2 && std::abs(int(pixBug[i + 1]) - 36) <= 2 &&
              std::abs(int(pixBug[i + 2]) - 43) <= 2))
            ++drawnBug;
    QVERIFY2(drawnBug < PW * PH / 3,
             qPrintable(QStringLiteral("逻辑视口对照组覆盖 %1/%2（应骤降到 1/3 以下）")
                            .arg(drawnBug).arg(PW * PH)));

    renderer.SetStackVisible(false);
    gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.glDeleteFramebuffers(1, &fbo);
    gl.glDeleteTextures(1, &colorTex);
    gl.glDeleteRenderbuffers(1, &depthRbo);
    renderer.Cleanup(&gl);
    context.doneCurrent();
}

QTEST_MAIN(TestSeismicHighDpi)
#include "tst_seismic_highdpi.moc"
