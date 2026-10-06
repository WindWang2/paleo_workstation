// 层：视图
#pragma once
#include "../../services/shortcutregistry.h"

#include <QList>
#include <QString>
#include <QStringList>

class QAction;
class QShortcut;
class QWidget;

// ui/shortcuts/shortcutcatalog — 快捷键中央登记处（方向 63）。
//
// 全仓快捷键的唯一真源：键序/上下文/描述/来源面板都在 shortcutcatalog.cpp
// 的条目表里；面板代码只按 id 绑定（bindShortcut/bindAction），不再自己
// 写 QKeySequence。焦点内按键处理器（keyPressEvent/eventFilter）以
// Binding::KeyHandler 登记说明，供总表展示与冲突/遮蔽审计——处理代码
// 本身留在原控件里（行为红线：只集中登记，不改键、不改上下文）。
//
// 描述/分组/来源面板是中文源串，经 QCoreApplication::translate 的
// "PaleoShortcuts" 上下文翻译（lupdate 通过 QT_TRANSLATE_NOOP 收录）。
namespace paleo::shortcuts
{

// 绑定对象上记录登记 id 的动态属性名（测试据此核对「全部经注册表」）。
inline constexpr char kShortcutIdProperty[] = "paleo.shortcutId";

// 进程级注册表（首次调用时按条目表构建）。
const ShortcutRegistry &registry();
// 条目表原样（测试用它构造变异注册表）。
QList<ShortcutEntry> catalogEntries();
// 构建注册表时被拒的条目（id 重复/空键等）；健康时为空。
QStringList catalogErrors();

// 按 id 取键序；未登记 → 空键序 + qWarning。
QKeySequence keyFor(const QString &id);

// 新建 QShortcut（parent 持有），键序/上下文取自登记；带 kShortcutIdProperty。
QShortcut *bindShortcut(const QString &id, QWidget *parent);
// QAction::setShortcut 的登记版（同带 kShortcutIdProperty）。
void bindAction(const QString &id, QAction *action);
// 可让出的动作快捷键（如「地层对比」页让出 Ctrl+S）：active=false 清空键序，
// true 恢复登记键序；属性始终保留。
void setActionShortcutActive(const QString &id, QAction *action, bool active);

// 用户可见文本（已翻译）。
QString displayDescription(const ShortcutEntry &entry);
QString displaySourcePanel(const ShortcutEntry &entry);
QString displayGroup(const QString &groupId);
QString displayKey(const QKeySequence &key);

// 由控件（通常是焦点控件）向上找最近的已知面板，返回其作用域路径；
// 找不到 → 空串。F1 总表「当前位置」过滤用。
QString contextForWidget(const QWidget *widget);

// 启动日志：注册表规模 + 每条冲突（warning）+ 每条遮蔽（info）。进程内只
// 打一次；返回冲突条数。
int logConflictsOnce();

} // namespace paleo::shortcuts
