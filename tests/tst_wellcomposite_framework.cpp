#include <QSignalSpy>
#include <QTest>
#include <QDir>
#include <QJsonArray>
#include <QFile>
#include <QPainter>
#include <QTableWidget>
#include <QToolButton>
#include <QMenu>
#include <QLayout>

#include <cmath>
#include <limits>

#include "ui/wellcomposite/trackregistry.h"
#include "ui/wellcomposite/curveconfigdialog.h"
#include "ui/paleotheme.h"
#include "ui/wellcomposite/trackops.h"
#include "ui/wellcomposite/wellcompositetrack.h"
#include "ui/wellcomposite/wellcompositecanvas.h"
#include "ui/wellcomposite/hiddentrackbar.h"
#include "ui/wellcomposite/trackconfigdialog.h"
#include "ui/wellcomposite/wellcompositestore.h"
#include "ui/wellcomposite/wellcompositepanel.h"
#include "domain/wellcompositemodel.h"
#include "qgis/qgisruntime.h"

using namespace WellComposite;

// wave/wellcomposite-deep — D1.x 道系统框架测试
class TestWellCompositeFramework : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase() { PaleoTheme::pinRenderEnvironment(); }
  void testStandardLayoutKeepsAuxiliaryCurvesAvailable()
  {
    QVector<CurveData> curves;
    for (const QString &name : QStringList{"GR", "SP", "CALI", "AC", "DEN", "CNL",
                                          "RLLD", "RLLS", "RXO", "GR_2", "AZIM", "CO2", "UNKNOWN"})
    {
      CurveData c; c.name = name; c.depths = {1000, 1010}; c.values = {10, 20};
      curves << c;
    }
    CurveData lab = curves.first(); lab.name = "POR"; lab.mode = CurveDisplayMode::Discrete;
    curves << lab;
    WellCompositeCanvas canvas;
    const auto tracks = TrackOps::standardCurveTracks(curves);
    QStringList visibleNames, allNames;
    int visible = 0;
    for (const auto &track : tracks)
    {
      canvas.addTrack(track);
      if (track->isVisible()) ++visible;
      for (const auto &c : std::static_pointer_cast<CurveTrack>(track)->curves())
      {
        allNames << c.name;
        if (track->isVisible()) visibleNames << c.name;
      }
    }
    QCOMPARE(visible, 3);
    // 默认三个单曲线「测井曲线」道：GR 必选，其后 SP、深电阻率
    QCOMPARE(visibleNames, (QStringList{"GR", "SP", "RLLD"}));
    for (const auto &track : tracks)
      if (track->isVisible()) QCOMPARE(track->title(), QStringLiteral("测井曲线"));
    QCOMPARE(allNames.size(), curves.size());
    QVERIFY(allNames.contains("GR_2"));
    QVERIFY(allNames.contains("POR"));
    CurveConfigDialog dialog(&canvas);
    dialog.onResetDefault(); dialog.applyConfiguration();
    int restored = 0;
    for (const auto &track : canvas.tracks())
      if (track->isVisible()) ++restored;
    QCOMPARE(restored, 3);
    const auto fallback = TrackOps::standardCurveTracks({curves.last()});
    QCOMPARE(fallback.size(), 1);
    QVERIFY(fallback.first()->isVisible());
  }

  void testHiddenAuxiliaryTracksStayCompact()
  {
    HiddenTrackBar bar;
    QList<TrackSpec> specs;
    for (int i = 0; i < 30; ++i)
    {
      TrackSpec spec; spec.typeId = "curve"; spec.title = QString::number(i); spec.visible = false;
      specs << spec;
    }
    bar.setTracks(specs);
    QCOMPARE(bar.hiddenCount(), 30);
    QCOMPARE(bar.findChildren<QToolButton *>().size(), 4);
    QCOMPARE(bar.findChild<QMenu *>()->actions().size(), 27);
    QSignalSpy restored(&bar, &HiddenTrackBar::trackRestoreRequested);
    bar.findChild<QMenu *>()->actions().last()->trigger();
    QTRY_COMPARE(restored.size(), 1);
    QCOMPARE(restored.first().first().toString(), QStringLiteral("29"));
    const int items = bar.layout()->count();
    bar.setTracks(specs);
    QCOMPARE(bar.layout()->count(), items);
  }

  void testLasAndCompositeUseTheSameDefaultTracks()
  {
    ComprehensiveWellData data;
    data.wellName = "UT-StandardComposite"; data.minDepth = 1000; data.maxDepth = 1100;
    for (const QString &name : QStringList{"CO2", "ZDEN", "CNCF", "GR", "AC", "RLLD", "AZIM", "SP"})
    {
      CurveData c; c.name = name;
      c.color = QColor::fromHsv((data.continuousCurves.size() * 67 + 100) % 360, 180, 160);
      for (int i = 0; i <= 1000; ++i)
      {
        c.depths << 1000.0f + i * .1f;
        c.values << static_cast<float>(50 + 30 * std::sin(i * .025 + data.continuousCurves.size()));
      }
      data.continuousCurves << c;
    }
    WellCompositeStore::clearSessionTracks({}, data.wellName);
    WellCompositeStore::clearSessionTracks({}, "UT-StandardLas");
    WellCompositePanel xml, las;
    QVERIFY(xml.loadWellData(data));
    QVERIFY(las.loadLasCurves("UT-StandardLas", data.continuousCurves, {}));
    auto names = [](const WellCompositePanel &panel) {
      QStringList out;
      for (const auto &track : panel.canvas()->tracks())
        if (track->type() == TrackType::Curve && track->isVisible())
          for (const auto &c : std::static_pointer_cast<CurveTrack>(track)->curves()) out << c.name;
      return out;
    };
    QCOMPARE(names(xml), names(las));
    QCOMPARE(names(xml), (QStringList{"GR", "SP", "RLLD"})); // 默认三曲线：GR + SP + 深电阻率
    const QString screenshot = qEnvironmentVariable("PALEO_WELL_LAYOUT_SCREENSHOT");
    if (!screenshot.isEmpty())
    {
      xml.resize(1100, 720); xml.canvas()->setScaleRatio(QStringLiteral("自适应")); xml.show(); QTest::qWait(100);
      QVERIFY(xml.grab().save(screenshot));
    }
  }

  void testCurveGridUsesDepthCoordinatesAndCanBeDisabled()
  {
    CurveTrack track;
    track.setGridDensity(3);
    const auto paper = PaleoTheme::tokens(PaleoTheme::Theme::Light).surface;
    auto render = [&track](double top) {
      QImage image(180, 180, QImage::Format_RGB32); image.fill(Qt::magenta);
      QPainter painter(&image);
      track.paintBody(painter, QRectF(0, 0, 180, 180), top, top + 18, 10);
      return image;
    };
    const auto before = render(1000);
    QVERIFY(before.pixelColor(90, 25) != paper); // 纵线
    QVERIFY(before.pixelColor(25, 50) != paper); // 1005 m 主深度线
    QVERIFY(before.pixelColor(25, 25) == paper);
    const auto scrolled = render(1000.5);
    QCOMPARE(before.pixelColor(25, 50), scrolled.pixelColor(25, 45));
    track.setShowGrid(false);
    const auto disabled = render(1000);
    QCOMPARE(disabled.pixelColor(90, 25), paper);
    QCOMPARE(disabled.pixelColor(25, 50), paper);
  }

  // ---- D1.2 注册表 ----
  void testRegistryBuiltins()
  {
    auto &reg = TrackRegistry::instance();
    // 词表全覆盖：Curve/Litho/Facies/Strat/Discrete/Text/Depth/GR + 既有 6 类
    const QStringList expected = {QStringLiteral("depth"),  QStringLiteral("text"),
                                  QStringLiteral("formation"), QStringLiteral("litho"),
                                  QStringLiteral("core"),   QStringLiteral("image"),
                                  QStringLiteral("curve"),  QStringLiteral("symbol"),
                                  QStringLiteral("strat"),  QStringLiteral("facies"),
                                  QStringLiteral("gr"),     QStringLiteral("discrete")};
    for (const QString &id : expected)
    {
      QVERIFY2(reg.contains(id), qPrintable(QStringLiteral("缺类型 %1").arg(id)));
      QVERIFY(!reg.displayName(id).isEmpty());
    }
    // depth 标尺道不可由用户新建
    QVERIFY(reg.userCreatableTypeIds().contains(QStringLiteral("curve")));
    QVERIFY(!reg.userCreatableTypeIds().contains(QStringLiteral("depth")));
    // 旧枚举映射往返
    QCOMPARE(TrackRegistry::enumForTypeId(QStringLiteral("facies")), TrackType::FaciesCompound);
    QCOMPARE(TrackRegistry::typeIdForEnum(TrackType::DepthScale), QStringLiteral("depth"));
  }

  void testRegistryCreateAndCapture()
  {
    auto &reg = TrackRegistry::instance();

    TrackSpec spec;
    spec.typeId = QStringLiteral("litho");
    spec.title = QStringLiteral("岩性剖面-A");
    spec.width = 95.0;
    spec.visible = false;
    spec.printIncluded = false;
    auto track = reg.createTrack(spec);
    QVERIFY(track);
    QCOMPARE(track->type(), TrackType::Lithology);
    QCOMPARE(track->title(), QStringLiteral("岩性剖面-A"));
    QCOMPARE(track->width(), 95.0);
    QVERIFY(!track->isVisible());
    QVERIFY(!track->isPrintIncluded());

    // capture 往返：渲染器 → spec 再建
    const TrackSpec back = reg.captureSpec(track);
    QCOMPARE(back.typeId, QStringLiteral("litho"));
    QCOMPARE(back.title, QStringLiteral("岩性剖面-A"));
    QCOMPARE(back.visible, false);
    QCOMPARE(back.printIncluded, false);

    auto track2 = reg.createTrack(back);
    QVERIFY(track2);
    QCOMPARE(track2->title(), track->title());

    // 未知类型返回空
    TrackSpec bogus;
    bogus.typeId = QStringLiteral("no-such-type");
    QVERIFY(!reg.createTrack(bogus));
  }

  void testTrackSpecRoundtrip()
  {
    TrackSpec spec;
    spec.typeId = QStringLiteral("curve");
    spec.title = QStringLiteral("三孔隙");
    spec.width = 180.0;
    spec.setCurveNames({QStringLiteral("AC"), QStringLiteral("DEN")});
    QVariantMap ov;
    QVariantMap one;
    one.insert(QStringLiteral("min"), 2.0);
    one.insert(QStringLiteral("max"), 3.0);
    ov.insert(QStringLiteral("DEN"), one);
    spec.setCurveOverrides(ov);
    spec.params.insert(QStringLiteral("showGrid"), false);
    spec.params.insert(QStringLiteral("gridDensity"), 3);

    const QVariantMap m = spec.toVariantMap();
    const TrackSpec back = TrackSpec::fromVariantMap(m);
    QCOMPARE(back.typeId, spec.typeId);
    QCOMPARE(back.title, spec.title);
    QCOMPARE(back.width, spec.width);
    QCOMPARE(back.curveNames(), spec.curveNames());
    QCOMPARE(back.curveOverrides().value(QStringLiteral("DEN")).toMap().value(QStringLiteral("max")).toDouble(), 3.0);
    QCOMPARE(back.showGrid(), false);
    QCOMPARE(back.gridDensity(), 3);
  }

  void testCurveOverrideApply()
  {
    CurveData c;
    c.name = QStringLiteral("RT");
    c.minScale = 0.0f;
    c.maxScale = 100.0f;
    c.unit = QStringLiteral("Ω·m");
    c.color = QColor(QStringLiteral("#000000"));

    QVariantMap ov;
    ov.insert(QStringLiteral("min"), 0.1);
    ov.insert(QStringLiteral("max"), 1000.0);
    ov.insert(QStringLiteral("log"), true);
    ov.insert(QStringLiteral("color"), QStringLiteral("#7B1FA2"));

    const CurveData over = applyCurveOverride(c, ov);
    QCOMPARE(over.minScale, 0.1f);
    QCOMPARE(over.maxScale, 1000.0f);
    QVERIFY(over.isLogarithmic);
    QCOMPARE(over.color, QColor(QStringLiteral("#7B1FA2")));
    // 未覆盖字段保留
    QCOMPARE(over.unit, QStringLiteral("Ω·m"));
    // 空覆盖 = 原样
    const CurveData same = applyCurveOverride(c, {});
    QCOMPARE(same.minScale, 0.0f);
  }

  // ---- D1.5/D1.9 TrackOps ----
  void testTrackCsvExport()
  {
    CurveTrack ct(QStringLiteral("测井"), 180.0);
    CurveData c1;
    c1.name = QStringLiteral("GR");
    c1.unit = QStringLiteral("API");
    c1.depths = {1000.0f, 1000.5f, 1001.0f};
    c1.values = {45.0f, std::numeric_limits<float>::quiet_NaN(), 55.0f};
    ct.addCurve(c1);

    const QString csv = TrackOps::exportTrackCsv(ct);
    const QStringList lines = csv.split(QLatin1Char('\n'));
    QCOMPARE(lines.at(0), QStringLiteral("depth,GR(API)"));
    // NaN 行留空
    QVERIFY(lines.at(2).contains(QStringLiteral(",,")) || lines.at(2).endsWith(QLatin1Char(',')));
    QVERIFY(lines.at(3).contains(QStringLiteral("55")));

    // 区间道
    FormationTrack ft(QStringLiteral("地层"), 75.0);
    FormationInterval fi;
    fi.topDepth = 1000.0f;
    fi.bottomDepth = 1200.0f;
    fi.name = QStringLiteral("珠江组");
    ft.addInterval(fi);
    const QString csv2 = TrackOps::exportTrackCsv(ft);
    QVERIFY(csv2.startsWith(QStringLiteral("top,bottom,name")));
    QVERIFY(csv2.contains(QStringLiteral("珠江组")));
  }

  void testDuplicateTrack()
  {
    LithologyTrack lt(QStringLiteral("岩性"), 80.0);
    LithologyInterval li;
    li.topDepth = 1000.0f;
    li.bottomDepth = 1100.0f;
    li.lithoName = QStringLiteral("细砂岩");
    lt.addInterval(li);

    auto dup = TrackOps::duplicateTrack(std::make_shared<LithologyTrack>(lt));
    QVERIFY(dup);
    QCOMPARE(dup->type(), TrackType::Lithology);
    auto *dupLitho = dynamic_cast<LithologyTrack *>(dup.get());
    QVERIFY(dupLitho);
    QCOMPARE(dupLitho->intervals().size(), 1);
    QCOMPARE(dupLitho->intervals().first().lithoName, QStringLiteral("细砂岩"));
    // 深拷贝：改副本不影响原件
    dupLitho->intervals().first().lithoName = QStringLiteral("泥岩");
    QCOMPARE(lt.intervals().first().lithoName, QStringLiteral("细砂岩"));
  }

  void testInjectCurves()
  {
    CurveData c1;
    c1.name = QStringLiteral("GR");
    c1.depths = {1000.0f};
    c1.values = {50.0f};
    CurveData c2;
    c2.name = QStringLiteral("RT");
    c2.depths = {1000.0f};
    c2.values = {10.0f};
    CurveData d1;
    d1.name = QStringLiteral("CPOR");
    d1.depths = {1005.0f};
    d1.values = {18.0f};
    d1.mode = CurveDisplayMode::Discrete;

    TrackSpec spec;
    spec.typeId = QStringLiteral("curve");
    spec.setCurveNames({QStringLiteral("GR"), QStringLiteral("RT")});
    CurveTrack track(QStringLiteral("组合"), 180.0);
    QCOMPARE(TrackOps::injectCurvesFromData(track, spec, {c1, c2}, {d1}), 2);
    QCOMPARE(track.curveCount(), 2);
    QCOMPARE(track.curves().first().name, QStringLiteral("GR"));

    // discrete 道从离散池取
    TrackSpec dspec;
    dspec.typeId = QStringLiteral("discrete");
    dspec.setCurveNames({QStringLiteral("CPOR")});
    CurveTrack dtrack(QStringLiteral("实测"), 160.0);
    QCOMPARE(TrackOps::injectCurvesFromData(dtrack, dspec, {c1, c2}, {d1}), 1);
    QCOMPARE(dtrack.curves().first().mode, CurveDisplayMode::Discrete);

    // 找不到的名字静默跳过
    TrackSpec missing;
    missing.typeId = QStringLiteral("curve");
    missing.setCurveNames({QStringLiteral("NOPE")});
    QCOMPARE(TrackOps::injectCurvesFromData(track, missing, {c1, c2}, {}), 0);
  }

  // ---- D1.10 隐藏道管理条 ----
  void testHiddenBarChips()
  {
    HiddenTrackBar bar;
    QList<TrackSpec> specs;
    TrackSpec s1;
    s1.typeId = QStringLiteral("litho");
    s1.title = QStringLiteral("岩性");
    s1.visible = false;
    TrackSpec s2;
    s2.typeId = QStringLiteral("curve");
    s2.title = QStringLiteral("GR");
    s2.visible = true;
    TrackSpec s3;
    s3.typeId = QStringLiteral("text");
    s3.title = QStringLiteral("结论");
    s3.visible = false;
    specs << s1 << s2 << s3;

    bar.setTracks(specs);
    QCOMPARE(bar.hiddenCount(), 2);
    QVERIFY(!bar.isHidden()); // 有隐藏道 → 显示

    QSignalSpy spy(&bar, &HiddenTrackBar::trackRestoreRequested);
    const auto chips = bar.findChildren<QToolButton *>();
    QCOMPARE(chips.size(), 2);
    // 点击第一个 chip（岩性）→ 恢复请求
    chips.at(0)->click();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).toString(), QStringLiteral("岩性"));

    // 全可见 → 自动隐藏（直接改列表元素——specs 按值持有）
    specs[0].visible = true;
    specs[2].visible = true;
    bar.setTracks(specs);
    QVERIFY(bar.isHidden());
    QCOMPARE(bar.hiddenCount(), 0);
  }

  // ---- D1.6/D1.7 道配置对话框 ----
  void testTrackConfigDialogResult()
  {
    CurveData gr;
    gr.name = QStringLiteral("GR");
    gr.minScale = 0.0f;
    gr.maxScale = 150.0f;
    gr.unit = QStringLiteral("API");

    TrackSpec spec;
    spec.typeId = QStringLiteral("curve");
    spec.title = QStringLiteral("GR 道");
    spec.width = 150.0;
    spec.setCurveNames({QStringLiteral("GR")});

    TrackConfigDialog dlg(spec, {gr});
    // 非曲线族配置区不显示（本例是 curve，应显示）
    QVERIFY(dlg.findChildren<QTableWidget *>().size() >= 1);

    // 直接槽位改值后 gatherResult（accept 会 exec？不——accept() 直接收结果）
    dlg.setCurveRowChecked(0, true);
    dlg.accept();
    const TrackSpec result = dlg.resultSpec();
    QCOMPARE(result.title, QStringLiteral("GR 道"));
    QCOMPARE(result.width, 150.0);
    QCOMPARE(result.curveNames(), QStringList{QStringLiteral("GR")});
  }

  void testConfigDialogCurveEditor()
  {
    QVector<CurveData> pool;
    for (const char *n : {"GR", "AC", "DEN", "RT", "CNL"})
    {
      CurveData c;
      c.name = QLatin1String(n);
      c.minScale = 0.0f;
      c.maxScale = 200.0f;
      c.depths = {1000.0f, 1001.0f};
      c.values = {1.0f, 2.0f};
      pool << c;
    }

    TrackSpec spec;
    spec.typeId = QStringLiteral("curve");
    spec.title = QStringLiteral("组合道");
    spec.setCurveNames({QStringLiteral("GR")});

    TrackConfigDialog dlg(spec, pool);
    // D1.7 选 4 根曲线进同一道
    for (int r = 0; r < 4; ++r)
      dlg.setCurveRowChecked(r, true);
    dlg.accept();

    const TrackSpec result = dlg.resultSpec();
    QCOMPARE(result.curveNames().size(), 4);
    QVERIFY(result.curveNames().contains(QStringLiteral("RT")));
    // 网格开关默认开
    QVERIFY(result.showGrid());
  }

  // ---- D1.8 会话持久化 ----
  void testSessionPersistence()
  {
    const QString project = QStringLiteral("UnitTestProject");
    const QString well = QStringLiteral("W-01");

    QList<TrackSpec> specs;
    TrackSpec s1;
    s1.typeId = QStringLiteral("depth");
    s1.title = QStringLiteral("深度 (m)");
    s1.width = 70.0;
    TrackSpec s2;
    s2.typeId = QStringLiteral("curve");
    s2.title = QStringLiteral("GR");
    s2.width = 175.0;
    s2.visible = false;
    s2.setCurveNames({QStringLiteral("GR")});
    specs << s1 << s2;

    WellCompositeStore::saveSessionTracks(project, well, specs);
    const QList<TrackSpec> back = WellCompositeStore::loadSessionTracks(project, well);
    QCOMPARE(back.size(), 2);
    QCOMPARE(back.at(1).title, QStringLiteral("GR"));
    QCOMPARE(back.at(1).visible, false);
    QCOMPARE(back.at(1).width, 175.0);
    QCOMPARE(back.at(1).curveNames(), QStringList{QStringLiteral("GR")});

    // 无记忆的井返回空
    QVERIFY(WellCompositeStore::loadSessionTracks(project, QStringLiteral("NO-WELL")).isEmpty());

    // 清除
    WellCompositeStore::clearSessionTracks(project, well);
    QVERIFY(WellCompositeStore::loadSessionTracks(project, well).isEmpty());
  }

  void testWidthMemory()
  {
    QVariantMap widths;
    widths.insert(QStringLiteral("岩性分析"), 99.0);
    widths.insert(QStringLiteral("取心数据"), 65.0);
    WellCompositeStore::saveWidthSet(QStringLiteral("P1"), QStringLiteral("W2"), widths);
    const QVariantMap back = WellCompositeStore::loadWidthSet(QStringLiteral("P1"), QStringLiteral("W2"));
    QCOMPARE(back.value(QStringLiteral("岩性分析")).toDouble(), 99.0);
    QCOMPARE(back.size(), 2);
  }

  // ---- 会话曲线配置恢复：重开后道内曲线集与用户最后一次 TrackConfig 一致 ----
  void testSessionCurveConfigRestore_data()
  {
    QTest::addColumn<QString>("trackTitle");
    QTest::newRow("existing-title") << QStringLiteral("岩性测井");
    QTest::newRow("custom-title") << QStringLiteral("自定义曲线组合");
  }
  void testSessionCurveConfigRestore()
  {
    QFETCH(QString, trackTitle);
    const QString project = QStringLiteral("UT-CurveRestore-P");
    const QString well = QStringLiteral("UT-CurveRestore-W");
    WellCompositeStore::clearSessionTracks(project, well);

    auto mk = [](const char *name, float lo, float hi) {
      CurveData c;
      c.name = QString::fromLatin1(name);
      c.unit = QStringLiteral("API");
      c.minScale = lo;
      c.maxScale = hi;
      c.depths = {1000.0f, 1100.0f};
      c.values = {lo, hi};
      return c;
    };
    QVector<CurveData> curves;
    curves << mk("GR", 0.0f, 150.0f) << mk("DEN", 1.9f, 2.7f)
           << mk("RT", 1.0f, 100.0f);
    QVector<FormationInterval> forms;
    forms.push_back({1000.0f, 1300.0f, QStringLiteral("组一"), QString(),
                     QColor(QStringLiteral("#FFE082"))});

    // 用户把默认「岩性测井」道（GR）改为 DEN+RT 并收窄 DEN 量程，记忆落盘
    TrackSpec edited;
    edited.typeId = QStringLiteral("curve");
    edited.title = trackTitle;
    edited.width = 180.0;
    edited.setCurveNames({QStringLiteral("DEN"), QStringLiteral("RT")});
    QVariantMap denOverride;
    denOverride.insert(QStringLiteral("min"), 2.0);
    denOverride.insert(QStringLiteral("max"), 2.5);
    QVariantMap overrides;
    overrides.insert(QStringLiteral("DEN"), denOverride);
    edited.params.insert(QStringLiteral("curveOverrides"), overrides);
    WellCompositeStore::saveSessionTracks(project, well, {edited});

    // 重开：默认分组后 restoreSessionState 须把记忆曲线注入对应道
    WellCompositePanel panel;
    panel.setProjectName(project);
    QVERIFY(panel.loadLasCurves(well, curves, forms));
    auto *canvas = panel.findChild<WellCompositeCanvas *>();
    QVERIFY(canvas);
    std::shared_ptr<CurveTrack> litho;
    for (const auto &t : canvas->tracks())
    {
      if (t && t->title() == trackTitle)
        litho = std::dynamic_pointer_cast<CurveTrack>(t);
    }
    QVERIFY2(litho, "会话须恢复原始标题与曲线配置");
    QCOMPARE(litho->curves().size(), 2);
    QCOMPARE(litho->curves().at(0).name, QStringLiteral("DEN"));
    QCOMPARE(litho->curves().at(1).name, QStringLiteral("RT"));
    QCOMPARE(litho->curves().at(0).minScale, 2.0f);
    QCOMPARE(litho->curves().at(0).maxScale, 2.5f);

    WellCompositeStore::clearSessionTracks(project, well);
  }

  // ---- D1.11 道头三行区 ----
  void testHeaderChromeElide()
  {
    TextTrack tt(QStringLiteral("超长道标题文本用于截断测试的道名"), 40.0);
    QImage img(40, 72, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    QPainter p(&img);
    tt.paintHeader(p, QRectF(0, 0, 40, 72), -1.0);
    p.end();
    QVERIFY(!img.isNull()); // 绘制不崩溃即通过（截断逻辑在 paintHeaderChrome 内）

    // 曲线道三行区文本
    CurveTrack ct(QStringLiteral("测井"), 170.0);
    CurveData c;
    c.name = QStringLiteral("GR");
    c.unit = QStringLiteral("API");
    c.minScale = 0.0f;
    c.maxScale = 150.0f;
    c.depths = {1000.0f};
    c.values = {50.0f};
    ct.addCurve(c);
    QCOMPARE(ct.headerScaleText(), QStringLiteral("0.0~150"));
    QCOMPARE(ct.headerUnitText(), QStringLiteral("API"));
  }


  // ---- sidecar 全段持久化往返（D2.3/D2.6/D3.6/D3.12/D4.10/D5.6）----
  void testSidecarRoundtrip()
  {
    const QString tmp = QDir::temp().absoluteFilePath(
        QStringLiteral("wc_framework_sidecar_%1.xml").arg(QCoreApplication::applicationPid()));
    QFile::remove(tmp);
    QFile::remove(tmp.left(tmp.lastIndexOf(QLatin1Char('.'))) + QStringLiteral(".wc.json"));

    WellCompositeStore store(tmp);
    QVERIFY(store.load()); // 不存在 sidecar = 合法空文档

    // 钉注
    QList<DepthPin> pins;
    pins << DepthPin{1234.5, QStringLiteral("油层顶")} << DepthPin{1300.0, QString()};
    store.setPins(pins);
    // 书签
    QList<DepthBookmark> bms;
    bms << DepthBookmark{QStringLiteral("目的层"), 2000.0};
    store.setBookmarks(bms);
    // 地层指派
    QList<StratAssignment> assigns;
    StratAssignment a;
    a.layerName = QStringLiteral("A");
    a.system = QStringLiteral("古近系");
    a.series = QStringLiteral("始新统");
    a.formation = QStringLiteral("文昌组");
    assigns << a;
    store.setStratAssignments(assigns);
    // 曲线覆盖层
    QVariantMap ov;
    QVariantMap one;
    one.insert(QStringLiteral("min"), 0.2);
    one.insert(QStringLiteral("max"), 2000.0);
    one.insert(QStringLiteral("log"), true);
    one.insert(QStringLiteral("unit"), QStringLiteral("Ω·m"));
    one.insert(QStringLiteral("color"), QStringLiteral("#7B1FA2"));
    ov.insert(QStringLiteral("RT"), one);
    store.setCurveOverrides(ov);
    // 审计
    store.appendAuditEntry(QStringLiteral("2026-09-29T12:00:00"), QStringLiteral("marker.move"),
                           QStringLiteral("T35: 1500 -> 1520"));
    QVERIFY(store.save());

    // 重载等价
    WellCompositeStore reread(tmp);
    QVERIFY(reread.load());
    QCOMPARE(reread.pins(), pins);
    QCOMPARE(reread.bookmarks(), bms);
    QCOMPARE(reread.stratAssignments(), assigns);
    QCOMPARE(reread.curveOverrides().value(QStringLiteral("RT")).toMap().value(QStringLiteral("unit")).toString(),
             QStringLiteral("Ω·m"));
    QVERIFY(reread.curveOverrides().value(QStringLiteral("RT")).toMap().value(QStringLiteral("log")).toBool());
    QCOMPARE(reread.auditLog().size(), 1);
    QVERIFY(reread.auditLog().at(0).toObject().value(QStringLiteral("op")).toString().contains(QStringLiteral("marker.move")));

    QFile::remove(tmp);
    QFile::remove(tmp.left(tmp.lastIndexOf(QLatin1Char('.'))) + QStringLiteral(".wc.json"));
  }

  void testTrackSpecCurveOverrideStorage()
  {
    // 覆盖层语义：曲线名键 + min/max/log/unit/color 五键（D1.7/D3.12 契约）
    TrackSpec spec;
    spec.typeId = QStringLiteral("discrete");
    QVariantMap ov;
    QVariantMap c;
    c.insert(QStringLiteral("min"), 5.0);
    c.insert(QStringLiteral("max"), 25.0);
    ov.insert(QStringLiteral("CPOR"), c);
    spec.setCurveOverrides(ov);

    const TrackSpec back = TrackSpec::fromVariantMap(spec.toVariantMap());
    const QVariantMap got = back.curveOverrides().value(QStringLiteral("CPOR")).toMap();
    QCOMPARE(got.value(QStringLiteral("min")).toDouble(), 5.0);
    QCOMPARE(got.value(QStringLiteral("max")).toDouble(), 25.0);

    // CurveData 覆盖后显示离散点仍可取值
    CurveData src;
    src.name = QStringLiteral("CPOR");
    src.minScale = 0.0f;
    src.maxScale = 30.0f;
    src.depths = {1000.0f, 1010.0f};
    src.values = {12.0f, 18.0f};
    const CurveData applied = applyCurveOverride(src, got);
    QCOMPARE(applied.minScale, 5.0f);
    QCOMPARE(applied.maxScale, 25.0f);
    QCOMPARE(applied.valueAtDepth(1000.0f), 12.0f);
  }

  // ---- D1.1 道增删改不重建画布 ----
  void testIncrementalTrackOps()
  {
    WellCompositeCanvas canvas;
    canvas.resize(800, 600);
    canvas.setDepthRange(1000.0, 2000.0);

    auto t1 = std::make_shared<DepthScaleTrack>(64.0);
    auto t2 = std::make_shared<TextTrack>(QStringLiteral("结论"), 110.0);
    auto t3 = std::make_shared<CurveTrack>(QStringLiteral("GR"), 170.0);
    canvas.addTrack(t1);
    canvas.addTrack(t2);
    canvas.addTrack(t3);
    QCOMPARE(canvas.trackCount(), 3);

    // moveTrack 不重建其它道对象（指针身份保持）
    canvas.moveTrack(2, 0);
    QCOMPARE(canvas.trackCount(), 3);
    QCOMPARE(canvas.tracks().at(0).get(), t3.get());
    QCOMPARE(canvas.tracks().at(1).get(), t1.get());
    QCOMPARE(canvas.tracks().at(2).get(), t2.get());

    canvas.removeTrack(1);
    QCOMPARE(canvas.trackCount(), 2);
    canvas.insertTrack(0, t2);
    QCOMPARE(canvas.tracks().at(0).get(), t2.get());
  }

  // ---- opt-02（W5）：NaN 空档读数不伪造 0 ----
  void valueAtDepthNanGapReturnsNan()
  {
    const auto nanf = [] { return std::numeric_limits<float>::quiet_NaN(); };
    CurveData c;
    c.name = QStringLiteral("NPHI");
    c.minScale = 0.0f;
    c.maxScale = 0.6f;
    c.depths = {1000.0f, 1005.0f};
    // 两端都缺值（LAS NULL 段）→ NaN：0 是合法物理量（NPHI 0），旧实现
    // 把空档伪造成 0.00 污染道头读数/tooltip（先红）；与绘制断笔口径一致。
    c.values = {nanf(), nanf()};
    QVERIFY(std::isnan(c.valueAtDepth(1002.0f)));
    // 一端有限取近邻（既有口径不变）。
    c.values = {nanf(), 0.15f};
    QCOMPARE(c.valueAtDepth(1002.0f), 0.15f);
    c.values = {0.10f, nanf()};
    QCOMPARE(c.valueAtDepth(1002.0f), 0.10f);
    // 两端有限照常插值（回归保护）。
    c.values = {0.10f, 0.20f};
    QCOMPARE(c.valueAtDepth(1002.0f), 0.14f);
  }
};

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestWellCompositeFramework tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_wellcomposite_framework.moc"
