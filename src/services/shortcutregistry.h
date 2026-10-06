// 层：数据
#pragma once
#include <QKeySequence>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

// services/shortcutregistry — 快捷键中央注册表的纯数据部分（方向 63）。
//
// 只登记「谁、在哪、用哪个键、干什么」，不建 QShortcut/QAction（那在
// ui/shortcuts/shortcutcatalog 里按条目绑定）——无 QtWidgets 依赖，可脱离
// 主窗单测。QKeySequence 属 QtGui，数据层允许。
//
// 上下文 = 以 '/' 分段的作用域路径（"main"、"main/data"、"layout-designer"、
// "dialog/data-palette"…）。路径前缀即包含关系：外层作用域里的窗口级快捷键
// 在内层作用域可见时同样处于激活态（Qt WindowShortcut 语义）。
//
// 冲突口径（conflicts()，启动日志 + 测试断言为零）：
//   - Ambiguous：两条 Qt 快捷键（Binding::Shortcut）同键序，且上下文相等
//     或一方包含另一方——Qt 的 QShortcutMap 对同时激活的同键快捷键判歧义，
//     两条都不触发（Ctrl+K 定位器/命令面板的历史事故即此类）。
//   - Duplicate：同一上下文同键序的其余组合（含焦点内按键处理器之间、
//     快捷键与同上下文按键处理器之间）。
// 遮蔽口径（shadows()，只告警不计冲突）：外层上下文的 Qt 快捷键与内层
// 上下文的焦点内按键处理器（Binding::KeyHandler，keyPressEvent/eventFilter
// 实现）同键序——Qt 先走快捷键映射，除非焦点控件接下 ShortcutOverride，
// 内层处理器收不到该键。是否真被吃掉取决于控件实现，故只告警、留待核实。
namespace paleo::shortcuts
{

enum class Binding
{
  Shortcut,   // QShortcut / QAction::setShortcut，经 ui/shortcuts 绑定
  KeyHandler, // 控件 keyPressEvent/eventFilter 里的焦点内按键（只登记说明）
};

struct ShortcutEntry
{
  QString id;                                     // 稳定标识，如 "main.page.data"
  QKeySequence key;                               // 键序（StandardKey 按平台求值后落这里）
  QString context;                                // 作用域路径
  Qt::ShortcutContext qtContext = Qt::WindowShortcut; // 仅 Binding::Shortcut 有意义
  Binding binding = Binding::Shortcut;
  QString group;       // 总表分组 id（taxonomy 见 ledger）
  QString description; // 中文源串（未翻译；视图层按 PaleoShortcuts 上下文 translate）
  QString sourcePanel; // 来源面板中文源串（同上）
  QString origin;      // 绑定/处理代码所在源文件（审计用，不进 UI）
};

struct ShortcutConflict
{
  enum class Kind
  {
    Ambiguous, // 两条 Qt 快捷键作用域重叠同键（Qt 判歧义，两条都失效）
    Duplicate, // 同上下文同键序
    Shadowed,  // 外层快捷键遮蔽内层焦点内按键处理器（告警）
  };
  Kind kind = Kind::Duplicate;
  ShortcutEntry first;
  ShortcutEntry second;

  // 日志用一行描述（非用户可见文案）。
  QString describe() const;
};

class ShortcutRegistry
{
public:
  // 登记一条；id 空/重复、键序空、上下文空、分组空、描述空 → 拒绝并写 error。
  bool registerEntry(const ShortcutEntry &entry, QString *error = nullptr);

  int size() const { return m_entries.size(); }
  bool contains(const QString &id) const;
  // 找不到返回 id 为空的条目。
  ShortcutEntry entry(const QString &id) const;
  QKeySequence key(const QString &id) const;
  const QList<ShortcutEntry> &entries() const { return m_entries; }
  QList<ShortcutEntry> entriesIn(Binding binding) const;
  // 分组 id 按首次登记顺序。
  QStringList groups() const;
  QList<ShortcutEntry> entriesInGroup(const QString &group) const;

  // 冲突（Ambiguous + Duplicate）；为空 = 注册表健康。
  QList<ShortcutConflict> conflicts() const;
  // 遮蔽告警（Shadowed）。
  QList<ShortcutConflict> shadows() const;

  // outer 是否包含 inner（相等或 outer 为 inner 的路径前缀，按段比较）。
  static bool contextCovers(const QString &outer, const QString &inner);
  // 两上下文是否「相关」（任一方包含另一方）——总表「当前位置」过滤用。
  static bool contextsRelated(const QString &a, const QString &b);
  // 上下文深度（段数）；空串为 0。
  static int contextDepth(const QString &context);

  // 上下文优先级解析：在 activeContext 处按下 key 时由哪条登记响应。
  //   1. 覆盖 activeContext 的 Qt 快捷键先于一切焦点内处理器（与 Qt 事件
  //      顺序一致）；恰一条 → 它；多于一条 → 歧义，返回空。
  //   2. 否则在覆盖 activeContext 的焦点内处理器中取上下文最深者（焦点控件
  //      先收键）；最深层并列 → 返回空。
  std::optional<ShortcutEntry> resolve(const QKeySequence &key,
                                       const QString &activeContext) const;

private:
  QList<ShortcutEntry> m_entries;
};

} // namespace paleo::shortcuts
