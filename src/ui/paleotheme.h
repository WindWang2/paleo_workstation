// 层：视图
#pragma once
#include <QColor>
#include <QString>
#include <QVariant>

class QFont;
class QLabel;
class QWidget;

// ui/paleotheme — DESIGN.md token 的唯一代码出口（wave3/ux-consistency T32）。
// 字体注册、焦点环、状态胶囊、mono 数字面都从这里取；面板代码不得再各写
// 一份颜色字面量。找不到 vendor 字体时如实降级（日志说明），不阻止启动。

namespace PaleoTheme
{
  // DESIGN.md typography tokens。
  constexpr int kDisplayPt = 15; // 图件标题 chip、页级标题 (15pt)
  constexpr int kTitlePt = 12;   // 标题 (12pt)
  constexpr int kBodyPt = 9;     // 正文基准（Noto Sans SC 9pt）
  constexpr int kLabelPt = 8;    // 次级标签 (8pt)
  constexpr int kMonoPt = 9;     // 数值/坐标/深度（JetBrains Mono 9pt，tnum 等宽）

  // DESIGN.md color tokens。
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

  // 启动时注册 vendor 字体（resources/fonts，qrc 前缀 :/paleo/fonts）。
  // 返回是否全部注册成功；单个失败记 qWarning 并继续（降级到系统字体，
  // DESIGN.md：Windows→Microsoft YaHei UI，Linux→Noto Sans CJK SC）。
  bool ensureApplicationFonts();

  // 正文字体：Noto Sans SC 9pt，退化链 Noto Sans CJK SC / Microsoft YaHei UI。
  QFont bodyFont();
  // 数字面字体：JetBrains Mono 9pt（tnum），退化 monospace。
  QFont monoFont();

  // 全局 2px #1B73D0 键盘焦点环（DESIGN.md focus-ring token）——覆盖可聚焦
  // 控件的 :focus 边框，替代 Fusion 虚线框。拼进主窗/应用样式表。
  QString focusRingStyleSheet();

  // 状态胶囊（DESIGN.md status-tag）：浅色底 + 深色语义字 + 圆角，永远配
  // 文字。Neutral = 中性「未计算」类（surface-alt 底 + text-muted 字）。
  enum class CapsuleKind { Success, Warning, Error, Neutral };
  QString capsuleStyleSheet(CapsuleKind kind);
  // 造一个胶囊 QLabel：objectName "statusCapsule"，property "capsuleKind"
  // 存枚举 int（测试可断言），居中对齐，样式走 capsuleStyleSheet。
  QLabel *capsuleLabel(const QString &text, CapsuleKind kind, QWidget *parent);

  // 应用默认浅色主题（DESIGN.md 是浅色规范）：Fusion + 显式浅色 QPalette
  // + vendor 字体 + body 字体——不跟随系统深色模式，平台差异全部钉死。
  // main() 在窗口创建前调用一次。
  void applyLightTheme();

  // 渲染回归稳定化（TODOS P1）：vendor 字体 + Fusion + 浅色 palette 钉死，
  // 消除跨平台字体替换/平台样式差异带来的截图噪点。测试 initTestCase 调用。
  void pinRenderEnvironment();

  // ---- ribbon（SARibbon office2021 模板）----
  // 模板 {{token}} → DESIGN.md 色值的调色板 JSON。全部写在 keyColors，不留
  // derived 规则（派生色会盖过同名键色，结果不再等于 token）。
  QByteArray ribbonPaletteJson();
  // 模板管不到的细节：hover/checked 态、运行类动作主色字（DESIGN.md
  // ribbon-button.primaryText）、组名 8pt、右侧按钮组与编辑图层下拉。
  QString ribbonStyleSheet();
} // namespace PaleoTheme
