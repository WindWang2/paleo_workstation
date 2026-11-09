// 层：视图
// ui/commandactionbinder.cpp — 见 commandactionbinder.h。
#include "commandactionbinder.h"

#include "paleoribbon.h"
#include "../workflow/commanddispatcher.h"

#include <algorithm>

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QMenu>
#include <QWidget>
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
  // 子菜单型动作：命令 = 弹出子菜单，至少要有一个可用子项，否则禁用并说明。
  if (st.enabled && a->menu())
  {
    const QList<QAction *> items = a->menu()->actions();
    const bool any = std::any_of(items.cbegin(), items.cend(), [](const QAction *i) {
      return !i->isSeparator() && i->isVisible() && i->isEnabled();
    });
    if (!any)
    {
      st.enabled = false;
      const auto first = std::find_if(items.cbegin(), items.cend(), [](const QAction *i) {
        return !i->isSeparator() && !i->toolTip().isEmpty();
      });
      st.reason = first != items.cend() ? (*first)->toolTip() : QObject::tr("当前没有可用的子项");
    }
  }
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

bool CommandActionBinder::targetChecked(const QVariantMap &params, bool current)
{
  const auto it = params.constFind(QStringLiteral("checked"));
  return it != params.constEnd() ? it->toBool() : !current;
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
            const auto it = params.constFind(QStringLiteral("checked"));
            PaleoRibbon::activateMirror(a, it != params.constEnd() ? int(it->toBool()) : -1);
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
            if (a->menu())
            {
              popupMenuFor(a); // 子菜单型：与 ribbon 工具按钮同一菜单，由用户挑子项
              return;
            }
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

QMenu *CommandActionBinder::popupMenuFor(QAction *action)
{
  QMenu *menu = action ? action->menu() : nullptr;
  if (!menu)
    return nullptr;
  for (QObject *o : action->associatedObjects())
  {
    auto *w = qobject_cast<QWidget *>(o);
    if (w && w != menu && !qobject_cast<QMenu *>(w) && w->isVisible())
    {
      menu->popup(w->mapToGlobal(QPoint(0, w->height())));
      return menu;
    }
  }
  if (QWidget *win = QApplication::activeWindow())
    menu->popup(win->mapToGlobal(win->rect().center()));
  else
    menu->popup(QCursor::pos());
  return menu;
}
