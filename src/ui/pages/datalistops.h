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

// 自然数字字母排序（Well-2 < Well-10）：逐段走查，数字段按数值、其余段按
// 字典序。数字段去前导 0 后「先比长度、后比字典序」等价于数值比较，免
// toInt 溢出，并正确处理带尾串的显示名（如 .las 扩展名）与多数字段名。
// finding-127：旧实现只取首个数字后的整段 toInt——多段名（同首数不同尾串）
// 会被判成「相等」而丢失段间次序，主要场景退化字典序。
inline bool naturalNameSort(const QString &na, const QString &nb)
{
  int ia = 0, ib = 0;
  const int la = na.size(), lb = nb.size();
  while (ia < la && ib < lb)
  {
    const QChar ca = na.at(ia);
    const QChar cb = nb.at(ib);
    if (ca.isDigit() && cb.isDigit())
    {
      int ea = ia, eb = ib;
      while (ea < la && na.at(ea).isDigit()) ++ea;
      while (eb < lb && nb.at(eb).isDigit()) ++eb;
      int za = ia, zb = ib;
      while (za < ea - 1 && na.at(za) == QChar('0')) ++za; // 去前导 0，留至少一位
      while (zb < eb - 1 && nb.at(zb) == QChar('0')) ++zb;
      const QString da = na.mid(za, ea - za);
      const QString db = nb.mid(zb, eb - zb);
      if (da.size() != db.size())
        return da.size() < db.size();
      if (da != db)
        return da < db;
      ia = ea;
      ib = eb; // 数值相等，继续比后续段
    }
    else if (ca != cb)
    {
      return ca < cb;
    }
    else
    {
      ++ia;
      ++ib;
    }
  }
  return (la - ia) < (lb - ib); // 前缀耗尽者小
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
