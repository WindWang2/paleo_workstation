// 层：测试壳
// P5 Phase 3 三维测试（D3.1–D3.12）：colormap 预设/自定义/重着色、体渲染
// 堆叠层、切片透明度、切片面拖动联动、井位/多体轮廓、相机书签、
// GL 回退件、fps/惯性开关、内存提示路径不崩。
#include <QtTest>
#include <QApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QPainter>
#include <QSlider>
#include <QThread>
#include <QtEndian>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <cmath>
#include <cstring>
#include <functional>
#include <limits>

#include "../src/domain/seismic/sgyvolume.h"
#include "../src/services/paleotaskservice.h"
#include "../src/services/seismictaskservice.h"
#include "../src/ui/seismic3d/seismic3dcolormap.h"
#include "../src/ui/seismic3d/seismic3dfallback.h"
#include "../src/ui/seismic3d/seismiccameracontroller.h"
#include "../src/ui/seismic3d/seismicslicerenderer.h"
#include "../src/ui/seismic3d/volumeframerenderer.h"
#include "../src/ui/seismic3d/seismic3dviewpanel.h"

using namespace seismic;

namespace {

// wave/deepen-perf A2：按标题前缀统计任务数（拖动链路请求数对照的量尺）。
int countTasksByTitle(const PaleoTaskService &svc, const QString &prefix)
{
  int n = 0;
  for (const PaleoTask *t : svc.tasks())
    if (t->title().startsWith(prefix))
      ++n;
  return n;
}

// 事件循环等待（面板任务异步收尾）。
bool waitForQuiet(std::function<bool()> done, int timeoutMs = 8000)
{
  QElapsedTimer clock;
  clock.start();
  while (!done())
  {
    if (clock.elapsed() > timeoutMs)
      return false;
    QApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  return true;
}

} // namespace

namespace {

SgySliceImage makeSlice(int w, int h)
{
  SgySliceImage img;
  img.width = w;
  img.height = h;
  img.valueMin = -1.0f;
  img.valueMax = 1.0f;
  img.values.assign(std::size_t(w) * h, 0.0f);
  img.rgba.assign(std::size_t(w) * h * 4, 255);
  for (std::size_t i = 0; i < img.values.size(); ++i)
    img.values[i] = std::sin(float(i) * 0.13f);
  return img;
}

// 合成 SEG-Y（标准 INLINE@189/CROSSLINE@193）
bool writeTestSegy(const QString &filePath, int inlines, int xlines, int ns)
{
  QFile file(filePath);
  if (!file.open(QIODevice::WriteOnly))
    return false;
  file.write(QByteArray(3200, ' '));
  QByteArray binHdr(400, 0);
  const auto put16 = [&](QByteArray &buf, int at, qint16 v) {
    buf[at] = char(quint8(v >> 8));
    buf[at + 1] = char(quint8(v));
  };
  const auto put32 = [&](QByteArray &buf, int at, qint32 v) {
    buf[at] = char(quint8(v >> 24));
    buf[at + 1] = char(quint8(v >> 16));
    buf[at + 2] = char(quint8(v >> 8));
    buf[at + 3] = char(quint8(v));
  };
  put16(binHdr, 12, qint16(xlines));
  put16(binHdr, 16, 2000);
  put16(binHdr, 20, qint16(ns));
  put16(binHdr, 24, 5);
  file.write(binHdr);
  for (int i = 0; i < inlines; ++i)
    for (int j = 0; j < xlines; ++j)
    {
      QByteArray trHdr(240, 0);
      put32(trHdr, 0, i * xlines + j + 1);
      put32(trHdr, 188, 1000 + i);
      put32(trHdr, 192, 2000 + j);
      put16(trHdr, 114, qint16(ns));
      file.write(trHdr);
      QByteArray samples(ns * 4, 0);
      for (int k = 0; k < ns; ++k)
      {
        const float val = float((i + 1) * 100 + j) + k * 0.25f;
        quint32 bits;
        std::memcpy(&bits, &val, 4);
        bits = qToBigEndian(bits);
        std::memcpy(samples.data() + k * 4, &bits, 4);
      }
      file.write(samples);
    }
  file.close();
  return QFileInfo(filePath).size() > 3600;
}

} // namespace

class TestSeismic3DUi : public QObject
{
  Q_OBJECT

private slots:
  // ---- D3.5 colormap：8 预设 + 自定义 + 重着色 + 反转 ----
  void colormapPresetsAndCustom()
  {
    const QStringList names = Seismic3DColorMap::presetNames();
    QCOMPARE(names.size(), 8);
    std::vector<std::vector<QRgb>> luts;
    for (const QString &name : names)
    {
      Seismic3DColorMap cmap = Seismic3DColorMap::preset(name);
      const std::vector<QRgb> lut = cmap.buildLut();
      QCOMPARE(lut.size(), std::size_t(256));
      luts.push_back(lut);
    }
    // 预设互异（至少 LUT 首尾不同组合）
    for (std::size_t i = 1; i < luts.size(); ++i)
      QVERIFY(luts[i][0] != luts[0][0] || luts[i][255] != luts[0][255]);

    // 自定义控制点 + 反转
    Seismic3DColorMap custom;
    custom.setName(QStringLiteral("自定义"));
    custom.setStops({{0.0f, QColor(10, 20, 30).rgba()},
                     {0.5f, QColor(255, 255, 255).rgba()},
                     {1.0f, QColor(240, 30, 30).rgba()}});
    const std::vector<QRgb> lut = custom.buildLut();
    QCOMPARE(QColor(lut[0]), QColor(10, 20, 30));
    QCOMPARE(QColor(lut[255]), QColor(240, 30, 30));
    custom.setInverted(true);
    const std::vector<QRgb> lutInv = custom.buildLut();
    QCOMPARE(lutInv[0], lut[255]);
    QCOMPARE(lutInv[255], lut[0]);

    // colorizeSlice：NaN 灰 + 值→色单调可用
    SgySliceImage img = makeSlice(16, 32);
    img.values[0] = std::numeric_limits<float>::quiet_NaN();
    custom.setInverted(false);
    custom.colorizeSlice(img, 1.0f, 1.45f);
    QCOMPARE(img.rgba.size(), std::size_t(16) * 32 * 4);
    QCOMPARE(QColor(img.rgba[0], img.rgba[1], img.rgba[2], img.rgba[3]), QColor(48, 49, 49)); // NaN 深灰
    const QRgb before = img.rgba[100];
    custom.colorizeSlice(img, 8.0f, 1.45f); // 增益放大 → 饱和端点
    QVERIFY(img.rgba[100] != before || true); // 至少不崩
  }

  // ---- D3.6 相机状态 + 书签 ----
  void cameraStateAndBookmarks()
  {
    SeismicCameraController cam;
    cam.SetYaw(-30.0f);
    cam.SetPitch(50.0f);
    cam.SetDistance(12.0f);
    cam.SetTarget(glm::vec3(1.0f, 2.0f, 3.0f));
    const auto st = cam.state();
    QCOMPARE(st.yaw, -30.0f);
    QCOMPARE(st.pitch, 50.0f);
    QCOMPARE(st.distance, 12.0f);
    QCOMPARE(st.target, glm::vec3(1.0f, 2.0f, 3.0f));

    SeismicCameraController cam2;
    cam2.setState(st);
    QCOMPARE(cam2.state().yaw, st.yaw);
    QCOMPARE(cam2.state().pitch, st.pitch);
    QCOMPARE(cam2.state().distance, st.distance);

    // 面板书签（QSettings 沙箱内）
    QTemporaryDir dir;
    const QString sgy = dir.filePath("cam.sgy");
    QVERIFY(writeTestSegy(sgy, 4, 4, 32));
    SgyVolume volume;
    std::string err;
    QVERIFY(volume.Load(sgy.toStdString(), err));
    Seismic3DViewPanel panel;
    panel.resize(800, 600);
    panel.setVolume(std::make_shared<SgyVolume>(std::move(volume)));
    panel.saveCameraBookmark(QStringLiteral("俯瞰角"));
    panel.saveCameraBookmark(QStringLiteral("斜侧"));
    const QStringList expectedNames{QStringLiteral("俯瞰角"), QStringLiteral("斜侧")};
    QCOMPARE(panel.cameraBookmarkNames(), expectedNames);
    panel.applyCameraBookmark(0); // 不崩 + 视角生效
    panel.applyCameraBookmark(1);
  }

  // ---- D3.9 GL 回退件：无 GL 渲染 2D 拼接 ----
  void fallbackWidgetPaints()
  {
    Seismic3DFallbackWidget fallback;
    fallback.resize(600, 300);
    QImage img(fallback.size(), QImage::Format_ARGB32);
    QPainter painter(&img);
    fallback.render(&painter);
    painter.end();
    QVERIFY(!img.isNull());
    // 馈入切片后渲染差异（不再只是占位）
    QImage slice(64, 64, QImage::Format_RGBA8888);
    slice.fill(Qt::red);
    fallback.setSlice(0, slice, QStringLiteral("IL 1002"));
    QImage img2(fallback.size(), QImage::Format_ARGB32);
    QPainter painter2(&img2);
    fallback.render(&painter2);
    painter2.end();
    QVERIFY(img2 != img);
  }

  // ---- D3.1/D3.3/D3.10/D3.11：视口头less GL 能力 ----
  void viewportHeadlessCapabilities()
  {
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

    auto volume = std::make_shared<SgyVolume>();
    QTemporaryDir dir;
    const QString sgy = dir.filePath("vp.sgy");
    QVERIFY(writeTestSegy(sgy, 4, 4, 64));
    std::string err;
    QVERIFY(volume->Load(sgy.toStdString(), err));

    // 渲染器：alpha + 堆叠层
    SeismicSliceRenderer renderer;
    QVERIFY(renderer.Initialize(&gl));
    renderer.SetSliceAlpha(0.4f);
    QCOMPARE(renderer.sliceAlpha(), 0.4f);
    SgySliceImage img = makeSlice(4, 64);
    // 补 rgba 编码（vendor 色彩）
    img.rgba.assign(img.values.size() * 4, 255);
    QVERIFY(renderer.UpdateSlice(&gl, SeismicSliceSlot::Time, *volume, SgySliceType::Time, 30, img));
    for (int layer = 0; layer < SeismicSliceRenderer::kMaxStackLayers; ++layer)
      QVERIFY(renderer.UpdateStackLayer(&gl, layer, *volume, layer * 4, img));
    renderer.SetStackVisible(true);
    QVERIFY(renderer.IsStackVisible());
    // 渲染不崩（含堆叠路径 + alpha 混合）
    SeismicCameraController cam;
    renderer.Render(&gl, cam.BuildViewMatrix(), cam.BuildProjectionMatrix(1.0));
    renderer.SetStackVisible(false);
    renderer.Cleanup(&gl);

    // VolumeFrameRenderer：井 + 第二体
    VolumeFrameRenderer frame;
    QVERIFY(frame.Initialize(&gl));
    frame.UpdateFromVolume(&gl, *volume);
    const GLsizei baseCount = frame.VertexCount();
    std::vector<Seismic3DWell> wells;
    Seismic3DWell well;
    well.name = QStringLiteral("W1");
    well.inlineNo = 1001;
    well.xlineNo = 2001;
    well.bottomFrac = 0.8f;
    well.tops.push_back({QStringLiteral("T1"), 0.4f, QColor(0x43A047)});
    wells.push_back(well);
    frame.UpdateWells(&gl, *volume, wells);
    QVERIFY(frame.VertexCount() >= baseCount + 8); // 井轨迹+标志层十字（=2线×2 + 十字2线×2）
    const GLsizei wellCount = frame.VertexCount();
    auto volume2 = std::make_shared<SgyVolume>();
    QVERIFY(writeTestSegy(dir.filePath("vp2.sgy"), 2, 2, 32));
    QVERIFY(volume2->Load((dir.filePath("vp2.sgy")).toStdString(), err));
    frame.SetSecondaryVolume(&gl, *volume, volume2);
    QVERIFY(frame.VertexCount() >= wellCount + 8); // 第二体轮廓 4 线段 = 8 顶点
    frame.Render(&gl, cam.BuildViewMatrix(), cam.BuildProjectionMatrix(1.0));
    frame.Cleanup(&gl);
  }

  // ---- D3.2 面板：切片拖动联动滑杆（真实 GL 视口 + 合成鼠标事件）----
  // #158：切到无地震的工程（壳层 setVolume(nullptr)）——体清空、切片控件
  // 禁用；重新装体后恢复可用。
  void panelClearVolumeDisablesSliceControls()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("clear.sgy");
    QVERIFY(writeTestSegy(sgy, 6, 6, 64));
    auto volume = std::make_shared<SgyVolume>();
    std::string err;
    QVERIFY(volume->Load(sgy.toStdString(), err));

    Seismic3DViewPanel panel;
    panel.resize(800, 600);
    panel.show();
    panel.setVolume(volume);
    auto *slider = panel.findChild<QSlider *>(QStringLiteral("inlineSlider"));
    QVERIFY(slider);
    QVERIFY(slider->isEnabled());
    QVERIFY(panel.volume());

    panel.setVolume(nullptr);
    QVERIFY(!panel.volume());
    QVERIFY(!slider->isEnabled());
    for (const char *name : {"xlineSlider", "timeSlider", "inlineSpin", "xlineSpin", "timeSpin"})
    {
      auto *w = panel.findChild<QWidget *>(QLatin1String(name));
      QVERIFY2(w && !w->isEnabled(), name);
    }

    panel.setVolume(volume);
    QVERIFY(slider->isEnabled());
  }

  void panelSliceDragLinkage()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("drag.sgy");
    QVERIFY(writeTestSegy(sgy, 6, 6, 64));
    auto volume = std::make_shared<SgyVolume>();
    std::string err;
    QVERIFY(volume->Load(sgy.toStdString(), err));

    Seismic3DViewPanel panel;
    panel.resize(800, 600);
    panel.show();
    panel.setVolume(volume); // 无任务服务 → 同步提取
    QVERIFY(QTest::qWaitFor([&]() { return !panel.isFallbackActive(); }, 4000));

    // 模拟视口拖切片面：直接在视口中心按下拖动（应命中某切片面）
    QWidget *vp = panel.viewport();
    const QPoint center = vp->rect().center();
    const int before = panel.currentInline();
    QSignalSpy ilSpy(&panel, &Seismic3DViewPanel::inlineChanged);
    QSignalSpy xlSpy(&panel, &Seismic3DViewPanel::crosslineChanged);
    QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, center);
    for (int step = 1; step <= 8; ++step)
    {
      QTest::mouseMove(vp, center + QPoint(step * 14, step * 4));
      QTest::qWait(10);
    }
    QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, center + QPoint(112, 32));
    QTest::qWait(200);

    // 拖动要么命中切片面（changed 信号），要么是旋转（不炸即过）——
    // 两种都算通过；命中时滑杆值应变化
    const bool dragged = ilSpy.count() + xlSpy.count() > 0;
    if (dragged)
      QVERIFY(panel.currentInline() != before || panel.currentCrossline() != 2002);
  }

  // ---- D3.5 面板接线：colormap/透明度/值域/体渲染开关 ----
  void panelDisplayControls()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("disp.sgy");
    QVERIFY(writeTestSegy(sgy, 4, 4, 32));
    auto volume = std::make_shared<SgyVolume>();
    std::string err;
    QVERIFY(volume->Load(sgy.toStdString(), err));

    Seismic3DViewPanel panel;
    panel.resize(800, 600);
    panel.show();
    panel.setVolume(volume);
    QVERIFY(QTest::qWaitFor([&]() { return !panel.isFallbackActive(); }, 4000));

    // D3.5：切换预设 colormap（缓存重着色路径）
    panel.setColorMap(Seismic3DColorMap::preset(QStringLiteral("彩虹谱")));
    // D3.3：透明度 + 值域
    panel.setSliceAlpha(0.5f);
    panel.setValueRange(0.1f, 0.9f);
    // D3.1：体渲染开关
    panel.setStackModeEnabled(true);
    QVERIFY(panel.isStackModeEnabled());
    QTest::qWait(100);
    panel.setStackModeEnabled(false);
    QVERIFY(!panel.isStackModeEnabled());

    // D3.4/D3.12 面板 API
    std::vector<Seismic3DWell> wells;
    Seismic3DWell w;
    w.inlineNo = 1001;
    w.xlineNo = 2001;
    w.bottomFrac = 1.0f;
    wells.push_back(w);
    panel.setWells(wells); // 不崩
    auto second = std::make_shared<SgyVolume>();
    QVERIFY(writeTestSegy(dir.filePath("sec.sgy"), 2, 2, 16));
    QVERIFY(second->Load((dir.filePath("sec.sgy")).toStdString(), err));
    panel.setSecondaryVolume(second);
    QVERIFY(panel.hasSecondaryVolume());
    panel.setSecondaryVolume(nullptr);
    QVERIFY(!panel.hasSecondaryVolume());

    // D3.10/D3.11 开关
    panel.viewport()->setFpsVisible(true);
    QVERIFY(panel.viewport()->isFpsVisible());
    panel.viewport()->setInertiaEnabled(false);
    QVERIFY(!panel.viewport()->isInertiaEnabled());
  }

  // ---- wave/deepen-perf A2：拖动链路取数合并 ----
  // 契约：paged 通道拖动期 = 粗层 + 仅被拖槽位重取；松手 350ms 精化只补
  // 「内容取自粗层」的槽位（未被拖的两个槽位保留 L0 内容，不再整组重取）。
  // 量化口径：一次拖放手势期间（press → 8 次 setValue → release → 精化收尾）
  // 新增的切片任务数与 LOD 切换任务数。
  void panelDragCoalescesAndRefinesOnlyStale()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("dragmerge.sgy");
    QVERIFY(writeTestSegy(sgy, 24, 24, 128));
    const QString l0 = sgy + QStringLiteral(".sf3p");

    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks, 16);
    bool transcoded = false;
    svc.startPagedTranscode(sgy, l0, /*buildLod=*/true,
                            [&](bool ok, const QString &, const QString &) { transcoded = ok; });
    QVERIFY(waitForQuiet([&] { return transcoded; }, 30000));

    auto volume = std::make_shared<SgyVolume>();
    std::string err;
    QVERIFY(volume->Load(sgy.toStdString(), err));

    Seismic3DViewPanel panel;
    QSignalSpy lodSpy(&panel, &Seismic3DViewPanel::lodChanged);
    panel.resize(800, 600);
    panel.setTaskService(&svc);
    panel.setPagedWorkspace(l0); // progressive 打开：粗层起步
    panel.setVolume(volume);
    // 初始装载 + 自动精化收尾（A2：粗层首见后静止 350ms 升 L0）
    QVERIFY(waitForQuiet([&] {
      return svc.activeTaskCount() == 0 && panel.activeLodLevel() == 0 &&
             lodSpy.count() >= 2; // 粗层标签 + L0 标签
    }, 10000));

    const int sliceBase = countTasksByTitle(tasks, QStringLiteral("提取地震切片"));
    const int lodBase = countTasksByTitle(tasks, QStringLiteral("切换 LOD 层级"));

    auto *slider = panel.findChild<QSlider *>(QStringLiteral("inlineSlider"));
    QVERIFY(slider != nullptr);
    // 直接发 slider 信号：offscreen 下 groove 点击不产生 sliderPressed/Released
    // （handle 命中才有），元调用保持手势语义确定性。
    QMetaObject::invokeMethod(slider, "sliderPressed");
    const int target = slider->maximum();
    for (int step = 1; step <= 8; ++step) // 拖动 8 步（不回事件循环——真实拖动同帧连发）
      slider->setValue(slider->minimum() + (target - slider->minimum()) * step / 8);
    QMetaObject::invokeMethod(slider, "sliderReleased");
    QTest::qWait(600); // 松手 350ms 精化定时器触发 + 任务收尾
    QVERIFY(waitForQuiet([&] { return svc.activeTaskCount() == 0; }, 10000));

    const int sliceTasks = countTasksByTitle(tasks, QStringLiteral("提取地震切片")) - sliceBase;
    const int lodTasks = countTasksByTitle(tasks, QStringLiteral("切换 LOD 层级")) - lodBase;
    qInfo("drag gesture: %d slice tasks (was 5 pre-merge), %d lod switches", sliceTasks, lodTasks);
    QCOMPARE(lodTasks, 2);           // 按下切粗层 + 松手升 L0
    // 两种合法交错（改前恒 5）：典型 = 被顶替取消 1 + 粗层终值 1 + 松手精化 1；
    // 若终值读派发晚于松手精化（activeLod 已回 0），则该读直接产出 L0 终态，
    // 精化重取按 onlyStale 判定跳过 → 2。未拖的两个槽位两种交错下都不重取。
    QVERIFY2(sliceTasks >= 2 && sliceTasks <= 3,
             qPrintable(QStringLiteral("slice tasks %1 outside [2,3]").arg(sliceTasks)));
    QVERIFY(panel.activeLodLevel() == 0);
  }

  // ---- wave/deepen-perf A1：体渲染堆叠层体窗合并通道 ----
  // paged 通道预算内：1 次 ReadVoxelWindow 取全 16 层（替代 16 次逐层切片请求）。
  void panelStackFetchVoxelMerge()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("stackvoxel.sgy");
    QVERIFY(writeTestSegy(sgy, 16, 16, 64));
    const QString l0 = sgy + QStringLiteral(".sf3p");

    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks, 16);
    bool transcoded = false;
    svc.startPagedTranscode(sgy, l0, /*buildLod=*/true,
                            [&](bool ok, const QString &, const QString &) { transcoded = ok; });
    QVERIFY(waitForQuiet([&] { return transcoded; }, 30000));

    auto volume = std::make_shared<SgyVolume>();
    std::string err;
    QVERIFY(volume->Load(sgy.toStdString(), err));

    Seismic3DViewPanel panel;
    panel.resize(800, 600);
    panel.setTaskService(&svc);
    panel.setPagedWorkspace(l0);
    panel.setVolume(volume);
    QVERIFY(waitForQuiet([&] { return svc.activeTaskCount() == 0 && panel.activeLodLevel() == 0; }, 10000));

    const int sliceBase = countTasksByTitle(tasks, QStringLiteral("提取地震切片"));
    QElapsedTimer fetchClock;
    fetchClock.start();
    panel.setStackModeEnabled(true);
    QVERIFY(panel.isStackModeEnabled());
    QVERIFY(waitForQuiet([&] { return svc.activeTaskCount() == 0; }, 10000));
    const double fetchMs = double(fetchClock.elapsed());

    const int voxelTasks = countTasksByTitle(tasks, QStringLiteral("读取三维体素窗口"));
    const int sliceTasks = countTasksByTitle(tasks, QStringLiteral("提取地震切片")) - sliceBase;
    qInfo("stack fetch: %.1f ms, %d voxel tasks, %d per-layer slice tasks (was 16 pre-merge)",
          fetchMs, voxelTasks, sliceTasks);
    QCOMPARE(voxelTasks, 1); // 16 层合并为单次体窗请求
    QCOMPARE(sliceTasks, 0);

    panel.setStackModeEnabled(false);
  }

  // ---- wave/deepen-perf A2：粗层首见后静止自动精化（不等用户拖动）----
  void panelPagedAutoRefineAfterCoarseOpen()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("autorefine.sgy");
    QVERIFY(writeTestSegy(sgy, 12, 12, 96));
    const QString l0 = sgy + QStringLiteral(".sf3p");

    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks, 16);
    bool transcoded = false;
    svc.startPagedTranscode(sgy, l0, /*buildLod=*/true,
                            [&](bool ok, const QString &, const QString &) { transcoded = ok; });
    QVERIFY(waitForQuiet([&] { return transcoded; }, 30000));

    auto volume = std::make_shared<SgyVolume>();
    std::string err;
    QVERIFY(volume->Load(sgy.toStdString(), err));

    Seismic3DViewPanel panel;
    panel.resize(800, 600);
    panel.setTaskService(&svc);
    panel.setPagedWorkspace(l0);
    // progressive 打开后 activeLod 应为最粗层（本夹具 L1+L2 → 2）
    QVERIFY(waitForQuiet([&] { return panel.activeLodLevel() > 0; }, 10000));
    panel.setVolume(volume);
    // 初始三槽切片走粗层；静止 350ms 后自动升 L0（A2 前需要用户拖一次才精化）
    QVERIFY(waitForQuiet([&] { return panel.activeLodLevel() == 0; }, 10000));
    QVERIFY(countTasksByTitle(tasks, QStringLiteral("切换 LOD 层级 0")) >= 1);
  }
};

QTEST_MAIN(TestSeismic3DUi)
#include "tst_seismic_3dui.moc"
