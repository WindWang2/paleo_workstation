// 层：视图
#include "paleotheme.h"

#include <QApplication>
#include <QEvent>
#include <QFont>
#include <QFontDatabase>
#include <QHeaderView>
#include <QList>
#include <QLabel>
#include <QPalette>
#include <QPointer>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>
#include <QTableView>

// qrc 对象住在静态库 paleo_core 里——链接器不会自动拉入无引用的目标文件，
// 显式引用其初始化符号把字体资源钉进每个最终二进制。
static const bool kFontsResourceLinked = [] {
  Q_INIT_RESOURCE(paleo_fonts);
  return true;
}();

// DESIGN.md 唯一视觉权威；这里的字面量就是 token 的落地处，别处引用本头。
namespace
{
  // qrc 前缀 :/paleo/fonts（resources/paleo_fonts.qrc）。
  struct FontSpec
  {
    const char *path;
    const char *family;
  };
  const FontSpec kVendoredFonts[] = {
      {":/paleo/fonts/NotoSansSC-Regular.otf", "Noto Sans SC"},
      {":/paleo/fonts/NotoSansSC-Bold.otf", "Noto Sans SC"},
      {":/paleo/fonts/JetBrainsMono-Regular.ttf", "JetBrains Mono"},
      {":/paleo/fonts/JetBrainsMono-Bold.ttf", "JetBrains Mono"},
  };

  QColor hx(const char *hex) { return QColor(QLatin1String(hex)); }

  // 浅色 token 全集（DESIGN.md colors + status-tag 原值）。
  // statusTag*Text = 胶囊文字色：浅底下语义色原值不足 4.5:1（实测 warning
  // 2.1:1 / success 3.0:1 / error 3.7:1），用深色变体拉到 AA（4.5+）；
  // 暗色深底上语义色提亮值本身已 AA，文字 = 语义色。
  const PaleoTheme::ThemeTokens kLight = {
      /*.primary =*/hx("#1B73D0"),        /*.onPrimary =*/hx("#FFFFFF"),
      /*.primaryHover =*/hx("#1565B8"),   /*.primaryText =*/hx("#1B73D0"),
      /*.focusRing =*/hx("#1B73D0"),      /*.surface =*/hx("#FFFFFF"),
      /*.surfaceAlt =*/hx("#EDF1F5"),     /*.surfaceAltRaised =*/hx("#EDF1F5"),
      /*.border =*/hx("#DFE5EC"),         /*.text =*/hx("#24303E"),
      /*.textMuted =*/hx("#5D6E80"),      /*.textDisabled =*/hx("#9AA7B4"),
      /*.placeholder =*/hx("#5D6E80"),    /*.success =*/hx("#43A047"),
      /*.warning =*/hx("#F29900"),        /*.error =*/hx("#E53935"),
      /*.successBg =*/hx("#E8F5E9"),      /*.warningBg =*/hx("#FFF4E0"),
      /*.errorBg =*/hx("#FDEBEB"),        /*.successText =*/hx("#2E7D32"),
      /*.warningText =*/hx("#9A5B00"),    /*.errorText =*/hx("#C62828"),
  };

  // 暗色 token 全集（DESIGN.md 决策日志 2026-09-29 翻案条）：中性阶翻转、
  // 语义色提亮保对比度；primary 填充色不变，文字位主色/焦点环提亮为
  // #5FA5F0。surfaceAltRaised = 暗色下比 surfaceAlt 略浮起一档的面
  // （hover/选中底，对应浅色 surface-alt 与 border 的关系）。
  const PaleoTheme::ThemeTokens kDark = {
      /*.primary =*/hx("#1B73D0"),        /*.onPrimary =*/hx("#FFFFFF"),
      /*.primaryHover =*/hx("#2F82DD"),   /*.primaryText =*/hx("#5FA5F0"),
      /*.focusRing =*/hx("#5FA5F0"),      /*.surface =*/hx("#252C36"),
      /*.surfaceAlt =*/hx("#1B212A"),     /*.surfaceAltRaised =*/hx("#2A313B"),
      /*.border =*/hx("#3B4552"),         /*.text =*/hx("#E4EAF2"),
      /*.textMuted =*/hx("#A3B1BF"),      /*.textDisabled =*/hx("#66717E"),
      /*.placeholder =*/hx("#A3B1BF"),    /*.success =*/hx("#5CB860"),
      /*.warning =*/hx("#FFB74D"),        /*.error =*/hx("#F76A61"),
      /*.successBg =*/hx("#1F3524"),      /*.warningBg =*/hx("#3A2E15"),
      /*.errorBg =*/hx("#3A1D1D"),        /*.successText =*/hx("#5CB860"),
      /*.warningText =*/hx("#FFB74D"),    /*.errorText =*/hx("#F76A61"),
  };

  // DESIGN.md status-tag：浅色底 + 深色文字变体（AA）；暗色为深底 +
  // 提亮语义字。Neutral（未计算）用 surface-alt 底 + text-muted 字 +
  // border 描边——中性状态不占语义色。
  struct CapsuleColors
  {
    const QColor bg;
    const QColor fg;
    bool hasBorder; // Neutral 描边，其余无
  };
  CapsuleColors capsuleColors(PaleoTheme::CapsuleKind kind,
                              const PaleoTheme::ThemeTokens &t)
  {
    switch (kind)
    {
      case PaleoTheme::CapsuleKind::Success: return {t.successBg, t.successText, false};
      case PaleoTheme::CapsuleKind::Warning: return {t.warningBg, t.warningText, false};
      case PaleoTheme::CapsuleKind::Error: return {t.errorBg, t.errorText, false};
      case PaleoTheme::CapsuleKind::Neutral:
      default: return {t.surfaceAltRaised, t.textMuted, true};
    }
  }

  QString qssHex(const QColor &c)
  {
    // 统一大写 #RRGGBB——浅色历史值（#EDF1F5 等）全部大写，逐字节可比。
    return c.name().toUpper();
  }

  // currentTheme() 的可写落点（实现细节不出本 TU）。
  PaleoTheme::Theme &themeRef()
  {
    static PaleoTheme::Theme s_current = PaleoTheme::Theme::Light;
    return s_current;
  }

  // currentDensity() 的可写落点（同 themeRef 口径）。
  PaleoTheme::Density &densityRef()
  {
    static PaleoTheme::Density s_density = PaleoTheme::Density::Comfort;
    return s_density;
  }

  // 活体主题样式的注册表：换主题（palette 变更事件）时重跑 builder。
  // relay 挂在 qApp 上，对每个注册 widget 装 eventFilter。
  class ThemedStyleSheetRelay : public QObject
  {
    public:
      static ThemedStyleSheetRelay *instance()
      {
        // 故意堆分配、父到 qApp、不自行 delete：随 QApplication 析构单次
        // 销毁。函数局部 static 对象会在 qApp 死后再跑一次析构 → 双重释放。
        static ThemedStyleSheetRelay *relay = new ThemedStyleSheetRelay(qApp);
        return relay;
      }

      void registerWidget(QWidget *w,
                          const std::function<QString()> &builder)
      {
        prune();
        m_entries.append({w, builder});
        w->installEventFilter(this);
        w->setStyleSheet(builder());
      }

      // widget 销毁（Destroy 类型事件）时清理。
      bool eventFilter(QObject *obj, QEvent *ev) override
      {
        if (ev->type() == QEvent::Destroy)
          prune();
        return QObject::eventFilter(obj, ev);
      }

      // 换主题后重算全部注册项。Qt6 实测：QApplication::setPalette 不给
      // 隐藏子控件发 ApplicationPaletteChange（事件只到顶层/不保证同步），
      // 所以由 applyTheme 显式直调——确定性优先于事件广播。
      void reapplyAll()
      {
        prune();
        for (const Entry &e : m_entries)
          if (e.widget)
            e.widget->setStyleSheet(e.builder());
      }

    private:
      ThemedStyleSheetRelay(QObject *parent) : QObject(parent) {}

      void prune()
      {
        for (int i = m_entries.size() - 1; i >= 0; --i)
          if (!m_entries.at(i).widget)
            m_entries.removeAt(i);
      }

      struct Entry
      {
        QPointer<QWidget> widget;
        std::function<QString()> builder;
      };
      QList<Entry> m_entries;
  };
} // namespace

namespace PaleoTheme
{
  Theme currentTheme() { return themeRef(); }

  const ThemeTokens &tokens(Theme theme)
  {
    return theme == Theme::Dark ? kDark : kLight;
  }

  bool ensureApplicationFonts()
  {
    bool all = true;
    for (const FontSpec &spec : kVendoredFonts)
    {
      const int id = QFontDatabase::addApplicationFont(QLatin1String(spec.path));
      if (id < 0)
      {
        all = false;
        qWarning("paleo: vendored font %s failed to register — falling back to "
                 "system fonts (Windows: Microsoft YaHei UI, Linux: Noto Sans CJK SC)",
                 spec.path);
      }
    }
    if (all)
      qInfo("paleo: vendored fonts registered (Noto Sans SC + JetBrains Mono)");
    return all;
  }

  QFont bodyFont()
  {
    QFont f;
    // DESIGN.md 退化链：vendor → 平台 CJK → 微软雅黑。
    f.setFamilies({QStringLiteral("Noto Sans SC"),
                   QStringLiteral("Noto Sans CJK SC"),
                   QStringLiteral("Microsoft YaHei UI")});
    f.setPointSize(kBodyPt);
    return f;
  }

  QFont monoFont()
  {
    QFont f;
    f.setFamilies({QStringLiteral("JetBrains Mono"), QStringLiteral("monospace")});
    f.setPointSize(kMonoPt);
    f.setStyleHint(QFont::TypeWriter);
    f.setFeature(QFont::Tag("tnum"), 1);
    return f;
  }

  QString focusRingStyleSheet(Theme theme)
  {
    // 2px 键盘焦点环（DESIGN.md focus-ring token，替代 Fusion 虚线框）。
    // QTableView/QTreeView 的环画在视口框上，避免与选区混淆。
    return QStringLiteral(
               "QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus,"
               "QTableView:focus, QTreeView:focus, QListView:focus, QListWidget:focus,"
               "QPushButton:focus, QToolButton:focus, QTabBar:focus,"
               "QSlider:focus, QCheckBox:focus, QRadioButton:focus,"
               "QTextEdit:focus, QPlainTextEdit:focus { border: 2px solid %1; }")
        .arg(qssHex(tokens(theme).focusRing));
  }

  QString capsuleStyleSheet(CapsuleKind kind, Theme theme)
  {
    const CapsuleColors c = capsuleColors(kind, tokens(theme));
    QString css = QStringLiteral(
                      "background: %1; color: %2; border-radius: 4px; padding: 1px 8px;")
                      .arg(qssHex(c.bg), qssHex(c.fg));
    if (c.hasBorder)
      css += QStringLiteral(" border: 1px solid %1;").arg(qssHex(tokens(theme).border));
    return css;
  }

  QLabel *capsuleLabel(const QString &text, CapsuleKind kind, QWidget *parent)
  {
    auto *l = new QLabel(text, parent);
    l->setObjectName(QStringLiteral("statusCapsule"));
    l->setProperty("capsuleKind", static_cast<int>(kind));
    // 活体注册：运行时切主题胶囊随 Relay 重算（不再停留构造时配色）。
    applyThemedStyleSheet(l, [kind] { return capsuleStyleSheet(kind); });
    l->setAlignment(Qt::AlignCenter);
    return l;
  }

  QPalette palette(Theme theme)
  {
    // DESIGN.md token 的 QPalette 落地：surface/surface-alt/text/
    // text-muted/primary。显式写 Active+Inactive+Disabled 三组——不写的组
    // 会继承系统 palette（暗色系统里浅色面板上出浅色字）。
    const ThemeTokens &t = tokens(theme);
    QPalette p;
    // 禁用态文本统一用 text-muted（与浅色历史行为一致——text-disabled 阶
    // 只用于显式 QSS）；禁用面 = 同角色的 active 值（旧浅色行为）。
    // 显式写 Active+Inactive+Disabled 三组，不写的组会继承系统 palette。
    const QColor disabledText = t.textMuted;
    const auto set = [&p](QPalette::ColorRole role, const QColor &active,
                          const QColor &disabled) {
      p.setColor(QPalette::Active, role, active);
      p.setColor(QPalette::Inactive, role, active);
      p.setColor(QPalette::Disabled, role, disabled);
    };
    set(QPalette::Window, t.surface, t.surface);
    set(QPalette::WindowText, t.text, disabledText);
    set(QPalette::Base, t.surface, t.surface);
    set(QPalette::AlternateBase, t.surfaceAltRaised, t.surfaceAltRaised);
    set(QPalette::Text, t.text, disabledText);
    set(QPalette::Button, t.surfaceAltRaised, t.surfaceAltRaised);
    set(QPalette::ButtonText, t.text, disabledText);
    set(QPalette::ToolTipBase, t.surface, t.surface);
    set(QPalette::ToolTipText, t.text, disabledText);
    set(QPalette::PlaceholderText, t.placeholder, disabledText);
    set(QPalette::Highlight, t.primary, t.primary.darker(theme == Theme::Dark ? 140 : 115));
    set(QPalette::HighlightedText, t.onPrimary, t.onPrimary);
    set(QPalette::Link, t.primaryText, disabledText);
    set(QPalette::LinkVisited, t.primaryHover, disabledText);
    set(QPalette::BrightText, t.error, disabledText);
    return p;
  }

  void applyTheme(Theme theme)
  {
    ensureApplicationFonts();
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    // currentTheme 先落再换 palette：ApplicationPaletteChange 事件风暴里
    // 的活体样式重算要读到新主题（builder 里调 tokens() 缺省取当前）。
    themeRef() = theme;
    QApplication::setPalette(palette(theme));
    QApplication::setFont(bodyFont());
    // 活体样式统一重算（palette 事件不保证送达隐藏子控件——见 relay 注释）。
    ThemedStyleSheetRelay::instance()->reapplyAll();
  }

  void applyLightTheme() { applyTheme(Theme::Light); }
  void applyDarkTheme() { applyTheme(Theme::Dark); }

  Theme themeFromSettings()
  {
    // 读自由、写克制（writeThemeToSettings 仅用户显式切换调用）。缺省浅色。
    QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
    const QString v = s.value(QStringLiteral("ui/theme")).toString();
    return v == QLatin1String("dark") ? Theme::Dark : Theme::Light;
  }

  void writeThemeToSettings(Theme theme)
  {
    QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
    s.setValue(QStringLiteral("ui/theme"),
               theme == Theme::Dark ? QStringLiteral("dark") : QStringLiteral("light"));
  }

  QString shellStyleSheet(Theme theme)
  {
    const ThemeTokens &t = tokens(theme);
    return QStringLiteral(
               "QMainWindow { background: %1; }"
               "QMainWindow::separator { width: 6px; height: 6px; background: %4; }"
               "QMainWindow::separator:hover { background: %3; }"
               "QDockWidget::title { background: %1; color: %2; padding: 6px 10px; }"
               "QStatusBar { background: %1; color: %3; }"
               "QWidget#mapInteractionContext { background: %1; color: %3;"
               " border-bottom: 1px solid %4; }"
               "QWidget#horizonChipRow { background: %5; border-bottom: 1px solid %4; }")
               .arg(qssHex(t.surfaceAlt), qssHex(t.text), qssHex(t.textMuted),
                    qssHex(t.border), qssHex(t.surface))
        // 条目视图三件套（选中/hover/斑马纹/密度 padding）统一出口——
        // goal/ui-experience-polish：三类列表控件不再各写选中态。
        + itemViewStyleSheet(theme);
  }

  QString itemViewStyleSheet(Theme theme, Density density)
  {
    const ThemeTokens &t = tokens(theme);
    // 树/列表行高走 padding；表行高走 verticalHeader（applyDensityToViewTree），
    // 不给 QTableView::item 加 padding 以免平移单元格内嵌件。
    return QStringLiteral(
               "QAbstractItemView { alternate-background-color: %1; }"
               "QTreeView::item, QListView::item { padding: %2px 3px; }"
               "QAbstractItemView::item:hover { background: %1; }"
               "QAbstractItemView::item:selected { background: %3; color: %4; }")
        .arg(qssHex(t.surfaceAltRaised))
        .arg(itemViewPaddingY(density))
        .arg(qssHex(t.primary))
        .arg(qssHex(t.onPrimary));
  }

  Density currentDensity() { return densityRef(); }

  Density densityFromSettings()
  {
    // 读自由、写克制（同 theme 口径）。缺省 comfort。
    QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
    return s.value(QStringLiteral("ui/density")).toString() ==
                   QLatin1String("compact")
               ? Density::Compact
               : Density::Comfort;
  }

  void writeDensityToSettings(Density density)
  {
    QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
    s.setValue(QStringLiteral("ui/density"),
               density == Density::Compact ? QStringLiteral("compact")
                                           : QStringLiteral("comfort"));
  }

  int itemViewPaddingY(Density density)
  {
    return density == Density::Compact ? 1 : 3;
  }

  int tableRowHeight(Density density)
  {
    return density == Density::Compact ? 20 : 26;
  }

  void applyDensity(Density density)
  {
    // 先落 current 再全量重算：shell QSS 的 builder 读 currentDensity()。
    densityRef() = density;
    ThemedStyleSheetRelay::instance()->reapplyAll();
  }

  void applyDensityToViewTree(QWidget *root, Density density)
  {
    if (!root)
      return;
    const int rowH = tableRowHeight(density);
    const auto tables = root->findChildren<QTableView *>();
    for (QTableView *table : tables)
      if (QHeaderView *vh = table->verticalHeader())
        vh->setDefaultSectionSize(rowH);
  }

  QString mutedCaptionStyleSheet(Theme theme)
  {
    return QStringLiteral("color: %1;").arg(qssHex(tokens(theme).textMuted));
  }

  QString sectionTitleStyleSheet(Theme theme)
  {
    return QStringLiteral("font-weight: 600; color: %1;")
        .arg(qssHex(tokens(theme).text));
  }

  void applyThemedStyleSheet(QWidget *widget,
                             const std::function<QString()> &builder)
  {
    if (!widget || !builder)
      return;
    ThemedStyleSheetRelay::instance()->registerWidget(widget, builder);
  }

  void pinRenderEnvironment()
  {
    // 渲染回归（TODOS P1）：跨平台噪点 = 字体替换 + 平台样式 + palette 差异。
    // 与应用同一条路径钉死（Fusion + 主题 palette + vendor 字体）。缺省浅色
    // ——截图基线是浅色；暗色用例自行 applyDarkTheme 后再钉一次。
    applyLightTheme();
  }

  QByteArray ribbonPaletteJson(Theme theme)
  {
    // 页签行 = surface-alt，ribbon 体 = surface；选中页签蓝字蓝下划线就是
    // 编号工作流标签（唯一签名元素）。hover = surfaceAltRaised（浅色下同
    // surface-alt），按下 = border 色。全部 keyColors 显式写，不留 derived。
    // dark 变体：isDark=true + 暗色阶（决策日志 2026-09-29 暗色翻案）。
    // white/black 语义是「模板要用的纯对比色」，两主题都不翻转（对齐
    // SARibbon 官方 office2021-dark 参考的 fixed 节）。
    const QByteArray light = QByteArrayLiteral(R"({
  "name": "Paleo Workbench",
  "version": "1.0",
  "isDark": false,
  "keyColors": {
    "accent": "#EDF1F5",
    "accent-hover": "#DFE5EC",
    "tab-accent": "#1B73D0",
    "tab-accent-hover": "#9AA7B4",
    "content-bg": "#FFFFFF",
    "content-hover-bg": "#EDF1F5",
    "content-pressed-bg": "#DFE5EC",
    "text-color": "#24303E",
    "subtitle": "#5D6E80",
    "separator": "#DFE5EC",
    "border-color": "#DFE5EC",
    "input-border": "#DFE5EC",
    "input-focus": "#1B73D0",
    "selection-bg": "#DFE5EC",
    "menu-border": "#DFE5EC",
    "close-bg": "#E53935",
    "close-bg-pressed": "#FDEBEB",
    "sys-button-hover": "#EDF1F5",
    "sys-button-pressed": "#DFE5EC",
    "white": "#FFFFFF",
    "black": "#24303E"
  }
})");
    const QByteArray dark = QByteArrayLiteral(R"({
  "name": "Paleo Workbench Dark",
  "version": "1.0",
  "isDark": true,
  "keyColors": {
    "accent": "#1B212A",
    "accent-hover": "#2A313B",
    "tab-accent": "#5FA5F0",
    "tab-accent-hover": "#66717E",
    "content-bg": "#252C36",
    "content-hover-bg": "#2A313B",
    "content-pressed-bg": "#3B4552",
    "text-color": "#E4EAF2",
    "subtitle": "#A3B1BF",
    "separator": "#3B4552",
    "border-color": "#3B4552",
    "input-border": "#3B4552",
    "input-focus": "#5FA5F0",
    "selection-bg": "#2A313B",
    "menu-border": "#3B4552",
    "close-bg": "#F76A61",
    "close-bg-pressed": "#3A1D1D",
    "sys-button-hover": "#2A313B",
    "sys-button-pressed": "#3B4552",
    "white": "#FFFFFF",
    "black": "#24303E"
  }
})");
    return theme == Theme::Dark ? dark : light;
  }

  QString ribbonStyleSheet(Theme theme)
  {
    const ThemeTokens &t = tokens(theme);
    return QStringLiteral(
               // office2021 模板把 hover 字色设成按下底色（浅灰字）——改回正文色。
               "SARibbonToolButton:hover { color: %1; }"
               // 选中态（活动地图工具 / 开着的过滤）：primary 描边 + 浮起面底。
               "SARibbonToolButton:checked, SARibbonToolButton:checked:hover {"
               " border: 1px solid %2; background-color: %3; color: %1; }"
               "SARibbonToolButton:disabled { color: %4; }"
               // 运行类动作（运行预测 / 单因素插值 / 运行验证）：文字用
               // primaryText（暗色提亮，保证深底对比度）。
               "SARibbonToolButton[paleoRun=\"true\"] { color: %5; }"
               "SARibbonToolButton[paleoRun=\"true\"]:disabled { color: %4; }"
               "SARibbonPanelLabel { font-size: 8pt; color: %8; }"
               "SARibbonSeparatorWidget { color: %6; background: transparent;"
               " border: none; border-left: 1px solid %6; margin: 4px 0; }"
               "SARibbonTabBar::tab { font-size: 9pt; color: %8;"
               " border: none; border-bottom: 2px solid transparent; }"
               "SARibbonTabBar::tab:selected { color: %5; border-bottom: 2px solid %5; }"
               "SARibbonButtonGroupWidget > QToolButton { padding: 0 6px; }"
               "SARibbonPanel QComboBox { border: 1px solid %6; border-radius: 4px;"
               " background: %7; color: %1; padding: 1px 6px; min-width: 132px; }"
               "SARibbonPanel QLabel#ribbonEditState { color: %8; }")
        .arg(qssHex(t.text), qssHex(t.primary), qssHex(t.surfaceAltRaised),
             qssHex(t.textDisabled), qssHex(t.primaryText), qssHex(t.border),
             qssHex(t.surface), qssHex(t.textMuted));
  }
} // namespace PaleoTheme
