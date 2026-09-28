// 层：视图
#pragma once
#include <QString>

class QAbstractButton;
class QAction;
class QWidget;
class SARibbonMainWindow;
class SARibbonPanel;
class SARibbonToolButton;

// src/ui/paleoribbon — SARibbon 壳的公用件（vendor/saribbon，MIT）。
//
// 命令与状态的分工：页面板（pages/pagepanels）的 QPushButton 仍是命令的
// 状态源——门控 enabled、reason tooltip（§35）、动态文案都由面板/壳写在
// 按钮上；ribbon 动作只做镜像并把触发转成 button->click()。一条命令只有
// 一条代码路径，面板单测照旧直接点按钮。
namespace PaleoRibbon
{
  // 必须在 SARibbonMainWindow 构造前调用：注册静态库 qrc，关掉「跟随系统
  // 暗色」自动换主题（DESIGN.md：浅色为主，不跟随系统深色模式）。
  void prepareLibrary();

  // DESIGN.md 调色板套 office2021 模板，再拼 PaleoTheme::ribbonStyleSheet()
  // 与壳样式 shellQss，一次性 setStyleSheet（SA::applyRibbonTheme 会整体
  // 覆盖窗口样式表，所以壳样式只能跟在它后面一起落）。
  void applyTheme(SARibbonMainWindow *win, const QString &shellQss);

  // 镜像绑定：action 的 enabled / tooltip（按钮无 tooltip 时保留 action 自己
  // 的说明）/ checked 跟随 button；syncText 时文案也跟随（如「生成 C6
  // 等厚图」「保存新版本」）。hideSource：命令入口移到 ribbon 后面板按钮
  // 藏起（隐藏的按钮照常 click()）。
  void mirror(QAction *action, QAbstractButton *button, bool syncText = false,
              bool hideSource = true);

  // 可勾选动作跟随部件的显式显隐（show()/hide()，与祖先是否可见无关），
  // 如「只看未决」跟随过滤条。
  void followVisibility(QAction *checkable, QWidget *widget);

  // panel 里 action 生成的那颗按钮（设 objectName / 运行类标记用）。
  SARibbonToolButton *buttonFor(SARibbonPanel *panel, QAction *action);

  // 运行类动作：DESIGN.md ribbon-button.primaryText——文字用 primary。
  void markRun(SARibbonToolButton *button);
} // namespace PaleoRibbon
