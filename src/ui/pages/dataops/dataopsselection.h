// 层：视图
// ui/pages/dataops/dataopsselection — D1 多选框架的纯逻辑面：选中集合的
// 提取/镜像/还原。树、表、图标视图统一 ExtendedSelection；选中身份 = 资产
// id 集合（跨视图聚合），刷新/过滤切换后按 id 还原（D1.10）。
#pragma once

#include <QAbstractItemView>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTableWidget>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include "dataopsmodel.h"

namespace paleo::dataops
{

// 从任意视图收集选中的资产 id（item data Qt::UserRole）。
inline QSet<QString> selectedAssetIds(QTreeWidget *tree)
{
  QSet<QString> out;
  if (!tree)
    return out;
  const QList<QTreeWidgetItem *> sel = tree->selectedItems();
  for (QTreeWidgetItem *it : sel)
  {
    const QString id = it->data(0, Qt::UserRole).toString();
    if (!id.isEmpty() && it->data(0, Qt::UserRole + 2).toString() != QLatin1String("category"))
      out.insert(id);
  }
  return out;
}

inline QSet<QString> selectedAssetIds(QTableWidget *table)
{
  QSet<QString> out;
  if (!table)
    return out;
  const QList<QTableWidgetItem *> sel = table->selectedItems();
  for (QTableWidgetItem *it : sel)
  {
    const QString id = it->data(Qt::UserRole).toString();
    if (!id.isEmpty())
      out.insert(id);
  }
  return out;
}

// 选中井实体 id（树中 nodeType=="well" 的节点，UserRole+1）。
inline QStringList selectedEntityIds(QTreeWidget *tree)
{
  QStringList out;
  if (!tree)
    return out;
  for (QTreeWidgetItem *it : tree->selectedItems())
    if (it->data(0, Qt::UserRole + 2).toString() == QLatin1String("well"))
    {
      const QString id = it->data(0, Qt::UserRole + 1).toString();
      if (!id.isEmpty() && !out.contains(id))
        out.append(id);
    }
  return out;
}

// D1.10 选择保持器：刷新前快照，重建后按 id 还原（多视图各自调用）。
class SelectionKeeper
{
public:
  void snapshot(QTreeWidget *tree, QTableWidget *table)
  {
    m_ids = selectedAssetIds(tree) + selectedAssetIds(table);
  }
  void snapshotIds(const QSet<QString> &ids) { m_ids = ids; }
  QSet<QString> ids() const { return m_ids; }

  // 还原到表：按 id 找行并选中（行不存在 = 过滤掉了，静默丢弃）。
  // 返回实际还原到的 id 集。
  QSet<QString> restoreTable(QTableWidget *table) const
  {
    QSet<QString> restored;
    if (!table || m_ids.isEmpty())
      return restored;
    QItemSelection sel;
    for (int r = 0; r < table->rowCount(); ++r)
    {
      QTableWidgetItem *it = table->item(r, 0);
      if (!it)
        continue;
      const QString id = it->data(Qt::UserRole).toString();
      if (!id.isEmpty() && m_ids.contains(id))
      {
        sel.select(table->model()->index(r, 0),
                   table->model()->index(r, table->columnCount() - 1));
        restored.insert(id);
      }
    }
    if (!sel.isEmpty())
      table->selectionModel()->select(
          sel, QItemSelectionModel::Select | QItemSelectionModel::Rows |
                   QItemSelectionModel::ClearAndSelect);
    else
      table->clearSelection();
    return restored;
  }

  // 还原到树：按 id 找资产节点（QSignalBlocker 防选中信号回环）。
  QSet<QString> restoreTree(QTreeWidget *tree) const
  {
    QSet<QString> restored;
    if (!tree || m_ids.isEmpty())
      return restored;
    const QSignalBlocker block(tree);
    tree->clearSelection();
    QTreeWidgetItemIterator it(tree);
    while (*it)
    {
      const QString id = (*it)->data(0, Qt::UserRole).toString();
      if (!id.isEmpty() && m_ids.contains(id))
      {
        (*it)->setSelected(true);
        restored.insert(id);
      }
      ++it;
    }
    return restored;
  }

  void clear() { m_ids.clear(); }

private:
  QSet<QString> m_ids;
};

// 表 ↔ 树选中镜像（单向调用避免回环；测试断言镜像口径）。
inline void mirrorSelectionToTree(QTableWidget *table, QTreeWidget *tree)
{
  if (!table || !tree)
    return;
  const QSet<QString> ids = selectedAssetIds(table);
  const QSignalBlocker block(tree);
  tree->clearSelection();
  if (ids.isEmpty())
    return;
  QTreeWidgetItemIterator it(tree);
  while (*it)
  {
    const QString id = (*it)->data(0, Qt::UserRole).toString();
    if (!id.isEmpty() && ids.contains(id))
      (*it)->setSelected(true);
    ++it;
  }
}

// D1.9 全选/反选/按过滤选中（表形态；对隐藏行不生效——所见即所选）。
inline void selectAllVisible(QTableWidget *table)
{
  if (!table)
    return;
  QItemSelection sel;
  for (int r = 0; r < table->rowCount(); ++r)
  {
    if (table->isRowHidden(r))
      continue;
    sel.select(table->model()->index(r, 0),
               table->model()->index(r, table->columnCount() - 1));
  }
  if (!sel.isEmpty())
    table->selectionModel()->select(
        sel, QItemSelectionModel::Select | QItemSelectionModel::Rows |
                 QItemSelectionModel::ClearAndSelect);
}

inline void invertSelection(QTableWidget *table)
{
  if (!table)
    return;
  QItemSelection sel;
  for (int r = 0; r < table->rowCount(); ++r)
  {
    if (table->isRowHidden(r))
      continue;
    const QTableWidgetItem *it = table->item(r, 0);
    if (!it || !(it->isSelected()))
      sel.select(table->model()->index(r, 0),
                 table->model()->index(r, table->columnCount() - 1));
  }
  if (!sel.isEmpty())
    table->selectionModel()->select(
        sel, QItemSelectionModel::Select | QItemSelectionModel::Rows);
  else
    table->clearSelection();
}

// 异构选中分流（D1.3）：资产 + 实体混合时给「公共子集」操作集。
struct SelectionMix
{
  QSet<QString> assetIds;
  QStringList entityIds;
  bool mixed() const { return !assetIds.isEmpty() && !entityIds.isEmpty(); }
  bool isEmpty() const { return assetIds.isEmpty() && entityIds.isEmpty(); }
  // 公共操作：资产子集（mixed 时实体只作上下文，不给实体专属操作）。
  QSet<QString> commonAssetIds() const { return assetIds; }
};

} // namespace paleo::dataops
