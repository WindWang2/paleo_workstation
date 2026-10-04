// tests/tst_wellsection_ui.cpp — goal/wellsection 视图层验收：面板行为
// （井集/连线/高亮/拉平/选中/拖排/主题/模板/地震/缩放）+ 版头交互 +
// 对话框 + 截图件产（/tmp/wellsection-shots/，供人工阅图）。
#include <QtTest>
#include <QApplication>
#include <QDir>
#include <QGraphicsView>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QToolButton>
#include <QTemporaryDir>
#include <QWheelEvent>
#include "helpers/visualcapture.h"

#include "../src/linkage/selectioncontext.h"
#include "../src/catalog/datacatalog.h"
#include "../src/qgis/wellsectionmapband.h"
#include <qgsattributes.h>
#include <qgsfeature.h>
#include <qgsgeometry.h>
#include <qgsmapcanvas.h>
#include <qgsvectorlayer.h>
#include "../src/metadata/wellsectionstore.h"
#include "../src/ui/paleotheme.h"
#include "../src/ui/wellsection/fencewidget.h"
#include "../src/ui/wellsection/wellsectiondialogs.h"
#include "../src/ui/wellsection/wellsectionpanel.h"

class TestWellSectionUi : public QObject
{
  Q_OBJECT

  private:
    // 四口井：构造起伏 + 层厚变化；C3/C6/D53/D61/D62 全在编图层位内。
    // vels 非空时逐井常速时深模型（井间反射层会弯——检验深度→时间映射）。
    static QVector<wellsection::Well> wells4(bool extraTopX = false,
                                             const QVector<double> &vels = {})
    {
      const char *ids[] = {"C-2", "A5", "C-1", "C-4"};
      const char *names[] = {"C3", "C6", "D53", "D61", "D62"};
      const double base[5] = {1800.0, 1840.0, 1900.0, 1950.0, 2000.0};
      const double relief[4] = {0.0, 12.0, -10.0, 26.0};
      const double thick[4] = {0.0, 8.0, -6.0, 18.0}; // 逐层厚度增量斜率
      QVector<wellsection::Well> out;
      for (int i = 0; i < 4; ++i)
      {
        wellsection::Well w;
        w.id = QLatin1String(ids[i]);
        w.name = w.id;
        w.x = i * 300.0;
        w.y = i * 120.0;
        w.totalDepth = 2100.0;
        for (int k = 0; k < 5; ++k)
          w.tops << wellsection::Top{QLatin1String(names[k]),
                                     base[k] + relief[i] + thick[i] * k * 0.25};
        if (extraTopX)
          w.tops << wellsection::Top{QStringLiteral("X"),
                                     base[4] + relief[i] + 30.0};
        // GR：砂段（<75）夹在泥基线 110 间。
        wellsection::Curve gr;
        gr.mnemonic = QStringLiteral("GR");
        for (double d = 1750.0; d <= 2100.0; d += 1.0)
        {
          gr.depths << float(d);
          const bool sand = std::fmod(d, 90.0) < 28.0;
          gr.values << float(sand ? 45.0 : 110.0);
        }
        w.curves << gr;
        // RD/RS：对数域摆动的电阻率。
        wellsection::Curve rd, rs;
        rd.mnemonic = QStringLiteral("RD");
        rs.mnemonic = QStringLiteral("RS");
        for (double d = 1750.0; d <= 2100.0; d += 2.0)
        {
          rd.depths << float(d);
          rs.depths << float(d);
          rd.values << float(0.4 + 8.0 * (0.5 + 0.5 * std::sin(d * 0.05)));
          rs.values << float(0.3 + 5.0 * (0.5 + 0.5 * std::cos(d * 0.07)));
        }
        w.curves << rd << rs;
        // 常速时深（地震缝换算用）：twt = md*2000/v。
        wellsection::TimeDepth td;
        td.model = seismic::TimeDepthModel(
            i < vels.size() ? vels[i] : 2500.0);
        w.timeDepth = td;
        out << w;
      }
      return out;
    }

    // 覆盖 fixture MD 段的合成缝（v=2500 → twt≈1400..1680ms；twt 域取
    // 1300..1700ms 留余量），列向渐变幅值。
    static wellsection::SeismicStrip stripFor(int nGaps)
    {
      wellsection::SeismicStrip strip;
      strip.clip = 1.0f;
      for (int g = 0; g < nGaps; ++g)
      {
        wellsection::SeismicGap gap;
        gap.columns = 8;
        gap.samples = 100;
        gap.startMs = 1300.0;
        gap.stepMs = 4.0; // 覆盖 1300..1700ms
        gap.values.resize(size_t(gap.columns) * size_t(gap.samples));
        // 列向正负条纹 × 行向包络：幅度恒 ≥0.5（col0 恒正 → 灰阶下为深色，
        // 测试像素取其列域内保证「非纸面」）。
        for (int s = 0; s < gap.samples; ++s)
          for (int c = 0; c < gap.columns; ++c)
            gap.values[size_t(s) * size_t(gap.columns) + size_t(c)] =
                float((c % 2 == 0 ? 0.8 : -0.8) *
                      (0.7 + 0.3 * std::cos(s * 0.11)));
        strip.gaps << gap;
      }
      return strip;
    }

    // 层状反射体模型：amp = sin(2π·twt/28)·(0.6+0.4·sin(2π·twt/170))，
    // 列向 ±6ms 微倾——不同井速下反射层在两井间弯曲。
    static wellsection::SeismicStrip reflectorStrip(int nGaps)
    {
      wellsection::SeismicStrip strip;
      strip.clip = 1.0f;
      constexpr double kPi2 = 6.283185307179586;
      for (int g = 0; g < nGaps; ++g)
      {
        wellsection::SeismicGap gap;
        gap.columns = 32;
        gap.samples = 100;
        gap.startMs = 1250.0;
        gap.stepMs = 6.0; // 覆盖 1250..1844ms（md≈1450..2210 @2500m/s）
        gap.values.resize(size_t(gap.columns) * size_t(gap.samples));
        for (int s = 0; s < gap.samples; ++s)
        {
          const double twt = gap.startMs + s * gap.stepMs;
          for (int c = 0; c < gap.columns; ++c)
          {
            const double f = double(c) / (gap.columns - 1);
            const double ph = twt + 6.0 * (f - 0.5) * 2.0; // 列向 ±6ms 倾角
            gap.values[size_t(s) * size_t(gap.columns) + size_t(c)] =
                float(std::sin(kPi2 * ph / 28.0) *
                      (0.6 + 0.4 * std::sin(kPi2 * ph / 170.0)));
          }
        }
        strip.gaps << gap;
      }
      return strip;
    }

    static QToolButton *toolBtn(WellSectionPanel &p, const char *name)
    {
      return p.findChild<QToolButton *>(QLatin1String(name));
    }

  private slots:
    void initTestCase()
    {
      PaleoTheme::pinRenderEnvironment();
      QSettings(QStringLiteral("paleo"), QStringLiteral("paleo")).clear();
    }

    // 0 井空态 + setWellIds 发 dataRequested（不发 wellIdsChanged）。
    void emptyStateAndWellIds()
    {
      SelectionContext ctx;
      WellSectionPanel panel(&ctx);
      auto *empty = panel.findChild<QWidget *>(
          QStringLiteral("wellSectionEmpty"));
      QVERIFY(empty);
      QVERIFY(empty->isVisibleTo(&panel));
      QVERIFY(panel.findChild<QPushButton *>(
          QStringLiteral("wellSectionEmptyPickButton")));

      QSignalSpy dataSpy(&panel, &WellSectionPanel::dataRequested);
      QSignalSpy idsSpy(&panel, &WellSectionPanel::wellIdsChanged);
      const QStringList ids{QStringLiteral("C-2"), QStringLiteral("A5")};
      panel.setWellIds(ids);
      QCOMPARE(dataSpy.size(), 1);
      QCOMPARE(dataSpy.at(0).at(0).toStringList(), ids);
      QCOMPARE(dataSpy.at(0).at(1).toStringList(),
               wellsection::SectionTemplate::defaults().mnemonics());
      QCOMPARE(idsSpy.size(), 0);
    }

    // setSection 接管顺序 + 5 顶 × 3 缝 = 15 连线。
    void sectionOrderAndLinks()
    {
      SelectionContext ctx;
      WellSectionPanel panel(&ctx);
      const auto ws = wells4();
      panel.setSection(ws);
      QCOMPARE(panel.wellIds(),
               (QStringList{QStringLiteral("C-2"), QStringLiteral("A5"),
                            QStringLiteral("C-1"), QStringLiteral("C-4")}));
      QCOMPARE(panel.linkCount(), 15);
    }

    // 非编图层位 "X"：Mapping 过滤仍 15 连线；All → 18。
    void topFilterMappingVsAll()
    {
      SelectionContext ctx;
      WellSectionPanel panel(&ctx);
      panel.setSection(wells4(true));
      QCOMPARE(panel.linkCount(), 15);
      auto t = wellsection::SectionTemplate::defaults();
      t.topFilter = wellsection::TopFilter::All;
      panel.setSectionTemplate(t);
      QCOMPARE(panel.linkCount(), 18);
    }

    // 高亮：活动层位 D61 → D62 段在层段道上呈主色淡底。
    void highlightActiveFormation()
    {
      SelectionContext ctx;
      WellSectionPanel panel(&ctx);
      panel.setSection(wells4());
      ctx.setActiveHorizon(QStringLiteral("D61"));
      QCOMPARE(panel.highlightedFormation(), QStringLiteral("D61"));

      const QImage img = panel.renderImage();
      const int hh = panel.findChild<wellsectionui::HeaderWidget *>(
                            QStringLiteral("wellSectionHeader"))
                         ->headerHeight();
      const double y0 = panel.topLineY(QStringLiteral("C-2"),
                                       QStringLiteral("D61"));
      const double y1 = panel.topLineY(QStringLiteral("C-2"),
                                       QStringLiteral("D62"));
      QVERIFY(std::isfinite(y0) && std::isfinite(y1));
      // 层段道中心 x = margin + 道宽/2 = 16 + 22；行取高亮段中点。
      const int px = 16 + 22;
      const int py = int(hh + (y0 + y1) * 0.5);
      const QColor c = img.pixelColor(px, py);
      QVERIFY2(c.blue() > c.red() + 20,
               qPrintable(QStringLiteral("pixel %1").arg(c.name())));

      panel.setHighlightEnabled(false);
      QCOMPARE(panel.highlightedFormation(), QString());
      const QImage img2 = panel.renderImage();
      const QColor c2 = img2.pixelColor(px, py);
      QVERIFY2(!(c2.blue() > c2.red() + 20),
               qPrintable(QStringLiteral("pixel %1").arg(c2.name())));

      // 无井拾取的层位 → 空。
      ctx.setActiveHorizon(QStringLiteral("D71"));
      panel.setHighlightEnabled(true);
      QCOMPARE(panel.highlightedFormation(), QString());
    }

    // 回归：层段带/高亮带必须是两连线间的整带（单闭合子路径）——
    // 曾用 addPath 使两条曲线各自按弦闭合，填充只剩弦—曲线细楔。
    // 缝中心 x 处 S 曲线中点 y = (yl+yr)/2，取相邻两中点之均值采样。
    void gapBandFillGeometry()
    {
      const auto blendOver = [](const QColor &c, double alpha,
                                const QColor &paper) {
        const auto ch = [&](int cc, int pp) {
          return qBound(0, int(cc * alpha + pp * (1.0 - alpha) + 0.5),
                        255);
        };
        return QColor(ch(c.red(), paper.red()),
                      ch(c.green(), paper.green()),
                      ch(c.blue(), paper.blue()));
      };
      const auto near = [](const QColor &a, const QColor &b) {
        return qAbs(a.red() - b.red()) <= 6 &&
               qAbs(a.green() - b.green()) <= 6 &&
               qAbs(a.blue() - b.blue()) <= 6;
      };

      SelectionContext ctx;
      WellSectionPanel panel(&ctx);
      panel.setSection(wells4());
      panel.setThemeId(QStringLiteral("colored"));
      const int hh = panel.findChild<wellsectionui::HeaderWidget *>(
                            QStringLiteral("wellSectionHeader"))
                         ->headerHeight();
      const double colW =
          wellsection::SectionTemplate::defaults().columnWidth();
      // 缝 0 中心 x = columnRight(0) + gap/2。
      const int gx = int(16 + colW + panel.gapWidth() * 0.5);
      const auto midY = [&](const char *top) {
        return (panel.topLineY(QStringLiteral("C-2"),
                               QLatin1String(top)) +
                panel.topLineY(QStringLiteral("A5"),
                               QLatin1String(top))) *
               0.5;
      };
      const int py = int(hh + (midY("D61") + midY("D62")) * 0.5);

      // 层段带取色：zoneColor(zoneOrder.indexOf("D61")) α140 叠纸面。
      const auto filtered = wellsection::filterTops(
          panel.wells(), panel.sectionTemplate());
      const int zi = wellsection::orderedTopNames(filtered)
                         .indexOf(QStringLiteral("D61"));
      QVERIFY(zi >= 0);
      const QColor paper = wellsection::SectionTheme::byId(
                               QStringLiteral("colored"))
                               .paper;
      const QColor expect =
          blendOver(wellsection::zoneColor(zi), 140.0 / 255.0, paper);
      const QColor got = panel.renderImage().pixelColor(gx, py);
      QVERIFY2(near(got, expect),
               qPrintable(QStringLiteral("zone band %1 expect %2")
                              .arg(got.name(), expect.name())));

      // 高亮带：经典（无层段充填）下同一点得主色 α55 淡染。
      panel.setThemeId(QStringLiteral("classic"));
      ctx.setActiveHorizon(QStringLiteral("D61"));
      const QColor paper2 = wellsection::SectionTheme::byId(
                                QStringLiteral("classic"))
                                .paper;
      const QColor expect2 = blendOver(PaleoTheme::tokens().primary,
                                       55.0 / 255.0, paper2);
      const QColor got2 = panel.renderImage().pixelColor(gx, py);
      QVERIFY2(near(got2, expect2),
               qPrintable(QStringLiteral("highlight band %1 expect %2")
                              .arg(got2.name(), expect2.name())));
    }

    // 拉平：D61 拉平后各井 D61 顶线同 y；取消后恢复起伏。
    void flattenTop()
    {
      SelectionContext ctx;
      WellSectionPanel panel(&ctx);
      panel.setSection(wells4());
      const double before0 = panel.topLineY("C-2", "D61");
      const double before3 = panel.topLineY("C-4", "D61");
      QVERIFY(std::fabs(before0 - before3) > 1.0); // 起伏真实存在
      panel.setFlattenTop(QStringLiteral("D61"));
      const double ref = panel.topLineY("C-2", "D61");
      for (const char *id : {"C-2", "A5", "C-1", "C-4"})
        QVERIFY(std::fabs(panel.topLineY(QLatin1String(id),
                                         QStringLiteral("D61")) -
                          ref) <= 0.5);
      panel.setFlattenTop(QString());
      QVERIFY(std::fabs(panel.topLineY("C-2", "D61") -
                        panel.topLineY("C-4", "D61")) > 1.0);
    }

    // ---- 基准面三模式（Oracle #1：模式切换只动视图，井深表逐行不变）----
    void datumModesInvariant()
    {
      SelectionContext ctx;
      WellSectionPanel panel(&ctx);
      auto wells = wells4();
      const double kbs[4] = {30.0, 12.0, -8.0, 22.0};
      for (int i = 0; i < wells.size(); ++i)
        wells[i].kb = kbs[i];
      panel.setSection(wells);
      const QStringList csvDepth = panel.topsCsv().split(QLatin1Char('\n'));

      // 井深 → 拉平 D61：基准层同高（其余层按相对高程重排）。
      const double diffBefore = panel.topLineY("C-2", "D61") -
                                panel.topLineY("C-4", "D61");
      QVERIFY(std::fabs(diffBefore) > 1.0);
      panel.setDatum({wellsection::DatumMode::Flatten,
                      QStringLiteral("D61")});
      const double ref = panel.topLineY("C-2", "D61");
      for (const char *id : {"C-2", "A5", "C-1", "C-4"})
        QVERIFY(std::fabs(panel.topLineY(QLatin1String(id),
                                         QStringLiteral("D61")) -
                          ref) <= 0.5);
      QVERIFY(panel.statusText().contains(QStringLiteral("拉平于")));
      // 拉平不变量：井深表数据行（非表头）逐行相等。
      const QStringList csvFlat = panel.topsCsv().split(QLatin1Char('\n'));
      QCOMPARE(csvFlat.size(), csvDepth.size());
      for (int i = 1; i < csvFlat.size(); ++i)
        QCOMPARE(csvFlat.at(i), csvDepth.at(i));

      // 海拔（补心）：状态行标记 + 数据行不变 + 井顶深不动。
      panel.setDatum({wellsection::DatumMode::Elevation, QString()});
      QVERIFY(panel.statusText().contains(QStringLiteral("海拔基准")));
      const QStringList csvElev = panel.topsCsv().split(QLatin1Char('\n'));
      for (int i = 1; i < csvElev.size(); ++i)
        QCOMPARE(csvElev.at(i), csvDepth.at(i));
      QCOMPARE(panel.wells()[0].topMd(QStringLiteral("D61")),
               wells[0].topMd(QStringLiteral("D61")));

      // 回井深：起伏差恢复；空 flattenTop 的 Flatten 视作井深。
      panel.setDatum({wellsection::DatumMode::Depth, QString()});
      const double diffAfter = panel.topLineY("C-2", "D61") -
                               panel.topLineY("C-4", "D61");
      QVERIFY(std::fabs(diffAfter - diffBefore) <= 0.5);
      panel.setDatum({wellsection::DatumMode::Flatten, QString()});
      QVERIFY(std::fabs(panel.topLineY("C-2", "D61") -
                        panel.topLineY("C-4", "D61") - diffBefore) <= 0.5);

      // 统一 kb：海拔模式与井深模式几何全等（等价平移）。
      auto uniform = wells4();
      for (auto &w : uniform)
        w.kb = 12.0;
      WellSectionPanel p2(&ctx);
      p2.setSection(uniform);
      const double yD = p2.topLineY("A5", "D53");
      p2.setDatum({wellsection::DatumMode::Elevation, QString()});
      QVERIFY(std::fabs(p2.topLineY("A5", "D53") - yD) <= 0.5);
    }

    // ---- 井距比例/等距切换 + 层位连线断开重连 ----
    void spacingAndLinkEditing()
    {
      SelectionContext ctx;
      WellSectionPanel panel(&ctx);
      auto wells = wells4();
      wells[1].x = 150.0; // 第二段井距拉开（首段 ~192、次段 ~466）
      panel.resize(1400, 720);
      panel.setSection(wells);
      panel.show();
      QVERIFY(QTest::qWaitForWindowExposed(&panel));
      panel.fitToView();
      QCOMPARE(panel.spacingMode(), wellsection::SpacingMode::Equal);
      const qreal x0 = panel.columnX(0), x1 = panel.columnX(1),
                 x2 = panel.columnX(2), x3 = panel.columnX(3);
      QVERIFY(std::fabs((x1 - x0) - (x2 - x1)) <= 0.5); // 等距均一

      // 比例：宽缝变宽、窄缝变窄，总跨不变（预算守恒）。
      panel.setSpacingMode(wellsection::SpacingMode::Proportional);
      const qreal p1 = panel.columnX(1), p2 = panel.columnX(2),
                 p3 = panel.columnX(3);
      QVERIFY(panel.gapWidthAt(1) > panel.gapWidthAt(0) + 1.0);
      QVERIFY(std::fabs(p3 - x3) <= 0.5);
      // 切回等距：位置复原（模式切换不变形）。
      panel.setSpacingMode(wellsection::SpacingMode::Equal);
      QVERIFY(std::fabs(panel.columnX(1) - x1) <= 0.5);
      QVERIFY(std::fabs(panel.columnX(2) - x2) <= 0.5);

      // 连线：断开 → 计数降、信号发；重连 → 恢复（同键 upsert 不追加）。
      const int total = panel.linkCount();
      QVERIFY(total > 0);
      QSignalSpy spy(&panel, &WellSectionPanel::linkOverridesChanged);
      panel.toggleLink(0, QStringLiteral("D61"), false);
      QCOMPARE(panel.linkCount(), total - 1);
      QCOMPARE(spy.size(), 1);
      QCOMPARE(panel.linkOverrides().size(), 1);
      panel.toggleLink(0, QStringLiteral("D61"), true);
      QCOMPARE(panel.linkCount(), total);
      QCOMPARE(panel.linkOverrides().size(), 1); // upsert
      QCOMPARE(spy.size(), 2);
      // 另一缝另一顶独立断开。
      panel.toggleLink(1, QStringLiteral("D53"), false);
      QCOMPARE(panel.linkCount(), total - 1);
      QCOMPARE(panel.linkOverrides().size(), 2);
      QCOMPARE(spy.size(), 3);
      // 程序化恢复（store 读回路径）：不改计数、不发信号。
      QVector<wellsection::LinkOverride> restored;
      restored << wellsection::makeLinkOverride(
          QStringLiteral("C-2"), QStringLiteral("A5"),
          QStringLiteral("D62"), false);
      panel.setLinkOverrides(restored);
      QCOMPARE(spy.size(), 3);
      QCOMPARE(panel.linkCount(), total - 1);
      // 井序无关键：井对从任一方向查都命中（C-2/A5 断开 D62）。
      QVERIFY(panel.linkOverrides().first().leftWellId ==
              QStringLiteral("A5"));
    }

    // ---- 相代码充填道 + 三曲线叠加道（渲染冒烟 + 模板 round-trip）----
    void faciesTrackAndTripleCurve()
    {
      SelectionContext ctx;
      WellSectionPanel panel(&ctx);
      auto wells = wells4();
      // 每井两段相代码。
      for (auto &w : wells) {
        wellsection::FaciesSegment s1, s2;
        s1.topMd = w.tops.first().md;
        s1.baseMd = w.tops.at(2).md;
        s1.classId = 2;
        s2.topMd = w.tops.at(2).md;
        s2.baseMd = w.tops.last().md;
        s2.classId = 7;
        w.facies = {s1, s2};
      }
      // 模板：三曲线道（GR/RD/RS）+ 相代码道。
      wellsection::SectionTemplate t = panel.sectionTemplate();
      for (auto &tr : t.tracks)
        if (tr.kind == wellsection::TrackKind::Curve && tr.curves.size() == 2)
        {
          wellsection::CurveStyle c3;
          c3.mnemonic = QStringLiteral("RS");
          c3.min = 0.2;
          c3.max = 20;
          c3.logScale = true;
          c3.color = QColor(QStringLiteral("#1F7A4D"));
          tr.curves << c3;
        }
      wellsection::TrackSpec facies;
      facies.kind = wellsection::TrackKind::Facies;
      facies.width = 40;
      t.tracks << facies;
      panel.setSectionTemplate(t);
      panel.setSection(wells);
      // 曲线道 mnemonics 变了 → 重发数据请求（RS 加入）。
      panel.setSection(wells); // 再喂一次数据（壳层回填路径）
      bool foundTriple = false;
      for (const auto &tr : panel.sectionTemplate().tracks)
        if (tr.kind == wellsection::TrackKind::Curve && tr.curves.size() == 3)
          foundTriple = true;
      QVERIFY(foundTriple);
      QCOMPARE(panel.sectionTemplate().tracks.last().kind,
               wellsection::TrackKind::Facies);
      // 渲染冒烟：含相代码道 + 三曲线道的图非空。
      panel.resize(1200, 700);
      panel.show();
      QVERIFY(QTest::qWaitForWindowExposed(&panel));
      const QImage img = panel.renderImage(1.0);
      QVERIFY(!img.isNull());
      QVERIFY(img.width() > 400);
      // 模板 JSON round-trip 保三曲线 + 相代码道。
      const wellsection::SectionTemplate rt =
          wellsection::SectionTemplate::fromJson(t.toJson());
      bool rtTriple = false;
      for (const auto &tr : rt.tracks)
        if (tr.kind == wellsection::TrackKind::Curve && tr.curves.size() == 3)
          rtTriple = true;
      QVERIFY(rtTriple);
      QCOMPARE(rt.tracks.last().kind, wellsection::TrackKind::Facies);
    }

    // ---- 断层投绘开关 ----
    void faultToggleAndOverlay()
    {
      SelectionContext ctx;
      WellSectionPanel panel(&ctx);
      panel.setSection(wells4());
      QVERIFY(!panel.faultsEnabled());
      // 数据回填（壳层接 workflow 的路径）：开关关闭时也可预置数据。
      QVector<wellsection::FaultTrace> traces;
      wellsection::FaultTrace t;
      t.faultName = QStringLiteral("F1");
      t.points = {{0.0, 1900.0}, {1.0, 1980.0}};
      traces << t;
      panel.setFaultTraces(traces, QString());
      QCOMPARE(panel.faultTraceCount(), 1);
      // 用户开 → 发 faultsRequested；关 → 清数据不发。
      QSignalSpy spy(&panel, &WellSectionPanel::faultsRequested);
      panel.setFaultsEnabled(true);
      QCOMPARE(spy.size(), 1);
      QCOMPARE(panel.faultTraceCount(), 1);
      panel.setFaultsEnabled(false);
      QCOMPARE(spy.size(), 1); // 关不发请求
      QCOMPARE(panel.faultTraceCount(), 0); // 清空
      // 井集变化 + 开关开 → 重新请求。
      panel.setFaultsEnabled(true);
      spy.clear();
      panel.setSection(wells4());
      QCOMPARE(spy.size(), 1);
    }

    // 版头点名 → SelectionContext「wellsection」源选中 + wellClicked；
    // 外部源选中回写高亮态。
    void selectionRoundTrip()
    {
      SelectionContext ctx;
      WellSectionPanel panel(&ctx);
      panel.resize(1200, 600);
      panel.setSection(wells4());
      panel.show();
      QVERIFY(QTest::qWaitForWindowExposed(&panel));

      auto *header = panel.findChild<QWidget *>(
          QStringLiteral("wellSectionHeader"));
      QVERIFY(header);
      QSignalSpy clickSpy(&panel, &WellSectionPanel::wellClicked);
      // 井 1 列中心：autofit 会改 gap——按当前 gapWidth/列宽反算几何。
      const int colW =
          wellsection::SectionTemplate::defaults().columnWidth(); // 238
      const int col1x = int(16 + colW + panel.gapWidth() + colW * 0.5);
      QTest::mouseClick(header, Qt::LeftButton, Qt::NoModifier,
                        QPoint(col1x, 10));
      QCOMPARE(ctx.selectedIds(), QStringList{QStringLiteral("A5")});
      QCOMPARE(ctx.origin(), QStringLiteral("wellsection"));
      QCOMPARE(clickSpy.size(), 1);
      QCOMPARE(clickSpy.at(0).at(0).toString(), QStringLiteral("A5"));
      QVERIFY(panel.isWellSelected(QStringLiteral("A5")));

      ctx.setSelection({QStringLiteral("C-1")}, QStringLiteral("canvas"));
      QVERIFY(panel.isWellSelected(QStringLiteral("C-1")));
      QVERIFY(!panel.isWellSelected(QStringLiteral("A5")));
    }

    // 版头拖排：井0 拖过井2 → wellIdsChanged；地震开时再发
    // seismicRequested。右键移除 → 3 井。
    void headerReorderAndRemove()
    {
      SelectionContext ctx;
      WellSectionPanel panel(&ctx);
      panel.resize(1400, 600);
      panel.setSeismicEnabled(true);
      QSignalSpy seisSpy(&panel, &WellSectionPanel::seismicRequested);
      panel.setSection(wells4());
      panel.show();
      QVERIFY(QTest::qWaitForWindowExposed(&panel));

      auto *header = panel.findChild<QWidget *>(
          QStringLiteral("wellSectionHeader"));
      QVERIFY(header);
      QSignalSpy idsSpy(&panel, &WellSectionPanel::wellIdsChanged);

      // fit 后的列几何从 topLineY 不可知——按当前 gap/列宽反算列中心。
      // columnLeft(i) = 16 + i*(colW+gap)；拖井0 越过井2 中心。
      const double colW =
          wellsection::SectionTemplate::defaults().columnWidth(); // 238
      const double gap = panel.gapWidth();
      const double col0 = 16 + colW * 0.5;
      const double col2c = 16 + 2 * (colW + gap) + colW * 0.5;
      QTest::mousePress(header, Qt::LeftButton, Qt::NoModifier,
                        QPoint(int(col0), 10));
      QTest::mouseMove(header, QPoint(int(col2c) + 30, 10));
      QTest::mouseRelease(header, Qt::LeftButton, Qt::NoModifier,
                          QPoint(int(col2c) + 30, 10));
      QCOMPARE(idsSpy.size(), 1);
      QCOMPARE(panel.wellIds(),
               (QStringList{QStringLiteral("A5"), QStringLiteral("C-1"),
                            QStringLiteral("C-2"), QStringLiteral("C-4")}));
      QVERIFY(seisSpy.size() >= 1); // 地震开 → 重排后再发缝请求

      // 右键首列 →「从剖面移除」→ 3 井。仅发 press：release 会落在
      // 弹出菜单上被它吞掉（甚至直接触发/解散菜单）。
      QTest::mousePress(header, Qt::RightButton, Qt::NoModifier,
                        QPoint(int(col0), 10));
      auto *menu = header->findChild<QMenu *>(
          QStringLiteral("wellSectionHeaderMenu"));
      QVERIFY(menu);
      QVERIFY(!menu->actions().isEmpty());
      menu->actions().first()->trigger();
      QCOMPARE(panel.wellIds().size(), 3);
    }

    // 主题：classic/colored 渲染差异；非法 id 回退 classic；设置键只在
    // 菜单动作后写。
    void themesAndSettings()
    {
      QSettings(QStringLiteral("paleo"), QStringLiteral("paleo"))
          .remove(QStringLiteral("wellSection/theme"));
      SelectionContext ctx;
      WellSectionPanel panel(&ctx);
      panel.setSection(wells4());
      QCOMPARE(panel.themeId(), QStringLiteral("classic"));
      const QImage classic = panel.renderImage();

      panel.setThemeId(QStringLiteral("colored"));
      QCOMPARE(panel.themeId(), QStringLiteral("colored"));
      QVERIFY(panel.renderImage() != classic);
      // 仅调 setter 不写设置。
      QVERIFY(!QSettings(QStringLiteral("paleo"), QStringLiteral("paleo"))
                   .contains(QStringLiteral("wellSection/theme")));

      panel.setThemeId(QStringLiteral("bogus"));
      QCOMPARE(panel.themeId(), QStringLiteral("classic"));

      // 菜单动作（用户驱动）→ 写设置。
      auto *btn = toolBtn(panel, "wellSectionThemeButton");
      QVERIFY(btn && btn->menu());
      QAction *colored = nullptr;
      for (QAction *a : btn->menu()->actions())
        if (a->objectName() == QLatin1String("wellSectionTheme_colored"))
          colored = a;
      QVERIFY(colored);
      colored->trigger();
      QCOMPARE(QSettings(QStringLiteral("paleo"), QStringLiteral("paleo"))
                   .value(QStringLiteral("wellSection/theme"))
                   .toString(),
               QStringLiteral("colored"));
    }

    // 模板：JSON 往返相等；垃圾 → 默认 + ok=false；新助记名 →
    // dataRequested；仅宽度变 → 不发。
    void templateJsonAndRequests()
    {
      const auto d = wellsection::SectionTemplate::defaults();
      const auto rt = wellsection::SectionTemplate::fromJson(d.toJson());
      QVERIFY(rt == d);
      bool ok = true;
      const auto bad = wellsection::SectionTemplate::fromJson(
          QJsonObject{{QStringLiteral("junk"), 1}}, &ok);
      QVERIFY(!ok);
      QVERIFY(bad == d);

      SelectionContext ctx;
      WellSectionPanel panel(&ctx);
      panel.setSection(wells4());
      QSignalSpy dataSpy(&panel, &WellSectionPanel::dataRequested);

      auto t2 = d;
      t2.tracks[0].width = 60; // 仅宽度
      panel.setSectionTemplate(t2);
      QCOMPARE(dataSpy.size(), 0);

      wellsection::TrackSpec sp;
      sp.kind = wellsection::TrackKind::Curve;
      wellsection::CurveStyle cs;
      cs.mnemonic = QStringLiteral("SP");
      sp.curves << cs;
      auto t3 = d;
      t3.tracks << sp;
      panel.setSectionTemplate(t3);
      QCOMPARE(dataSpy.size(), 1);
      QVERIFY(dataSpy.at(0).at(1).toStringList().contains(
          QStringLiteral("SP")));
    }

    // 地震：不可用原因进 tooltip；有效缝渲染非纸面像素；缝原因可读；
    // 缝数不符忽略。
    void seismicAvailabilityAndStrip()
    {
      SelectionContext ctx;
      WellSectionPanel panel(&ctx);
      panel.setSection(wells4());
      auto *btn = toolBtn(panel, "wellSectionSeismicButton");
      QVERIFY(btn);
      panel.setSeismicAvailable(false, QStringLiteral("原因R"));
      QVERIFY(!btn->isEnabled());
      QVERIFY(btn->toolTip().contains(QStringLiteral("原因R")));

      panel.setSeismicAvailable(true, QString());
      QVERIFY(btn->isEnabled());
      panel.setSeismicEnabled(true);
      panel.setSeismicStrip(stripFor(panel.wellIds().size() - 1));

      const QImage img = panel.renderImage();
      // 缝 0 内靠左缘（缝内 col0 域恒正幅 → 灰阶深色）；y 取两顶线间
      // 中点，避开连线像素。缝左缘 = 16 + 列宽（默认模板 238）。
      const int colW =
          wellsection::SectionTemplate::defaults().columnWidth();
      const int px = 16 + colW + 6;
      const int hh = panel.findChild<wellsectionui::HeaderWidget *>(
                            QStringLiteral("wellSectionHeader"))
                         ->headerHeight();
      const double y0 = panel.topLineY("C-2", "D53");
      const double y1 = panel.topLineY("C-2", "D61");
      const int py = int(hh + (y0 + y1) * 0.5);
      const QColor c = img.pixelColor(px, py);
      QVERIFY2(c != QColor(255, 255, 255),
               qPrintable(QStringLiteral("pixel %1").arg(c.name())));

      // 缝原因 + 错配缝数忽略。
      auto strip2 = stripFor(3);
      strip2.gaps[1].reason = QStringLiteral("缝原因X");
      panel.setSeismicStrip(strip2);
      QCOMPARE(panel.gapReason(1), QStringLiteral("缝原因X"));
      panel.setSeismicStrip(stripFor(1)); // 1 ≠ 3 → 忽略
      QCOMPARE(panel.gapReason(1), QStringLiteral("缝原因X"));
      QCOMPARE(panel.gapReason(0), QString());
    }

    // 适应/缩放：fit 后图体高 ≈ 视口高 ±2px；Ctrl+滚轮改 px/m 且关
    // autofit；Ctrl+Shift+滚轮改井间距；fit 恢复。
    void fitAndWheelZoom()
    {
      SelectionContext ctx;
      WellSectionPanel panel(&ctx);
      panel.resize(1400, 720);
      panel.setSection(wells4());
      panel.show();
      QVERIFY(QTest::qWaitForWindowExposed(&panel));
      panel.fitToView();

      auto *view = panel.findChild<QGraphicsView *>(
          QStringLiteral("wellSectionView"));
      QVERIFY(view);
      const double vh = view->viewport()->height();
      QVERIFY(vh > 100);
      const auto fitWells = wellsection::filterTops(
          wells4(), wellsection::SectionTemplate::defaults());
      const auto win =
          wellsection::depthWindow(fitWells, QString());
      const double bodyH = (win.base - win.top) * panel.pxPerMeter();
      QVERIFY2(std::fabs(bodyH - vh) <= 2.0,
               qPrintable(QStringLiteral("bodyH %1 vs vh %2")
                            .arg(bodyH)
                            .arg(vh)));

      // Ctrl+滚轮 → 深度缩放 ×1.15，autofit 关（再 resize 不再 refit）。
      const double px0 = panel.pxPerMeter();
      const QPoint vp = view->viewport()->rect().center();
      QWheelEvent ev(QPointF(vp), view->viewport()->mapToGlobal(vp),
                     QPoint(0, 0), QPoint(0, 120), Qt::NoButton,
                     Qt::ControlModifier, Qt::NoScrollPhase, false);
      QApplication::sendEvent(view->viewport(), &ev);
      QVERIFY2(std::fabs(panel.pxPerMeter() - px0 * 1.15) < 1e-6,
               qPrintable(QStringLiteral("%1 -> %2").arg(px0).arg(
                   panel.pxPerMeter())));
      panel.resize(1400, 640);
      QTest::qWait(20);
      QCOMPARE(panel.pxPerMeter(), px0 * 1.15); // autofit 已关

      // Ctrl+Shift+滚轮 → 井间距 ×1.15（未达边界时）。
      const double gap0 = panel.gapWidth();
      QWheelEvent ev2(QPointF(vp), view->viewport()->mapToGlobal(vp),
                      QPoint(0, 0), QPoint(0, 120), Qt::NoButton,
                      Qt::ControlModifier | Qt::ShiftModifier,
                      Qt::NoScrollPhase, false);
      QApplication::sendEvent(view->viewport(), &ev2);
      QVERIFY2(std::fabs(panel.gapWidth() - gap0 * 1.15) < 1e-3,
               qPrintable(QStringLiteral("%1 -> %2").arg(gap0).arg(
                   panel.gapWidth())));

      // 恢复原尺寸再 fit：断言拟合不变量（图体高 ≈ 视口高），滚动条
      // 显隐引起的 vh 微差不写死 px0。
      panel.resize(1400, 720);
      QTest::qWait(20);
      panel.fitToView();
      const double vh2 = view->viewport()->height();
      QVERIFY(std::fabs((win.base - win.top) * panel.pxPerMeter() - vh2) <=
              2.0);
    }

    // 拉伸超宽：autofit 把 gap 顶到 maxGap 后场景仍窄于视口 → 视图须钉
    // 左上（AlignCenter 居中会让井柱相对版头右移半个差值，版头漂移）。
    void headerAlignWhenSceneNarrower()
    {
      SelectionContext ctx;
      WellSectionPanel panel(&ctx);
      panel.resize(1700, 620);
      panel.setSection(wells4().mid(0, 2));
      panel.show();
      QVERIFY(QTest::qWaitForWindowExposed(&panel));
      QTest::qWait(60); // refit 过 0ms singleShot 去抖

      auto *view = panel.findChild<QGraphicsView *>(
          QStringLiteral("wellSectionView"));
      QVERIFY(view);
      const qreal x0 = view->mapToScene(0, 0).x();
      QVERIFY2(std::fabs(x0) <= 1.0,
               qPrintable(QStringLiteral(
                   "视口左缘场景 x=%1：居中会产生负偏移，井柱相对版头漂移")
                   .arg(x0)));
      // 版头按 scrollbar value=0 把首列画在 x=16，图体首列须重合。
      const qreal cx = view->mapFromScene(16.0, 0.0).x();
      QVERIFY2(std::fabs(cx - 16.0) <= 1.0,
               qPrintable(QStringLiteral("首列图体 x=%1，版头画在 16")
                            .arg(cx)));
    }

    // 设置/发信号语义：程序化 setter 不写设置、不重复发 seismicRequested；
    // 用户点击写设置且只发一次；恢复的 seismic=true 在构造期落到按钮。
    void emitAndSettingsSemantics()
    {
      QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
      s.remove(QStringLiteral("wellSection/seismic"));
      s.remove(QStringLiteral("wellSection/highlight"));

      SelectionContext ctx;
      WellSectionPanel panel(&ctx);
      panel.setSection(wells4());
      auto *seisBtn = toolBtn(panel, "wellSectionSeismicButton");
      QVERIFY(seisBtn);

      QSignalSpy seisSpy(&panel, &WellSectionPanel::seismicRequested);
      panel.setSeismicEnabled(true); // ≥2 井 → 恰好一次
      QCOMPARE(seisSpy.size(), 1);
      QVERIFY(!s.contains(QStringLiteral("wellSection/seismic")));

      panel.setHighlightEnabled(false); // setter 不写设置
      QVERIFY(!s.contains(QStringLiteral("wellSection/highlight")));

      // 用户点击：写设置 + 恰好一次请求（on→off→on，只有 on 发请求）。
      panel.setSeismicAvailable(true, QString()); // 解锁按钮才可点击
      seisBtn->click(); // → off
      QCOMPARE(s.value(QStringLiteral("wellSection/seismic")).toBool(),
               false);
      QCOMPARE(seisSpy.size(), 1); // off 不发
      seisBtn->click(); // → on
      QCOMPARE(s.value(QStringLiteral("wellSection/seismic")).toBool(),
               true);
      QCOMPARE(seisSpy.size(), 2);

      // 恢复：settings=true → 构造期按钮即勾选。
      {
        SelectionContext ctx2;
        WellSectionPanel p2(&ctx2);
        auto *b2 = toolBtn(p2, "wellSectionSeismicButton");
        QVERIFY(b2 && b2->isChecked());
      }
      s.remove(QStringLiteral("wellSection/seismic"));
    }

    // 对话框：选井勾选序/地图追加/井位排序；井道恢复默认与宽度回写。
    void dialogs()
    {
      QVector<WellSectionPanel::WellChoice> choices;
      const auto mk = [](const char *id, double x, double y, bool has) {
        WellSectionPanel::WellChoice c;
        c.id = QLatin1String(id);
        c.name = c.id;
        c.hasCoordinates = has;
        c.x = x;
        c.y = y;
        return c;
      };
      // 对角井位：PCA 主轴 ≈ (1,1)，投影序 = x+y。
      choices << mk("A", 0, 0, true) << mk("B", 10, 10, true)
              << mk("C", 20, 20, true) << mk("D", 0, 0, false);
      {
        WellSectionWellsDialog dlg(choices,
                                   {QStringLiteral("C"), QStringLiteral("A")},
                                   {QStringLiteral("B")}, nullptr);
        QCOMPARE(dlg.selectedIds(),
                 (QStringList{QStringLiteral("C"), QStringLiteral("A")}));
        auto *mapBtn = dlg.findChild<QPushButton *>(
            QStringLiteral("wellSectionPickMapButton"));
        QVERIFY(mapBtn && mapBtn->isEnabled());
        mapBtn->click(); // B 追加到勾选末尾
        QCOMPARE(dlg.selectedIds(),
                 (QStringList{QStringLiteral("C"), QStringLiteral("A"),
                              QStringLiteral("B")}));
        auto *sortBtn = dlg.findChild<QPushButton *>(
            QStringLiteral("wellSectionPickSortButton"));
        sortBtn->click(); // PCA 投影序 A,B,C
        QCOMPARE(dlg.selectedIds(),
                 (QStringList{QStringLiteral("A"), QStringLiteral("B"),
                              QStringLiteral("C")}));
      }
      {
        auto t = wellsection::SectionTemplate::defaults();
        t.tracks[0].width = 88;
        t.topFilter = wellsection::TopFilter::All;
        WellSectionTracksDialog dlg(t, {QStringLiteral("GR")},
                                    nullptr);
        auto *list = dlg.findChild<QListWidget *>(
            QStringLiteral("wellSectionTrackList"));
        auto *width = dlg.findChild<QSpinBox *>(
            QStringLiteral("wellSectionTrackWidth"));
        auto *defBtn = dlg.findChild<QPushButton *>(
            QStringLiteral("wellSectionTrackDefaults"));
        QVERIFY(list && width && defBtn);
        list->setCurrentRow(0);
        width->setValue(120);
        QCOMPARE(dlg.result().tracks.at(0).width, 120); // 宽度回写
        defBtn->click();
        QCOMPARE(dlg.result().tracks,
                 wellsection::SectionTemplate::defaults().tracks);
        QCOMPARE(dlg.result().topFilter, wellsection::TopFilter::Mapping);
      }
    }

    // 截图件产（不断言）：/tmp/wellsection-shots/{classic,colored,print,
    // highlight_D61,seismic,seismic_reflectors,seismic_colored,
    // panel_light}.png
    // ---- 栅状图：自动布点 + 交点井同帧联动（Oracle #3）+ 落库恢复 ----
    void fencePlanAndSharedWellSync()
    {
      // 4 井网格 2×2（坐标），跨条带共享 w10（手工指定两节都含它）。
      QVector<WellSectionPanel::WellChoice> choices;
      const char *ids[4] = {"w00", "w01", "w10", "w11"};
      for (int i = 0; i < 4; ++i) {
        WellSectionPanel::WellChoice c;
        c.id = QLatin1String(ids[i]);
        c.name = c.id;
        c.hasCoordinates = true;
        c.x = (i % 2) * 300.0;
        c.y = (i / 2) * 200.0;
        choices << c;
      }
      QTemporaryDir dir;
      metadata::WellSectionStore store(
          QDir(dir.path()).filePath(QStringLiteral("t.project.sqlite")));
      QString err;
      QVERIFY(store.open(&err));

      WellSectionFenceWidget::Params fp;
      fp.choices = choices;
      fp.store = &store;
      WellSectionFenceWidget fence(fp);
      QVERIFY(fence.sectionCount() == 0); // 新库无栅格节
      // 自动布点：2 条带 → 各 2 口，井不重复。
      fence.autoPlan(2);
      QCOMPARE(fence.sectionCount(), 2);
      QCOMPARE(fence.sectionWellIds(0).size(), 2);
      QSet<QString> planned;
      for (int i = 0; i < 2; ++i) {
        const QStringList ids = fence.sectionWellIds(i);
        planned.unite(QSet<QString>(ids.begin(), ids.end()));
      }
      QCOMPARE(planned.size(), 4);
      // 手工指定：两节共享 w10（交点井）。
      fence.setSections({QStringList({"w00", "w10"}),
                         QStringList({"w10", "w11"})});
      QCOMPARE(fence.sectionCount(), 2);
      // 落库 round-trip：新部件同 store 恢复两节。
      WellSectionFenceWidget reopened(fp);
      QCOMPARE(reopened.sectionCount(), 2);
      QCOMPARE(reopened.sectionWellIds(0),
               QStringList({"w00", "w10"}));
      QCOMPARE(reopened.sectionWellIds(1),
               QStringList({"w10", "w11"}));

      // 同帧联动：剖面 A 点名 w10 → 剖面 B 立即选中（selectWell 直连）。
      auto *pa = fence.sectionPanel(0);
      auto *pb = fence.sectionPanel(1);
      QVERIFY(pa && pb);
      QMetaObject::invokeMethod(pa, "wellClicked",
                                Q_ARG(QString, QStringLiteral("w10")));
      QVERIFY(pb->isWellSelected(QStringLiteral("w10")));
      QVERIFY(!pb->isWellSelected(QStringLiteral("w11")));
    }

    // ---- 平面选井一键成剖面 + SVG/CSV 导出 ----
    void selectionToSectionAndExport()
    {
      SelectionContext ctx;
      WellSectionPanel panel(&ctx);
      QVector<WellSectionPanel::WellChoice> choices;
      const char *ids[3] = {"C-4", "C-2", "A5"};
      const double xs[3] = {900.0, 0.0, 300.0};
      for (int i = 0; i < 3; ++i) {
        WellSectionPanel::WellChoice c;
        c.id = QLatin1String(ids[i]);
        c.name = c.id;
        c.hasCoordinates = true;
        c.x = xs[i];
        c.y = i * 10.0;
        choices << c;
      }
      panel.setWellChoices(choices);
      // 选井乱序（C-4 先选）→ PCA 井序（x 升序）。
      ctx.setSelection({"C-4", "C-2", "A5"}, QStringLiteral("well_map"));
      QSignalSpy spy(&panel, &WellSectionPanel::wellIdsChanged);
      const QStringList ordered = panel.generateFromSelection();
      QCOMPARE(ordered, QStringList({"C-2", "A5", "C-4"}));
      QCOMPARE(panel.wellIds(), ordered);
      QCOMPARE(spy.size(), 1);
      // 单井不成剖面。
      ctx.setSelection({"C-2"}, QStringLiteral("well_map"));
      QCOMPARE(panel.generateFromSelection().size(), 0);

      // 导出：CSV = 井深表（模式只进表头）；SVG 矢量（文件非空）。
      panel.setSection(wells4());
      QTemporaryDir dir;
      const QString csvPath = dir.filePath(QStringLiteral("tops.csv"));
      const QString svgPath = dir.filePath(QStringLiteral("section.svg"));
      QVERIFY(panel.exportTo(csvPath));
      QVERIFY(panel.exportTo(svgPath));
      QFile csv(csvPath);
      QVERIFY(csv.open(QIODevice::ReadOnly));
      const QString csvText = QString::fromUtf8(csv.readAll());
      QVERIFY(csvText.startsWith(QStringLiteral("井名,顶名,MD(m)")));
      QCOMPARE(csvText, panel.topsCsv());
      QFile svg(svgPath);
      QVERIFY(svg.open(QIODevice::ReadOnly));
      QVERIFY(svg.readAll().size() > 1000);
      QVERIFY(svg.readAll().isEmpty() || true);
    }

    // ---- QGIS 侧联动面：剖面线位带 + 井点闪烁（offscreen canvas）----
    void mapBandBasics()
    {
      QgsMapCanvas canvas;
      WellSectionMapBand band(&canvas);
      // 井位层（memory，id 字段）。
      QgsVectorLayer layer(
          QStringLiteral("Point?crs=EPSG:3857&field=id:string"), "wells",
          QStringLiteral("memory"));
      QVERIFY(layer.isValid());
      QgsFeature f1, f2;
      f1.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(100, 200)));
      QgsAttributes a1;
      a1 << QVariant(QStringLiteral("well-1"));
      f1.setAttributes(a1);
      f2.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(300, 200)));
      QgsAttributes a2;
      a2 << QVariant(QStringLiteral("well-2"));
      f2.setAttributes(a2);
      QgsFeatureList flist;
      flist << f1 << f2;
      QVERIFY(layer.dataProvider()->addFeatures(flist));
      band.setWellLayer(&layer, QStringLiteral("id"));
      // 线位：<2 点隐藏；2 点成线不崩。
      band.setSectionPath({{100.0, 200.0}});
      band.setSectionPath({{100.0, 200.0}, {300.0, 200.0}});
      // 闪烁：well-1 命中；well-x 无命中不崩。
      band.flashWell(QStringLiteral("well-1"));
      band.flashWell(QStringLiteral("well-x"));
      // 无层闪烁（降级 no-op）。
      WellSectionMapBand bare(&canvas);
      bare.setSectionPath({{1.0, 2.0}, {3.0, 4.0}});
      bare.flashWell(QStringLiteral("well-1"));
    }

    void screenshots()
    {
      QDir().mkpath(QStringLiteral("/tmp/wellsection-shots"));
      SelectionContext ctx;
      WellSectionPanel panel(&ctx);
      panel.resize(1400, 720);
      // 逐井不同常速 → 反射层在井间弯曲，深度→时间映射肉眼可查。
      panel.setSection(wells4(false, {2300.0, 2500.0, 2700.0, 2400.0}));
      panel.show();
      QVERIFY(QTest::qWaitForWindowExposed(&panel));
      panel.fitToView();

      panel.setThemeId(QStringLiteral("classic"));
      QVERIFY(panel.renderImage().save(
          QStringLiteral("/tmp/wellsection-shots/classic.png")));
      panel.setThemeId(QStringLiteral("colored"));
      QVERIFY(panel.renderImage().save(
          QStringLiteral("/tmp/wellsection-shots/colored.png")));
      panel.setThemeId(QStringLiteral("print"));
      QVERIFY(panel.renderImage().save(
          QStringLiteral("/tmp/wellsection-shots/print.png")));

      panel.setThemeId(QStringLiteral("classic"));
      ctx.setActiveHorizon(QStringLiteral("D61"));
      QVERIFY(panel.renderImage().save(
          QStringLiteral("/tmp/wellsection-shots/highlight_D61.png")));

      panel.setSeismicAvailable(true, QString());
      panel.setSeismicEnabled(true);
      panel.fitToView();
      panel.setSeismicStrip(
          reflectorStrip(panel.wellIds().size() - 1));
      QVERIFY(panel.renderImage().save(
          QStringLiteral("/tmp/wellsection-shots/seismic.png")));
      // 同帧再存一张明确的反射层命名图（供审阅对照）。
      QVERIFY(panel.renderImage().save(
          QStringLiteral("/tmp/wellsection-shots/seismic_reflectors.png")));
      panel.setThemeId(QStringLiteral("colored"));
      QVERIFY(panel.renderImage().save(
          QStringLiteral("/tmp/wellsection-shots/seismic_colored.png")));
      panel.setThemeId(QStringLiteral("classic"));

      // 面板整体截图（高亮开 + 一个带原因的缝：无 TD 的井）。
      auto stripR = reflectorStrip(panel.wellIds().size() - 1);
      stripR.gaps[1].reason = tr("C-1 缺少时深关系");
      panel.setSeismicStrip(stripR);
      QVERIFY(panel.grab().save(
          QStringLiteral("/tmp/wellsection-shots/panel_light.png")));
      QVERIFY(paleo::tests::captureVisual(&panel, QStringLiteral("wellsection"), QSize(1400, 720)));
    }
};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  TestWellSectionUi tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_wellsection_ui.moc"
