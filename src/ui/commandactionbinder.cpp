// 层：视图
// ui/commandactionbinder.cpp — 见 commandactionbinder.h。
#include "commandactionbinder.h"

#include "paleoribbon.h"
#include "../workflow/commanddispatcher.h"

#include <QAction>
#include <QLoggingCategory>

using paleo::command::CommandContext;
using paleo::command::CommandDispatcher;
using paleo::command::CommandState;

namespace
{
Q_LOGGING_CATEGORY(lcCommands, "paleo.commands")

// 动作自身的使能/提示即命令状态：禁用时 tooltip 承载原因（镜像同步与 applyGate 都写它）。
CommandState actionState(const QPointer<QAction> &a)
{
  CommandState st;
  if (!a)
    return st; // 入口已销毁：dispatcher 兜底补「当前不可用」
  st.enabled = a->isEnabled();
  st.checkable = a->isCheckable();
  st.checked = a->isChecked();
  if (!st.enabled)
    st.reason = a->toolTip();
  return st;
}
} // namespace

CommandActionBinder::CommandActionBinder(CommandDispatcher *dispatcher, QObject *parent)
  : QObject(parent), m_dispatcher(dispatcher)
{
}

QString CommandActionBinder::commandIdOf(const QObject *action)
{
  return action ? action->property(kCommandIdProperty).toString() : QString();
}

QAction *CommandActionBinder::route(QAction *action, const QString &commandId, const QVariantMap &params)
{
  if (!action || !m_dispatcher)
    return action;
  if (!commandIdOf(action).isEmpty())
    return action; // 幂等：同一颗动作只接一次（共享动作会被多个页签复用）
  action->setProperty(kCommandIdProperty, commandId);
  QPointer<CommandDispatcher> d = m_dispatcher;
  connect(action, &QAction::triggered, this, [d, action, commandId, params](bool checked) {
    if (!d)
      return;
    QVariantMap p = params;
    if (action->isCheckable())
      p.insert(QStringLiteral("checked"), checked);
    QString reason;
    if (!d->trigger(commandId, d->currentContext(), p, &reason))
      qCDebug(lcCommands) << "command" << commandId << "rejected:" << reason;
  });
  return action;
}

QAction *CommandActionBinder::bindMirror(QAction *action, const QString &commandId,
                                         std::function<void(const QVariantMap &)> before)
{
  if (!action || !m_dispatcher)
    return action;
  QPointer<QAction> a(action);
  QString error;
  if (!m_dispatcher->bind(
          commandId,
          [a, before](const CommandContext &, const QVariantMap &params) {
            if (!a)
              return;
            if (before)
              before(params);
            PaleoRibbon::activateMirror(a);
          },
          [a](const CommandContext &) { return actionState(a); }, &error))
    qCWarning(lcCommands) << error;
  return route(action, commandId);
}

QAction *CommandActionBinder::bindNative(QAction *action, const QString &commandId)
{
  if (!action || !m_dispatcher)
    return action;
  if (!commandIdOf(action).isEmpty())
    return action;
  action->setProperty(kCommandIdProperty, commandId);
  QPointer<QAction> a(action);
  QString error;
  if (!m_dispatcher->bind(
          commandId,
          [a](const CommandContext &, const QVariantMap &params) {
            if (!a)
              return;
            // 可勾选动作（dock 显隐、地图工具）：给了目标态且已在该态则不动，
            // 否则 trigger——toggleViewAction 只认 triggered，setChecked 不会显隐 dock。
            if (a->isCheckable() && params.contains(QStringLiteral("checked")) &&
                a->isChecked() == params.value(QStringLiteral("checked")).toBool())
              return;
            a->trigger();
          },
          [a](const CommandContext &) { return actionState(a); }, &error))
    qCWarning(lcCommands) << error;
  return action;
}
