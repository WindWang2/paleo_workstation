// 层：视图
#pragma once
#include <QDialog>
#include <QString>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace paleo::shortcuts
{
class ShortcutRegistry;

// ui/shortcuts/shortcutsheetdialog — 快捷键总表（方向 63，F1 /「帮助 › 快捷键」）。
//
// 按注册表分组列出全部登记条目（含焦点内按键说明），可搜索、可复制单条；
// 非模态，单例由 HelpSurface 持有。视觉按 DESIGN.md token（提案见 ledger）：
// 16px 外边距 / 8px 间距、正文 9pt、按键列 JetBrains Mono、分组行加粗、
// 次级说明 text-muted；无自绘、无装饰色。
class ShortcutSheetDialog : public QDialog
{
  Q_OBJECT
public:
  explicit ShortcutSheetDialog(const ShortcutRegistry &registry, QWidget *parent = nullptr);

  // 条目总数（不含分组行）= 注册表条目数；visibleEntryCount 为过滤后可见数。
  int entryCount() const;
  int visibleEntryCount() const;

  // 搜索：按键（本地化/可移植两种写法）、功能描述、来源面板、分组名任一命中。
  void setFilterText(const QString &text);
  QString filterText() const;

  // 「当前位置」：传入作用域路径（如 "main/data"），勾选过滤时只留与之相关
  // （互为包含）的条目；同时选中并滚到最近一条。空串 = 不限。
  void setActiveContext(const QString &context);
  QString activeContext() const { return m_activeContext; }
  void setContextFilterEnabled(bool on);

  // 复制：单条的剪贴板文本「按键<TAB>功能<TAB>适用范围」。
  QString clipboardTextFor(const QString &entryId) const;
  // 复制当前选中条目到剪贴板；返回复制的文本（无选中返回空）。
  QString copySelected();
  // 选中某条（测试与「当前位置」定位用）；返回是否找到。
  bool selectEntry(const QString &entryId);
  QString selectedEntryId() const;

  QTreeWidget *tree() const { return m_tree; }

private:
  void rebuild();
  void applyFilter();
  void updateCountLabel();
  QTreeWidgetItem *itemFor(const QString &entryId) const;

  const ShortcutRegistry &m_registry;
  QLineEdit *m_search = nullptr;
  QCheckBox *m_contextOnly = nullptr;
  QTreeWidget *m_tree = nullptr;
  QLabel *m_count = nullptr;
  QLabel *m_hint = nullptr;
  QPushButton *m_copy = nullptr;
  QString m_activeContext;
};

} // namespace paleo::shortcuts
