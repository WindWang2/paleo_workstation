// 层：视图
#include "datalist.h"
#include "dataops/dataopsfilter.h"
#include "dataopswidgets.h"
#include "dataopsviews.h"
#include "dataops/dataopsmodel.h"

#include <QComboBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTableWidgetItem>

void DataListPanel::applyListFilter()
{
  using namespace paleo::dataops;
  auto *table = findChild<QTableWidget *>(QStringLiteral("assetTable"));
  if (!table)
    return;
  syncLegacyControlsIntoFilter();
  // 可见集 = 行快照 × FilterGroup × 激活标签（D2.2/D2.4）。
  QSet<QString> visibleIds;
  int shown = 0;
  for (const AssetRowInfo &row : m_rows)
  {
    if (!m_activeTag.isEmpty() && !row.tags.contains(m_activeTag, Qt::CaseInsensitive))
      continue;
    if (!m_filter.matches(row))
      continue;
    visibleIds.insert(row.assetId);
    ++shown;
  }
  QVector<AssetRowInfo> filtered;
  for (const auto &row : m_rows)
    if (visibleIds.contains(row.assetId))
      filtered.append(row);
  const QString sortField = m_assetSortColumn == 1 ? QStringLiteral("type")
                           : m_assetSortColumn == 2 ? QStringLiteral("entity") : QStringLiteral("name");
  if (m_assetSortColumn >= 0) sortAssetRows(&filtered, sortField, m_assetSortAscending);
  m_filteredRows = filtered;
  const bool paged = m_rows.size() > 200;
  const int pageSize = qMax(1, table->viewport()->height() / qMax(1, table->verticalHeader()->defaultSectionSize()));
  const int pages = qMax(1, int((filtered.size() + pageSize - 1) / pageSize));
  m_assetPage = qBound(0, m_assetPage, pages - 1);
  auto pageRows = paged ? filtered.mid(m_assetPage * pageSize, pageSize) : m_rows;
  if (!paged && m_assetSortColumn >= 0) sortAssetRows(&pageRows, sortField, m_assetSortAscending);
  bool samePage = pageRows.size() == m_pageRows.size();
  for (int i = 0; samePage && i < pageRows.size(); ++i)
    samePage = pageRows.at(i).assetId == m_pageRows.at(i).assetId;
  m_pageRows = pageRows;
  if (m_tableDirty || !samePage) { renderAssetPage(); m_tableDirty = false; }
  table->setProperty("paleo.totalAssets", m_rows.size());
  table->setProperty("paleo.pageSize", pageSize);
  if (auto *pager = findChild<QWidget *>(QStringLiteral("assetPager")))
    // 树形视图（0）永不分页（完整实体树 + 滚动）；分组视图（3）自管懒载。
    pager->setVisible(paged && (!m_viewStack ||
                                (m_viewStack->currentIndex() != 0 &&
                                 m_viewStack->currentIndex() != 3)));
  if (auto *label = findChild<QLabel *>(QStringLiteral("assetPageLabel")))
    label->setText(tr("第 %1 / %2 页 · %3 项").arg(m_assetPage + 1).arg(pages).arg(shown));
  if (auto *btn = findChild<QPushButton *>(QStringLiteral("assetPreviousPage")))
    btn->setEnabled(m_assetPage > 0);
  if (auto *btn = findChild<QPushButton *>(QStringLiteral("assetNextPage")))
    btn->setEnabled(m_assetPage + 1 < pages);
  const int total = int(m_rows.size());
  for (int r = 0; r < table->rowCount(); ++r)
  {
    const QTableWidgetItem *name = table->item(r, 0);
    // 空态指引行（无 UserRole）不算数据，也永远不藏。
    if (!name || name->data(Qt::UserRole).toString().isEmpty())
    {
      table->setRowHidden(r, false);
      continue;
    }
    table->setRowHidden(r, !visibleIds.contains(name->data(Qt::UserRole).toString()));
  }
  if (auto *count = findChild<QLabel *>(QStringLiteral("assetCountLabel")))
    count->setText(shown == total ? tr("共 %1 条").arg(total)
                                  : tr("显示 %1 / 共 %2 条").arg(shown).arg(total));

  // D2.7 高亮 needle（搜索词来自 Search 条件或旧搜索框）。
  if (m_delegate)
  {
    QString needle;
    for (const FilterCondition &c : m_filter.conditions)
      if (c.dim == FilterDim::Search && !c.negate)
        needle = c.value;
    m_delegate->setNeedle(needle);
    if (m_delegate->needle().isEmpty())
    {
      const auto *search = findChild<QLineEdit *>(QStringLiteral("assetSearchEdit"));
      m_delegate->setNeedle(search ? search->text().trimmed() : QString());
    }
  }

  // 树形视图过滤（P3：按可见 id 集 + 文本匹配递归）。
  const bool filtering = !m_filter.conditions.isEmpty() || !m_activeTag.isEmpty();
  applyFilterToTree(visibleIds, filtering);

  // D2.9 空结果态：有过滤而零命中 → 放宽/清除快捷钮（空态指引行仍显示）。
  if (m_emptyState)
  {
    const bool noHits = filtering && shown == 0 && total > 0;
    m_emptyState->setVisible(noHits);
    if (noHits)
      m_emptyState->setMessage(
          tr("没有匹配的资产 — 试试放宽条件（当前 %1 个条件%2）")
              .arg(m_filter.conditions.size())
              .arg(m_activeTag.isEmpty() ? QString()
                                         : tr(" + 标签「%1」").arg(m_activeTag)));
  }

  // D7 新视图页同口径重灌（图标/分组；虚拟模型给全量行——fetchMore 自管）。
  if (m_iconView)
  {
    QVector<AssetRowInfo> vis;
    for (const AssetRowInfo &row : m_rows)
      if (visibleIds.contains(row.assetId))
        vis.append(row);
    m_iconView->loadRows(paged ? m_pageRows : vis);
    if (m_groupTree)
      m_groupTree->loadRows(paged ? m_pageRows : vis);
  }
  if (m_virtualView)
  {
    QVector<AssetRowInfo> vis;
    for (const AssetRowInfo &row : m_rows)
      if (visibleIds.contains(row.assetId))
        vis.append(row);
    // D7.4 稳定排序（多列次级）：名称升序为次级键。
    sortAssetRows(&vis, QStringLiteral("name"), true);
    m_virtualView->flatModel()->setRows(vis);
    if (m_virtualView->horizontalHeader())
      restoreColumnState(QStringLiteral("assetVirtualTable"),
                         m_virtualView->horizontalHeader());
  }
}

void DataListPanel::setUnresolvedFilter(bool on)
{
  setProperty("paleo.page.filterUnresolved", on);
  if (auto *bar = findChild<QWidget *>(QStringLiteral("unresolvedFilterBar")))
    bar->setVisible(on);
  refreshAssetTable();
}

void DataListPanel::refreshChipBar()
{
  if (m_chipBar)
    m_chipBar->setConditions(m_filter);
  if (m_filterBar)
  {
    // 维度词表（下拉候选）。
    QSet<QString> types, statuses, roles, entities, tags;
    for (const paleo::dataops::AssetRowInfo &r : m_rows)
    {
      if (!types.contains(r.effectiveType) && !r.effectiveType.isEmpty())
        types.insert(r.effectiveType);
      if (!statuses.contains(r.status))
        statuses.insert(r.status);
      for (const QString &ro : r.roles)
        if (!roles.contains(ro))
          roles.insert(ro);
      for (const QString &en : r.entityNames)
        if (!entities.contains(en))
          entities.insert(en);
      for (const QString &t : r.tags)
        if (!tags.contains(t))
          tags.insert(t);
    }
    const auto sorted = [](const QSet<QString> &set) { QStringList list = set.values(); list.sort(); return list; };
    m_filterBar->setValueVocabulary(paleo::dataops::FilterDim::Type, sorted(types));
    m_filterBar->setValueVocabulary(paleo::dataops::FilterDim::Status,
                                    {QStringLiteral("RAW"), QStringLiteral("DERIVED")});
    m_filterBar->setValueVocabulary(paleo::dataops::FilterDim::Role, sorted(roles));
    m_filterBar->setValueVocabulary(paleo::dataops::FilterDim::Entity, sorted(entities));
    m_filterBar->setValueVocabulary(paleo::dataops::FilterDim::Tag, sorted(tags));
    m_filterBar->reloadPresets();
  }
}

void DataListPanel::refreshTagCloud()
{
  if (m_tagCloud)
    m_tagCloud->setCloud(m_tags.tagCloud(), m_activeTag);
}

void DataListPanel::updatePendingCounts()
{
  using namespace paleo::dataops;
  if (!m_quickBar)
    return;
  int unlinked = 0, unknown = 0, warned = 0;
  for (const AssetRowInfo &r : m_rows)
  {
    FilterCondition c;
    c.dim = FilterDim::Unlinked;
    if (c.matches(r)) ++unlinked;
    c.dim = FilterDim::UnknownType;
    if (c.matches(r)) ++unknown;
    c.dim = FilterDim::Warned;
    if (c.matches(r)) ++warned;
  }
  m_quickBar->setCounts(unlinked, unknown, warned);
}

void DataListPanel::syncLegacyControlsIntoFilter()
{
  using namespace paleo::dataops;
  // Search/Type 两维有双输入面：旧控件（assetSearchEdit/assetTypeFilter，
  // 兼容面）与 FilterBar/预设/状态串。reconcile 双向：
  //   1) FilterGroup 已有该维条件 → 写回旧控件（镜像一致）；
  //   2) 控件有值而条件缺 → 从控件带入条件。
  // 旧测试（直接打字/选类型）与新路径（FilterBar/预设/状态串）互不覆盖。
  auto *search = findChild<QLineEdit *>(QStringLiteral("assetSearchEdit"));
  auto *typeFilter = findChild<QComboBox *>(QStringLiteral("assetTypeFilter"));
  const auto condValue = [this](FilterDim d) {
    for (const FilterCondition &c : m_filter.conditions)
      if (c.dim == d && !c.negate)
        return c.value;
    return QString();
  };
  if (search)
  {
    const QString inFilter = condValue(FilterDim::Search);
    if (!inFilter.isEmpty() && inFilter != search->text().trimmed())
    {
      const QSignalBlocker b(search);
      search->setText(inFilter);
    }
    else if (inFilter.isEmpty() && !search->text().trimmed().isEmpty())
    {
      m_filter.clearDim(FilterDim::Search);
      m_filter.conditions.prepend(
          {FilterDim::Search, search->text().trimmed(), false});
    }
    else if (inFilter.isEmpty() && search->text().trimmed().isEmpty())
      m_filter.clearDim(FilterDim::Search);
  }
  if (typeFilter)
  {
    const QString inFilter = condValue(FilterDim::Type);
    const QString combo = typeFilter->currentData().toString();
    if (!inFilter.isEmpty() && inFilter != combo)
    {
      const QSignalBlocker b(typeFilter);
      const int idx = typeFilter->findData(inFilter);
      typeFilter->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    else if (inFilter.isEmpty() && !combo.isEmpty())
    {
      m_filter.clearDim(FilterDim::Type);
      m_filter.conditions.prepend({FilterDim::Type, combo, false});
    }
    else if (inFilter.isEmpty() && combo.isEmpty())
      m_filter.clearDim(FilterDim::Type);
  }
}

void DataListPanel::applyFilterGroup(const paleo::dataops::FilterGroup &g)
{
  m_filter = g;
  if (m_filterBar)
    m_filterBar->setOrMode(g.orMode);
  applyListFilter();
  refreshChipBar();
}

void DataListPanel::setFilterFromStateString(const QString &s)
{
  applyFilterGroup(paleo::dataops::FilterGroup::fromStateString(s));
}
