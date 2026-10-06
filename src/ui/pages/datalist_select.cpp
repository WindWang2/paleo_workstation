// 层：视图
#include "datalist.h"
#include "../../catalog/datacatalog.h"
#include "../../services/previewdoc.h"
#include "dataops/dataopsselection.h"
#include "dataopswidgets.h"
#include "datanavtree.h"
#include "dataopsviews.h"

#include <QItemSelection>
#include <QItemSelectionModel>
#include <QListWidget>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

void DataListPanel::selectAssetsForEntities(const QStringList &entityIds)
{
  auto *table = findChild<QTableWidget *>(QStringLiteral("assetTable"));
  PreviewDocService *svc = m_doc;
  if (!table || !svc || entityIds.isEmpty())
    return;
  // 实体 → 已决关联资产集合（未决链接实体 id 为空，天然不命中）。
  QSet<QString> wanted;
  for (const QString &eid : entityIds)
    for (const EntityAssetLink &l : svc->catalog()->linksForEntity(eid))
      if (!l.unresolved)
        wanted.insert(l.assetId);
  QSet<QString> live;
  for (const auto &row : m_rows) live.insert(row.assetId);
  wanted.intersect(live);
  if (wanted.isEmpty()) return;
  if (m_rows.size() > 200)
  {
    QString first = *wanted.constBegin();
    for (const auto &row : m_filteredRows)
      if (wanted.contains(row.assetId)) { first = row.assetId; break; }
    selectAssetInViews(first);
    m_pageSelection = wanted;
  }
  // 一次应用整份选中（QTableView::selectRow 是单点替换语义，逐行调会互相
  // 顶掉）；选中变化照发 itemSelectionChanged → assetActivated 首个命中行。
  QItemSelection sel;
  QTableWidgetItem *firstHit = nullptr;
  for (int r = 0; r < table->rowCount(); ++r)
  {
    QTableWidgetItem *it = table->item(r, 0);
    if (!it || !wanted.contains(it->data(Qt::UserRole).toString()))
      continue;
    sel.select(table->model()->index(r, 0),
               table->model()->index(r, table->columnCount() - 1));
    if (!firstHit)
      firstHit = it;
  }
  if (sel.isEmpty())
    return;
  // P3 D1：程序化多选守卫——选中变化仍发首个命中行的 assetActivated（旧行为），
  // 但用户 Ctrl 多选不逐个激活。刷新期间的选中还原走 QSignalBlocker 不入此路径。
  m_progSelect = true;
  table->selectionModel()->select(
      sel, QItemSelectionModel::Select | QItemSelectionModel::Rows);
  m_progSelect = false;
  if (m_rows.size() > 200) m_pageSelection.unite(wanted);
  if (firstHit)
    table->scrollToItem(firstHit);
}

void DataListPanel::selectAssetInViews(const QString &assetId)
{
  auto *table = findChild<QTableWidget *>(QStringLiteral("assetTable"));
  if (table && m_rows.size() > 200)
  {
    bool live = false;
    for (const auto &row : m_rows) if (row.assetId == assetId) { live = true; break; }
    if (!live) return;
    m_pageSelection = {assetId};
    { const QSignalBlocker block(table); table->clearSelection(); }
    if (m_tree) { const QSignalBlocker block(m_tree); m_tree->clearSelection(); }
    for (int i = 0; i < m_filteredRows.size(); ++i)
      if (m_filteredRows.at(i).assetId == assetId)
      {
        const int pageSize = table->property("paleo.pageSize").toInt();
        m_assetPage = i / qMax(1, pageSize);
        m_pageSelection = {assetId};
        applyListFilter();
        break;
      }
  }
  if (table)
  {
    for (int r = 0; r < table->rowCount(); ++r)
    {
      QTableWidgetItem *it = table->item(r, 0);
      if (it && it->data(Qt::UserRole).toString() == assetId)
      {
        const QSignalBlocker b(table);
        table->setCurrentCell(r, 0);
        break;
      }
    }
  }

  if (m_tree)
  {
    const QList<QTreeWidgetItem *> sel = m_tree->selectedItems();
    if (sel.isEmpty() || sel.front()->data(0, Qt::UserRole).toString() != assetId)
    {
      QTreeWidgetItemIterator it(m_tree);
      while (*it)
      {
        if ((*it)->data(0, Qt::UserRole).toString() == assetId)
        {
          const QSignalBlocker b(m_tree);
          m_tree->setCurrentItem(*it);
          break;
        }
        ++it;
      }
    }
  }
}

QSet<QString> DataListPanel::currentAssetSelection() const
{
  using namespace paleo::dataops;
  QSet<QString> out = selectedAssetIds(m_tree) + m_pageSelection;
  if (auto *table = findChild<QTableWidget *>(QStringLiteral("assetTable")))
    out += selectedAssetIds(table);
  if (m_iconView)
  {
    for (QListWidgetItem *it : m_iconView->selectedItems())
    {
      const QString id = it->data(Qt::UserRole).toString();
      if (!id.isEmpty())
        out.insert(id);
    }
  }
  // 软删资产不在表里——选中集只含现存可见资产。
  return out;
}

QStringList DataListPanel::currentEntitySelection() const
{
  return paleo::dataops::selectedEntityIds(m_tree);
}

paleo::dataops::SelectionMix DataListPanel::currentSelectionMix() const
{
  paleo::dataops::SelectionMix mix;
  mix.assetIds = currentAssetSelection();
  mix.entityIds = currentEntitySelection();
  return mix;
}

void DataListPanel::refreshSelectionBadge()
{
  using namespace paleo::dataops;
  if (auto *badge = findChild<SelectionBadge *>(QStringLiteral("selectionBadge")))
    badge->setCount(int(currentAssetSelection().size()));
  emit selectionCountChanged(int(currentAssetSelection().size()),
                            currentEntitySelection().size());
}

void DataListPanel::selectAllVisibleAssets()
{
  using namespace paleo::dataops;
  if (m_rows.size() > 200) {
    m_pageSelection.clear();
    for (const auto &row : m_filteredRows) m_pageSelection.insert(row.assetId);
    m_selKeep.snapshotIds(m_pageSelection);
    renderAssetPage();
  }
  if (auto *table = findChild<QTableWidget *>(QStringLiteral("assetTable")))
    selectAllVisible(table);
  // 树/图标视图同步全选（可见项）。
  if (m_iconView)
    m_iconView->selectAll();
}

void DataListPanel::invertAssetSelection()
{
  using namespace paleo::dataops;
  if (m_rows.size() > 200) {
    QSet<QString> inverted;
    for (const auto &row : m_filteredRows) if (!m_pageSelection.contains(row.assetId)) inverted.insert(row.assetId);
    m_pageSelection = inverted; renderAssetPage(); refreshSelectionBadge(); return;
  }
  if (auto *table = findChild<QTableWidget *>(QStringLiteral("assetTable")))
    invertSelection(table);
}

void DataListPanel::selectByCurrentFilter()
{
  // 按过滤器选中 = 全选可见项（过滤已先行生效）。
  selectAllVisibleAssets();
  emit statusMessage(tr("已选中全部 %1 个可见资产").arg(currentAssetSelection().size()));
}
