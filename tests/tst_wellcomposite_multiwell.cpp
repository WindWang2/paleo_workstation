#include <QSignalSpy>
#include <QTest>

#include "ui/wellcomposite/multiwellview.h"
#include "ui/wellcomposite/wellcompositepanel.h"
#include "ui/wellcomposite/wellcompositestore.h"
#include "domain/wellcompositemodel.h"
#include "qgis/qgisruntime.h"

using namespace WellComposite;

namespace {

QVector<CurveData> oneCurve(const QString &name)
{
  CurveData c;
  c.name = name;
  c.depths = {1000.0f, 1200.0f, 1500.0f};
  c.values = {40.0f, 60.0f, 55.0f};
  return {c};
}

QVector<FormationInterval> twoForms()
{
  return {{1000.0f, 1300.0f, QStringLiteral("珠江组"), QString(), QColor(QStringLiteral("#FFE082"))},
          {1300.0f, 1500.0f, QStringLiteral("珠海组"), QString(), QColor(QStringLiteral("#FFCC80"))}};
}

} // namespace

// wave/wellcomposite-deep — D5.x 多井对比测试
class TestWellCompositeMultiWell : public QObject
{
  Q_OBJECT

private slots:
  // ---- D5.1/D5.2 布局模式 ----
  void testLayoutModes()
  {
    MultiWellView view;
    view.resize(1200, 800);
    QCOMPARE(view.layoutMode(), MultiWellView::LayoutMode::Single);

    QSignalSpy spy(&view, &MultiWellView::layoutModeChanged);
    view.setLayoutMode(MultiWellView::LayoutMode::Dual);
    QCOMPARE(view.layoutMode(), MultiWellView::LayoutMode::Dual);
    QCOMPARE(spy.count(), 1);

    view.setLayoutMode(MultiWellView::LayoutMode::Quad);
    QCOMPARE(view.layoutMode(), MultiWellView::LayoutMode::Quad);
    QCOMPARE(spy.count(), 2);

    // 幂等
    view.setLayoutMode(MultiWellView::LayoutMode::Quad);
    QCOMPARE(spy.count(), 2);
  }

  void testPanelSlots()
  {
    MultiWellView view;
    view.resize(1200, 800);
    view.setLayoutMode(MultiWellView::LayoutMode::Dual);

    auto *p1 = new WellCompositePanel(&view);
    QVERIFY(p1->loadLasCurves(QStringLiteral("A1"), oneCurve(QStringLiteral("GR")), twoForms()));
    auto *p2 = new WellCompositePanel(&view);
    QVERIFY(p2->loadLasCurves(QStringLiteral("A2"), oneCurve(QStringLiteral("GR")), twoForms()));

    QCOMPARE(view.setPanel(0, p1), 0);
    QCOMPARE(view.setPanel(1, p2), 1);
    QCOMPARE(view.panelCount(), 2);
    QCOMPARE(view.panelAt(0)->wellName(), QStringLiteral("A1"));
    QCOMPARE(view.panelAt(1)->wellName(), QStringLiteral("A2"));

    // 超容量槽位夹到末位
    auto *p3 = new WellCompositePanel(&view);
    QVERIFY(p3->loadLasCurves(QStringLiteral("A3"), oneCurve(QStringLiteral("GR")), twoForms()));
    QCOMPARE(view.setPanel(9, p3), 1); // 替换槽 1
    QCOMPARE(view.panelAt(1)->wellName(), QStringLiteral("A3"));
    QCOMPARE(view.panelCount(), 2);
  }

  // ---- D5.1/D2.9 锁步滚动 ----
  void testLinkScroll()
  {
    MultiWellView view;
    view.resize(1200, 800);
    view.show();
    QApplication::processEvents();
    view.setLayoutMode(MultiWellView::LayoutMode::Dual);

    auto *p1 = new WellCompositePanel(&view);
    p1->setProjectName(QStringLiteral("MW-Link1"));
    QVERIFY(p1->loadLasCurves(QStringLiteral("A1"), oneCurve(QStringLiteral("GR")), twoForms()));
    auto *p2 = new WellCompositePanel(&view);
    p2->setProjectName(QStringLiteral("MW-Link2"));
    QVERIFY(p2->loadLasCurves(QStringLiteral("A2"), oneCurve(QStringLiteral("GR")), twoForms()));
    view.setPanel(0, p1);
    view.setPanel(1, p2);
    QApplication::processEvents();

    QVERIFY(view.linkScroll()); // 缺省锁步

    // 滚动井 A1 → 井 A2 视口跟随
    p1->canvas()->setScrollDepth(1200.0);
    QApplication::processEvents();
    QVERIFY(std::abs(p2->canvas()->scrollDepth() - 1200.0) < 1.0);

    // D2.9 关锁后独立
    view.setLinkScroll(false);
    QVERIFY(!view.linkScroll());
    p1->canvas()->setScrollDepth(1300.0);
    QApplication::processEvents();
    QVERIFY(std::abs(p2->canvas()->scrollDepth() - 1200.0) < 1.0);
  }

  // ---- D5.3 correlation pairs ----
  void testCorrelationPairs()
  {
    MultiWellView view;
    view.resize(1200, 800);
    view.setLayoutMode(MultiWellView::LayoutMode::Dual);

    auto *p1 = new WellCompositePanel(&view);
    p1->setProjectName(QStringLiteral("MW-Corr1"));
    QVERIFY(p1->loadLasCurves(QStringLiteral("A1"), oneCurve(QStringLiteral("GR")), twoForms()));
    auto *p2 = new WellCompositePanel(&view);
    p2->setProjectName(QStringLiteral("MW-Corr2"));
    QVERIFY(p2->loadLasCurves(QStringLiteral("A2"), oneCurve(QStringLiteral("GR")), twoForms()));
    view.setPanel(0, p1);
    view.setPanel(1, p2);

    // 同名标志层（不同深度——典型对比场景）
    p1->canvas()->setMarkerLines({{1300.0, QStringLiteral("T35")}, {1500.0, QStringLiteral("T40")}});
    p2->canvas()->setMarkerLines({{1350.0, QStringLiteral("T35")}, {1560.0, QStringLiteral("T40")}});

    const auto pairs = view.correlationPairs();
    QCOMPARE(pairs.size(), 2); // T35-T35 + T40-T40
    QVERIFY(pairs.at(0).first == QStringLiteral("T35"));

    // 开关
    QVERIFY(view.showCorrelationLines());
    view.setShowCorrelationLines(false);
    QVERIFY(!view.showCorrelationLines());
    view.setShowCorrelationLines(true);

    // 单井模式无连线对
    view.setLayoutMode(MultiWellView::LayoutMode::Single);
    QVERIFY(view.correlationPairs().isEmpty());
  }

  // ---- D5.4 datum 校平 ----
  void testDatumAlign()
  {
    MultiWellView view;
    view.resize(1200, 800);
    view.show();
    QApplication::processEvents();
    view.setLayoutMode(MultiWellView::LayoutMode::Dual);

    auto *p1 = new WellCompositePanel(&view);
    p1->setProjectName(QStringLiteral("MW-Datum1"));
    QVERIFY(p1->loadLasCurves(QStringLiteral("A1"), oneCurve(QStringLiteral("GR")), twoForms()));
    auto *p2 = new WellCompositePanel(&view);
    p2->setProjectName(QStringLiteral("MW-Datum2"));
    QVERIFY(p2->loadLasCurves(QStringLiteral("A2"), oneCurve(QStringLiteral("GR")), twoForms()));
    view.setPanel(0, p1);
    view.setPanel(1, p2);
    p1->canvas()->setMarkerLines({{1300.0, QStringLiteral("T35")}});
    p2->canvas()->setMarkerLines({{1350.0, QStringLiteral("T35")}});

    QVERIFY(view.alignToDatum(QStringLiteral("T35")));
    QCOMPARE(view.datumMarker(), QStringLiteral("T35"));
    // 校平后：两井 T35 屏幕高度一致（40% 视口位）
    const qreal y1 = p1->canvas()->depthToY(1300.0);
    const qreal y2 = p2->canvas()->depthToY(1350.0);
    if (std::abs(y1 - y2) >= 2.0)
      qInfo() << "PROBE datum y1" << y1 << "y2" << y2
              << "scroll1" << p1->canvas()->scrollDepth() << "scroll2" << p2->canvas()->scrollDepth()
              << "ppm1" << p1->canvas()->pxPerMeter() << "ppm2" << p2->canvas()->pxPerMeter()
              << "span1" << p1->canvas()->visibleDepthSpan() << "span2" << p2->canvas()->visibleDepthSpan();
    QVERIFY(std::abs(y1 - y2) < 2.0);

    // 不存在的标志层
    QVERIFY(!view.alignToDatum(QStringLiteral("T99")));
    view.clearDatum();
    QVERIFY(view.datumMarker().isEmpty());
  }

  // ---- D5.5 井选择器 ----
  void testWellSelection()
  {
    const QStringList all = {QStringLiteral("A1"), QStringLiteral("A5"), QStringLiteral("A20"),
                             QStringLiteral("REF-1"), QStringLiteral("REF-2")};
    const QStringList refs = {QStringLiteral("REF-1"), QStringLiteral("REF-2")};
    WellSelectionDialog dlg(all, refs, {QStringLiteral("A1")});

    // 初始勾选
    QCOMPARE(dlg.selectedWells(), QStringList{QStringLiteral("A1")});
    dlg.toggleWell(QStringLiteral("A20"), true);
    dlg.toggleWell(QStringLiteral("A1"), false);
    QCOMPARE(dlg.selectedWells(), QStringList{QStringLiteral("A20")});

    // 视图侧结果集
    MultiWellView view;
    view.setAvailableWells(all);
    view.setSelectedWells(dlg.selectedWells());
    QCOMPARE(view.selectedWells(), QStringList{QStringLiteral("A20")});
    QCOMPARE(view.availableWells().size(), 5);
  }

  // ---- D5.7 厚度差表 ----
  void testDeltaTable()
  {
    MultiWellView view;
    view.resize(1200, 800);
    view.setLayoutMode(MultiWellView::LayoutMode::Dual);

    auto *p1 = new WellCompositePanel(&view);
    p1->setProjectName(QStringLiteral("MW-DT1"));
    QVERIFY(p1->loadLasCurves(QStringLiteral("A1"), oneCurve(QStringLiteral("GR")), twoForms()));
    auto *p2 = new WellCompositePanel(&view);
    p2->setProjectName(QStringLiteral("MW-DT2"));
    QVERIFY(p2->loadLasCurves(QStringLiteral("A2"), oneCurve(QStringLiteral("GR")), twoForms()));
    view.setPanel(0, p1);
    view.setPanel(1, p2);

    p1->canvas()->setMarkerLines({{1000.0, QStringLiteral("T30")}, {1200.0, QStringLiteral("T35")},
                                  {1500.0, QStringLiteral("T40")}});
    p2->canvas()->setMarkerLines({{1000.0, QStringLiteral("T30")}, {1230.0, QStringLiteral("T35")},
                                  {1470.0, QStringLiteral("T40")}});

    const QString table = view.deltaTableText();
    const QStringList rows = table.split(QLatin1Char('\n'));
    QCOMPARE(rows.size(), 3); // 表头 + 2 段（3 条标志层）
    QVERIFY(rows.at(0).contains(QStringLiteral("A1")));
    QVERIFY(rows.at(0).contains(QStringLiteral("A2")));
    // T30->T35 段：A1=200m，A2=230m
    const QString seg = rows.at(1);
    QVERIFY(seg.contains(QStringLiteral("T30->T35")));
    QVERIFY(seg.contains(QStringLiteral("200.0")));
    QVERIFY(seg.contains(QStringLiteral("230.0")));

    // 单井无表
    view.setLayoutMode(MultiWellView::LayoutMode::Single);
    QVERIFY(view.deltaTableText().isEmpty());
  }

  // ---- D5.6 对比模板持久化 ----
  void testComparisonTemplates()
  {
    const QString tmp = QDir::temp().absoluteFilePath(
        QStringLiteral("wc_tpl_%1.xml").arg(QCoreApplication::applicationPid()));
    QFile::remove(tmp);

    WellCompositeStore store(tmp);
    QVERIFY(store.load());

    QList<ComparisonTemplate> tpls;
    ComparisonTemplate t;
    t.name = QStringLiteral("连井剖面对比");
    t.wells = {QStringLiteral("A1"), QStringLiteral("A2")};
    TrackSpec spec;
    spec.typeId = QStringLiteral("curve");
    spec.title = QStringLiteral("GR");
    t.tracks = {spec};
    t.linkScroll = true;
    tpls << t;
    store.setComparisonTemplates(tpls);
    QVERIFY(store.save());

    WellCompositeStore reread(tmp);
    QVERIFY(reread.load());
    QCOMPARE(reread.comparisonTemplates().size(), 1);
    const auto &back = reread.comparisonTemplates().first();
    QCOMPARE(back.name, QStringLiteral("连井剖面对比"));
    QCOMPARE(back.wells, (QStringList{QStringLiteral("A1"), QStringLiteral("A2")}));
    QCOMPARE(back.tracks.size(), 1);
    QVERIFY(back.linkScroll);

    QFile::remove(tmp);
    QFile::remove(QDir::temp().absoluteFilePath(
        QStringLiteral("wc_tpl_%1.wc.json").arg(QCoreApplication::applicationPid())));
  }
};

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestWellCompositeMultiWell tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_wellcomposite_multiwell.moc"
