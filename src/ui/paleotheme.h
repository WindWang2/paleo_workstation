// 层：视图
#pragma once
#include <QColor>
#include <QString>
#include <QVariant>

#include <functional>

class QFont;
class QLabel;
class QPalette;
class QWidget;

// ui/paleotheme — DESIGN.md token 的唯一代码出口（wave3/ux-consistency T32）。
// 字体注册、焦点环、状态胶囊、mono 数字面都从这里取；面板代码不得再各写
// 一份颜色字面量。找不到 vendor 字体时如实降级（日志说明），不阻止启动。
//
// 暗色翻案（DESIGN.md 决策日志 2026-09-29）：Theme::Dark 全套 token 与
// 浅色同构——UI chrome 跟随主题，数据符号色（DESIGN.md「地图域配色」：
// 相色标/物源线/井曲线/相名色）不属 UI token，暗色下保持不变。

namespace PaleoTheme
{
  // DESIGN.md typography tokens。
  constexpr int kDisplayPt = 15; // 图件标题 chip、页级标题 (15pt)
  constexpr int kTitlePt = 12;   // 标题 (12pt)
  constexpr int kBodyPt = 9;     // 正文基准（Noto Sans SC 9pt）
  constexpr int kLabelPt = 8;    // 次级标签 (8pt)
  constexpr int kMonoPt = 9;     // 数值/坐标/深度（JetBrains Mono 9pt，tnum 等宽）

  // DESIGN.md light color tokens（浅色规范主体，历史常量保留——测试与
  // 既有代码按名引用；新代码优先走 tokens(Theme)）。
  inline const QColor kColorPrimary = QColor(QStringLiteral("#1B73D0"));
  inline const QColor kColorOnPrimary = QColor(QStringLiteral("#FFFFFF"));
  inline const QColor kColorPrimaryHover = QColor(QStringLiteral("#1565B8"));
  inline const QColor kColorSurface = QColor(QStringLiteral("#FFFFFF"));
  inline const QColor kColorSurfaceAlt = QColor(QStringLiteral("#EDF1F5"));
  inline const QColor kColorBorder = QColor(QStringLiteral("#DFE5EC"));
  inline const QColor kColorText = QColor(QStringLiteral("#24303E"));
  inline const QColor kColorTextMuted = QColor(QStringLiteral("#5D6E80"));
  inline const QColor kColorTextDisabled = QColor(QStringLiteral("#9AA7B4"));
  inline const QColor kColorSuccess = QColor(QStringLiteral("#43A047"));
  inline const QColor kColorWarning = QColor(QStringLiteral("#F29900"));
  inline const QColor kColorError = QColor(QStringLiteral("#E53935"));

  enum class Theme { Light, Dark };

  // 当前主题（applyTheme 落的那个；进程初始为 Light）。
  Theme currentTheme();

  // 双主题 token 全集（DESIGN.md colors + status-tag）。primary/onPrimary/
  // success/warning/error 语义色两主题同值；surface/border/text 阶与
  // 状态胶囊底色按主题翻转；primaryText = 「文字位用的主色」（浅色 =
  // primary，暗色提亮保证深底对比度，兼 focus-ring）。
  struct ThemeTokens
  {
    QColor primary, onPrimary, primaryHover;
    QColor primaryText;   // 文字位主色（暗色提亮）
    QColor focusRing;     // 键盘焦点环（= primaryText）
    QColor surface, surfaceAlt, surfaceAltRaised;
    QColor border, text, textMuted, textDisabled, placeholder;
    QColor success, warning, error;
    QColor successBg, warningBg, errorBg;     // status-tag 底色（capsule）
    QColor successText, warningText, errorText; // 胶囊文字色（浅色=深色变体保 AA）
  };
  // theme 省略 = 当前主题（缺省实参在调用点求值，暗色下自动取暗色阶）。
  const ThemeTokens &tokens(Theme theme = currentTheme());

  // 启动时注册 vendor 字体（resources/fonts，qrc 前缀 :/paleo/fonts）。
  // 返回是否全部注册成功；单个失败记 qWarning 并继续（降级到系统字体，
  // DESIGN.md：Windows→Microsoft YaHei UI，Linux→Noto Sans CJK SC）。
  bool ensureApplicationFonts();

  // 正文字体：Noto Sans SC 9pt，退化链 Noto Sans CJK SC / Microsoft YaHei UI。
  QFont bodyFont();
  // 数字面字体：JetBrains Mono 9pt（tnum），退化 monospace。
  QFont monoFont();

  // 全局 2px 键盘焦点环（DESIGN.md focus-ring token）——覆盖可聚焦
  // 控件的 :focus 边框，替代 Fusion 虚线框。拼进主窗/应用样式表。
  QString focusRingStyleSheet(Theme theme = currentTheme());

  // 状态胶囊（DESIGN.md status-tag）：浅色底 + 深色语义字 + 圆角，永远配
  // 文字（暗色为深底 + 提亮语义字）。Neutral = 中性「未计算」类
  // （surface-alt 底 + text-muted 字）。
  enum class CapsuleKind { Success, Warning, Error, Neutral };
  QString capsuleStyleSheet(CapsuleKind kind, Theme theme = currentTheme());
  // 造一个胶囊 QLabel：objectName "statusCapsule"，property "capsuleKind"
  // 存枚举 int（测试可断言），居中对齐，样式走 capsuleStyleSheet。
  QLabel *capsuleLabel(const QString &text, CapsuleKind kind, QWidget *parent);

  // 应用主题（DESIGN.md 双主题规范）：Fusion + 显式 palette + vendor 字体
  // + body 字体——不跟随系统深色模式（SA::setEnableSystemDarkModeAutoSwitch
  // 同语义：主题只由 PaleoTheme 显式驱动），平台差异全部钉死。
  // main() 在窗口创建前调用一次；PaleoMainWindow 构造时按用户设置再钉一次。
  // 换 palette 会给所有 widget 发 ApplicationPaletteChange——注册在
  // applyThemedStyleSheet 里的活体样式随事件重算。
  void applyTheme(Theme theme);
  void applyLightTheme(); // 兼容旧调用 = applyTheme(Theme::Light)
  void applyDarkTheme();

  // DESIGN.md token 的 palette 落地（Active+Inactive+Disabled 三组显式写齐，
  // 不允许系统 palette 泄漏）。
  QPalette palette(Theme theme);

  // 渲染回归稳定化（TODOS P1）：vendor 字体 + Fusion + 主题 palette 钉死，
  // 消除跨平台字体替换/平台样式差异带来的截图噪点。测试 initTestCase 调用。
  void pinRenderEnvironment();

  // QSettings 持久化（缺省 = Light；读是自由的，写只在用户显式切换主题时
  // 发生——测试路径不得经此写盘，沿用 setPath 隔离惯例）。
  Theme themeFromSettings();
  void writeThemeToSettings(Theme theme);

  // ---- 壳级样式表（原散在 paleomainwindow/paleoribbon 的字面量收编）----
  // 主窗壳 QSS：QMainWindow/QDockWidget 标题/QStatusBar/地图上下文行/
  // 层位 chip 行及共用原生控件。
  QString shellStyleSheet(Theme theme = currentTheme());
  // 原生表单、普通页签和表头共用的中性 chrome；应用级与 ribbon 壳级
  // 同时安装，独立窗口与主窗口不再继承不同的默认样式。
  QString controlStyleSheet(Theme theme = currentTheme());
  // 紧凑工具按钮（剖面、断层、井道导航）：正文 9pt、4px 圆角，完整
  // hover/pressed/checked/disabled/focus 状态，不使用装饰性蓝色。
  QString toolButtonStyleSheet(Theme theme = currentTheme());
  // 次级说明文字统一出口（“color: text-muted”），替代各面板字面量。
  QString mutedCaptionStyleSheet(Theme theme = currentTheme());
  // 区块标题统一出口（12pt + font-weight 600 + text 色）——datapreview 各页
  // 原各自字面量，goal/ui-experience-polish 收敛。
  QString sectionTitleStyleSheet(Theme theme = currentTheme());

  // ---- 界面密度（goal/ui-experience-polish 新基建）----
  // DESIGN.md「密度对齐专业 GIS 工具」：Comfort = 现行规格；Compact =
  // 树/列表 item padding 收紧一档 + 表缺省行高收紧。持久化 ui/density
  //（缺省 comfort，写只在用户显式切换时——同 theme 口径）。
  enum class Density { Comfort, Compact };
  Density currentDensity();
  Density densityFromSettings();
  void writeDensityToSettings(Density density);
  // 密度切换：落 current + 活体样式全量重算（shell QSS 里的条目 padding
  // 档随之变化）。表行高不走 QSS——由 applyDensityToViewTree 落。
  void applyDensity(Density density);
  // 密度度量（QSS/QHeaderView 用）：树/列表 item 纵向 padding、表缺省行高。
  int itemViewPaddingY(Density density);
  int tableRowHeight(Density density);
  // root 下全部 QTableView/QTableWidget 的缺省行高按密度落一档；树/列表
  // 行高由 itemViewStyleSheet 的 padding 承担。主窗装配后与切密度时各调
  // 一次（后建的表不自动跟，reapplyThemeChrome 会补扫）。
  void applyDensityToViewTree(QWidget *root, Density density = currentDensity());

  // 条目视图统一 QSS（三类列表控件选中态/hover 一致化的唯一出口）：
  // 选中 = primary 底 + onPrimary 字（palette 同款，QGIS 惯例——收敛各
  // 面板自写 ::item:selected）；hover = surfaceAltRaised；斑马纹底 =
  // surfaceAltRaised token；树/列表 item 纵向 padding 随密度。随主题与
  // 密度活体重算（拼进 shellStyleSheet）。
  QString itemViewStyleSheet(Theme theme = currentTheme(),
                             Density density = currentDensity());

  // 活体主题样式：builder 现在跑一次 setStyleSheet；之后每次换主题
  // （ApplicationPaletteChange）自动重算重设。builder 里按
  // tokens(currentTheme()) 取色。widget 销毁自动出注册表。
  void applyThemedStyleSheet(QWidget *widget,
                             const std::function<QString()> &builder);

  // ---- ribbon（SARibbon office2021 模板）----
  // 模板 {{token}} → DESIGN.md 色值的调色板 JSON。全部写在 keyColors，不留
  // derived 规则（派生色会盖过同名键色，结果不再等于 token）。
  QByteArray ribbonPaletteJson(Theme theme = currentTheme());
  // 模板管不到的细节：hover/checked 态、运行类动作主色字（DESIGN.md
  // ribbon-button.primaryText）、组名 8pt、右侧按钮组与编辑图层下拉。
  QString ribbonStyleSheet(Theme theme = currentTheme());
} // namespace PaleoTheme
