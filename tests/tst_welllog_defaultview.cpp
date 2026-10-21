// 默认测井查看模板 / 滚轮惯例 / 岩性道宽度-颜色-高度粒度 —— 纯合成数据
#include <QApplication>
#include <QPainter>
#include <QSignalSpy>
#include <QTest>
#include <QToolButton>
#include <QWheelEvent>
#include <QSet>
#include <algorithm>

#include "domain/wellcompositemodel.h"
#include "qgis/qgisruntime.h"
#include "ui/paleotheme.h"
#include "domain/lithologystyle.h"
#include "ui/wellcomposite/trackops.h"
#include "ui/wellcomposite/wellcompositecanvas.h"
#include "ui/wellcomposite/wellcompositepanel.h"
#include "ui/wellcomposite/wellcompositestore.h"
#include "ui/wellcomposite/wellcompositetrack.h"
#include "ui/wellcomposite/hiddentrackbar.h"
#include "ui/wellcomposite/trackconfigdialog.h"
#include "ui/wellcomposite/trackregistry.h"
#include <QDoubleSpinBox>
#include <QMap>

using namespace WellComposite;

namespace
{
CurveData curve(const QString &name, float lo = 0.0f, float hi = 100.0f)
{
  CurveData c;
  c.name = name;
  c.minScale = lo;
  c.maxScale = hi;
  for (int i = 0; i <= 20; ++i)
  {
    c.depths << 1000.0f + i * 10.0f;
    c.values << lo + (hi - lo) * float(i % 5) / 4.0f;
  }
  return c;
}

QStringList visibleCurveNames(const QList<std::shared_ptr<WellTrack>> &tracks, QStringList *titles = nullptr)
{
  QStringList names;
  for (const auto &t : tracks)
  {
    if (!t->isVisible() || t->type() != TrackType::Curve) continue;
    const auto ct = std::static_pointer_cast<CurveTrack>(t);
    if (titles) *titles << ct->title();
    for (const auto &c : ct->curves()) names << c.name;
  }
  return names;
}

ComprehensiveWellData syntheticWell(bool withSequence)
{
  ComprehensiveWellData d;
  d.wellName = QStringLiteral("SYN-DEFAULT");
  d.minDepth = 1000.0;
  d.maxDepth = 1200.0;
  d.continuousCurves = {curve("CALI"), curve("AC"), curve("GR"), curve("RLLD", 1, 100), curve("DEN", 2, 3), curve("SP")};
  d.formationIntervals.push_back({1000.0f, 1100.0f, QStringLiteral("合成组一"), QString(), QColor("#FFE082")});
  d.formationIntervals.push_back({1100.0f, 1200.0f, QStringLiteral("合成组二"), QString(), QColor("#C5E1A5")});
  LithologyInterval a; a.topDepth = 1000; a.bottomDepth = 1035; a.lithoName = QStringLiteral("黑色砂岩");
  LithologyInterval b; b.topDepth = 1035; b.bottomDepth = 1200; b.lithoName = QStringLiteral("灰绿色泥岩");
  d.lithologyIntervals = {a, b};
  FaciesInterval f; f.topDepth = 1000; f.bottomDepth = 1200;
  f.majorFacies = QStringLiteral("三角洲"); f.subFacies = QStringLiteral("三角洲前缘"); f.microFacies = QStringLiteral("河口坝");
  d.faciesIntervals = {f};
  if (withSequence)
    d.sequenceIntervals.push_back({1000.0f, 1200.0f, QStringLiteral("SQ1"), QStringLiteral("层序"), QColor("#D1C4E9")});
  return d;
}
} // namespace

class TestWellLogDefaultView : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase() { PaleoTheme::pinRenderEnvironment(); LithologyStyle::setWidthRules({}); }

  // ---- 1. 默认曲线选择 ----
  void grIsAlwaysOneOfThree_data()
  {
    QTest::addColumn<QStringList>("names");
    QTest::addColumn<QStringList>("expected");
    QTest::newRow("full-suite") << QStringList{"CALI", "AC", "GR", "RLLD", "DEN", "SP"}
                                << QStringList{"GR", "SP", "RLLD"};
    QTest::newRow("gr-suffix-lowercase") << QStringList{"den", "gr_edtc", "ac"}
                                         << QStringList{"gr_edtc", "den", "ac"};
    QTest::newRow("sgr-variant") << QStringList{"RT", "SGR", "CNL"} << QStringList{"SGR", "RT", "CNL"};
    QTest::newRow("no-gr") << QStringList{"CNL", "AC", "DEN", "RT"} << QStringList{"RT", "DEN", "AC"};
    QTest::newRow("unknown-fallback") << QStringList{"XYZ", "GR", "FOO"} << QStringList{"GR", "XYZ", "FOO"};
    QTest::newRow("only-two") << QStringList{"GR", "SP"} << QStringList{"GR", "SP"};
  }
  void grIsAlwaysOneOfThree()
  {
    QFETCH(QStringList, names);
    QFETCH(QStringList, expected);
    QVector<CurveData> curves;
    for (const auto &n : names) curves << curve(n);
    const auto tracks = TrackOps::standardCurveTracks(curves);
    QStringList titles;
    QCOMPARE(visibleCurveNames(tracks, &titles), expected);
    QCOMPARE(titles.size(), expected.size());
    for (const QString &t : titles)
      QCOMPARE(t, QStringLiteral("测井曲线"));
    // 其余曲线一根不丢，进隐藏辅助道
    int total = 0;
    for (const auto &t : tracks) total += std::static_pointer_cast<CurveTrack>(t)->curveCount();
    QCOMPARE(total, curves.size());
  }

  // ---- 1. 默认模板道序 ----
  void defaultTemplateOrder_data()
  {
    QTest::addColumn<bool>("withSequence");
    QTest::newRow("with-sequence") << true;
    QTest::newRow("without-sequence") << false;
  }
  void defaultTemplateOrder()
  {
    QFETCH(bool, withSequence);
    const auto tracks = TrackOps::defaultLayoutTracks(syntheticWell(withSequence), QStringLiteral("1:500"));
    QStringList visible;
    for (const auto &t : tracks)
      if (t->isVisible()) visible << t->title();
    QStringList expected{QStringLiteral("深度 (m)"), QStringLiteral("地层"), QStringLiteral("岩性"),
                         QStringLiteral("测井曲线"), QStringLiteral("测井曲线"), QStringLiteral("测井曲线"),
                         QStringLiteral("沉积相")};
    if (withSequence) expected << QStringLiteral("层序");
    QCOMPARE(visible, expected);
    for (const auto &t : tracks)
      if (t->title() == QStringLiteral("沉积相"))
        QCOMPARE(t->type(), TrackType::FaciesCompound);
  }

  // ---- 1. 恢复默认按钮 ----
  void restoreDefaultButton()
  {
    const QString project = QStringLiteral("UT-DefaultView-P");
    const QString well = QStringLiteral("SYN-DEFAULT");
    WellCompositeStore::clearSessionTracks(project, well);
    WellCompositePanel panel;
    panel.setProjectName(project);
    panel.resize(1100, 600);
    QVERIFY(panel.loadWellData(syntheticWell(true), QString(), false));
    auto *btn = panel.findChild<QToolButton *>(QStringLiteral("btnDefaultLayout"));
    QVERIFY(btn);
    QVERIFY(panel.isDefaultLayout());
    QVERIFY(!btn->isEnabled());

    // 隐藏一条测井曲线道 → 偏离默认，按钮可用
    auto *canvas = panel.canvas();
    int curveIdx = -1;
    for (int i = 0; i < canvas->trackCount(); ++i)
      if (canvas->tracks().at(i)->type() == TrackType::Curve) { curveIdx = i; break; }
    QVERIFY(curveIdx >= 0);
    canvas->tracks().at(curveIdx)->setVisible(false);
    emit canvas->trackVisibilityChanged(curveIdx);
    QVERIFY(!panel.isDefaultLayout());
    QVERIFY(btn->isEnabled());

    // 再调换道序 + 改宽度
    canvas->moveTrack(0, 2);
    canvas->tracks().at(1)->setWidth(222.0);
    emit canvas->trackOrderChanged();
    QVERIFY(btn->isEnabled());

    btn->click();
    QVERIFY(panel.isDefaultLayout());
    QVERIFY(!btn->isEnabled());
    QCOMPARE(canvas->tracks().first()->type(), TrackType::DepthScale);
    QCOMPARE(visibleCurveNames(canvas->tracks()), (QStringList{"GR", "SP", "RLLD"}));
    WellCompositeStore::clearSessionTracks(project, well);
  }

  // ---- 2. 滚轮：裸滚轮滚深度，Ctrl+滚轮改比例尺 ----
  void wheelScrollsCtrlWheelZooms()
  {
    WellCompositeCanvas canvas;
    canvas.resize(600, 500);
    canvas.setDepthRange(1000.0, 3000.0);
    auto track = std::make_shared<CurveTrack>(QStringLiteral("测井曲线"), 150.0);
    track->addCurve(curve("GR"));
    canvas.addTrack(track);
    canvas.show();
    QVERIFY(QTest::qWaitForWindowExposed(&canvas));
    canvas.setZoomFactor(4.0);
    canvas.setScrollDepth(1500.0);
    auto *body = canvas.findChild<WellCompositeBody *>();
    QVERIFY(body);
    const QPointF pos(50, 100);
    const auto send = [&](int delta, Qt::KeyboardModifiers mods) {
      QWheelEvent ev(pos, body->mapToGlobal(pos), QPoint(), QPoint(0, delta), Qt::NoButton, mods,
                     Qt::NoScrollPhase, false);
      QApplication::sendEvent(body, &ev);
    };
    const double z0 = canvas.zoomFactor();
    const double s0 = canvas.scrollDepth();
    const double span = canvas.visibleDepthSpan();
    send(-120, Qt::NoModifier); // 向下滚
    QCOMPARE(canvas.zoomFactor(), z0);
    QVERIFY2(canvas.scrollDepth() > s0, "裸滚轮向下应加深");
    QVERIFY(qAbs(canvas.scrollDepth() - s0 - span / 8.0) < span * 0.01);
    const double s1 = canvas.scrollDepth();
    send(120, Qt::NoModifier); // 向上滚回
    QVERIFY(qAbs(canvas.scrollDepth() - s0) < span * 0.01);
    Q_UNUSED(s1);

    const double s2 = canvas.scrollDepth();
    send(120, Qt::ControlModifier);
    QVERIFY2(canvas.zoomFactor() > z0, "Ctrl+滚轮向上应放大比例尺");
    const double z1 = canvas.zoomFactor();
    send(-120, Qt::ControlModifier);
    QVERIFY(canvas.zoomFactor() < z1);
    Q_UNUSED(s2);
  }

  // ---- 3. 岩性宽度表 ----
  void widthEncodesLithology()
  {
    using namespace LithologyStyle;
    const double mud = widthFraction(QStringLiteral("泥岩"));
    const double silt = widthFraction(QStringLiteral("粉砂岩"));
    const double fine = widthFraction(QStringLiteral("细砂岩"));
    const double coarse = widthFraction(QStringLiteral("粗砂岩"));
    const double cong = widthFraction(QStringLiteral("砾岩"));
    QVERIFY(mud < silt); QVERIFY(silt < fine); QVERIFY(fine < coarse); QVERIFY(coarse < cong);
    QCOMPARE(mud, 0.2); QCOMPARE(silt, 0.4); QCOMPARE(fine, 0.6); QCOMPARE(cong, 1.0);
    // 核心词在后：泥质粉砂岩=粉砂岩，粉砂质泥岩=泥岩，黑色砂岩=砂岩
    QCOMPARE(matchedWidthKeyword(QStringLiteral("泥质粉砂岩")), QStringLiteral("粉砂岩"));
    QCOMPARE(matchedWidthKeyword(QStringLiteral("粉砂质泥岩")), QStringLiteral("泥岩"));
    QCOMPARE(matchedWidthKeyword(QStringLiteral("黑色砂岩")), QStringLiteral("砂岩"));
    QCOMPARE(matchedWidthKeyword(QStringLiteral("深灰色泥灰岩")), QStringLiteral("泥灰岩"));
    QCOMPARE(widthFraction(QStringLiteral("未知岩性")), defaultWidthFraction());
    // 可配置
    setWidthRules({{QStringLiteral("泥岩"), 0.5}});
    QCOMPARE(widthFraction(QStringLiteral("泥岩")), 0.5);
    setWidthRules({});
    QCOMPARE(widthFraction(QStringLiteral("泥岩")), 0.2);
  }

  void colorFromInterpretation()
  {
    using namespace LithologyStyle;
    QString word;
    QCOMPARE(colorFromName(QStringLiteral("黑色砂岩"), &word).lightness() < 50, true);
    QCOMPARE(word, QStringLiteral("黑"));
    colorFromName(QStringLiteral("灰绿色泥岩"), &word); QCOMPARE(word, QStringLiteral("灰绿"));
    colorFromName(QStringLiteral("灰黑色页岩"), &word); QCOMPARE(word, QStringLiteral("灰黑"));
    colorFromName(QStringLiteral("深灰色泥岩"), &word); QCOMPARE(word, QStringLiteral("深灰"));
    // 「灰岩」「白云岩」「灰质泥岩」不含颜色词
    QVERIFY(!colorFromName(QStringLiteral("灰岩")).isValid());
    QVERIFY(!colorFromName(QStringLiteral("白云岩")).isValid());
    QVERIFY(!colorFromName(QStringLiteral("灰质泥岩")).isValid());
    // 无颜色词 → 区间颜色属性
    QCOMPARE(resolveFillColor(QStringLiteral("细砂岩"), QColor("#123456")), QColor("#123456"));
  }

  void grainQuantum()
  {
    QCOMPARE(LithologyStyle::heightInQuanta(35.0, 10.0), 3.5);
    QCOMPARE(LithologyStyle::heightInQuanta(35.0, 0.0), 0.0);
    LithologyTrack track(QStringLiteral("岩性"), 100.0);
    track.setGrainQuantum(10.0);
    LithologyInterval it; it.topDepth = 1000; it.bottomDepth = 1035; it.lithoName = QStringLiteral("黑色砂岩");
    track.setIntervals({it});
    const QRectF body(0, 0, 100, 400);
    const double ppm = 4.0; // 粒度 10 m = 40 px
    const QRectF r = track.blockRect(it, body, 1000.0, ppm);
    QCOMPARE(r.height(), 35.0 * ppm);           // 3.5 个粒度 × 40 px
    QCOMPARE(r.height() / (10.0 * ppm), 3.5);
    QCOMPARE(r.width(), 60.0);                   // 砂岩 3/5 道宽
    QVERIFY(track.trackToolTip(1010.0).contains(QStringLiteral("3.5")));
  }

  void lithologyRendersWidthAndColor()
  {
    LithologyTrack track(QStringLiteral("岩性"), 100.0);
    track.setGrainQuantum(10.0);
    LithologyInterval black; black.topDepth = 1000; black.bottomDepth = 1035; black.lithoName = QStringLiteral("黑色砂岩");
    LithologyInterval mud; mud.topDepth = 1035; mud.bottomDepth = 1100; mud.lithoName = QStringLiteral("泥岩");
    track.setIntervals({black, mud});
    QImage img(100, 400, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    QPainter p(&img);
    track.paintBody(p, QRectF(0, 0, 100, 400), 1000.0, 1100.0, 4.0);
    p.end();
    // 黑色砂岩块（0~60px 宽）主体偏暗；块外（>60px）是道底色
    int dark = 0, n = 0;
    for (int y = 5; y < 135; y += 3)
      for (int x = 2; x < 56; x += 3) { ++n; if (QColor(img.pixel(x, y)).lightness() < 120) ++dark; }
    QVERIFY2(dark > n / 2, qPrintable(QStringLiteral("dark=%1/%2").arg(dark).arg(n)));
    QVERIFY(QColor(img.pixel(80, 60)).lightness() > 200);
    // 泥岩只占 1/5：x=40 处应是道底色
    QVERIFY(QColor(img.pixel(40, 300)).lightness() > 200);
  }

  void grainQuantumCells_data()
  {
    QTest::addColumn<double>("thickness");
    QTest::addColumn<double>("quantum");
    QTest::addColumn<QVector<double>>("cells");
    QTest::newRow("35/10 余数单独成格") << 35.0 << 10.0 << QVector<double>{10, 10, 10, 5};
    QTest::newRow("30/10 整除无零格") << 30.0 << 10.0 << QVector<double>{10, 10, 10};
    QTest::newRow("7/10 不足一格") << 7.0 << 10.0 << QVector<double>{7};
    QTest::newRow("关闭") << 35.0 << 0.0 << QVector<double>{35};
    QTest::newRow("零厚度") << 0.0 << 10.0 << QVector<double>{};
  }
  void grainQuantumCells()
  {
    QFETCH(double, thickness);
    QFETCH(double, quantum);
    QFETCH(QVector<double>, cells);
    const QVector<double> got = LithologyTrack::quantumCells(thickness, quantum);
    QCOMPARE(got.size(), cells.size());
    for (int i = 0; i < got.size(); ++i)
      QVERIFY2(qAbs(got.at(i) - cells.at(i)) < 1e-9, qPrintable(QString::number(got.at(i))));
    if (quantum > 0 && !got.isEmpty())
      QCOMPARE(got.last() / quantum, cells.last() / quantum); // 末格 0.5 个粒度
  }

  void grainQuantumSeparatorsSpanBlock()
  {
    // 开/关粒度对比：差异 = 格间分隔线，横贯岩性块全宽（泥岩 1/5 = 20 px），只在 40/80/120 px 三行
    LithologyInterval mud; mud.topDepth = 1000; mud.bottomDepth = 1035; mud.lithoName = QStringLiteral("浅灰色泥岩");
    const auto render = [&](double quantum, double ppm) {
      LithologyTrack track(QStringLiteral("岩性"), 100.0);
      track.setGrainQuantum(quantum);
      track.setIntervals({mud});
      QImage img(100, 200, QImage::Format_ARGB32_Premultiplied);
      img.fill(Qt::white);
      QPainter p(&img);
      track.paintBody(p, QRectF(0, 0, 100, 200), 1000.0, 1050.0, ppm);
      return img;
    };
    const QImage on = render(10.0, 4.0), off = render(0.0, 4.0);
    QMap<int, int> diffPerRow;
    for (int y = 0; y < on.height(); ++y)
      for (int x = 0; x < on.width(); ++x)
        if (on.pixel(x, y) != off.pixel(x, y))
        {
          QVERIFY2(x <= 20, qPrintable(QStringLiteral("x=%1 超出岩性块").arg(x)));
          ++diffPerRow[y];
        }
    QSet<int> bands;
    for (auto it = diffPerRow.cbegin(); it != diffPerRow.cend(); ++it)
    {
      QVERIFY2(it.key() >= 39 && it.key() <= 121, qPrintable(QStringLiteral("y=%1").arg(it.key())));
      bands.insert(qRound(it.key() / 40.0));
    }
    QCOMPARE(bands, (QSet<int>{1, 2, 3})); // 4 格 → 3 条分隔线；余数格(5 m)之后无线
    for (int band : {1, 2, 3})
    {
      int widest = 0;
      for (auto it = diffPerRow.cbegin(); it != diffPerRow.cend(); ++it)
        if (qRound(it.key() / 40.0) == band) widest = qMax(widest, it.value());
      QVERIFY2(widest >= 16, qPrintable(QStringLiteral("band %1 width %2").arg(band).arg(widest))); // 全宽线
    }
    // 跳过规则：每格 < 6 px（粒度 1 m × 4 px/m）时不画
    QCOMPARE(render(1.0, 4.0), render(0.0, 4.0));
  }

  void grainQuantumSettingRoundTrip()
  {
    // 注册表 capture/create
    auto lt = std::make_shared<LithologyTrack>(QStringLiteral("岩性"), 90.0);
    lt->setGrainQuantum(10.0);
    TrackSpec spec = TrackRegistry::instance().captureSpec(lt);
    QCOMPARE(spec.params.value(QStringLiteral("grainQuantum")).toDouble(), 10.0);
    const auto back = std::dynamic_pointer_cast<LithologyTrack>(
        TrackRegistry::instance().createTrack(TrackSpec::fromVariantMap(spec.toVariantMap())));
    QVERIFY(back);
    QCOMPARE(back->grainQuantum(), 10.0);
    // 道配置对话框
    TrackConfigDialog dlg(spec, {});
    auto *spin = dlg.findChild<QDoubleSpinBox *>(QStringLiteral("lithoGrainQuantum"));
    QVERIFY(spin);
    QCOMPARE(spin->value(), 10.0);
    spin->setValue(5.0);
    dlg.accept();
    QCOMPARE(dlg.resultSpec().params.value(QStringLiteral("grainQuantum")).toDouble(), 5.0);
    // 曲线道无此项
    TrackSpec curveSpec; curveSpec.typeId = QStringLiteral("curve"); curveSpec.title = QStringLiteral("测井曲线");
    TrackConfigDialog curveDlg(curveSpec, {});
    QVERIFY(!curveDlg.findChild<QDoubleSpinBox *>(QStringLiteral("lithoGrainQuantum")));

    // 面板：改粒度 = 偏离默认 → 「恢复默认」可用，恢复后回 1 m；会话记忆随布局持久化
    const QString project = QStringLiteral("UT-Quantum-P");
    WellCompositeStore::clearSessionTracks(project, QStringLiteral("SYN-DEFAULT"));
    {
      WellCompositePanel panel;
      panel.setProjectName(project);
      QVERIFY(panel.loadWellData(syntheticWell(false), QString(), false));
      std::shared_ptr<LithologyTrack> litho;
      for (const auto &t : panel.canvas()->tracks())
        if (t->type() == TrackType::Lithology) litho = std::static_pointer_cast<LithologyTrack>(t);
      QVERIFY(litho);
      QCOMPARE(litho->grainQuantum(), 1.0);
      litho->setGrainQuantum(10.0);
      emit panel.canvas()->trackWidthChanged(0); // 面板记忆落盘入口
      QVERIFY(!panel.isDefaultLayout());
    }
    {
      WellCompositePanel panel;
      panel.setProjectName(project);
      QVERIFY(panel.loadWellData(syntheticWell(false), QString(), false));
      std::shared_ptr<LithologyTrack> litho;
      for (const auto &t : panel.canvas()->tracks())
        if (t->type() == TrackType::Lithology) litho = std::static_pointer_cast<LithologyTrack>(t);
      QCOMPARE(litho->grainQuantum(), 10.0); // 重开恢复
      auto *btn = panel.findChild<QToolButton *>(QStringLiteral("btnDefaultLayout"));
      QVERIFY(btn->isEnabled());
      btn->click();
      for (const auto &t : panel.canvas()->tracks())
        if (t->type() == TrackType::Lithology)
          QCOMPARE(std::static_pointer_cast<LithologyTrack>(t)->grainQuantum(), 1.0);
      QVERIFY(!btn->isEnabled());
    }
    WellCompositeStore::clearSessionTracks(project, QStringLiteral("SYN-DEFAULT"));
  }

  void allCurveTracksNamedAndChipsDisambiguated()
  {
    QVector<CurveData> curves;
    for (const QString &n : QStringList{"GR", "SP", "RLLD", "GR_2", "CO2", "AZIM", "XYZ", "ABC", "DEF"})
      curves << curve(n);
    const auto tracks = TrackOps::standardCurveTracks(curves);
    int hidden = 0;
    QList<TrackSpec> specs;
    for (const auto &t : tracks)
    {
      QCOMPARE(t->title(), QStringLiteral("测井曲线")); // 含隐藏辅助道
      if (!t->isVisible()) ++hidden;
      specs << TrackRegistry::instance().captureSpec(t);
    }
    QVERIFY(hidden >= 1);
    TrackSpec one; one.typeId = QStringLiteral("curve"); one.title = QStringLiteral("测井曲线");
    one.setCurveNames({QStringLiteral("SP")});
    QCOMPARE(HiddenTrackBar::chipText(one), QStringLiteral("测井曲线 · SP"));
    TrackSpec plain; plain.typeId = QStringLiteral("formation"); plain.title = QStringLiteral("砂层组");
    QCOMPARE(HiddenTrackBar::chipText(plain), QStringLiteral("砂层组"));

    // 隐藏道条按序号精确恢复同名道
    HiddenTrackBar bar;
    bar.setTracks(specs);
    QSignalSpy spy(&bar, &HiddenTrackBar::trackRestoreIndexRequested);
    const auto chips = bar.findChildren<QToolButton *>();
    QVERIFY(!chips.isEmpty());
    QVERIFY(chips.first()->text().startsWith(QStringLiteral("测井曲线 · ")));
    chips.first()->click();
    QCOMPARE(spy.count(), 1);
    QVERIFY(!specs.at(spy.first().first().toInt()).visible);
  }
};

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestWellLogDefaultView tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_welllog_defaultview.moc"
