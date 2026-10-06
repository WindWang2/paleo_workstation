// 层：视图
#pragma once

#include <QCoreApplication>
#include <QHash>
#include <QString>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVector>
#include "../paleotheme.h"

class DataCatalog;

namespace paleo::pagesinternal
{

// §42.4/T31: 表格空态指引行与变灰辅助格
inline void refreshAssetEmptyState(QTableWidget *t, const QString &text = QString())
{
  if (t->rowCount() > 0)
    return;
  t->insertRow(0);
  auto *it = new QTableWidgetItem(text.isEmpty()
                                      ? QCoreApplication::translate("DataListPanel", "还没有数据资产 — 先导入工区文件夹，"
                                                                     "或用上方按钮导入单个文件")
                                      : text);
  it->setFlags(Qt::NoItemFlags);
  PaleoTheme::setItemTextColor(it, PaleoTheme::ItemTextColor::Muted); // text-muted（现取随主题）
  it->setTextAlignment(Qt::AlignCenter);                // T31 居中提示
  t->setItem(0, 0, it);
  t->setSpan(0, 0, 1, t->columnCount());
}

inline QTableWidgetItem *mutedCell(const QString &text)
{
  auto *it = new QTableWidgetItem(text);
  it->setFlags(Qt::NoItemFlags);
  PaleoTheme::setItemTextColor(it, PaleoTheme::ItemTextColor::Muted); // text-muted（现取随主题）
  return it;
}

// T28：链接身份寻址 + undo 库
struct UndoRecord
{
  QString assetId, entityId, role, entityType, note;
  // 被 attach 降级的前主关联（D4：撤销恢复降级 primary）；空 = 没有。
  QString demotedAssetId, demotedEntityId, demotedRole, demotedEntityType;
  bool isValid() const
  {
    return !assetId.isEmpty() && !entityId.isEmpty() && !role.isEmpty();
  }
};

int indexOfLink(DataCatalog *cat, const QString &assetId, const QString &role,
                const QString &entityId);
QString undoVaultPath(DataCatalog *cat);
QVector<UndoRecord> loadUndoVault(DataCatalog *cat, QHash<QString, QString> *notesOut = nullptr);
void saveUndoVault(DataCatalog *cat, const QVector<UndoRecord> &records,
                   const QHash<QString, QString> &notes);

// 自然数字字母排序（Well-2 < Well-10）
inline bool naturalNameSort(const QString &na, const QString &nb)
{
  int ia = 0, ib = 0;
  while (ia < na.size() && !na.at(ia).isDigit()) ++ia;
  while (ib < nb.size() && !nb.at(ib).isDigit()) ++ib;
  const QString preA = na.left(ia);
  const QString preB = nb.left(ib);
  if (preA != preB)
    return preA < preB;
  bool okA = false, okB = false;
  const int numA = na.mid(ia).toInt(&okA);
  const int numB = nb.mid(ib).toInt(&okB);
  if (okA && okB)
    return numA < numB;
  return na < nb;
}

// 快捷键表（D6.3 数据源；registerCommands 同步登记）
struct ShortcutSpec
{
  const char *id;
  const char *title;
  const char *shortcut;
  const char *category;
};

inline constexpr ShortcutSpec kShortcutSpecs[] = {
  {"dataops.undo", QT_TR_NOOP("撤销"), "Ctrl+Z", QT_TR_NOOP("编辑")},
  {"dataops.redo", QT_TR_NOOP("重做"), "Ctrl+Y", QT_TR_NOOP("编辑")},
  {"dataops.selectAll", QT_TR_NOOP("全选可见项"), "Ctrl+A", QT_TR_NOOP("选择")},
  {"dataops.invertSelection", QT_TR_NOOP("反选"), "Ctrl+Shift+A", QT_TR_NOOP("选择")},
  {"dataops.selectFiltered", QT_TR_NOOP("按过滤器选中"), "Ctrl+Shift+F", QT_TR_NOOP("选择")},
  {"dataops.commandPalette", QT_TR_NOOP("命令面板"), "Ctrl+Shift+P", QT_TR_NOOP("工具")},
  {"dataops.shortcutsDialog", QT_TR_NOOP("快捷键表"), "?", QT_TR_NOOP("帮助")},
  {"dataops.focusSearch", QT_TR_NOOP("聚焦搜索"), "Ctrl+F", QT_TR_NOOP("工具")},
  {"dataops.vimToggle", QT_TR_NOOP("Vim 风导航开关"), "Ctrl+Alt+V", QT_TR_NOOP("工具")},
  {"dataops.recursiveExpand", QT_TR_NOOP("全部展开"), "", QT_TR_NOOP("视图")},
  {"dataops.collapseAll", QT_TR_NOOP("全部折叠"), "", QT_TR_NOOP("视图")},
};

} // namespace paleo::pagesinternal
