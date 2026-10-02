#include <QtTest>
#include <QApplication>
#include <QFile>
#include <QFontDatabase>
#include <QFontInfo>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QSettings>
#include <QTemporaryDir>
#include <QStyle>
#include <QStyleFactory>
#include <QHeaderView>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWidget>

#include "../src/ui/paleotheme.h"
#include "../src/ui/paleoviewport.h"
#include "../src/ui/pages/pageshared.h"
#include "../src/ui/paleoemptystate.h"
#include "../src/ui/paleoicons.h"

// wave3/ux-consistency T32 — DESIGN.md token 的代码出口（PaleoTheme）契约：
// vendor 字体注册、mono/正文字体、2px #1B73D0 焦点环、状态胶囊 token、
// 渲染回归稳定化（字体 + Fusion 钉死 + 同帧两次渲染逐字节一致）。
class TestUxTheme : public QObject
{
  Q_OBJECT

  private slots:
    void initTestCase()
    {
      QVERIFY2(PaleoTheme::ensureApplicationFonts(), "vendored fonts must register");
    }

    // ---- goal/ui-experience-polish：密度切换与条目视图统一 QSS ----

    // 密度：设置往返守恒 + 活体 builder 随 applyDensity 重算档位。
    void densityRoundTripAndLiveRelayout()
    {
      QCOMPARE(PaleoTheme::densityFromSettings(), PaleoTheme::Density::Comfort);
      QCOMPARE(PaleoTheme::itemViewPaddingY(PaleoTheme::Density::Comfort), 3);
      QCOMPARE(PaleoTheme::itemViewPaddingY(PaleoTheme::Density::Compact), 1);
      QCOMPARE(PaleoTheme::tableRowHeight(PaleoTheme::Density::Comfort), 26);
      QCOMPARE(PaleoTheme::tableRowHeight(PaleoTheme::Density::Compact), 20);

      QWidget host;
      PaleoTheme::applyThemedStyleSheet(
          &host, [] { return PaleoTheme::itemViewStyleSheet(); });
      QVERIFY(host.styleSheet().contains(QStringLiteral("padding: 3px")));
      PaleoTheme::applyDensity(PaleoTheme::Density::Compact);
      QCOMPARE(PaleoTheme::currentDensity(), PaleoTheme::Density::Compact);
      QVERIFY(host.styleSheet().contains(QStringLiteral("padding: 1px")));
      PaleoTheme::applyDensity(PaleoTheme::Density::Comfort);
      QVERIFY(host.styleSheet().contains(QStringLiteral("padding: 3px")));
    }

    // 条目视图三件套：选中（primary 一致化）/hover/斑马纹双主题 token。
    void itemViewSheetUnifiesSelectionHoverZebra()
    {
      const QString light =
          PaleoTheme::itemViewStyleSheet(PaleoTheme::Theme::Light);
      QVERIFY(light.contains(QStringLiteral("alternate-background-color: #EDF1F5")));
      QVERIFY(light.contains(QStringLiteral("::item:hover { background: #EDF1F5; }")));
      QVERIFY(light.contains(
          QStringLiteral("::item:selected { background: #1B73D0; color: #FFFFFF; }")));
      const QString dark = PaleoTheme::itemViewStyleSheet(PaleoTheme::Theme::Dark);
      QVERIFY(dark.contains(QStringLiteral("::item:hover { background: #2A313B; }")));
      QVERIFY(dark.contains(
          QStringLiteral("::item:selected { background: #1B73D0; color: #FFFFFF; }")));
      QVERIFY(dark.contains(QStringLiteral("alternate-background-color: #2A313B")));
      // 壳级全量拼进了三件套（主窗/父挂对话框全覆盖）。
      QVERIFY(PaleoTheme::shellStyleSheet(PaleoTheme::Theme::Light)
                  .contains(QStringLiteral("::item:hover")));
    }

    // 表行高密度落档：applyDensityToViewTree 扫 QTableView 族。
    void densityAppliesRowHeightsToTables()
    {
      QWidget host;
      auto *lay = new QVBoxLayout(&host);
      auto *table = new QTableWidget(&host);
      lay->addWidget(table);
      PaleoTheme::applyDensityToViewTree(&host, PaleoTheme::Density::Compact);
      QCOMPARE(table->verticalHeader()->defaultSectionSize(), 20);
      PaleoTheme::applyDensityToViewTree(&host, PaleoTheme::Density::Comfort);
      QCOMPARE(table->verticalHeader()->defaultSectionSize(), 26);
    }

    void narrowEmptyStatesAndLiveCaptions()
    {
      PaleoTheme::applyLightTheme();
      QWidget host;
      host.resize(220, 180);
      PaleoEmptyStateLabel empty(QStringLiteral("图层树为空，请先导入数据后再选择编图层位。"), &host);
      host.show();
      QTest::qWait(10);
      QVERIFY(host.rect().contains(empty.geometry()));
      empty.setDetailText(QStringLiteral("找不到源文件，请在数据管理中重新定位文件，再重试当前操作。"));
      QVERIFY(host.rect().contains(empty.geometry()));
      host.resize(160, 180);
      QTest::qWait(10);
      QVERIFY(host.rect().contains(empty.geometry()));
      auto *caption = paleo::pagesinternal::caption(QStringLiteral("时间残差"), &host);
      const auto light = caption->styleSheet();
      PaleoTheme::applyDarkTheme();
      QVERIFY(caption->styleSheet() != light);
      QVERIFY(caption->styleSheet().contains(PaleoTheme::tokens().textMuted.name().toUpper()));
      PaleoTheme::applyLightTheme();
    }

    void panelViewportOnlyMeasuresCurrentPage()
    {
      QScrollArea viewport;
      viewport.setWidgetResizable(true);
      viewport.resize(320, 400);
      auto *host = new PaleoPanelHost;
      auto *stack = static_cast<QStackedLayout *>(host->layout());
      auto *longForm = new QWidget(host);
      longForm->setMinimumHeight(1200);
      auto *longContent = new QVBoxLayout(longForm);
      auto *wrapped = new QLabel(QStringLiteral("长参数说明 ").repeated(200), longForm);
      wrapped->setWordWrap(true);
      longContent->addWidget(wrapped);
      stack->addWidget(longForm);
      auto *compact = new QWidget(host);
      auto *content = new QVBoxLayout(compact);
      auto *guidance = new QLabel(QStringLiteral("问题清单"), compact);
      guidance->setWordWrap(true);
      content->addWidget(guidance);
      stack->addWidget(compact);
      viewport.setWidget(host);
      viewport.show();
      QTRY_VERIFY(viewport.verticalScrollBar()->maximum() > 0);
      stack->setCurrentWidget(compact);
      QTRY_COMPARE(viewport.verticalScrollBar()->maximum(), 0);
      QVERIFY(viewport.viewport()->height() >= compact->height());
      stack->setCurrentWidget(longForm);
      QTRY_VERIFY(viewport.verticalScrollBar()->maximum() > 0);
    }

    // vendor 字体真的进了 QFontDatabase（不是系统字体顶包）。
    void vendoredFontsRegistered()
    {
      const QStringList families = QFontDatabase::families();
      QVERIFY2(families.contains(QStringLiteral("Noto Sans SC")),
               "Noto Sans SC (vendored) missing from QFontDatabase");
      QVERIFY2(families.contains(QStringLiteral("JetBrains Mono")),
               "JetBrains Mono (vendored) missing from QFontDatabase");
    }

    // mono 数字面：JetBrains Mono 9pt（DESIGN.md mono token），解析命中
    // vendor 家族而非系统替换。
    void monoFontIsJetBrainsNinePt()
    {
      const QFont f = PaleoTheme::monoFont();
      QCOMPARE(f.pointSize(), PaleoTheme::kMonoPt);
      const QFontInfo info(f);
      QVERIFY2(info.family() == QStringLiteral("JetBrains Mono"),
               qPrintable(QStringLiteral("resolved family: ") + info.family()));
      // tnum 等宽：数字位宽一致（同一字符串里 1 和 8 宽度相同）。
      QFontMetricsF fm(f);
      const qreal w1 = fm.horizontalAdvance(QStringLiteral("1111"));
      const qreal w8 = fm.horizontalAdvance(QStringLiteral("8888"));
      QCOMPARE(w1, w8);
    }

    // 正文字体：Noto Sans SC 9pt（DESIGN.md body token）。
    void bodyFontIsNotoSansScNinePt()
    {
      const QFont f = PaleoTheme::bodyFont();
      QCOMPARE(f.pointSize(), PaleoTheme::kBodyPt);
      const QFontInfo info(f);
      QVERIFY2(info.family() == QStringLiteral("Noto Sans SC"),
               qPrintable(QStringLiteral("resolved family: ") + info.family()));
    }

    // 焦点环：2px #1B73D0，覆盖可聚焦控件（DESIGN.md focus-ring token）。
    void focusRingCoversFocusableWidgets()
    {
      const QString css = PaleoTheme::focusRingStyleSheet();
      QVERIFY(css.contains(QStringLiteral("2px solid #1B73D0")));
      for (const char *sel : {"QLineEdit:focus", "QComboBox:focus", "QTableView:focus",
                              "QPushButton:focus", "QSpinBox:focus", "QTreeView:focus"})
        QVERIFY2(css.contains(QLatin1String(sel)),
                 qPrintable(QStringLiteral("focus ring misses %1").arg(QString::fromLatin1(sel))));
    }

    // 状态胶囊：DESIGN.md status-tag token 逐色断言（浅底 + 深色文字变体，
    // 语义原色在浅底上不足 AA，文字走 successText/warningText/errorText）。
    void capsuleTokensMatchDesignMd()
    {
      using K = PaleoTheme::CapsuleKind;
      const struct { K kind; const char *bg; const char *fg; } cases[] = {
          {K::Success, "#E8F5E9", "#2E7D32"},
          {K::Warning, "#FFF4E0", "#9A5B00"},
          {K::Error, "#FDEBEB", "#C62828"},
          {K::Neutral, "#EDF1F5", "#5D6E80"},
      };
      for (const auto &c : cases)
      {
        const QString css = PaleoTheme::capsuleStyleSheet(c.kind);
        QVERIFY2(css.contains(c.bg),
                 qPrintable(QStringLiteral("capsule bg %1 missing").arg(c.bg)));
        QVERIFY2(css.contains(c.fg),
                 qPrintable(QStringLiteral("capsule fg %1 missing").arg(c.fg)));
        QVERIFY(css.contains(QStringLiteral("border-radius"))); // 胶囊圆角
      }
    }

    void capsuleLabelCarriesKindAndCentering()
    {
      using K = PaleoTheme::CapsuleKind;
      auto *l = PaleoTheme::capsuleLabel(QStringLiteral("通过"), K::Success, nullptr);
      QVERIFY(l);
      QCOMPARE(l->objectName(), QStringLiteral("statusCapsule"));
      QCOMPARE(l->text(), QStringLiteral("通过"));
      QCOMPARE(l->property("capsuleKind").toInt(), static_cast<int>(K::Success));
      QVERIFY(l->styleSheet().contains(QStringLiteral("#E8F5E9")));
      QVERIFY(l->alignment() & Qt::AlignHCenter);
      delete l;
    }

    // 渲染稳定化：Fusion + vendor 字体钉住后，同帧两次渲染逐字节一致
    // （跨平台字体噪点消除的基线条件）。
    void renderEnvironmentIsPinnedAndDeterministic()
    {
      PaleoTheme::pinRenderEnvironment();
      QCOMPARE(qApp->style()->objectName().compare(QLatin1String("Fusion"),
                                                   Qt::CaseInsensitive),
               0);
      QCOMPARE(qApp->font().pointSize(), PaleoTheme::kBodyPt);

      const auto render = [] {
        QLabel l(QStringLiteral("残差 12.5 ms · 井 A1"));
        l.setFont(PaleoTheme::monoFont());
        l.resize(220, 40);
        QImage img(l.size(), QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::white);
        QPainter p(&img);
        l.render(&p);
        return img;
      };
      const QImage a = render();
      const QImage b = render();
      QCOMPARE(a, b); // QImage operator== 逐像素
    }

    // 默认浅色主题：applyLightTheme 钉死 DESIGN.md 浅色 palette——系统
    // 深色模式下 Window/Base 仍为浅色、WindowText/Text 仍为深色，三组
    // （Active/Inactive/Disabled）显式写齐，不允许系统 palette 泄漏。
    void lightThemePaletteIsExplicit()
    {
      PaleoTheme::applyLightTheme();
      QCOMPARE(qApp->style()->objectName().compare(QLatin1String("Fusion"),
                                                   Qt::CaseInsensitive),
               0);
      QCOMPARE(qApp->font().pointSize(), PaleoTheme::kBodyPt);

      for (const auto group : {QPalette::Active, QPalette::Inactive,
                               QPalette::Disabled})
      {
        const QPalette p = qApp->palette();
        QVERIFY2(p.color(group, QPalette::Window).lightness() > 200,
                 "Window must stay light under a dark system palette");
        QVERIFY2(p.color(group, QPalette::Base).lightness() > 200,
                 "Base must stay light under a dark system palette");
        QVERIFY2(p.color(group, QPalette::WindowText).lightness() < 120,
                 "WindowText must stay dark");
        QVERIFY2(p.color(group, QPalette::Text).lightness() < 120,
                 "Text must stay dark");
        QVERIFY2(p.color(group, QPalette::ButtonText).lightness() < 120,
                 "ButtonText must stay dark");
      }
    }

    void colorTokensAndTypographyMatchDesignMd()
    {
      QCOMPARE(PaleoTheme::kDisplayPt, 15);
      QCOMPARE(PaleoTheme::kTitlePt, 12);
      QCOMPARE(PaleoTheme::kBodyPt, 9);
      QCOMPARE(PaleoTheme::kLabelPt, 8);
      QCOMPARE(PaleoTheme::kMonoPt, 9);

      QCOMPARE(PaleoTheme::kColorPrimary.name().toUpper(), QStringLiteral("#1B73D0"));
      QCOMPARE(PaleoTheme::kColorOnPrimary.name().toUpper(), QStringLiteral("#FFFFFF"));
      QCOMPARE(PaleoTheme::kColorPrimaryHover.name().toUpper(), QStringLiteral("#1565B8"));
      QCOMPARE(PaleoTheme::kColorSurface.name().toUpper(), QStringLiteral("#FFFFFF"));
      QCOMPARE(PaleoTheme::kColorSurfaceAlt.name().toUpper(), QStringLiteral("#EDF1F5"));
      QCOMPARE(PaleoTheme::kColorBorder.name().toUpper(), QStringLiteral("#DFE5EC"));
      QCOMPARE(PaleoTheme::kColorText.name().toUpper(), QStringLiteral("#24303E"));
      QCOMPARE(PaleoTheme::kColorTextMuted.name().toUpper(), QStringLiteral("#5D6E80"));
      QCOMPARE(PaleoTheme::kColorTextDisabled.name().toUpper(), QStringLiteral("#9AA7B4"));
      QCOMPARE(PaleoTheme::kColorSuccess.name().toUpper(), QStringLiteral("#43A047"));
      QCOMPARE(PaleoTheme::kColorWarning.name().toUpper(), QStringLiteral("#F29900"));
      QCOMPARE(PaleoTheme::kColorError.name().toUpper(), QStringLiteral("#E53935"));
    }

    void cleanupTestCase()
    {
      // 暗色用例把全局主题拨到 Dark——收尾还原浅色，进程级状态不外漏。
      PaleoTheme::applyLightTheme();
    }

  private slots:
    // ---- 暗色翻案（DESIGN.md 决策日志 2026-09-28）----

    // 暗色 palette 三组显式写齐：深底浅字，不允许系统/浅色泄漏。
    void darkThemePaletteIsExplicit()
    {
      PaleoTheme::applyDarkTheme();
      QCOMPARE(PaleoTheme::currentTheme(), PaleoTheme::Theme::Dark);
      QCOMPARE(qApp->style()->objectName().compare(QLatin1String("Fusion"),
                                                   Qt::CaseInsensitive),
               0);
      for (const auto group : {QPalette::Active, QPalette::Inactive,
                               QPalette::Disabled})
      {
        const QPalette p = qApp->palette();
        QVERIFY2(p.color(group, QPalette::Window).lightness() < 80,
                 "Window must stay dark");
        QVERIFY2(p.color(group, QPalette::Base).lightness() < 80,
                 "Base must stay dark");
        QVERIFY2(p.color(group, QPalette::Text).lightness() > 150,
                 "Text must stay light on dark surfaces");
        QVERIFY2(p.color(group, QPalette::WindowText).lightness() > 150,
                 "WindowText must stay light");
        QVERIFY2(p.color(group, QPalette::ButtonText).lightness() > 120,
                 "Disabled ButtonText uses text-muted (>=A3B1BF lightness)");
      }
      // 回浅色，不打扰后续用例的隐含浅色基线。
      PaleoTheme::applyLightTheme();
    }

    // 暗色 token / QSS 全集：焦点环提亮、壳 QSS 深底、胶囊深底提亮字、
    // ribbon 调色板 isDark。
    void darkTokensAndStyleSheetsCarryDarkValues()
    {
      const auto &d = PaleoTheme::tokens(PaleoTheme::Theme::Dark);
      QCOMPARE(d.surface.name().toUpper(), QStringLiteral("#252C36"));
      QCOMPARE(d.surfaceAlt.name().toUpper(), QStringLiteral("#1B212A"));
      QCOMPARE(d.border.name().toUpper(), QStringLiteral("#3B4552"));
      QCOMPARE(d.text.name().toUpper(), QStringLiteral("#E4EAF2"));
      QCOMPARE(d.textMuted.name().toUpper(), QStringLiteral("#A3B1BF"));
      QCOMPARE(d.primaryText.name().toUpper(), QStringLiteral("#5FA5F0"));
      QCOMPARE(d.focusRing.name().toUpper(), QStringLiteral("#5FA5F0"));
      // primary 填充色两主题同值（交互蓝不随主题漂移）。
      QCOMPARE(d.primary, PaleoTheme::kColorPrimary);

      QVERIFY(PaleoTheme::focusRingStyleSheet(PaleoTheme::Theme::Dark)
                  .contains(QStringLiteral("2px solid #5FA5F0")));

      const QString shell = PaleoTheme::shellStyleSheet(PaleoTheme::Theme::Dark);
      QVERIFY(shell.contains(QStringLiteral("QMainWindow { background: #1B212A; }")));
      QVERIFY(shell.contains(QStringLiteral("QStatusBar { background: #1B212A; color: #A3B1BF; }")));
      QVERIFY(!shell.contains(QStringLiteral("background: #FFFFFF")));

      const QString capsule =
          PaleoTheme::capsuleStyleSheet(PaleoTheme::CapsuleKind::Error,
                                        PaleoTheme::Theme::Dark);
      QVERIFY(capsule.contains(QStringLiteral("#3A1D1D")));
      QVERIFY(capsule.contains(QStringLiteral("#F76A61")));

      const QByteArray ribbon =
          PaleoTheme::ribbonPaletteJson(PaleoTheme::Theme::Dark);
      QVERIFY(ribbon.contains("\"isDark\": true"));
      QVERIFY(ribbon.contains("\"content-bg\": \"#252C36\""));

      // 浅色输出不受暗色影响（显式 Theme 参数的两态独立）。
      QVERIFY(PaleoTheme::shellStyleSheet(PaleoTheme::Theme::Light)
                  .contains(QStringLiteral("background: #EDF1F5;")));
    }

    // 缺省实参跟随当前主题：applyDarkTheme 后无参调用给暗色阶。
    void defaultArgsFollowCurrentTheme()
    {
      PaleoTheme::applyLightTheme();
      QVERIFY(PaleoTheme::focusRingStyleSheet().contains(
          QStringLiteral("2px solid #1B73D0")));
      PaleoTheme::applyDarkTheme();
      QVERIFY(PaleoTheme::focusRingStyleSheet().contains(
          QStringLiteral("2px solid #5FA5F0")));
      QVERIFY(PaleoTheme::tokens().surface.name().toUpper() ==
              QStringLiteral("#252C36"));
      PaleoTheme::applyLightTheme();
    }

    // 活体主题样式：注册后换主题，widget 样式表自动重算（palette 事件风暴）。
    void themedStyleSheetRelayRebuildsOnThemeChange()
    {
      QWidget host;
      QLabel l(QStringLiteral("次级说明"), &host);
      PaleoTheme::applyThemedStyleSheet(
          &l, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
      QVERIFY(l.styleSheet().contains(QStringLiteral("#5D6E80")));
      PaleoTheme::applyDarkTheme();
      QVERIFY2(l.styleSheet().contains(QStringLiteral("#A3B1BF")),
               qPrintable(QStringLiteral("style after dark switch: ") + l.styleSheet()));
      PaleoTheme::applyLightTheme();
      QVERIFY(l.styleSheet().contains(QStringLiteral("#5D6E80")));
    }

    // 图标暗色再着色：自绘图标墨色翻转；QGIS 深 glyph 经 Plus 提亮。
    void iconsRetintForDarkTheme()
    {
      PaleoTheme::applyLightTheme();
      QImage lightGlyph = PaleoIcons::maximize()
                              .pixmap(32, 32)
                              .toImage()
                              .convertToFormat(QImage::Format_ARGB32);
      PaleoTheme::applyDarkTheme();
      QImage darkGlyph = PaleoIcons::maximize()
                             .pixmap(32, 32)
                             .toImage()
                             .convertToFormat(QImage::Format_ARGB32);
      // 描边像素扫描（浅色深墨 / 暗色浅墨）：取整幅最亮的不透明像素比较。
      const auto brightest = [](const QImage &img) {
        int best = -1;
        for (int y = 0; y < img.height(); ++y)
          for (int x = 0; x < img.width(); ++x)
          {
            const QRgb c = img.pixel(x, y);
            if (qAlpha(c) > 200)
              best = qMax(best, (qRed(c) + qGreen(c) + qBlue(c)) / 3);
          }
        return best;
      };
      QVERIFY2(brightest(lightGlyph) < 120,
               "light glyph must be dark ink on transparent");
      QVERIFY2(brightest(darkGlyph) > 150,
               qPrintable(QStringLiteral("dark glyph must be lifted ink, brightest=%1")
                              .arg(brightest(darkGlyph))));

      // tintForDarkTheme：手工深色 icon 提亮、透明区保持透明。
      QPixmap pm(8, 8);
      pm.fill(Qt::transparent);
      QPainter p(&pm);
      p.setPen(QColor(36, 48, 62)); // #24303E
      p.drawLine(0, 4, 7, 4);
      p.end();
      QIcon tinted = PaleoIcons::tintForDarkTheme(QIcon(pm));
      const QImage img =
          tinted.pixmap(8, 8).toImage().convertToFormat(QImage::Format_ARGB32);
      QVERIFY(qRed(img.pixel(4, 4)) > 150);
      QCOMPARE(qAlpha(img.pixel(0, 0)), 0);
      PaleoTheme::applyLightTheme();
    }

    // 空态卡片共享组件：objectName 三态 + 样式随主题活体重算。
    void emptyStateCardIsThemed()
    {
      PaleoTheme::applyLightTheme(); // 用例自钉基线，不依赖前序用例收尾
      QWidget host;
      host.resize(400, 300);
      auto *card = new PaleoEmptyStateLabel(
          QStringLiteral("还没有数据 — 先导入工区"), &host);
      QCOMPARE(card->objectName(), QStringLiteral("emptyStateCard"));
      QVERIFY(card->styleSheet().contains(QStringLiteral("#5D6E80")));
      auto *err = new PaleoEmptyStateLabel(QStringLiteral("加载失败"), &host,
                                           PaleoEmptyStateLabel::Kind::Error);
      QCOMPARE(err->objectName(), QStringLiteral("emptyStateCardError"));
      QVERIFY(err->styleSheet().contains(QStringLiteral("#C62828")));
      PaleoTheme::applyDarkTheme();
      QVERIFY2(card->styleSheet().contains(QStringLiteral("#A3B1BF")),
               qPrintable(card->styleSheet()));
      QVERIFY2(err->styleSheet().contains(QStringLiteral("#F76A61")),
               qPrintable(err->styleSheet()));
      PaleoTheme::applyLightTheme();
    }

    // QSettings 持久化：缺省浅色；显式写后读回。测试进程经 setPath 隔离
    // （main 里重定向 IniFormat UserScope 到临时目录，tst_ui 同惯例）。
    void themeSettingsRoundTripDefaultsLight()
    {
      QCOMPARE(PaleoTheme::themeFromSettings(), PaleoTheme::Theme::Light);
      PaleoTheme::writeThemeToSettings(PaleoTheme::Theme::Dark);
      QCOMPARE(PaleoTheme::themeFromSettings(), PaleoTheme::Theme::Dark);
      PaleoTheme::writeThemeToSettings(PaleoTheme::Theme::Light);
      QCOMPARE(PaleoTheme::themeFromSettings(), PaleoTheme::Theme::Light);
      QSettings(QStringLiteral("paleo"), QStringLiteral("paleo"))
          .remove(QStringLiteral("ui/theme"));
    }

    // 双主题对照截图（视觉审计取证）：PALEO_UI_CAPTURE 设了才落盘，
    // light/dark 各一张；未设完全跳过。
    void dualThemeScreenshotsForVisualAudit()
    {
      const QString dir = qEnvironmentVariable("PALEO_UI_CAPTURE");
      if (dir.isEmpty())
        return;

      const auto render = [dir](PaleoTheme::Theme theme, const QString &name) {
        PaleoTheme::applyTheme(theme);
        QWidget surface;
        surface.setObjectName(QStringLiteral("auditSurface"));
        auto *lay = new QVBoxLayout(&surface);
        lay->setContentsMargins(24, 24, 24, 24);
        auto *title = new QLabel(QStringLiteral("Paleo Workbench · 双主题对照"));
        QFont f = title->font();
        f.setPointSize(12);
        title->setFont(f);
        lay->addWidget(title);
        lay->addWidget(PaleoTheme::capsuleLabel(
            QStringLiteral("通过"), PaleoTheme::CapsuleKind::Success, &surface));
        lay->addWidget(PaleoTheme::capsuleLabel(
            QStringLiteral("未执行"), PaleoTheme::CapsuleKind::Neutral, &surface));
        auto *muted = new QLabel(QStringLiteral("次级说明文字（text-muted）"), &surface);
        PaleoTheme::applyThemedStyleSheet(
            muted, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
        lay->addWidget(muted);
        auto *card = new PaleoEmptyStateLabel(
            QStringLiteral("空态卡片 — 下一步动作指引写在这里"), &surface,
            PaleoEmptyStateLabel::Kind::Empty);
        lay->addWidget(card, 1);
        surface.resize(420, 300);
        QImage img(surface.size(), QImage::Format_ARGB32_Premultiplied);
        QPainter p(&img);
        surface.render(&p);
        p.end();
        img.save(dir + QLatin1Char('/') + name);
      };
      render(PaleoTheme::Theme::Light, QStringLiteral("uxtheme_light.png"));
      render(PaleoTheme::Theme::Dark, QStringLiteral("uxtheme_dark.png"));
      QVERIFY(QFile::exists(dir + QLatin1Char('/') + "uxtheme_dark.png"));
      PaleoTheme::applyLightTheme();
    }
};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  // 每运行一次的临时目录（对齐 tst_seismic_sectionui 惯例）：既隔离直跑时
  // 的真实用户配置，也消除固定 /tmp 路径跨运行/跨用户的陈旧状态向量
  // （ctest 路径另有 add_paleo_test 的 XDG/HOME 沙箱兜底）。
  static QTemporaryDir settingsDir;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
  TestUxTheme tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_uxtheme.moc"
