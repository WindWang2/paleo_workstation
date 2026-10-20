// 层：测试壳
// P5 Phase 2 剖面 2D 测试（D2.1–D2.14）：纹理缓存命中、显示三模、阈值/极性、
// AGC/增益曲线、双刻度、LOD 帧预算、纵向拉伸、8 档色标+反转、导出 PNG、
// 卷帘对比、道头服务、书签往返、复制/抓图、空数据原因态。
// goal/seismic-runtime-closure 增补：IL/XL/Time+卷帘取数经 SeismicTaskService
// 的链路与治理面——服务通道/LRU 命中、顶替取消（cancelled≠failed）、销毁/
// 切体/任意线迟到结果守卫。
#include <QtTest>
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QPainter>
#include <QtEndian>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QSettings>

#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <functional>

#include <QDeadlineTimer>
#include <QComboBox>
#include <QDialog>
#include <QSpinBox>
#include <QTableWidget>
#include <QThread>
#include <QToolButton>
#include <QUndoStack>

#include "../src/services/paleotaskservice.h"
#include "../src/services/seismictaskservice.h"
#include "../src/ui/seismicsection/seismicsectioncanvas.h"
#include "../src/ui/seismicsection/seismicsectiondockwidget.h"
#include "uipolish_capture.h"
#include "helpers/visualcapture.h"
#include "perfbudget_relax.h"

using namespace seismic;

namespace {

// 合成剖面：正弦同相轴（道间能量差给 AGC 测试用）。
// 行序按 vendor 契约生成：行 0 = 最深样（ExtractSlice 的 row = samples-1-s），
// 图案按物理样 s 计算后写入行 samples-1-s——画布入口会翻转到升序显示空间
// （契约见 seismicsectioncanvas_internal.h），displayValueAt(t, s) 仍读回
// 图案在 s 的值。
SgySliceImage makeSection(int traces, int samples, float ampScale = 1.0f,
                          float weakTraceFraction = 1.0f)
{
  SgySliceImage img;
  img.width = traces;
  img.height = samples;
  img.valueMin = -1.0f * ampScale;
  img.valueMax = 1.0f * ampScale;
  img.values.assign(std::size_t(traces) * samples, std::numeric_limits<float>::quiet_NaN());
  for (int t = 0; t < traces; ++t)
  {
    const float traceAmp = (t % 2 == 1) ? ampScale * weakTraceFraction : ampScale;
    for (int s = 0; s < samples; ++s)
      img.values[std::size_t(samples - 1 - s) * traces + t] =
          traceAmp * std::sin(s * 0.08f + t * 0.4f);
  }
  return img;
}

QImage renderCanvas(SeismicSectionCanvas &canvas)
{
  QImage img(canvas.size(), QImage::Format_ARGB32);
  img.fill(Qt::white);
  QPainter painter(&img);
  canvas.render(&painter);
  painter.end();
  return img;
}

// #421：闸闩填料——release 置位前持续占住并发槽，被测请求确定性排队
//（原 4×msleep(600) 全靠墙钟凑交错，重载机器可反转）；30s QDeadlineTimer
// 保险丝防挂死。调用方在「需要排队态」的断言之后 release 放行。
void saturateGate(SeismicTaskService &svc, std::atomic_bool &release)
{
  for (int i = 0; i < SeismicTaskService::kMaxConcurrentTasks; ++i)
    svc.startBounded(QStringLiteral("filler-%1").arg(i),
                     [&release](PaleoTask *) {
                       for (QDeadlineTimer t(30'000);
                            !release.load(std::memory_order_acquire) &&
                            !t.hasExpired();)
                         QThread::msleep(5);
                       return QString();
                     },
                     QString(), /*quiet=*/true);
}

qint64 imageDiff(const QImage &a, const QImage &b)
{
  if (a.size() != b.size() || a.format() != b.format())
    return -1;
  qint64 diff = 0;
  for (int y = 0; y < a.height(); ++y)
  {
    const auto *ra = reinterpret_cast<const QRgb *>(a.scanLine(y));
    const auto *rb = reinterpret_cast<const QRgb *>(b.scanLine(y));
    for (int x = 0; x < a.width(); ++x)
      diff += (ra[x] != rb[x]) ? 1 : 0;
  }
  return diff;
}

// #482：hover 脏区观测壳——重写 paintEvent 记录 Qt 实际投递的重绘区域
// （QPaintEvent::region() 的精确面积，而非外接矩形——十字带是「非矩形」
// 区域，外接矩形会退化成整幅视口而失去分辨力）。
class HoverBandSpy : public SeismicSectionCanvas
{
public:
  int paintCount = 0;
  qint64 paintArea = 0;
  void resetCounters()
  {
    paintCount = 0;
    paintArea = 0;
  }

protected:
  void paintEvent(QPaintEvent *event) override
  {
    ++paintCount;
    qint64 area = 0;
    for (const QRect &rc : event->region())
      area += static_cast<qint64>(rc.width()) * rc.height();
    paintArea += area;
    SeismicSectionCanvas::paintEvent(event);
  }
};

// 服务任务静默等待（同 tst_seismic_3dui 的口径）：事件循环轮转到
// 条件满足或超时。
bool waitForQuiet(const std::function<bool()> &done, int timeoutMs = 8000)
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

class TestSeismicSectionUi : public QObject
{
  Q_OBJECT

private:
  QTemporaryDir m_settingsDir; // initTestCase 前构造：settings 沙箱路径

private slots:
  void initTestCase()
  {
    // goal/ui-experience-polish：显式给了取证目录就尊重（captureEvidence 用），
    // 仅未设置时钉 "0" 防渲染循环型用例误落盘。
    const QString cap = qEnvironmentVariable("PALEO_UI_CAPTURE");
    if (cap.isEmpty())
      qputenv("PALEO_UI_CAPTURE", "0");
    // 书签/相机态走 QSettings() 默认构造：测试进程没设组织名，Windows
    // NativeFormat（注册表）在空组织名下的行为不可靠且污染宿主注册表——
    // 钉死为沙箱内 IniFormat，两平台同一路径语义。
    QCoreApplication::setOrganizationName(QStringLiteral("paleo-tests"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_seismic_sectionui"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       m_settingsDir.path());
  }

  // goal/ui-experience-polish：拾取面板键盘安全网——空选时 Delete 不弹框
  // 不崩溃；表格 ↓/↑ 行导航可达。
  void pickPanelKeyboardSafety()
  {
    SeismicSectionDockWidget dock;
    auto *pickTable = dock.findChild<QTableWidget *>();
    if (!pickTable)
      QSKIP("该构造路径未挂拾取面板");
    // goal/ui-experience-polish：空拾取不静默——指引标签可见。
    auto *pickHint = dock.findChild<QLabel *>(QStringLiteral("pickEmptyHint"));
    QVERIFY2(pickHint, "拾取面板空态指引应在位");
    dock.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dock));
    // pick 面板本体按解释模式显隐——空会话下指引不额外隐藏。
    QVERIFY(!pickHint->isHidden());
    pickTable->setFocus();
    const int rows = pickTable->model()->rowCount();
    QTest::keyClick(pickTable, Qt::Key_Down);
    QTest::keyClick(pickTable, Qt::Key_Up);
    QCOMPARE(pickTable->model()->rowCount(), rows); // 导航不改行数
  }

  // goal/ui-experience-polish：空体占位 + 拾取面板的修前/修后截图证据
  //（无体态：画布占位文案 + pick 面板空表——本轮补空态的对象）。
  void captureEvidence()
  {
    SeismicSectionDockWidget dock;
    uipolish::capturePanel(&dock, QStringLiteral("seismicsection_empty"),
                           QSize(900, 650));
  }

  void captureTokenSectionEvidence()
  {
    if (qEnvironmentVariable("PALEO_VISUAL_CAPTURE").isEmpty())
      QSKIP("Optional populated section visual evidence");
    SeismicSectionCanvas canvas;
    canvas.setSectionData(makeSection(64, 256), 2.0f);
    SectionWellInfo well;
    well.wellName = "合成井 A1";
    well.isWithinBuffer = true;
    well.tracePosition = 32;
    well.totalDepth = 250;
    well.calibrated = true;
    well.bottomTwtMs = 450;
    canvas.setWells({well});
    canvas.setShowWells(true);
    QVERIFY(paleo::tests::captureVisual(&canvas, "section-data", QSize(1100, 700)));
  }

  // ---- D2.2 显示三模：密度 / wiggle / 混合渲染互异 ----
  void displayModesRenderDistinctly()
  {
    SeismicSectionCanvas canvas;
    canvas.resize(800, 600);
    canvas.setSectionData(makeSection(64, 256), 2.0f);
    QVERIFY(canvas.hasData());

    const QImage density = renderCanvas(canvas);
    canvas.setDisplayMode(SectionDisplayMode::WiggleVA);
    const QImage wiggle = renderCanvas(canvas);
    canvas.setDisplayMode(SectionDisplayMode::Mixed);
    const QImage mixed = renderCanvas(canvas);

    QVERIFY(imageDiff(density, wiggle) > 1000);
    QVERIFY(imageDiff(density, mixed) > 1000);
    QVERIFY(imageDiff(wiggle, mixed) > 1000);
    QCOMPARE(canvas.displayMode(), SectionDisplayMode::Mixed);
  }

  // ---- #482：wiggle 纵向裁剪——窄纵窗只迭代可见样本段 ----
  // 同一窄视口下，「超长道 + 只露出中段」与「等可见段短道 + 全露」的
  // 渲染耗时必须同量级：旧实现 continue 只跳过不可见样本却仍逐样迭代
  // 全道（O(可见道 × 总样本)），此处按比例断言（容差 8×，实测修复后
  // ≈1×，未修复 ≈总样本/可见段 倍）。
  void wiggleVerticalClipNarrowWindow()
  {
    constexpr int kTraces = 4;
    constexpr int kVisibleSamples = 540;  // 600px 高 - 60px 上边距
    constexpr int kLongSamples = 1000000; // 总样本远大于可见段
    constexpr int kFrames = 3;

    // 基准：等可见段短道（zoomY=1 时全道可见）
    SeismicSectionCanvas shortCanvas;
    shortCanvas.resize(600, 340);
    shortCanvas.setSectionData(makeSection(kTraces, kVisibleSamples), 2.0f);
    shortCanvas.setDisplayMode(SectionDisplayMode::WiggleVA);
    shortCanvas.setZoom(1.0, 1.0);

    // 靶：同样视口只露出前 kVisibleSamples 样，其余 99.95% 在视口外
    SeismicSectionCanvas longCanvas;
    longCanvas.resize(600, 340);
    longCanvas.setSectionData(makeSection(kTraces, kLongSamples), 2.0f);
    longCanvas.setDisplayMode(SectionDisplayMode::WiggleVA);
    longCanvas.setZoom(1.0, 1.0);
    QCOMPARE(longCanvas.sampleCount(), kLongSamples);
    QVERIFY(longCanvas.zoomY() > 0.99);

    auto timeFrames = [](SeismicSectionCanvas &c) {
      (void)renderCanvas(c); // 预热（纹理/LOD 缓存）
      QElapsedTimer clock;
      clock.start();
      for (int i = 0; i < kFrames; ++i)
        (void)renderCanvas(c);
      return static_cast<double>(clock.elapsed()) / kFrames;
    };

    const double shortMs = timeFrames(shortCanvas);
    const double longMs = timeFrames(longCanvas);
    qInfo("wiggle vertical clip: short(visible-only) %.2f ms/frame, "
          "long(%d samples, narrow window) %.2f ms/frame, ratio %.1f",
          shortMs, kLongSamples, longMs, shortMs > 1e-6 ? longMs / shortMs : 0.0);
    QVERIFY2(longMs <= shortMs * 8.0 + 5.0,
             qPrintable(QStringLiteral("wiggle 纵向未裁剪：长道 %1 ms vs 可见段 %2 ms")
                            .arg(longMs, 0, 'f', 2).arg(shortMs, 0, 'f', 2)));
  }

  // ---- #482：hover 只重绘十字带脏区，不整幅 update ----
  void hoverRepaintsOnlyCrossBand()
  {
    HoverBandSpy canvas;
    canvas.resize(800, 600);
    canvas.setSectionData(makeSection(48, 256), 2.0f);
    canvas.setDisplayMode(SectionDisplayMode::WiggleVA);
    canvas.show();
    QVERIFY(QTest::qWaitForWindowExposed(&canvas));

    // 等首帧（整幅）落地后清零计数
    auto waitPaint = [&canvas] {
      for (QDeadlineTimer t(10'000); canvas.paintCount == 0 && !t.hasExpired();)
      {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
      }
      return canvas.paintCount > 0;
    };
    QVERIFY(waitPaint());
    canvas.resetCounters();

    // 同行移动：脏区应为「旧/新纵带 + 一条横带」（十字线外接区域是整幅
    // 视口，故按 QPaintEvent::region() 精确面积断言）。
    const QPoint a{int(canvas.traceToPixelX(6)), int(canvas.timeToPixelY(80))};
    const QPoint b{int(canvas.traceToPixelX(30)), int(canvas.timeToPixelY(80))};
    const QPoint c{int(canvas.traceToPixelX(38)), int(canvas.timeToPixelY(80))};
    QTest::mouseMove(&canvas, a);
    QVERIFY(waitPaint());
    canvas.resetCounters();
    QTest::mouseMove(&canvas, b);
    QVERIFY(waitPaint());

    const qint64 widgetArea = static_cast<qint64>(canvas.width()) * canvas.height();
    qInfo("hover dirty region: %lld px of %lld px (%d paints) after same-row move",
          static_cast<long long>(canvas.paintArea), static_cast<long long>(widgetArea),
          canvas.paintCount);
    QVERIFY(canvas.paintCount >= 1); // 十字线必须重绘（不能靠不重绘作弊）
    QVERIFY2(canvas.paintArea < widgetArea / 8,
             qPrintable(QStringLiteral("hover 整幅重绘：%1 px >= 视口 1/8")
                            .arg(static_cast<qlonglong>(canvas.paintArea))));

    // 正确性：十字线确实随鼠标移动（两次抓图不同），而非脏区收窄后
    // 残留旧十字线不动。
    const QImage atB = renderCanvas(canvas);
    QTest::mouseMove(&canvas, c);
    QVERIFY(waitPaint());
    const QImage atC = renderCanvas(canvas);
    QVERIFY(imageDiff(atB, atC) > 0);
  }

  // ---- D2.3 阈值 + 极性 ----
  void thresholdAndPolarity()
  {
    SeismicSectionCanvas canvas;
    canvas.resize(800, 600);
    canvas.setSectionData(makeSection(64, 256), 2.0f);

    // 极性反转：显示值取反
    const float v0 = canvas.displayValueAt(10, 100);
    canvas.setPolarityInverted(true);
    const float v1 = canvas.displayValueAt(10, 100);
    QVERIFY(v0 != 0.0f);
    QVERIFY(std::abs(v0 + v1) < 1e-5f);
    QVERIFY(canvas.polarityInverted());

    // 阈值：低于阈值的振幅在渲染中压白（图像变化）
    canvas.setPolarityInverted(false);
    const QImage before = renderCanvas(canvas);
    canvas.setAmplitudeThreshold(0.5f);
    const QImage after = renderCanvas(canvas);
    QVERIFY(imageDiff(before, after) > 100);
    QCOMPARE(canvas.amplitudeThreshold(), 0.5f);
  }

  // ---- D2.4 AGC + 手动增益曲线 ----
  void agcAndGainCurve()
  {
    SeismicSectionCanvas canvas;
    canvas.resize(800, 600);
    // 奇数道能量压到 1%（道间能量差是 AGC 的靶子）
    canvas.setSectionData(makeSection(16, 512, 1.0f, 0.01f), 2.0f);

    const float strongMid = canvas.displayValueAt(0, 256);
    const float weakMid = canvas.displayValueAt(1, 256);
    QVERIFY(std::abs(strongMid) > 0.5f);
    QVERIFY(std::abs(weakMid) < 0.05f);

    // AGC 后：弱道与强道中点幅值同量级（RMS 归一）
    canvas.setAgcEnabled(true, 200);
    const float strongAgc = canvas.displayValueAt(0, 256);
    const float weakAgc = canvas.displayValueAt(1, 256);
    QVERIFY(std::abs(weakAgc) > 0.3f);
    QVERIFY(std::abs(std::abs(weakAgc) - std::abs(strongAgc)) < 0.15f);
    QVERIFY(canvas.agcEnabled());
    QCOMPARE(canvas.agcWindowMs(), 200);

    // 增益曲线：0ms→1.0x、1000ms→4.0x → 750ms 处 ≈ 2.5x
    canvas.setAgcEnabled(false, 200);
    std::vector<SectionGainNode> nodes{{0.0, 1.0}, {1000.0, 4.0}};
    canvas.setGainCurve(nodes);
    const float g0 = canvas.displayValueAt(0, 256); // 512ms → 期望 1+0.512*3≈2.5x
    const float expected = strongMid * (1.0 + 0.512 * 3.0);
    QVERIFY(std::abs(g0 - expected) < std::abs(expected) * 0.02f + 1e-4f);
    QCOMPARE(canvas.gainCurve().size(), std::size_t(2));
  }

  // ---- #286：时间切片入口重建显示缓冲 ----
  // 开着 AGC/极性的剖面切到时间切片：显示值必须来自新切片（时间切片方向
  // AGC/增益曲线关闭，只保留极性）。旧实现沿用剖面缓冲，按新网格索引
  // 显示旧数据；新网格更大时越界读（ASan 必报）。
  void timeSliceEntryRebuildsDisplayValues()
  {
    SeismicSectionCanvas canvas;
    canvas.resize(800, 600);
    canvas.setSectionData(makeSection(100, 50), 2.0f);
    canvas.setAgcEnabled(true, 200);
    // 剖面显示缓冲已建立（AGC 后值与原始值不同）
    QVERIFY(std::abs(canvas.displayValueAt(0, 25) -
                     canvas.displayValueAt(0, 25)) < 1e30f); // 不 NaN、可读

    // 已知图案时间切片：100(XL) × 200(IL)，常量 0.5
    SgySliceImage slice;
    slice.width = 100;
    slice.height = 200;
    slice.valueMin = 0.5f;
    slice.valueMax = 0.5f;
    slice.values.assign(std::size_t(100) * 200, 0.5f);
    canvas.setTimeSliceData(slice, 500.0, 1000, 1005, 2000, 2099);

    // 显示值来自新切片（索引 199*100+50 远超旧剖面缓冲 100*50=5000 ——
    // 旧实现这里越界读旧 AGC 缓冲）
    QCOMPARE(canvas.displayValueAt(50, 199), 0.5f);
    QCOMPARE(canvas.displayValueAt(0, 0), 0.5f);

    // 渲染像素同样只认新切片：与「只喂同一张切片」的参照画布逐像素一致
    // （旧实现把 IL 剖面 AGC 缓冲按时间切片网格上色，两者必然不同）
    SeismicSectionCanvas ref;
    ref.resize(800, 600);
    ref.setTimeSliceData(slice, 500.0, 1000, 1005, 2000, 2099);
    QCOMPARE(imageDiff(renderCanvas(canvas), renderCanvas(ref)), 0);

    // 极性开着时切时间切片：显示值 = -新切片值（极性是唯一保留的处理）
    canvas.setPolarityInverted(true);
    SgySliceImage slice2;
    slice2.width = 100;
    slice2.height = 200;
    slice2.valueMin = 0.25f;
    slice2.valueMax = 0.25f;
    slice2.values.assign(std::size_t(100) * 200, 0.25f);
    canvas.setTimeSliceData(slice2, 500.0, 1000, 1005, 2000, 2099);
    QCOMPARE(canvas.displayValueAt(10, 150), -0.25f);
    canvas.setPolarityInverted(false);

    // 瓦片流路径：begin 建 NaN 底图（显示缓冲同步），append 逐瓦片改写
    canvas.setSectionData(makeSection(100, 50), 2.0f); // 回到剖面 + AGC 缓冲
    canvas.setAgcEnabled(true, 200);
    canvas.beginTimeSliceTiled(100, 200, 500.0, 1000, 1005, 2000, 2099);
    QVERIFY(std::isnan(canvas.displayValueAt(12, 22))); // 未到瓦片 = NaN
    SgySliceImage tile;
    tile.width = 8;
    tile.height = 6;
    tile.valueMin = 0.125f;
    tile.valueMax = 0.125f;
    tile.values.assign(std::size_t(8) * 6, 0.125f);
    canvas.appendTimeSliceTile(tile, 10, 20); // 覆盖 (10..17, 20..25)
    QCOMPARE(canvas.displayValueAt(12, 22), 0.125f);
    QVERIFY(std::isnan(canvas.displayValueAt(50, 100))); // 区域外仍 NaN

    // finish 整体替换同样重建显示缓冲
    SgySliceImage full;
    full.width = 100;
    full.height = 200;
    full.valueMin = -0.75f;
    full.valueMax = -0.75f;
    full.values.assign(std::size_t(100) * 200, -0.75f);
    canvas.finishTimeSliceTiled(full);
    QCOMPARE(canvas.displayValueAt(99, 199), -0.75f);
  }

  // ---- D2.5 双刻度 ----
  void dualScaleRender()
  {
    SeismicSectionCanvas canvas;
    canvas.resize(800, 600);
    canvas.setTimeDepthModel(TimeDepthModel(2500.0));
    canvas.setSectionData(makeSection(64, 256), 2.0f);

    const QImage off = renderCanvas(canvas);
    canvas.setDualScaleEnabled(true);
    const QImage on = renderCanvas(canvas);
    QVERIFY(imageDiff(off, on) > 20); // 右缘深度刻度出现
    QVERIFY(canvas.dualScaleEnabled());
  }

  // ---- D2.1 纹理缓存：同参数重复喂图即出 ----
  void textureCacheHitFast()
  {
    SeismicSectionCanvas canvas;
    canvas.resize(800, 600);
    const SgySliceImage img = makeSection(221, 1024);

    QElapsedTimer clock;
    clock.start();
    canvas.setSectionData(img, 2.0f);
    const qint64 firstMs = clock.elapsed();
    clock.restart();
    canvas.setSectionData(img, 2.0f); // 同内容同参数 → LRU 命中
    const qint64 secondMs = clock.elapsed();

    QVERIFY(firstMs >= 0);
    QVERIFY2(secondMs <= firstMs + 2,
             qPrintable(QStringLiteral("cache hit %1 ms not faster than build %2 ms")
                            .arg(secondMs).arg(firstMs)));
    QVERIFY2(secondMs < paleo::perfbudget::relaxedBudgetMs(25),
             qPrintable(QStringLiteral("cache hit too slow: %1 ms").arg(secondMs)));
  }

  // ---- D2.6 缩放平移帧预算：<16ms（LOD 抽稀路径）----
  void panZoomLodBudget()
  {
    SeismicSectionCanvas canvas;
    canvas.resize(1200, 800);
    canvas.setSectionData(makeSection(221, 1024), 2.0f);
    canvas.setZoom(0.12); // 最小缩放（多样本/像素，触发 LOD 抽稀）

    // 预热一遍（LOD 缓存构建）
    renderCanvas(canvas);
    QElapsedTimer clock;
    constexpr int kFrames = 20;
    clock.start();
    for (int i = 0; i < kFrames; ++i)
    {
      canvas.panBy(3, 1); // 平移不重建图像
      (void)renderCanvas(canvas);
    }
    const double perFrameMs = double(clock.elapsed()) / kFrames;
    qInfo("pan frame budget: %.2f ms/frame (LOD stride path)", perFrameMs);
    QVERIFY2(perFrameMs < paleo::perfbudget::relaxedBudgetMs(16),
             qPrintable(QStringLiteral("frame over 16ms budget: %1").arg(perFrameMs)));
  }

  // ---- D2.7 纵向拉伸系数 ----
  void verticalExaggeration()
  {
    SeismicSectionCanvas canvas;
    canvas.resize(800, 600);
    canvas.setSectionData(makeSection(64, 256), 2.0f);
    canvas.fitToWindow();
    const double baseY = canvas.zoomY();
    canvas.setVerticalExaggeration(3.0);
    QCOMPARE(canvas.verticalExaggeration(), 3.0);
    QVERIFY(canvas.zoomY() > baseY * 2.8);
    QVERIFY(canvas.zoomY() < baseY * 3.2);
  }

  // ---- D2.8 8 档色标 + 反转 ----
  void colormapsAndInvert()
  {
    SeismicSectionCanvas canvas;
    canvas.resize(700, 500);
    canvas.setSectionData(makeSection(64, 256), 2.0f);

    QImage prev;
    for (int cm = 0; cm <= int(SectionColorMapType::CyanWhiteOrange); ++cm)
    {
      canvas.setColorMap(static_cast<SectionColorMapType>(cm));
      const QImage img = renderCanvas(canvas);
      QVERIFY(!img.isNull());
      if (!prev.isNull())
        QVERIFY2(imageDiff(prev, img) > 100,
                 qPrintable(QStringLiteral("colormap %1 indistinct").arg(cm)));
      prev = img;
    }
    // 反转：同一 colormap 下图像变化
    const QImage before = renderCanvas(canvas);
    canvas.setColorMapInverted(true);
    const QImage after = renderCanvas(canvas);
    QVERIFY(imageDiff(before, after) > 100);
    QVERIFY(canvas.colorMapInverted());
    canvas.setColorMapInverted(false);
    QVERIFY(imageDiff(before, renderCanvas(canvas)) == 0);
  }

  // ---- D2.9 导出 PNG（含坐标轴）----
  void exportPng()
  {
    SeismicSectionCanvas canvas;
    canvas.resize(800, 600);
    canvas.setSectionData(makeSection(64, 256), 2.0f);

    QTemporaryDir dir;
    const QString path = dir.filePath("section.png");
    QVERIFY(canvas.exportPng(path, 2.0));
    QVERIFY(QFileInfo(path).size() > 10000); // 有内容（含轴/色标）
    const QImage reloaded(path);
    QVERIFY(!reloaded.isNull());
    QVERIFY(reloaded.width() == 1600 && reloaded.height() == 1200);
  }

  // ---- D2.10 卷帘对比 ----
  void curtainCompare()
  {
    SeismicSectionCanvas canvas;
    canvas.resize(800, 600);
    canvas.setSectionData(makeSection(64, 256), 2.0f);

    SgySliceImage other = makeSection(64, 256);
    for (float &v : other.values)
      if (std::isfinite(v))
        v = -v;
    canvas.setCompareData(other, QStringLiteral("相邻线"));

    canvas.setCompareEnabled(true);
    QVERIFY(canvas.compareEnabled());
    canvas.setCurtainPos(0.3);
    const QImage leftHeavy = renderCanvas(canvas);
    canvas.setCurtainPos(0.7);
    const QImage rightHeavy = renderCanvas(canvas);
    QVERIFY(imageDiff(leftHeavy, rightHeavy) > 500); // 分割位置改变画面
    QCOMPARE(canvas.compareLabel(), QStringLiteral("相邻线"));
  }

  // 卷帘 B 图随显示参数重烘：开启对比后改极性/增益，帘右 B 必须跟随
  // （旧实现只重烘 A——参数变了 B 保持旧配方渲染，两半对比口径漂移）。
  void curtainCompareFollowsDisplayParams()
  {
    SeismicSectionCanvas canvas;
    canvas.resize(800, 600);
    canvas.setSectionData(makeSection(64, 256), 2.0f);

    SgySliceImage other = makeSection(64, 256);
    for (float &v : other.values)
      if (std::isfinite(v))
        v = -v; // 与 A 反相：极性翻转后 B 视觉应趋近 A
    canvas.setCompareData(other, QStringLiteral("相邻线"));
    canvas.setCompareEnabled(true);
    canvas.setCurtainPos(0.5);

    const QImage before = renderCanvas(canvas);
    canvas.setPolarityInverted(true);
    const QImage flipped = renderCanvas(canvas);
    QVERIFY2(imageDiff(before, flipped) > 500,
             "极性翻转必须同时重烘 A/B（B 侧极性同配方）");
    canvas.setPolarityInverted(false);

    const QImage g0 = renderCanvas(canvas);
    canvas.setGain(4.0f);
    const QImage g4 = renderCanvas(canvas);
    QVERIFY2(imageDiff(g0, g4) > 500, "增益变化必须同时重烘 A/B");
  }

  // #482：卷帘 A/B 标签文案——A 侧恒为「当前」（死三元两分支同串，
  // 与 B 标签无关）；B 侧标签缺失回落「对比」而非渲染尾随空（"B · "）。
  // 文本不可直接断言，用「空标签渲染 == 显式回落文案渲染」钉住回落值，
  // 用「左标签区不随 B 标签变化」钉住 A 侧恒定语义。
  void curtainCompareLabelFallback()
  {
    SeismicSectionCanvas canvas;
    canvas.resize(800, 600);
    canvas.setSectionData(makeSection(48, 256), 2.0f);
    SgySliceImage other = makeSection(48, 256);
    for (float &v : other.values)
      if (std::isfinite(v))
        v = -v;
    canvas.setCompareData(other, QString());
    canvas.setCompareEnabled(true);
    canvas.setCurtainPos(0.5);

    // 空标签 → B 回落「对比」：与显式传「对比」逐像素一致
    const QImage emptyLabel = renderCanvas(canvas);
    canvas.setCompareData(other, QStringLiteral("对比"));
    const QImage explicitFallback = renderCanvas(canvas);
    QCOMPARE(imageDiff(emptyLabel, explicitFallback), 0);

    // A 侧恒定：换 B 标签只动右标签区，左标签区逐像素不变
    canvas.setCompareData(other, QStringLiteral("相邻线"));
    const QImage labelled = renderCanvas(canvas);
    const QRect leftLabel(72 + 4, 60 + 2, 120, 16);  // vp.left()+4, vp.top()+2
    const QRect rightLabel(800 - 64 - 160, 60 + 2, 156, 16); // vp.right()-160
    qint64 leftDiff = 0;
    qint64 rightDiff = 0;
    for (int y = 0; y < 16; ++y)
    {
      const auto *r0 = reinterpret_cast<const QRgb *>(emptyLabel.scanLine(leftLabel.top() + y));
      const auto *r1 = reinterpret_cast<const QRgb *>(labelled.scanLine(leftLabel.top() + y));
      const auto *s0 = reinterpret_cast<const QRgb *>(emptyLabel.scanLine(rightLabel.top() + y));
      const auto *s1 = reinterpret_cast<const QRgb *>(labelled.scanLine(rightLabel.top() + y));
      for (int x = 0; x < 120; ++x)
        leftDiff += (r0[leftLabel.left() + x] != r1[leftLabel.left() + x]) ? 1 : 0;
      for (int x = 0; x < 156; ++x)
        rightDiff += (s0[rightLabel.left() + x] != s1[rightLabel.left() + x]) ? 1 : 0;
    }
    QCOMPARE(leftDiff, 0);  // A 侧文案与 B 标签无关（恒「A · 当前」）
    QVERIFY(rightDiff > 0); // B 侧确实渲染了所给标签
  }

  // 跨线书签的视口恢复：换线触发异步重提取，到达回调 fitToWindow 会复位
  // 视口——书签视口必须重放（旧实现 apply 时提取未完成即被 fit 覆盖，
  // 跨线书签的缩放/平移从不恢复）。同线书签（无重提取）保持直接应用。
  void crossLineBookmarkRestoresViewport()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("bm2.sgy");
    QVERIFY(writeTestSegy(sgy, 6, 5, 64));
    SgyVolume volume;
    std::string err;
    QVERIFY(volume.Load(sgy.toStdString(), err));

    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks, 16);
    SeismicSectionDockWidget dock;
    dock.resize(900, 650);
    dock.setTaskService(&svc);
    QSignalSpy done(&dock, &SeismicSectionDockWidget::sectionExtractionFinished);
    dock.setVolume(std::make_shared<SgyVolume>(std::move(volume)));
    QVERIFY(waitForQuiet([&] { return !done.isEmpty(); })); // 初线（中线）
    const int initialLine = dock.canvas()->sectionRef().index;

    // 在当前线上造一个非默认视口的书签
    dock.canvas()->setZoom(2.5, 2.5);
    dock.canvas()->panBy(60, -40);
    const SectionViewState saved = dock.canvas()->viewState();
    dock.addBookmark(QStringLiteral("缩放态"));
    QCOMPARE(dock.bookmarks().size(), 1);

    // 换线（书签线 ≠ 新线）：spin 触发重提取，视口被 fit 复位
    QSpinBox *spin = dock.findChild<QSpinBox *>();
    QVERIFY(spin);
    const int otherLine = initialLine + 1;
    spin->setValue(otherLine);
    QVERIFY(waitForQuiet([&] { return done.size() >= 2; }));
    QVERIFY(std::abs(dock.canvas()->viewState().zoomX - saved.zoomX) > 0.5);

    // 应用跨线书签：setValue 触发重提取 → 完成后视口必须重放到书签值
    dock.applyBookmark(0);
    QVERIFY(waitForQuiet([&] { return done.size() >= 3; }));
    const SectionViewState restored = dock.canvas()->viewState();
    QVERIFY2(std::abs(restored.zoomX - saved.zoomX) < 1e-6,
             qPrintable(QStringLiteral("zoomX %1 != %2")
                        .arg(restored.zoomX).arg(saved.zoomX)));
    QVERIFY2(std::abs(restored.panY - saved.panY) < 1e-6,
             qPrintable(QStringLiteral("panY %1 != %2")
                        .arg(restored.panY).arg(saved.panY)));
  }

  // ---- D2.11 道头服务 ----
  void traceHeaderService()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("th.sgy");
    QVERIFY(writeTestSegy(sgy, 4, 4, 64));

    const SeismicTraceHeaderInfo info = SeismicTaskService::readTraceHeader(sgy, 3);
    QVERIFY2(info.ok, qPrintable(info.error));
    QCOMPARE(info.traceIndex, qint64(3));
    QCOMPARE(info.inlineNo, 1000);
    QCOMPARE(info.xlineNo, 2003);
    QCOMPARE(info.sampleCount, 64);
    QCOMPARE(info.sampleIntervalUs, 2000);
    QCOMPARE(info.fieldRecord, 4); // TRACL：0 基道 3 → 1 基序号 4
    QCOMPARE(info.fileOffset, qint64(3600 + 3 * (240 + 64 * 4)));

    // 越界道号 → 明确错误
    const SeismicTraceHeaderInfo bad = SeismicTaskService::readTraceHeader(sgy, 999);
    QVERIFY(!bad.ok);
    QVERIFY(!bad.error.isEmpty());
  }

  // 带扩展文本头的 SEG-Y：首道偏移 = 3600 + N·3200（rev0-1 由二进制头
  // 3505 扩展头数字段给出）。旧实现硬编码 3600——读到错误道的头
  // （inline/xline 全错）。与 vendor 读取通道（segyio trace0）同源断言。
  void traceHeaderServiceExtendedTextHeaders()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("th_ext.sgy");
    const int extHeaders = 2; // 3600 + 2*3200 = 10400 首道偏移
    QVERIFY(writeTestSegy(sgy, 4, 4, 64, extHeaders));

    const SeismicTraceHeaderInfo info = SeismicTaskService::readTraceHeader(sgy, 3);
    QVERIFY2(info.ok, qPrintable(info.error));
    QCOMPARE(info.fileOffset, qint64(3600 + extHeaders * 3200 + 3 * (240 + 64 * 4)));
    QCOMPARE(info.inlineNo, 1000);
    QCOMPARE(info.xlineNo, 2003);
    QCOMPARE(info.sampleCount, 64);
    QCOMPARE(info.sampleIntervalUs, 2000);
  }

  // ---- opt-02 井震剖面一致性（undo/道头/卷帘/叠加清理/方向闸）----

  // Q1：拾取 undo/redo 后画布叠加与会话同源刷新——旧实现 Add::undo /
  // Remove::undo/redo / Rename 只刷面板不刷画布：撤销后画布残留幽灵拾取
  // 点、被恢复的点不显示（面板/落盘正确，画布与真值脱钩）。
  void pickUndoRedoSyncsCanvasOverlay()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("undopick.sgy");
    QVERIFY(writeTestSegy(sgy, 6, 5, 64));
    SgyVolume volume;
    std::string err;
    QVERIFY(volume.Load(sgy.toStdString(), err));
    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks, 16);
    SeismicSectionDockWidget dock;
    dock.resize(900, 650);
    dock.setTaskService(&svc);
    QSignalSpy done(&dock, &SeismicSectionDockWidget::sectionExtractionFinished);
    dock.setVolume(std::make_shared<SgyVolume>(std::move(volume)));
    QVERIFY(waitForQuiet([&] { return !done.isEmpty(); }));

    dock.addPickFromCanvas(2, 64.0);
    QCOMPARE(dock.interpretationSession().picks.size(), 1);
    QCOMPARE(dock.canvas()->pickOverlays().size(), 1);
    const int id = dock.interpretationSession().picks.first().id;

    auto *undo = dock.findChild<QUndoStack *>();
    QVERIFY(undo);
    dock.removePick(id);
    QCOMPARE(dock.interpretationSession().picks.size(), 0);
    QCOMPARE(dock.canvas()->pickOverlays().size(), 0);
    // 撤销删除 → 会话恢复且画布叠加恢复（旧实现画布不回来，先红）。
    undo->undo();
    QCOMPARE(dock.interpretationSession().picks.size(), 1);
    QCOMPARE(dock.canvas()->pickOverlays().size(), 1);
    // 撤销添加 → 幽灵点必须消失（旧实现残留，先红）。
    undo->undo();
    QCOMPARE(dock.interpretationSession().picks.size(), 0);
    QCOMPARE(dock.canvas()->pickOverlays().size(), 0);
    // 重做添加 → 回来。
    undo->redo();
    QCOMPARE(dock.interpretationSession().picks.size(), 1);
    QCOMPARE(dock.canvas()->pickOverlays().size(), 1);
  }

  // L3.12：断层折线草稿的收笔/清理对任意释放键生效——旧实现只写在左/中键
  // 分支里，左键起草后以其它键释放时草稿既不清除也不收笔（红色虚线残留
  // 并继续随鼠标生长，直到下一次左键按下）。
  void faultDraftClearedOnNonDrawingButtonRelease()
  {
    SeismicSectionCanvas canvas;
    canvas.resize(800, 600);
    canvas.setSectionData(makeSection(64, 256), 2.0f);
    canvas.setPickMode(SectionPickMode::Fault);
    canvas.show();
    QVERIFY(QTest::qWaitForWindowExposed(&canvas));

    QSignalSpy drawn(&canvas, &SeismicSectionCanvas::faultDrawn);
    const QPoint a{int(canvas.traceToPixelX(8)), int(canvas.timeToPixelY(40))};
    const QPoint b{int(canvas.traceToPixelX(30)), int(canvas.timeToPixelY(90))};
    // 左键起草 → 移动生长 → 左键释放收笔（≥2 点才发 faultDrawn）。
    QTest::mousePress(&canvas, Qt::LeftButton, Qt::NoModifier, a);
    QCOMPARE(canvas.faultDraftPointCount(), 1);
    QTest::mouseMove(&canvas, b);
    QCOMPARE(canvas.faultDraftPointCount(), 2);
    QTest::mouseRelease(&canvas, Qt::LeftButton, Qt::NoModifier, b);
    QCOMPARE(drawn.size(), 1);
    QCOMPARE(canvas.faultDraftPointCount(), 0);

    // 左键起草后以右键释放：不收笔、草稿清除，且后续移动不生长。
    QTest::mousePress(&canvas, Qt::LeftButton, Qt::NoModifier, a);
    QCOMPARE(canvas.faultDraftPointCount(), 1);
    QTest::mouseMove(&canvas, b);
    QCOMPARE(canvas.faultDraftPointCount(), 2);
    QTest::mouseRelease(&canvas, Qt::RightButton, Qt::NoModifier, b);
    QCOMPARE(drawn.size(), 1);
    QCOMPARE(canvas.faultDraftPointCount(), 0);
    QTest::mouseMove(&canvas, QPoint(b.x() + 5, b.y() + 5));
    QCOMPARE(canvas.faultDraftPointCount(), 0);
    // 收尾：左键按下后以右键释放会在 QTest 的静态按键表里留下幻影左键
    // （qtestMouseButtons 只清释放事件声明的那个键）——后续用例的 mouseMove
    // 会带着一个并不存在的按下键，事件投递失效。补一次左键释放清表。
    QTest::mouseRelease(&canvas, Qt::LeftButton, Qt::NoModifier, QPoint(b.x() + 5, b.y() + 5));
  }

  // Q2：道头卡按「剖面列 → 实际线号 → 文件道号」换算——旧实现把列号直
  // 当文件道序，非首条 inline / XL 剖面全读错道；时间切片如实给原因。
  void traceHeaderCardResolvesColumnToPhysicalTrace()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("thcard.sgy");
    QVERIFY(writeTestSegy(sgy, 6, 5, 64));
    SgyVolume volume;
    std::string err;
    QVERIFY(volume.Load(sgy.toStdString(), err));
    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks, 16);
    SeismicSectionDockWidget dock;
    dock.resize(900, 650);
    dock.setTaskService(&svc);
    QSignalSpy done(&dock, &SeismicSectionDockWidget::sectionExtractionFinished);
    dock.setVolume(std::make_shared<SgyVolume>(std::move(volume)));
    QVERIFY(waitForQuiet([&] { return !done.isEmpty(); }));
    const int cur = dock.canvas()->sectionRef().index; // 中线（> 1000）
    QVERIFY(cur > 1000);

    // 点第 2 列（xline 2002）→ 卡片 INLINE = 当前线、CROSSLINE = 2002
    //（旧实现：直读文件道 2 → INLINE 恒 1000，先红）。
    emit dock.canvas()->traceClicked(2, 64.0, 80.0, 0.5f, 0.0, 0.0);
    auto *card = dock.findChild<QDialog *>();
    QVERIFY(card);
    auto *table = card->findChild<QTableWidget *>();
    QVERIFY(table);
    QCOMPARE(table->item(2, 1)->text(), QString::number(cur));
    QCOMPARE(table->item(3, 1)->text(), QStringLiteral("2002"));
    card->close();

    // 切时间切片 → 无单一物理道：卡片给原因行，不展示错道数据。
    auto *mode = dock.findChild<QComboBox *>("cboSectionMode");
    QVERIFY(mode);
    mode->setCurrentIndex(2);
    QVERIFY(waitForQuiet([&] { return done.size() >= 2; }));
    emit dock.canvas()->traceClicked(2, 64.0, 80.0, 0.5f, 0.0, 0.0);
    auto *card2 = dock.findChild<QDialog *>();
    QVERIFY(card2);
    auto *table2 = card2->findChild<QTableWidget *>();
    QVERIFY(table2);
    QCOMPARE(table2->rowCount(), 1); // 只有「错误」原因行
    QVERIFY(table2->item(0, 1)->text().contains(
        QStringLiteral("无单一物理道")));
  }

  // Q3：卷帘只对 IL/XL 相邻线有意义——切时间切片显式关帘；旧实现只跳过
  // 刷新不关帘，B 半幅残留上一条相邻线图像被拉伸进新几何。
  void curtainDisabledOnTimeSliceMode()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("curtainmode.sgy");
    QVERIFY(writeTestSegy(sgy, 6, 5, 64));
    SgyVolume volume;
    std::string err;
    QVERIFY(volume.Load(sgy.toStdString(), err));
    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks, 16);
    SeismicSectionDockWidget dock;
    dock.resize(900, 650);
    dock.setTaskService(&svc);
    QSignalSpy done(&dock, &SeismicSectionDockWidget::sectionExtractionFinished);
    dock.setVolume(std::make_shared<SgyVolume>(std::move(volume)));
    QVERIFY(waitForQuiet([&] { return !done.isEmpty(); }));

    auto *curtain = dock.findChild<QToolButton *>("btnSectionCurtain");
    QVERIFY(curtain);
    curtain->setChecked(true);
    QVERIFY(dock.canvas()->compareEnabled());
    // 相邻线 B 走切片服务通道，不发 sectionExtractionFinished——按
    // compareLabel 到位等待。
    QVERIFY(waitForQuiet(
        [&] { return !dock.canvas()->compareLabel().isEmpty(); }));

    auto *mode = dock.findChild<QComboBox *>("cboSectionMode");
    QVERIFY(mode);
    mode->setCurrentIndex(2); // 时间切片（B 图不发完成信号：done 只 +1）
    QVERIFY(waitForQuiet([&] { return done.size() >= 2; }));
    QVERIFY(!curtain->isChecked());
    QVERIFY(!dock.canvas()->compareEnabled());
  }

  // Q4/Q5：换线清层段叠加、任意线清属性+层段叠加——#224 只修了切片路径
  // 的 attr 清理；zone 在两路径都缺、attr 在任意线路径缺（同体同尺寸时
  // 画布尺寸防线挡不住旧线叠加残留）。
  void overlaysClearedOnLineSwitchAndArbitraryLine()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("ovl.sgy");
    QVERIFY(writeTestSegy(sgy, 6, 5, 64));
    auto volume = std::make_shared<SgyVolume>();
    std::string err;
    QVERIFY(volume->Load(sgy.toStdString(), err));
    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks, 16);
    SeismicSectionDockWidget dock;
    dock.resize(900, 650);
    dock.setTaskService(&svc);
    QSignalSpy done(&dock, &SeismicSectionDockWidget::sectionExtractionFinished);
    dock.setVolume(volume);
    QVERIFY(waitForQuiet([&] { return !done.isEmpty(); }));
    const int cur = dock.canvas()->sectionRef().index;

    // 换线（同尺寸相邻线）→ zone 叠加必须清（旧实现残留，先红）。
    dock.canvas()->setZoneOverlay(makeSection(5, 64));
    QVERIFY(dock.canvas()->hasZoneOverlay());
    auto *spin = dock.findChild<QSpinBox *>("spinSectionSlice");
    QVERIFY(spin);
    spin->setValue(cur + 1);
    QVERIFY(waitForQuiet([&] { return done.size() >= 2; }));
    QVERIFY(!dock.canvas()->hasZoneOverlay());

    // 任意线（不经 onSectionModeChanged）：attr + zone 都要清。
    dock.canvas()->setAttrOverlay(makeSection(5, 64));
    dock.canvas()->setZoneOverlay(makeSection(5, 64));
    QVERIFY(dock.canvas()->hasAttrOverlay() && dock.canvas()->hasZoneOverlay());
    dock.extractSectionFromVolumeAsync(volume, {{1000, 2000}, {1003, 2000}},
                                       "任意线", {{0, 0}, {300, 0}});
    QVERIFY(waitForQuiet([&] { return done.size() >= 3; }));
    QVERIFY(!dock.canvas()->hasAttrOverlay());
    QVERIFY(!dock.canvas()->hasZoneOverlay());
  }

  // Q6：时间切片上点「追踪」给方向闸——旧实现用 (Time, 采样号) 伪身份在
  // 陈旧垂直剖面上追踪，线号域与采样号域重叠时（真实工区常态）产出垃圾
  // 拾取并入会话自动落盘。夹具线号取 1..4 与采样号域重叠以暴露旧路径。
  void trackingGatedOnTimeSlice()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("trackgate.sgy");
    // 自写 4×4×64 体（inline 1..4 × xline 1..4）——writeTestSegy 的
    // 1000/2000 线号基与采样号域不相交，暴露不了伪身份分支。
    {
      QFile file(sgy);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write(QByteArray(3200, ' '));
      QByteArray binary(400, 0);
      const auto put16 = [](QByteArray &data, int offset, qint16 value) {
        qToBigEndian(value, reinterpret_cast<uchar *>(data.data()) + offset);
      };
      const auto put32 = [](QByteArray &data, int offset, qint32 value) {
        qToBigEndian(value, reinterpret_cast<uchar *>(data.data()) + offset);
      };
      put16(binary, 12, 4);
      put16(binary, 16, 2000);
      put16(binary, 20, 64);
      put16(binary, 24, 4);
      file.write(binary);
      for (int il = 0; il < 4; ++il)
        for (int xl = 0; xl < 4; ++xl)
        {
          QByteArray header(240, 0);
          put32(header, 188, 1 + il);
          put32(header, 192, 1 + xl);
          put16(header, 114, 64);
          file.write(header);
          QByteArray samples(64 * 4, 0);
          for (int k = 0; k < 64; ++k)
          {
            const float v = std::sin(k * 0.2f + il + xl);
            quint32 bits;
            std::memcpy(&bits, &v, 4);
            qToBigEndian(bits,
                         reinterpret_cast<uchar *>(samples.data()) + 4 * k);
          }
          file.write(samples);
        }
      file.close();
    }
    SgyVolume volume;
    std::string err;
    QVERIFY(volume.Load(sgy.toStdString(), err));
    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks, 16);
    SeismicSectionDockWidget dock;
    dock.resize(900, 650);
    dock.setTaskService(&svc);
    QSignalSpy done(&dock, &SeismicSectionDockWidget::sectionExtractionFinished);
    dock.setVolume(std::make_shared<SgyVolume>(std::move(volume)));
    QVERIFY(waitForQuiet([&] { return !done.isEmpty(); }));

    // inline 2 剖面上放种子（列 1 → xline 2；twt 64ms → 样 32）。
    dock.addPickFromCanvas(1, 64.0);
    const int before = dock.interpretationSession().picks.size();
    QCOMPARE(before, 1);

    // 切时间切片并把采样位滑到 2（与种子 xlineNo 同值 → 旧实现伪身份
    // 命中：onSection 按 p.xlineNo == 采样号筛选）。
    auto *mode = dock.findChild<QComboBox *>("cboSectionMode");
    QVERIFY(mode);
    mode->setCurrentIndex(2);
    QVERIFY(waitForQuiet([&] { return done.size() >= 2; }));
    auto *spin = dock.findChild<QSpinBox *>("spinSectionSlice");
    QVERIFY(spin);
    spin->setValue(2);
    QVERIFY(waitForQuiet([&] { return done.size() >= 3; }));

    // 方向闸：不开任务、会话不被伪身份追踪结果换血（旧实现立即开任务
    // → trackingActive 为真，先红）。
    dock.runTracking();
    QVERIFY(!dock.trackingActive());
    QCOMPARE(dock.interpretationSession().picks.size(), before);
    QVERIFY(dock.pickPanel());
  }

  // ---- D2.12 书签往返 ----
  void bookmarksRoundtrip()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("bm.sgy");
    QVERIFY(writeTestSegy(sgy, 6, 5, 64));

    SgyVolume volume;
    std::string err;
    QVERIFY(volume.Load(sgy.toStdString(), err));

    SeismicSectionDockWidget dock;
    dock.resize(900, 650);
    dock.setVolume(std::make_shared<SgyVolume>(std::move(volume)));

    dock.canvas()->zoomIn();
    dock.addBookmark(QStringLiteral("测试书签"));
    QCOMPARE(dock.bookmarks().size(), 1);
    QCOMPARE(dock.bookmarks().first().name, QStringLiteral("测试书签"));
    QCOMPARE(dock.bookmarks().first().sliceValue, dock.canvas() ? 1002 : 0);

    // 第二个实例从 QSettings 恢复（同体身份）
    {
      // 诊断：实存书签键（若仍失败，CI 日志直接给出注册/INI 内容）
      QSettings probe;
      const QStringList keys = probe.allKeys();
      for (const QString &k : keys)
        if (k.contains(QStringLiteral("sectionBookmarks")))
          qWarning("bookmark probe: %s", qPrintable(QStringLiteral("%1 = %2")
                         .arg(k, probe.value(k).toString().left(60))));
      SgyVolume volume2;
      QVERIFY(volume2.Load(sgy.toStdString(), err));
      SeismicSectionDockWidget dock2;
      dock2.setVolume(std::make_shared<SgyVolume>(std::move(volume2)));
      QCOMPARE(dock2.bookmarks().size(), 1);
      QCOMPARE(dock2.bookmarks().first().name, QStringLiteral("测试书签"));
      // 应用书签 → 视口恢复
      dock2.canvas()->zoomIn();
      dock2.applyBookmark(0);
      QCOMPARE(dock2.bookmarks().first().view.zoomX, dock2.canvas()->zoomX());
      // 删除
      dock2.removeBookmark(0);
      QCOMPARE(dock2.bookmarks().size(), 0);
    }
  }

  // ---- D2.13 抓图/复制 ----
  void grabAndCopy()
  {
    SeismicSectionCanvas canvas;
    canvas.resize(800, 600);
    canvas.setSectionData(makeSection(64, 256), 2.0f);
    const QImage img = canvas.grabCanvasImage(2.0);
    QVERIFY(!img.isNull());
    QCOMPARE(img.width(), 1600);

    QApplication::clipboard()->setImage(img);
    QVERIFY(!QApplication::clipboard()->image().isNull());
  }

  // ---- D2.14 空数据原因态 ----
  void noDataReason()
  {
    SeismicSectionCanvas canvas;
    canvas.resize(600, 400);
    QVERIFY(!canvas.hasData());
    QVERIFY(canvas.noDataReason().isEmpty());

    canvas.setNoDataReason(QStringLiteral("线号 1234 不在测网 [1000..1050]"));
    QCOMPARE(canvas.noDataReason(), QStringLiteral("线号 1234 不在测网 [1000..1050]"));
    const QImage img = renderCanvas(canvas); // 带原因渲染不崩
    QVERIFY(!img.isNull());

    // 数据到达即清原因
    canvas.setSectionData(makeSection(16, 64), 2.0f);
    QVERIFY(canvas.hasData());
    QVERIFY(canvas.noDataReason().isEmpty());
  }

  // ---- goal/seismic-runtime-closure：IL/XL/Time 取数经 SeismicTaskService ----

  // 基础链路：切片经服务出结果、进切片 LRU、任务收敛归零。
  void sliceExtractionThroughService()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("svc.sgy");
    QVERIFY(writeTestSegy(sgy, 6, 5, 64));
    SgyVolume volume;
    std::string err;
    QVERIFY(volume.Load(sgy.toStdString(), err));

    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks, 16);
    SeismicSectionDockWidget dock;
    dock.setTaskService(&svc);
    QSignalSpy done(&dock, &SeismicSectionDockWidget::sectionExtractionFinished);
    dock.setVolume(std::make_shared<SgyVolume>(std::move(volume)));

    QVERIFY(waitForQuiet([&] { return !done.isEmpty(); }));
    QVERIFY(done.last().at(0).toBool());
    QVERIFY(dock.canvas()->hasData());
    // dock 切片路径经服务：结果落切片 LRU（裸 QThreadPool 时代不进缓存）
    QVERIFY(svc.dataCache().EntryCount() >= 1);
    QVERIFY(waitForQuiet([&] { return svc.activeTaskCount() == 0; }));
  }

  // ---- 显示真值对拍（vendor 真切片 → dock → canvas 行序/TWT 对应）----
  // vendor ExtractSlice 契约：行 0 = 最深样（row = samples-1-s）。画布的
  // 标尺/hover/拾取全链是「行 y = 采样 y 升序」——同一物理样点的幅值必须
  // 出现在画布采样序 k（displayValueAt）、hover 读数的 TWT 必须是该样的
  // 物理时间 t0+k·dt。旧实现把 vendor 行序直当升序画：displayValueAt(t,k)
  // 读到镜像样 63-k，hover 报的 TWT 是镜像时间——本用例先红后绿钉死契约。
  // writeTestSegy 样值公式：(i+1)*100 + j + k*0.25（i=inline, j=xline, k=样）。
  void sectionRowOrderMatchesPhysicalTime()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("truth.sgy");
    QVERIFY(writeTestSegy(sgy, 6, 5, 64));
    SgyVolume volume;
    std::string err;
    QVERIFY(volume.Load(sgy.toStdString(), err));

    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks, 16);
    SeismicSectionDockWidget dock;
    dock.resize(900, 650);
    dock.show();
    dock.setTaskService(&svc);
    QSignalSpy done(&dock, &SeismicSectionDockWidget::sectionExtractionFinished);
    dock.setVolume(std::make_shared<SgyVolume>(std::move(volume)));
    QVERIFY(waitForQuiet([&] { return !done.isEmpty() && done.last().at(0).toBool(); }));

    SeismicSectionCanvas *canvas = dock.canvas();
    QVERIFY(canvas->hasData());
    QCOMPARE(canvas->traceCount(), 5);    // IL 剖面宽 = XL 数
    QCOMPARE(canvas->sampleCount(), 64);
    const SectionRef ref = canvas->sectionRef();
    QVERIFY(ref.valid);
    QCOMPARE(ref.type, SgySliceType::Inline);
    const int i = ref.index - 1000; // writeTestSegy inline 基 1000
    QVERIFY(i >= 0 && i < 6);

    // 真值 1：displayValueAt(道 j, 采样 k) = 样值公式在 (i, j, k)。
    const float want5 = float((i + 1) * 100 + 2) + 5 * 0.25f;
    const float want58 = float((i + 1) * 100 + 2) + 58 * 0.25f;
    QCOMPARE(canvas->displayValueAt(2, 5), want5);
    QCOMPARE(canvas->displayValueAt(2, 58), want58);

    // 真值 2：hover 读数自洽——采样 k 的像素上报 TWT=t0+k·dt 且幅值=样值。
    // t0=0（writeTestSegy 无记录延迟）、dt=2ms。
    const double py = canvas->timeToPixelY(5 * 2.0);
    const double px = canvas->traceToPixelX(2.4);
    const QPoint pos{int(px), int(py)};
    // 自检：像素往返确实落在采样 5（防缩放取整把用例自身搞错）
    QCOMPARE(int(std::round((canvas->pixelToTime(pos.y()) - 0.0) / 2.0)), 5);
    QSignalSpy hover(canvas, &SeismicSectionCanvas::traceHovered);
    QTest::mouseMove(canvas, pos);
    QVERIFY(waitForQuiet([&] { return !hover.isEmpty(); }));
    const QList<QVariant> h = hover.last();
    QCOMPARE(h.at(0).toInt(), 2); // 道取整
    // 像素取整回程不保证精确 10.0ms——按采样序断言（±半采样内即样 5）
    QCOMPARE(int(std::round(h.at(1).toDouble() / 2.0)), 5);
    QCOMPARE(h.at(3).toFloat(), want5);
  }

  // 顶替语义：快速换线时在途任务被协作取消（cancelled ≠ failed，
  // 无误导失败信号），仅最新线出结果；高频请求不无上限排队。
  void sliceSupersedeCancelsInFlight()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("sup.sgy");
    QVERIFY(writeTestSegy(sgy, 8, 5, 64));
    SgyVolume volume;
    std::string err;
    QVERIFY(volume.Load(sgy.toStdString(), err));

    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks, 16);
    // 饱和并发闸：闸闩填料占满 4 槽，切片任务确定性地排队
    //（入池前取消 = 直接判 Cancelled 不执行，见 startBounded）。
    std::atomic_bool gateRelease{false};
    saturateGate(svc, gateRelease);
    SeismicSectionDockWidget dock;
    dock.setTaskService(&svc);
    QSignalSpy done(&dock, &SeismicSectionDockWidget::sectionExtractionFinished);
    dock.setVolume(std::make_shared<SgyVolume>(std::move(volume)));
    QCOMPARE(svc.activeTaskCount(), SeismicTaskService::kMaxConcurrentTasks + 1);

    auto *spin = dock.findChild<QSpinBox *>(QStringLiteral("spinSectionSlice"));
    QVERIFY(spin);
    const int target = spin->value() + 2; // 换线：顶替排队的首请求
    spin->setValue(target);
    gateRelease.store(true, std::memory_order_release); // 放闸：排队态已钉住

    QVERIFY(waitForQuiet([&] { return svc.activeTaskCount() == 0; }, 15000));
    int cancelled = 0, succeeded = 0;
    for (const PaleoTask *t : tasks.tasks()) {
      if (!t->title().contains(QStringLiteral("提取地震切片")))
        continue;
      if (t->state() == PaleoTask::State::Cancelled)
        ++cancelled;
      if (t->state() == PaleoTask::State::Succeeded)
        ++succeeded;
    }
    QVERIFY2(cancelled >= 1, "superseded slice task must end Cancelled, not fake-success");
    QCOMPARE(succeeded, 1); // 只有最新线出结果
    for (const auto &args : done)
      if (!args.at(0).toBool())
        QFAIL("superseded/cancelled slice surfaced as failure signal");
    QVERIFY(dock.canvas()->hasData());
    QCOMPARE(dock.canvas()->traceCount(), 5);
  }

  // 生命周期：切片在途时销毁 dock——回调守卫丢弃，无 UAF、无迟到的
  // 画面污染，服务侧任务自然收敛。
  void dockDestroyedMidExtractionSurvives()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("dly.sgy");
    QVERIFY(writeTestSegy(sgy, 6, 5, 64));
    SgyVolume volume;
    std::string err;
    QVERIFY(volume.Load(sgy.toStdString(), err));

    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks, 16);
    std::atomic_bool gateRelease{false};
    saturateGate(svc, gateRelease);
    auto *dock = new SeismicSectionDockWidget;
    dock->setTaskService(&svc);
    dock->setVolume(std::make_shared<SgyVolume>(std::move(volume)));
    QVERIFY(svc.activeTaskCount() > SeismicTaskService::kMaxConcurrentTasks);
    delete dock; // 在途即销毁（BASE 的裸 QThreadPool 路径此处在池线程
                 // invokeMethod 悬空 this 上有 UAF 窗口）
    gateRelease.store(true, std::memory_order_release); // 放闸排空
    QVERIFY(waitForQuiet([&] { return svc.activeTaskCount() == 0; }, 15000));
  }

  // 切体顶替：切片在途时换体——旧体结果被世代号丢弃，画布落新体数据。
  void volumeSwitchMidSliceDropsStaleResult()
  {
    QTemporaryDir dir;
    const QString sgy1 = dir.filePath("v1.sgy");
    const QString sgy2 = dir.filePath("v2.sgy");
    QVERIFY(writeTestSegy(sgy1, 6, 5, 64)); // IL 剖面宽 = XL 数 = 5
    QVERIFY(writeTestSegy(sgy2, 6, 4, 64)); // 宽 = 4：可判数据归属
    SgyVolume volume1, volume2;
    std::string err;
    QVERIFY(volume1.Load(sgy1.toStdString(), err));
    QVERIFY(volume2.Load(sgy2.toStdString(), err));

    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks, 16);
    std::atomic_bool gateRelease{false};
    saturateGate(svc, gateRelease);
    SeismicSectionDockWidget dock;
    dock.setTaskService(&svc);
    QSignalSpy done(&dock, &SeismicSectionDockWidget::sectionExtractionFinished);
    dock.setVolume(std::make_shared<SgyVolume>(std::move(volume1))); // T1 排队
    dock.setVolume(std::make_shared<SgyVolume>(std::move(volume2))); // T1 取消，T2 排队
    gateRelease.store(true, std::memory_order_release); // 放闸：T2 执行
    QVERIFY(waitForQuiet([&] { return svc.activeTaskCount() == 0; }, 15000));

    QVERIFY(dock.canvas()->hasData());
    QCOMPARE(dock.canvas()->traceCount(), 4); // 新体的 XL 数，不是旧体的 5
    QVERIFY(done.last().at(0).toBool());
  }

  // 切片 LRU：看过邻线再回头，重复线缓存命中（hits 计数增长）。
  void sliceRevisitHitsServiceCache()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("lru.sgy");
    QVERIFY(writeTestSegy(sgy, 8, 5, 64));
    SgyVolume volume;
    std::string err;
    QVERIFY(volume.Load(sgy.toStdString(), err));

    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks, 16);
    SeismicSectionDockWidget dock;
    dock.setTaskService(&svc);
    QSignalSpy done(&dock, &SeismicSectionDockWidget::sectionExtractionFinished);
    dock.setVolume(std::make_shared<SgyVolume>(std::move(volume)));
    QVERIFY(waitForQuiet([&] { return done.count() >= 1; }));
    auto *spin = dock.findChild<QSpinBox *>(QStringLiteral("spinSectionSlice"));
    QVERIFY(spin);
    const int first = spin->value();

    spin->setValue(first + 1);
    QVERIFY(waitForQuiet([&] { return done.count() >= 2; }));
    const auto hits0 = svc.dataCache().Hits();
    spin->setValue(first); // 回头：命中
    QVERIFY(waitForQuiet([&] { return done.count() >= 3; }));
    QVERIFY(svc.dataCache().Hits() > hits0);
    QVERIFY(waitForQuiet([&] { return svc.activeTaskCount() == 0; }));
  }

  // 卷帘 B 图经服务：相邻线异步到位、换线跟随、失败有原因不静默。
  void curtainNeighborThroughService()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("ctn.sgy");
    QVERIFY(writeTestSegy(sgy, 8, 5, 64));
    SgyVolume volume;
    std::string err;
    QVERIFY(volume.Load(sgy.toStdString(), err));

    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks, 16);
    SeismicSectionDockWidget dock;
    dock.setTaskService(&svc);
    QSignalSpy done(&dock, &SeismicSectionDockWidget::sectionExtractionFinished);
    dock.setVolume(std::make_shared<SgyVolume>(std::move(volume)));
    QVERIFY(waitForQuiet([&] { return done.count() >= 1; }));
    auto *spin = dock.findChild<QSpinBox *>(QStringLiteral("spinSectionSlice"));
    QVERIFY(spin);
    auto *curtain =
        dock.findChild<QToolButton *>(QStringLiteral("btnSectionCurtain"));
    QVERIFY(curtain);

    curtain->setChecked(true);
    QVERIFY(waitForQuiet(
        [&] { return dock.canvas()->compareLabel() == QStringLiteral("IL %1").arg(spin->value() + 1); }));
    QVERIFY(dock.canvas()->compareEnabled());

    spin->setValue(spin->value() + 1);
    QVERIFY(waitForQuiet(
        [&] { return dock.canvas()->compareLabel() == QStringLiteral("IL %1").arg(spin->value() + 1); },
        10000));
    QVERIFY(waitForQuiet([&] { return svc.activeTaskCount() == 0; }));
  }

  // 任意线在途切回 IL：迟到的任意线结果不得覆盖切片显示（世代号守卫）。
  void sliceAfterArbitraryLineDropsLateSection()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("mix.sgy");
    QVERIFY(writeTestSegy(sgy, 6, 5, 64));
    SgyVolume volume;
    std::string err;
    QVERIFY(volume.Load(sgy.toStdString(), err));
    auto vol = std::make_shared<SgyVolume>(std::move(volume));

    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks, 16);
    // 占满闸 → 任意线请求排队（迟到的完成回放可复现）
    std::atomic_bool gateRelease{false};
    saturateGate(svc, gateRelease);
    SeismicSectionDockWidget dock;
    dock.setTaskService(&svc);
    QSignalSpy done(&dock, &SeismicSectionDockWidget::sectionExtractionFinished);
    dock.setVolume(vol);
    // 任意线请求（测网内真实线号 + 非空地图折线：hasRoute() 判定有效——
    // 若迟到结果未被世代号丢弃，route 会落下且画布变成 ~7 列的剖面）
    dock.extractSectionFromVolumeAsync(
        vol, {{1000, 2000}, {1002, 2002}, {1004, 2004}}, QStringLiteral("late"),
        {{0.0, 0.0}, {1500.0, 0.0}, {3000.0, 900.0}});
    // 切回 IL 模式：切片顶替任意线
    dock.setSectionMode(0);
    gateRelease.store(true, std::memory_order_release); // 放闸：迟到结果走世代丢弃
    QVERIFY(waitForQuiet([&] { return svc.activeTaskCount() == 0; }, 15000));

    QVERIFY(dock.canvas()->hasData());
    QVERIFY(!dock.hasRoute()); // 任意线结果没有落下
    QCOMPARE(dock.canvas()->traceCount(), 5); // 画布是切片数据
  }

private:
  // 标准 INLINE@189/CROSSLINE@193 合成 SEG-Y（与转码测试同构的最小版）
  static bool writeTestSegy(const QString &filePath, int inlines, int xlines, int ns,
                            int extTextHeaders = 0)
  {
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly))
      return false;
    file.write(QByteArray(3200, ' '));
    QByteArray binHdr(400, 0);
    const auto putBytes16 = [](QByteArray &buf, int at, qint16 v) {
      buf[at] = char(quint8(v >> 8));
      buf[at + 1] = char(quint8(v));
    };
    const auto putBytes32 = [](QByteArray &buf, int at, qint32 v) {
      buf[at] = char(quint8(v >> 24));
      buf[at + 1] = char(quint8(v >> 16));
      buf[at + 2] = char(quint8(v >> 8));
      buf[at + 3] = char(quint8(v));
    };
    const auto put16 = [&](int at, qint16 v) { putBytes16(binHdr, at, v); };
    put16(12, qint16(xlines));
    put16(16, 2000);
    put16(20, qint16(ns));
    put16(24, 5);
    // rev0-1 扩展文本头数（1 基字段 3505 → bin 头内 0 基 304）：首道偏移
    // = 3600 + N·3200（segy_trace0 同源规则）
    put16(304, qint16(extTextHeaders));
    file.write(binHdr);
    for (int e = 0; e < extTextHeaders; ++e)
      file.write(QByteArray(3200, ' '));
    for (int i = 0; i < inlines; ++i)
      for (int j = 0; j < xlines; ++j)
      {
        QByteArray trHdr(240, 0);
        const auto put32 = [&](int at, qint32 v) { putBytes32(trHdr, at, v); };
        const auto putTr16 = [&](int at, qint16 v) { putBytes16(trHdr, at, v); };
        put32(0, i * xlines + j + 1);
        put32(8, i * xlines + j + 1);  // field record = TRACL（转码测试口径）
        put32(20, j + 1);              // CDP ensemble
        put32(188, 1000 + i);
        put32(192, 2000 + j);
        putTr16(114, qint16(ns));
        putTr16(116, 2000);            // dt μs
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
};

QTEST_MAIN(TestSeismicSectionUi)
#include "tst_seismic_sectionui.moc"
