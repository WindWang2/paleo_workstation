#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QSignalSpy>
#include <QTest>
#include <QToolButton>

#include "ui/wellcomposite/depthtools.h"
#include "ui/wellcomposite/intervalstatistics.h"
#include "ui/wellcomposite/wellcompositecanvas.h"
#include "ui/wellcomposite/wellcompositepanel.h"
#include "ui/wellcomposite/wellcompositestore.h"
#include "ui/wellcomposite/wellpositionlegendwidget.h"
#include "domain/wellcompositemodel.h"
#include "qgis/qgisruntime.h"

#include <cmath>

using namespace WellComposite;

// wave/wellcomposite-deep — D2.x 深度交互测试
class TestWellCompositeDepth : public QObject
{
  Q_OBJECT

private slots:
  // ---- D2.1 吸附 ----
  void testSnapToMarkersAndGrid()
  {
    QVector<DepthTools::MarkerLine> markers = {{1500.0, QStringLiteral("T35")},
                                               {1800.0, QStringLiteral("T40")}};

    // 标志层吸附：1m 阈值内
    QCOMPARE(DepthTools::snapDepth(1500.4, markers, 1.0, true, false, 10.0), 1500.0);
    QCOMPARE(DepthTools::snapDepth(1499.7, markers, 1.0, true, false, 10.0), 1500.0);
    // 阈值外不吸附
    QCOMPARE(DepthTools::snapDepth(1495.0, markers, 1.0, true, false, 10.0), 1495.0);
    // 网格吸附：50 刻度
    QCOMPARE(DepthTools::snapDepth(1048.0, markers, 3.0, false, true, 50.0), 1050.0);
    // 标志层优先于网格
    QCOMPARE(DepthTools::snapDepth(1501.0, markers, 2.0, true, true, 50.0), 1500.0);
    // 全关 = 原值
    QCOMPARE(DepthTools::snapDepth(1048.0, markers, 1.0, false, false, 50.0), 1048.0);
    // 空标志层 + 网格开（阈值 3m 内贴 10m 刻度）
    QCOMPARE(DepthTools::snapDepth(1002.0, {}, 3.0, true, true, 10.0), 1000.0);
    QCOMPARE(DepthTools::snapDepth(1006.0, {}, 3.0, true, true, 10.0), 1006.0);

    // nice 步长
    QCOMPARE(DepthTools::niceStepFor(7.559), 5.0);   // 36/7.56 ≈ 4.76 → 5
    QCOMPARE(DepthTools::niceStepFor(0.377), 100.0); // 36/0.377 ≈ 95 → 100
  }

  // ---- D2.12 gap 段 ----
  void testGapSegments()
  {
    QVector<DepthTools::MarkerLine> markers = {
        {1000.0, QStringLiteral("T30")}, {1100.0, QStringLiteral("T31")},
        {1500.0, QStringLiteral("T35")}, {1510.0, QStringLiteral("T36")}};
    const auto gaps = DepthTools::gapSegments(markers, 200.0);
    QCOMPARE(gaps.size(), 1);
    QCOMPARE(gaps.first().top, 1100.0);
    QCOMPARE(gaps.first().bottom, 1500.0);
    QCOMPARE(gaps.first().span, 400.0);
    QCOMPARE(gaps.first().upperMarker, QStringLiteral("T31"));
    QCOMPARE(gaps.first().lowerMarker, QStringLiteral("T35"));

    // 阈值外无 gap
    QVERIFY(DepthTools::gapSegments(markers, 500.0).isEmpty());
    // 少于 2 条无 gap
    QVERIFY(DepthTools::gapSegments({{100.0, QStringLiteral("A")}}, 1.0).isEmpty());
  }

  // ---- D2.11 读数 ----
  void testReadoutNearestMarker()
  {
    QVector<DepthTools::MarkerLine> markers = {{1500.0, QStringLiteral("T35")},
                                               {1800.0, QStringLiteral("T40")}};
    double dist = -1.0;
    QCOMPARE(DepthTools::nearestMarkerName(1502.0, markers, 50.0, &dist), QStringLiteral("T35"));
    QCOMPARE(dist, 2.0);
    // 超距返回空
    QVERIFY(DepthTools::nearestMarkerName(1700.0, markers, 50.0).isEmpty());
    // 读数条文本
    const QString text = DepthTools::readoutText(1501.5, markers);
    QVERIFY(text.contains(QStringLiteral("1501.5")));
    QVERIFY(text.contains(QStringLiteral("T35")));
  }

  // ---- D2.3/D2.6 钉注与书签 CRUD ----
  void testPinsAndBookmarks()
  {
    QList<DepthPin> pins;
    QCOMPARE(DepthTools::addPin(&pins, 1234.5, QStringLiteral("油层顶")), 0);
    QCOMPARE(DepthTools::addPin(&pins, 1300.0, QStringLiteral("水层")), 1);
    QCOMPARE(pins.size(), 2);
    QVERIFY(DepthTools::updatePinText(&pins, 0, QStringLiteral("油层顶(改)")));
    QCOMPARE(pins.at(0).text, QStringLiteral("油层顶(改)"));
    QVERIFY(!DepthTools::updatePinText(&pins, 9, QStringLiteral("x")));
    QVERIFY(DepthTools::removePinAt(&pins, 1));
    QCOMPARE(pins.size(), 1);

    QList<DepthBookmark> bms;
    QCOMPARE(DepthTools::addBookmark(&bms, QStringLiteral("目的层"), 2000.0), 0);
    // 同名覆盖
    DepthTools::addBookmark(&bms, QStringLiteral("目的层"), 2100.0);
    QCOMPARE(bms.size(), 1);
    QCOMPARE(bms.first().depth, 2100.0);
    // 空名拒绝
    QCOMPARE(DepthTools::addBookmark(&bms, QStringLiteral("  "), 100.0), -1);
    QVERIFY(DepthTools::removeBookmark(&bms, QStringLiteral("目的层")));
    QVERIFY(bms.isEmpty());
  }

  // ---- D2.7 跳深度对话框 ----
  void testGotoDialogRange()
  {
    GotoDepthDialog dlg(1000.0, 2000.0, 1500.0);
    auto *spin = dlg.findChild<QDoubleSpinBox *>(QStringLiteral("gotoDepthSpin"));
    QVERIFY(spin);
    QCOMPARE(spin->minimum(), 1000.0);
    QCOMPARE(spin->maximum(), 2000.0);
    QCOMPARE(spin->value(), 1500.0);
    // 直接越界输入会被 QDoubleSpinBox 夹住；accept 落界内
    spin->setValue(1800.0);
    dlg.accept();
    QCOMPARE(dlg.selectedDepth(), 1800.0);

    // 英尺模式往返
    GotoDepthDialog ftDlg(1000.0, 2000.0, 1000.0, true);
    QCOMPARE(ftDlg.selectedDepth(), 1000.0); // 未接受前保持初值
  }

  // ---- D6.3 单位换算 ----
  void testFeetConversion()
  {
    QCOMPARE(DepthTools::metersToFeet(0.3048), 1.0);
    QCOMPARE(DepthTools::feetToMeters(1.0), 0.3048);
    QCOMPARE(DepthTools::formatDepth(1524.0, false), QStringLiteral("1524.0"));
    // 1524 m = 5000 ft
    QCOMPARE(DepthTools::formatDepth(1524.0, true), QStringLiteral("5000.0"));
  }

  // ---- D2.8 缩放上下限 ----
  void testZoomSpanLimits()
  {
    WellCompositeCanvas canvas;
    canvas.resize(400, 500);
    canvas.show(); // 真实几何
    QApplication::processEvents();
    canvas.setDepthRange(1000.0, 3000.0);
    canvas.setScaleRatio(QStringLiteral("1:200")); // 18.9 px/m → span ≈ 22m

    canvas.setZoomSpanLimits(50.0, 150.0);
    QCOMPARE(canvas.minVisibleSpan(), 50.0);
    QCOMPARE(canvas.maxVisibleSpan(), 150.0);

    // 放大到突破下限（<50m 视口）→ 被钳到 50m
    canvas.setZoomFactor(canvas.zoomFactor() * 50.0, -1.0);
    QVERIFY(canvas.visibleDepthSpan() >= 50.0 - 0.5);

    // 缩小到突破上限（>150m 视口）→ 被钳到 150m
    canvas.setZoomFactor(canvas.zoomFactor() / 500.0, -1.0);
    QVERIFY(canvas.visibleDepthSpan() <= 150.0 + 0.5);
  }

  // ---- D2.4 锚点模式 ----
  void testWheelAnchorMode()
  {
    WellCompositeCanvas canvas;
    canvas.resize(400, 500);
    canvas.show();
    QApplication::processEvents();
    canvas.setDepthRange(1000.0, 3000.0);
    canvas.setScaleRatio(QStringLiteral("1:1000"));

    QVERIFY(canvas.wheelZoomAtMouse()); // 缺省鼠标锚点
    canvas.setWheelZoomAtMouse(false);
    QVERIFY(!canvas.wheelZoomAtMouse());
    canvas.setWheelZoomAtMouse(true);
    QVERIFY(canvas.wheelZoomAtMouse());
  }

  // ---- D2.2 橡皮筋区间（程序化三步驱动）----
  void testRubberBandInterval()
  {
    WellCompositeCanvas canvas;
    canvas.resize(400, 500);
    canvas.show();
    QApplication::processEvents();
    canvas.setDepthRange(1000.0, 2000.0);
    canvas.setScaleRatio(QStringLiteral("自适应"));

    QSignalSpy spy(&canvas, &WellCompositeCanvas::intervalSelected);
    WellCompositeBody *body = canvas.findChild<WellCompositeBody *>();
    QVERIFY(body);

    // 画布至少一道保证 paint 路径（无道也行——rubber band 不依赖道）
    body->beginRubberBand(QPoint(100, 10));
    QVERIFY(canvas.isRubberBandActive());
    body->updateRubberBand(QPoint(100, 100));
    body->endRubberBand();
    QVERIFY(!canvas.isRubberBandActive());

    QCOMPARE(spy.count(), 1);
    const double top = spy.first().at(0).toDouble();
    const double bottom = spy.first().at(1).toDouble();
    QVERIFY(bottom > top);
    QCOMPARE(top, canvas.yToDepth(10.0)); // 顶 = 起点深度

    // 抖动 (<6px) 不产区间
    body->beginRubberBand(QPoint(50, 20));
    body->updateRubberBand(QPoint(50, 23));
    body->endRubberBand();
    QCOMPARE(spy.count(), 1);
  }

  // ---- D2.2 区间统计 ----
  void testIntervalStats()
  {
    ComprehensiveWellData data;
    CurveData gr;
    gr.name = QStringLiteral("GR");
    gr.unit = QStringLiteral("API");
    gr.depths = {1000.0f, 1010.0f, 1020.0f, 1030.0f, 1040.0f};
    gr.values = {10.0f, 30.0f, 20.0f, 50.0f, 40.0f};
    data.continuousCurves << gr;

    CurveData discrete;
    discrete.name = QStringLiteral("CPOR");
    discrete.depths = {1015.0f, 1025.0f};
    discrete.values = {5.0f, 7.0f};
    data.discreteCurves << discrete;

    LithologyInterval l1;
    l1.topDepth = 1000.0f;
    l1.bottomDepth = 1012.0f;
    l1.lithoName = QStringLiteral("泥岩");
    LithologyInterval l2;
    l2.topDepth = 1012.0f;
    l2.bottomDepth = 1030.0f;
    l2.lithoName = QStringLiteral("砂岩");
    data.lithologyIntervals << l1 << l2;
    data.standardHorizons = {{1012.0, QStringLiteral("T1")}, {1035.0, QStringLiteral("T2")}};

    // 区间 [1000, 1020)：GR 2 样本 10/30；泥岩 12m 全在、砂岩 8m（跨界截断）
    const auto rep = computeIntervalStats(1000.0, 1020.0, data);
    QVERIFY(rep.isValid());
    QCOMPARE(rep.curves.size(), 2);
    QCOMPARE(rep.curves.at(0).name, QStringLiteral("GR"));
    QCOMPARE(rep.curves.at(0).sampleCount, 2);
    QCOMPARE(rep.curves.at(0).min, 10.0);
    QCOMPARE(rep.curves.at(0).max, 30.0);
    QCOMPARE(rep.curves.at(0).mean, 20.0);
    // 样本 std：|10-20|²+|30-20|² / (n-1) = 200 → sqrt ≈ 14.14
    QVERIFY(std::abs(rep.curves.at(0).stdDev - 14.142) < 0.01);
    QCOMPARE(rep.curves.at(1).name, QStringLiteral("CPOR"));
    QCOMPARE(rep.curves.at(1).sampleCount, 1); // 1015 ∈ [1000,1020)，1025 出界

    QCOMPARE(rep.lithology.size(), 2);
    QCOMPARE(rep.lithology.at(0).name, QStringLiteral("泥岩")); // 等厚度时稳定序
    QCOMPARE(rep.markerCount, 1);                               // T1@1012 在区间内

    // TSV
    const QString tsv = rep.toTsv();
    QVERIFY(tsv.contains(QStringLiteral("曲线\t单位\tn\tmin\tmax\tmean\tstd")));
    QVERIFY(tsv.contains(QStringLiteral("泥岩\t12.0\t")));
    QVERIFY(tsv.contains(QStringLiteral("标志层计数\t1")));
  }

  // ---- D1.3/D1.4 道头拖拽换位与分隔线 ----
  void testHeaderDragReorderAndSplitter()
  {
    WellCompositeCanvas canvas;
    canvas.resize(500, 400);
    canvas.show();
    QApplication::processEvents();
    canvas.setDepthRange(1000.0, 2000.0);

    auto t1 = std::make_shared<TextTrack>(QStringLiteral("A"), 100.0);
    auto t2 = std::make_shared<TextTrack>(QStringLiteral("B"), 100.0);
    auto t3 = std::make_shared<TextTrack>(QStringLiteral("C"), 100.0);
    canvas.addTrack(t1);
    canvas.addTrack(t2);
    canvas.addTrack(t3);

    WellCompositeHeader *header = canvas.findChild<WellCompositeHeader *>();
    QVERIFY(header);

    // 分隔线命中：x=100（A|B 边界）→ 左侧道可见序 0
    QCOMPARE(canvas.splitterIndexAtX(100.0), 0);
    QCOMPARE(canvas.splitterIndexAtX(200.0), 1);
    QCOMPARE(canvas.splitterIndexAtX(203.0), 1);   // 5px 热区
    QCOMPARE(canvas.splitterIndexAtX(210.0), -1);  // 热区外
    QCOMPARE(canvas.splitterIndexAtX(50.0), -1);   // 首道左边界不算

    // hitTest 语义
    int vi = -1;
    QCOMPARE(header->hitTest(QPoint(50, 30), &vi), WellCompositeHeader::HitKind::TrackTitle);
    QCOMPARE(vi, 0);
    QCOMPARE(header->hitTest(QPoint(100, 30), &vi), WellCompositeHeader::HitKind::Splitter);

    // 模拟拖拽换位：按住 A 道头，移到最右，松手 → A 到末尾
    QSignalSpy orderSpy(&canvas, &WellCompositeCanvas::trackOrderChanged);
    QTest::mousePress(header, Qt::LeftButton, Qt::NoModifier, QPoint(50, 30));
    QVERIFY(canvas.isHeaderDragActive());
    QTest::mouseMove(header, QPoint(250, 30));
    QCOMPARE(canvas.headerDragInsertIndex(), 3); // 移到 C 之后
    QTest::mouseRelease(header, Qt::LeftButton, Qt::NoModifier, QPoint(250, 30));
    QVERIFY(!canvas.isHeaderDragActive());
    QCOMPARE(orderSpy.count(), 1);
    QCOMPARE(canvas.tracks().at(0)->title(), QStringLiteral("B"));
    QCOMPARE(canvas.tracks().at(2)->title(), QStringLiteral("A"));
  }

  // ---- D1.4 分隔线拖拽调宽 ----
  void testSplitterResize()
  {
    WellCompositeCanvas canvas;
    canvas.resize(500, 400);
    canvas.show();
    QApplication::processEvents();
    canvas.setDepthRange(1000.0, 2000.0);

    auto t1 = std::make_shared<TextTrack>(QStringLiteral("A"), 100.0);
    auto t2 = std::make_shared<TextTrack>(QStringLiteral("B"), 100.0);
    canvas.addTrack(t1);
    canvas.addTrack(t2);

    WellCompositeHeader *header = canvas.findChild<WellCompositeHeader *>();
    QSignalSpy widthSpy(&canvas, &WellCompositeCanvas::trackWidthChanged);

    // 在边界 x=100 按住并拖到 x=140 → A 道 140px
    QTest::mousePress(header, Qt::LeftButton, Qt::NoModifier, QPoint(100, 30));
    QTest::mouseMove(header, QPoint(140, 30));
    QTest::mouseRelease(header, Qt::LeftButton, Qt::NoModifier, QPoint(140, 30));
    QVERIFY(widthSpy.count() >= 1);
    QCOMPARE(t1->width(), 140.0);

    // 下限 24px：拖到 x=10 → A 道 24px（qBound）
    QTest::mousePress(header, Qt::LeftButton, Qt::NoModifier, QPoint(140, 30));
    QTest::mouseMove(header, QPoint(10, 30));
    QTest::mouseRelease(header, Qt::LeftButton, Qt::NoModifier, QPoint(10, 30));
    QCOMPARE(t1->width(), 24.0);
  }

  // ---- D1.5 右键菜单（菜单对象存在 + 动作经信号路径）----
  void testContextMenuPresent()
  {
    WellCompositeCanvas canvas;
    canvas.resize(500, 400);
    canvas.show();
    QApplication::processEvents();
    canvas.setDepthRange(1000.0, 2000.0);
    canvas.addTrack(std::make_shared<DepthScaleTrack>(64.0));
    canvas.addTrack(std::make_shared<TextTrack>(QStringLiteral("结论"), 100.0));

    WellCompositeBody *body = canvas.findChild<WellCompositeBody *>();
    QVERIFY(body);

    // contextMenuEvent 会 exec 阻塞——测试只验 hit 逻辑与 CSV 产出（菜单项 exec 由人工 QA）
    QCOMPARE(canvas.trackIndexAtX(80.0), 1); // 绝对序：depth(0) 宽 64，x=80 落文本道
    QVERIFY(!canvas.trackCsvAt(0).isEmpty());
    QVERIFY(!canvas.trackCsvAt(1).isEmpty());
    QCOMPARE(canvas.trackCsvAt(99), QString());
  }

  // ---- D3.1 标志层拖拽（编辑模式）----
  void testMarkerDragEditing()
  {
    WellCompositeCanvas canvas;
    canvas.resize(500, 500);
    canvas.show();
    QApplication::processEvents();
    canvas.setDepthRange(1000.0, 2000.0);
    canvas.setScaleRatio(QStringLiteral("自适应")); // 1px = 1m

    canvas.setMarkerLines({{1500.0, QStringLiteral("T35")}});
    canvas.setEditMode(true);
    QVERIFY(canvas.editMode());

    WellCompositeBody *body = canvas.findChild<WellCompositeBody *>();
    QSignalSpy spy(&canvas, &WellCompositeCanvas::markerMoved);

    // 标志层屏幕 y 动态求（自适应比例尺）
    const qreal markerY = canvas.depthToY(1500.0);
    QCOMPARE(body->markerHitTest(markerY), 0);
    QCOMPARE(body->markerHitTest(markerY - 100.0), -1);

    const qreal targetY = markerY - 100.0;
    QTest::mousePress(body, Qt::LeftButton, Qt::NoModifier, QPoint(100, static_cast<int>(markerY)));
    QTest::mouseMove(body, QPoint(100, static_cast<int>(targetY)));
    QTest::mouseRelease(body, Qt::LeftButton, Qt::NoModifier, QPoint(100, static_cast<int>(targetY)));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).toString(), QStringLiteral("T35"));
    const double expectedDepth = 1500.0 - 100.0 / canvas.pxPerMeter(); // 上移 100px
    QVERIFY(std::abs(spy.first().at(1).toDouble() - expectedDepth) < 2.0);
    QVERIFY(std::abs(canvas.markerLines().first().first - expectedDepth) < 2.0); // 画布线随拖更新
  }

  // ---- D2.5 迷你导航条拖拽跳转 ----
  void testMiniNavigatorJump()
  {
    WellCompositePanel panel;
    panel.resize(900, 600);
    panel.show();
    // #151：先等窗口真正 exposed，几何有效后再点击并强断言（旧的
    // spy.count() >= 0 恒真，测不出任何回归）。
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    QApplication::processEvents();

    QVERIFY(panel.legendWidget()->miniBar() != nullptr);
    QSignalSpy spy(panel.legendWidget()->miniBar(), &WellOverviewMiniBar::requestScrollDepth);
    panel.legendWidget()->miniBar()->setVisibleRange(100.0, 200.0);

    // 程序化点击中点 → 请求滚动（现有 handleMouseAt 行为）
    QWidget *bar = panel.legendWidget()->miniBar();
    if (!bar->isVisible() || bar->width() <= 0)
      QSKIP("迷你导航条在当前布局下不可见/无宽度，无法做点击断言");
    QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, QPoint(bar->width() / 2, bar->height() / 2));
    QCOMPARE(spy.count(), 1); // 按下发一次跳转请求，松开不再发
    QVERIFY(std::isfinite(spy.first().at(0).toDouble()));
  }

  // ---- D2.9/D1.12 视口同步锁 ----
  void testPanelReadoutAndUnit()
  {
    WellCompositePanel panel;
    panel.resize(900, 600);
    panel.show();
    QApplication::processEvents();

    // D2.11 读数条存在且随悬停更新
    QVERIFY(panel.readoutLabel());
    panel.canvas()->setDepthRange(1000.0, 2000.0);
    panel.canvas()->setMarkerLines({{1500.0, QStringLiteral("T35")}});
    panel.canvas()->setHoverDepth(1501.0);
    QVERIFY(panel.readoutLabel()->text().contains(QStringLiteral("1501")));
    QVERIFY(panel.readoutLabel()->text().contains(QStringLiteral("T35")));

    // D6.3 英尺切换
    panel.setDepthUnitFeet(true);
    QVERIFY(panel.depthUnitFeet());
    panel.setDepthUnitFeet(false);
    QVERIFY(!panel.depthUnitFeet());

    // D2.12 gap 阈值
    panel.setGapThresholdMeters(300.0);
    QCOMPARE(panel.gapThresholdMeters(), 300.0);
    QCOMPARE(panel.canvas()->gapThresholdMeters(), 300.0);
  }

  // ---- D2.3 面板钉注 API（含 sidecar 持久化）----
  void testPanelPinsSidecar()
  {
    const QString tmpSidecarSource = QDir::temp().absoluteFilePath(
        QStringLiteral("wc_test_pins_%1.xml").arg(QCoreApplication::applicationPid()));
    QFile(tmpSidecarSource).remove();

    WellCompositePanel panel;
    panel.setSourceDataPath(tmpSidecarSource);
    panel.addPinAt(1234.0, QStringLiteral("油层"));
    QCOMPARE(panel.pins().size(), 1);
    QCOMPARE(panel.pins().first().text, QStringLiteral("油层"));

    // sidecar 落盘（<源名>.wc.json）
    const QString sidecarPath = tmpSidecarSource.left(tmpSidecarSource.lastIndexOf(QLatin1Char('.')))
                                + QStringLiteral(".wc.json");
    QVERIFY(QFile::exists(sidecarPath));

    // 重建 store 读回
    WellCompositeStore store(tmpSidecarSource);
    QVERIFY(store.load());
    QCOMPARE(store.pins().size(), 1);
    QCOMPARE(store.pins().first().depth, 1234.0);

    panel.clearPins();
    QVERIFY(panel.pins().isEmpty());
    QFile::remove(tmpSidecarSource);
    QFile::remove(sidecarPath);
  }
};

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestWellCompositeDepth tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_wellcomposite_depth.moc"
