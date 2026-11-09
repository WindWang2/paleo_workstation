// 层：视图
// ui/commandactionbinder — 把 QAction 接到统一命令注册表（UI 重设计 U1）。
//
// 三种接法，objectName / 图标 / 文案 / 快捷键都不动（tst_ui 等按 objectName 取动作）：
//   route(a, id)      ：a 触发 → CommandDispatcher::trigger(id)，处理器在别处 bind；
//                       可勾选动作把 checked 作为参数 "checked" 带过去。
//   bindMirror(a, id) ：a 是 PaleoRibbon::mirror 镜像（页面板按钮仍是状态源）——登记
//                       「激活镜像源」处理器 + 「跟随 action 使能/提示」状态，再 route。
//                       ButtonMirror 见到 paleoCommandId 就不再自己 click()。
//   bindNative(a, id) ：a 本身就是实现（QGIS 地图工具、dock toggleViewAction、编辑条
//                       共享动作）——只打 commandId 标记；Ctrl+K/Agent 经 dispatcher
//                       调时由处理器 trigger 这颗动作。
// 每颗绑定过的动作带动态属性 "paleoCommandId"（tst_commandshell 闸门据此盘点）。
#pragma once

#include "../workflow/commanddispatcher.h" // QPointer 需要完整类型

#include <QObject>
#include <QPointer>
#include <QVariantMap>

#include <functional>

class QAction;

class CommandActionBinder : public QObject
{
  Q_OBJECT
public:
  static constexpr const char *kCommandIdProperty = "paleoCommandId";

  explicit CommandActionBinder(paleo::command::CommandDispatcher *dispatcher, QObject *parent = nullptr);

  paleo::command::CommandDispatcher *dispatcher() const { return m_dispatcher; }

  QAction *route(QAction *action, const QString &commandId, const QVariantMap &params = {});
  // before：激活镜像源之前按参数预置面板（如约束形状下拉）。
  QAction *bindMirror(QAction *action, const QString &commandId,
                      std::function<void(const QVariantMap &)> before = {});
  QAction *bindNative(QAction *action, const QString &commandId);

  static QString commandIdOf(const QObject *action);
  // 勾选类处理器的目标态：给了 checked 就照办；没给（Ctrl+K / Agent 等无状态入口）就取反当前态。
  static bool targetChecked(const QVariantMap &params, bool current);

private:
  QPointer<paleo::command::CommandDispatcher> m_dispatcher;
};
