// 层：测试壳
// P5 Phase 2 剖面 2D 测试（D2.1–D2.14）：纹理缓存命中、显示三模、阈值/极性、
// AGC/增益曲线、双刻度、LOD 帧预算、纵向拉伸、8 档色标+反转、导出 PNG、
// 卷帘对比、道头服务、书签往返、复制/抓图、空数据原因态。
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

#include <cmath>
#include <cstring>
#include <limits>

#include "../src/domain/seismic/sgyvolume.h"
#include "../src/services/seismictaskservice.h"
#include "../src/ui/seismicsection/seismicsectioncanvas.h"
#include "../src/ui/seismicsection/seismicsectiondockwidget.h"

using namespace seismic;

namespace {

// 合成剖面：正弦同相轴（道间能量差给 AGC 测试用）
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
      img.values[std::size_t(s) * traces + t] =
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

} // namespace

class TestSeismicSectionUi : public QObject
{
  Q_OBJECT

private:
  QTemporaryDir m_settingsDir; // initTestCase 前构造：settings 沙箱路径

private slots:
  void initTestCase()
  {
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
    QVERIFY2(secondMs < 25,
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
    QVERIFY2(perFrameMs < 16.0,
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

  // ---- CONC-01: 剖面后台切片/卷帘提取中宿主控件析构安全（QPointer 防 UAF）----
  void extractSliceAsyncDestructionSafety()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("slice_uaf.sgy");
    QVERIFY(writeTestSegy(sgy, 10, 10, 128));

    SgyVolume volume;
    std::string err;
    QVERIFY(volume.Load(sgy.toStdString(), err));
    auto volPtr = std::make_shared<SgyVolume>(std::move(volume));

    // Case 1: 异步切片提取中析构
    for (int i = 0; i < 5; ++i) {
      auto *dock = new SeismicSectionDockWidget;
      dock->setVolume(volPtr);
      dock->extractSliceAsync(SgySliceType::Inline, 1000 + i);
      delete dock;
    }

    // Case 2: 卷帘对比提取中析构
    for (int i = 0; i < 5; ++i) {
      auto *dock = new SeismicSectionDockWidget;
      dock->setVolume(volPtr);
      dock->setSectionMode(0);
      dock->extractSliceAsync(SgySliceType::Inline, 1000);
      delete dock;
    }

    QThreadPool::globalInstance()->waitForDone(5000);
    QApplication::processEvents();
  }

  // ---- Issue #25: 连续快速切线/切片并发安全（防数据竞态与陈旧数据覆盖）----
  void rapidLineSwitchingNoRaceOrCrash()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("rapid_switching.sgy");
    // 30 inlines, 30 crosslines, 64 samples
    QVERIFY(writeTestSegy(sgy, 30, 30, 64));

    SgyVolume volume;
    std::string err;
    QVERIFY(volume.Load(sgy.toStdString(), err));
    auto volPtr = std::make_shared<SgyVolume>(std::move(volume));

    SeismicSectionDockWidget dock;
    dock.setVolume(volPtr);
    dock.show();

    // 1. 模拟用户极快速拖拽 Inline 滑块（连续触发 20 次切线请求）
    for (int il = 1000; il < 1020; ++il) {
      dock.onSliceSliderChanged(il);
    }

    // 2. 模拟切片模式极快速切换（Inline -> Xline -> Time -> Inline）
    dock.setSectionMode(1); // Xline
    dock.onSliceSliderChanged(2005);
    dock.setSectionMode(2); // Time
    dock.onSliceSliderChanged(10);
    dock.setSectionMode(0); // Inline
    dock.onSliceSliderChanged(1015);

    // 3. 开启卷帘对比，再次快速切换 10 次
    if (dock.m_btnCurtain) {
      dock.m_btnCurtain->setChecked(true);
      for (int il = 1010; il <= 1018; ++il) {
        dock.onSliceSliderChanged(il);
      }
    }

    // 4. 等待所有后台任务收敛完成
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5000) {
      QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    QThreadPool::globalInstance()->waitForDone(5000);
    QApplication::processEvents();

    // 5. 断言：最终收敛状态必须严格等于最后一次请求的测线号（1018），而非中间陈旧测线！
    const auto ref = dock.canvas()->sectionRef();
    QVERIFY(ref.valid);
    QCOMPARE(ref.type, SgySliceType::Inline);
    QCOMPARE(ref.index, 1018);
    QVERIFY(dock.m_lblTitle && dock.m_lblTitle->text().contains("1018"));
    QVERIFY(dock.canvas()->noDataReason().isEmpty());
    QVERIFY(dock.canvas()->traceCount() > 0);
  }

private:
  // 标准 INLINE@189/CROSSLINE@193 合成 SEG-Y（与转码测试同构的最小版）
  static bool writeTestSegy(const QString &filePath, int inlines, int xlines, int ns)
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
    file.write(binHdr);
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
