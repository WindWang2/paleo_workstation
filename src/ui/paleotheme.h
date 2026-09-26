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
  constexpr int kBodyPt = 9;  // 正文基准（Noto Sans SC）
  constexpr int kLabelPt = 8; // 次级标签
  constexpr int kMonoPt = 9;  // 数值/坐标/深度（JetBrains Mono，tnum 等宽）

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
} // namespace PaleoTheme
