#include <QLabel>
#include <QListWidget>
#include <QSignalSpy>
#include "../src/ui/paleotheme.h"
#include <QTest>

#include "ui/wellcomposite/wellcompositecanvas.h"
#include "ui/wellcomposite/wellcompositepanel.h"
#include "ui/wellcomposite/depthtools.h"
#include "ui/wellcomposite/intervaleditor.h"
#include "ui/wellcomposite/trackconfigdialog.h"
#include "ui/wellcomposite/stratassignment.h"
#include "ui/wellcomposite/hiddentrackbar.h"
#include "ui/wellcomposite/multiwellview.h"
#include "domain/wellcompositemodel.h"
#include "qgis/qgisruntime.h"

using namespace WellComposite;

// wave/wellcomposite-deep — D7.x 打磨与可达性测试
class TestWellCompositeA11y : public QObject
{
  Q_OBJECT

private slots:
  // ---- D7.1 键盘导航：Tab 道间焦点 ----
  void testKeyboardFocusCycle()
  {
    WellCompositeCanvas canvas;
    canvas.resize(600, 400);
    canvas.show();
    QApplication::processEvents();
    canvas.setDepthRange(1000.0, 2000.0);
    canvas.addTrack(std::make_shared<DepthScaleTrack>(64.0));
    canvas.addTrack(std::make_shared<TextTrack>(QStringLiteral("结论"), 100.0));
    canvas.addTrack(std::make_shared<CurveTrack>(QStringLiteral("GR"), 150.0));

    QCOMPARE(canvas.focusTrackIndex(), -1);
    QTest::keyClick(&canvas, Qt::Key_Tab);
    QCOMPARE(canvas.focusTrackIndex(), 0);
    QTest::keyClick(&canvas, Qt::Key_Tab);
    QCOMPARE(canvas.focusTrackIndex(), 1);
    QTest::keyClick(&canvas, Qt::Key_Tab);
    QCOMPARE(canvas.focusTrackIndex(), 2);
    // 环绕
    QTest::keyClick(&canvas, Qt::Key_Tab);
    QCOMPARE(canvas.focusTrackIndex(), 0);
    // Shift+Tab 反向
    QTest::keyClick(&canvas, Qt::Key_Tab, Qt::ShiftModifier);
    QCOMPARE(canvas.focusTrackIndex(), 2);

    // 程序化设置
    canvas.setFocusTrackIndex(1);
    QCOMPARE(canvas.focusTrackIndex(), 1);
  }

  // ---- D7.1 Enter 开配置（焦点道）----
  void testEnterOpensConfig()
  {
    WellCompositeCanvas canvas;
    canvas.resize(600, 400);
    canvas.show();
    QApplication::processEvents();
    canvas.setDepthRange(1000.0, 2000.0);
    canvas.addTrack(std::make_shared<TextTrack>(QStringLiteral("结论"), 100.0));

    QSignalSpy spy(&canvas, &WellCompositeCanvas::trackConfigRequested);
    QTest::keyClick(&canvas, Qt::Key_Tab);    // 焦点 0
    QTest::keyClick(&canvas, Qt::Key_Return); // Enter → 配置意图
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).toInt(), 0);

    // 无焦点道时 Enter 不发意图
    canvas.setFocusTrackIndex(-1);
    QTest::keyClick(&canvas, Qt::Key_Return);
    QCOMPARE(spy.count(), 1);
  }

  // ---- D7.1 Esc 逐级取消（拖拽换位中断）----
  void testEscCancelsDrag()
  {
    WellCompositeCanvas canvas;
    canvas.resize(600, 400);
    canvas.show();
    QApplication::processEvents();
    canvas.setDepthRange(1000.0, 2000.0);
    auto t1 = std::make_shared<TextTrack>(QStringLiteral("A"), 100.0);
    auto t2 = std::make_shared<TextTrack>(QStringLiteral("B"), 100.0);
    canvas.addTrack(t1);
    canvas.addTrack(t2);

    WellCompositeHeader *header = canvas.findChild<WellCompositeHeader *>();
    QVERIFY(header);
    QSignalSpy orderSpy(&canvas, &WellCompositeCanvas::trackOrderChanged);

    QTest::mousePress(header, Qt::LeftButton, Qt::NoModifier, QPoint(50, 30));
    QVERIFY(canvas.isHeaderDragActive());
    QTest::keyClick(header, Qt::Key_Escape); // Esc 取消
    QVERIFY(!canvas.isHeaderDragActive());
    QVERIFY(canvas.headerDragCancelled());
    QTest::mouseRelease(header, Qt::LeftButton, Qt::NoModifier, QPoint(150, 30));
    QCOMPARE(orderSpy.count(), 0); // 取消后不提交
    QCOMPARE(canvas.tracks().at(0).get(), t1.get());
  }

  // ---- D7.2 焦点环绘制（焦点道 header 2px primary 描边）----
  void testFocusRingPaint()
  {
    WellCompositeCanvas canvas;
    canvas.resize(600, 400);
    canvas.show();
    QApplication::processEvents();
    canvas.setDepthRange(1000.0, 2000.0);
    canvas.addTrack(std::make_shared<TextTrack>(QStringLiteral("结论"), 100.0));

    WellCompositeHeader *header = canvas.findChild<WellCompositeHeader *>();
    QImage noFocus(600, 72, QImage::Format_ARGB32_Premultiplied);
    noFocus.fill(Qt::white);
    {
      QPainter p(&noFocus);
      header->render(&p);
    }

    canvas.setFocusTrackIndex(0);
    QImage withFocus(600, 72, QImage::Format_ARGB32_Premultiplied);
    withFocus.fill(Qt::white);
    {
      QPainter p(&withFocus);
      header->render(&p);
    }

    // 焦点环边界列必须引入 primary 蓝 (#1B73D0)
    bool hasBlueRing = false;
    for (int y = 4; y < 68; y += 4)
    {
      if (withFocus.pixelColor(1, y).name().compare(QStringLiteral("#1b73d0"), Qt::CaseInsensitive) == 0)
      {
        hasBlueRing = true;
        break;
      }
    }
    QVERIFY(hasBlueRing);
    // 无焦点帧同位置无环
    QVERIFY(noFocus.pixelColor(1, 36).name().compare(QStringLiteral("#1b73d0"), Qt::CaseInsensitive) != 0);
  }

  // ---- D7.1 方向键滚动 / +- 缩放 ----
  void testArrowKeysAndZoomKeys()
  {
    WellCompositeCanvas canvas;
    canvas.resize(600, 400);
    canvas.show();
    QApplication::processEvents();
    canvas.setDepthRange(1000.0, 2000.0);
    canvas.setScaleRatio(QStringLiteral("1:200")); // 可滚比例尺（自适应=全井适配电平滚不动）

    const double before = canvas.scrollDepth();
    QTest::keyClick(&canvas, Qt::Key_Down);
    QVERIFY(canvas.scrollDepth() > before);
    QTest::keyClick(&canvas, Qt::Key_Up);
    QVERIFY(std::abs(canvas.scrollDepth() - before) < 1e-6);

    const double zoomBefore = canvas.zoomFactor();
    QTest::keyClick(&canvas, Qt::Key_Plus);
    QVERIFY(canvas.zoomFactor() > zoomBefore);
    QTest::keyClick(&canvas, Qt::Key_Minus);
    QVERIFY(std::abs(canvas.zoomFactor() - zoomBefore) < 1e-6);
  }

  // ---- D1.9 tooltip 内容 ----
  void testTooltipContent()
  {
    WellCompositeCanvas canvas;
    canvas.setDepthRange(1000.0, 2000.0);

    auto ct = std::make_shared<CurveTrack>(QStringLiteral("测井"), 150.0);
    CurveData gr;
    gr.name = QStringLiteral("GR");
    gr.unit = QStringLiteral("API");
    gr.minScale = 0.0f;
    gr.maxScale = 150.0f;
    gr.depths = {1000.0f, 1100.0f, 1200.0f};
    gr.values = {40.0f, 50.0f, 60.0f};
    ct->addCurve(gr);
    canvas.addTrack(ct);

    const QString tip = canvas.toolTipFor(0, 1100.0);
    QVERIFY(tip.contains(QStringLiteral("GR")));
    QVERIFY(tip.contains(QStringLiteral("50")));       // 当前深度值
    QVERIFY(tip.contains(QStringLiteral("0~150")));    // 量程
    QVERIFY(tip.contains(QStringLiteral("API")));      // 单位

    // 地层道 tooltip
    auto ft = std::make_shared<FormationTrack>(QStringLiteral("地层"), 80.0);
    ft->addInterval({1000.0f, 1500.0f, QStringLiteral("珠江组")});
    canvas.addTrack(ft);
    const QString fTip = canvas.toolTipFor(1, 1200.0);
    QVERIFY(fTip.contains(QStringLiteral("珠江组")));
  }

  // ---- D7.5 空态 ----
  void testEmptyStateHint()
  {
    WellCompositeCanvas canvas;
    canvas.resize(600, 400);
    canvas.show();
    QApplication::processEvents();

    WellCompositeBody *body = canvas.findChild<WellCompositeBody *>();
    QImage img(600, 400, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    QPainter p(&img);
    body->render(&p);
    p.end();

    // 空态提示文字（非纯白像素群——画了提示文本）
    QVERIFY(canvas.trackCount() == 0);
    int darkPixels = 0;
    for (int y = 150; y < 250; ++y)
      for (int x = 100; x < 500; ++x)
        if (img.pixelColor(x, y).lightness() < 200)
          ++darkPixels;
    QVERIFY(darkPixels > 50);
  }

  // ---- D7.4 高对比切换 ----
  void testHighContrastToggle()
  {
    WellCompositePanel panel;
    panel.setProjectName(QStringLiteral("A11yHC"));
    QVERIFY(!panel.highContrast());
    panel.setHighContrast(true);
    QVERIFY(panel.highContrast());
    panel.setHighContrast(false);
    QVERIFY(!panel.highContrast());
  }

  // ---- D7.9 对话框尺寸规范 ----
  void testDialogsSizeHints()
  {
    LithologyInterval li;
    LithoIntervalDialog lithoDlg(li, 1000.0, 2000.0);
    QVERIFY(lithoDlg.minimumSizeHint().isValid());
    QVERIFY(lithoDlg.sizeHint().isValid());

    FaciesInterval fi;
    FaciesIntervalDialog faciesDlg(fi, 1000.0, 2000.0);
    QVERIFY(faciesDlg.sizeHint().isValid());

    GotoDepthDialog gotoDlg(1000.0, 2000.0, 1500.0);
    QVERIFY(gotoDlg.sizeHint().isValid());

    StratAssignmentDialog assignDlg({QStringLiteral("A"), QStringLiteral("C1")}, {});
    QVERIFY(assignDlg.minimumSize().width() >= 400); // setMinimumSize 规范

    TrackSpec spec;
    spec.typeId = QStringLiteral("curve");
    TrackConfigDialog cfgDlg(spec, {});
    QVERIFY(cfgDlg.minimumSize().width() >= 400);
  }

  // ---- D7.8/D7.10 objectName 与 i18n 就绪冒烟 ----
  void testObjectNamesAndI18n()
  {
    WellCompositePanel panel;
    panel.setProjectName(QStringLiteral("A11yNames"));
    // objectName 稳定（自动化/测试锚点）
    for (const char *name : {"wellCompositeCanvas", "lblWellName", "scaleCombo",
                             "btnCompZoomIn", "btnCompZoomOut", "btnCompResetZoom",
                             "btnConfigCurves", "btnCompGoto", "btnCompBookmarks",
                             "btnCompSnap", "btnCompExport", "btnCompEdit",
                             "lblCompReadout", "lblStatus", "wellCompositeHiddenBar",
                             "wellPositionLegendWidget", "wellCompositeTopBar"})
    {
      QVERIFY2(panel.findChild<QWidget *>(QLatin1String(name)),
               qPrintable(QStringLiteral("缺 objectName: %1").arg(QLatin1String(name))));
    }

    // 新 UI 文本走 tr()（中文源串非空；lupdate 可收集）
    QVERIFY(!panel.findChild<QWidget *>(QStringLiteral("btnCompGoto"))->toolTip().isEmpty());
    QVERIFY(!panel.findChild<QWidget *>(QStringLiteral("btnCompSnap"))->toolTip().isEmpty());
    QVERIFY(!panel.findChild<QWidget *>(QStringLiteral("btnCompEdit"))->toolTip().isEmpty());

    // D7.10 参考井/测区井徽章语义——主题化收口（ui-deep-polish）后样式走
    // token（中性徽章 surfaceAltRaised / 参考井 warning 胶囊），不再钉字面
    // 色值；断言语义本身：两态样式可区分。
    panel.setWellName(QStringLiteral("A1"), false);
    const QString surveyStyle = panel.findChild<QLabel *>(QStringLiteral("lblWellName"))->styleSheet();
    panel.setWellName(QStringLiteral("REF-9"), true);
    const QString refStyle = panel.findChild<QLabel *>(QStringLiteral("lblWellName"))->styleSheet();
    QVERIFY(!surveyStyle.isEmpty());
    QVERIFY(refStyle != surveyStyle);
  }

  // ---- D7.7 参考井/测区井语义在对比模式一致（D5.5 对话框分区着色）----
  void testSelectionDialogColorSemantics()
  {
    WellSelectionDialog dlg(QStringList{QStringLiteral("A1"), QStringLiteral("REF-1")},
                            QStringList{QStringLiteral("REF-1")}, {});
    auto *list = dlg.findChild<QListWidget *>(QStringLiteral("wellSelectionList"));
    QVERIFY(list);
    QCOMPARE(list->count(), 2);
    QCOMPARE(list->item(0)->data(Qt::UserRole).toString(), QStringLiteral("survey"));
    QCOMPARE(list->item(1)->data(Qt::UserRole).toString(), QStringLiteral("ref"));
    // 琥珀/蓝的角色语义取 DESIGN 文字 token；已存在的条目随主题往返更新。
    const auto originalTheme = PaleoTheme::currentTheme();
    QSignalSpy changes(list, &QListWidget::itemChanged);
    for (const auto theme : {PaleoTheme::Theme::Light, PaleoTheme::Theme::Dark, PaleoTheme::Theme::Light})
    {
      PaleoTheme::applyTheme(theme);
      const auto &tokens = PaleoTheme::tokens();
      QCOMPARE(list->item(1)->foreground().color(), tokens.warningText);
      QCOMPARE(list->item(0)->foreground().color(), tokens.primaryText);
      QCOMPARE(list->item(1)->data(Qt::UserRole).toString(), QStringLiteral("ref"));
      QCOMPARE(list->item(0)->data(Qt::UserRole).toString(), QStringLiteral("survey"));
    }
    QCOMPARE(changes.count(), 0);
    PaleoTheme::applyTheme(originalTheme);
  }

  // ---- D1.10 隐藏条 objectName 语义（面板装配）----
  void testHiddenBarIntegration()
  {
    WellCompositePanel panel;
    panel.setProjectName(QStringLiteral("A11yHidden"));
    panel.resize(900, 600);
    QVERIFY(panel.hiddenBar());
    // 无隐藏道时隐藏条不显示
    QVERIFY(panel.hiddenBar()->isHidden());
  }
};

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestWellCompositeA11y tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_wellcomposite_a11y.moc"
