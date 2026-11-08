// 层：视图
#include "datalist.h"
#include "dataops/dataopsfilter.h"
#include "dataopswidgets.h"
#include "dataopsviews.h"
#include "dataops/dataopsmodel.h"
#include "pageshared.h"

#include <QComboBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTableWidgetItem>

void DataListPanel::applyListFilter()
{
  using namespace paleo::dataops;
  const paleo::pagesinternal::PagePerfStage perfTotal("applyListFilter");
  auto *table = findChild<QTableWidget *>(QStringLiteral("assetTable"));
  if (!table || !m_doc)
    return;
  const QScopedValueRollback<bool> restoring(m_restoringSelection, true);
  // P2 W2：一次编译、逐行消费——正则/比较值准备与匹配分离，五种视图共用
  // 同一份逻辑结果（不再各写一套过滤器）。
  const query::CompiledQuery compiled = query::CompiledQuery::compile(m_query);
  QSet<QString> visibleIds;
  int shown = 0;
  {
    const paleo::pagesinternal::PagePerfStage perf("filter-pass");
    for (const AssetRowInfo &row : m_rows)
    {
      if (!compiled.matches(row))
        continue;
      visibleIds.insert(row.assetId);
      ++shown;
    }
  }
  QVector<AssetRowInfo> filtered;
  for (const auto &row : m_rows)
    if (visibleIds.contains(row.assetId))
      filtered.append(row);
  const QString sortField = m_assetSortColumn == 1 ? QStringLiteral("type")
                           : m_assetSortColumn == 2 ? QStringLiteral("entity") : QStringLiteral("name");
  if (m_assetSortColumn >= 0) {
    const paleo::pagesinternal::PagePerfStage perf("sort");
    sortAssetRows(&filtered, sortField, m_assetSortAscending);
  }
  m_filteredRows = filtered;
  m_visibleAssetIds = visibleIds;
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
    // 树形视图（0）展示完整实体树；虚拟表（3）自管 fetchMore。
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

  // D2.7 高亮 needle（首个正向 Search 条件；旧搜索框是该条件的镜像编辑器）。
  if (m_delegate)
  {
    QString needle;
    for (const FilterCondition &c : m_query.group.conditions)
      if (c.dim == FilterDim::Search && !c.negate)
      {
        needle = c.value;
        break;
      }
    m_delegate->setNeedle(needle);
  }

  // 树形视图过滤（P3：按可见 id 集 + 文本匹配递归）。
  const bool filtering = !m_query.isEmpty();
  if (!m_viewStack || m_viewStack->currentIndex() == 0)
    applyFilterToTree(visibleIds, filtering);

  // D2.9/P2 W6 空结果态：有过滤而零命中 → 空态 + 实际生效条件说明（含
  // 列约束/标签/正则字面量回退——解释的与执行的同一份准备产物）。
  if (m_emptyState)
  {
    const bool noHits = filtering && shown == 0 && total > 0;
    m_emptyState->setVisible(noHits);
    if (noHits)
    {
      QStringList parts;
      for (const auto &s : compiled.summaries())
        parts << s.text;
      m_emptyState->setMessage(
          tr("没有匹配的资产 — 当前过滤：%1").arg(parts.join(QStringLiteral("；"))));
    }
  }

  // D7 新视图页同口径重灌（图标/分组；虚拟模型给全量行——fetchMore 自管）。
  if (m_iconView && (!m_viewStack || m_viewStack->currentIndex() == 2 ||
                     m_viewStack->currentIndex() == 4))
  {
    QVector<AssetRowInfo> vis;
    for (const AssetRowInfo &row : m_rows)
      if (visibleIds.contains(row.assetId))
        vis.append(row);
    if (!m_viewStack || m_viewStack->currentIndex() == 2)
      m_iconView->loadRows(paged ? m_pageRows : vis);
    if (m_groupTree && (!m_viewStack || m_viewStack->currentIndex() == 4))
      m_groupTree->loadRows(paged ? m_pageRows : vis);
  }
  if (m_virtualView && (!m_viewStack || m_viewStack->currentIndex() == 3))
  {
    QVector<AssetRowInfo> vis;
    for (const AssetRowInfo &row : m_rows)
      if (visibleIds.contains(row.assetId))
        vis.append(row);
    // D7.4 稳定排序（多列次级）：名称升序为次级键。
    sortAssetRows(&vis, QStringLiteral("name"), true);
    // 同集同序同数据则跳过重建：setRows 的 reset 会清掉虚拟视图选区/滚动
    //（批量改型等「先 updateRows 再 applyListFilter」的路径依赖这一点）；
    // 行进出或任何数据差异照常重建。
    if (!m_virtualView->flatModel()->sameRowsAs(vis))
      m_virtualView->flatModel()->setRows(vis);
    if (m_virtualView->horizontalHeader())
      restoreColumnState(QStringLiteral("assetVirtualTable"),
                         m_virtualView->horizontalHeader());
  }
  restoreAssetSelection();
  refreshSelectionBadge();
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
  {
    m_chipBar->setConditions(m_query.group);
    m_chipBar->setColumnConstraints(m_query.columns); // P2 W3：列约束可清除
  }
  if (m_quickBar)
  {
    // P2 W6：快捷按钮勾选态随查询（预设/分享串替换后不再残留旧态）；同维
    // 存在否定条件时 tooltip 明示「显示开但被反向条件排除」。
    m_quickBar->setActiveStates(m_query.hasPositive(paleo::dataops::FilterDim::Unlinked),
                                m_query.hasPositive(paleo::dataops::FilterDim::UnknownType),
                                m_query.hasPositive(paleo::dataops::FilterDim::Warned));
    const struct
    {
      paleo::dataops::FilterDim d;
      const char *btn;
    } kQuick[] = {{paleo::dataops::FilterDim::Unlinked, "quickUnlinked"},
                  {paleo::dataops::FilterDim::UnknownType, "quickUnknownType"},
                  {paleo::dataops::FilterDim::Warned, "quickWarned"}};
    for (const auto &q : kQuick)
      if (auto *btn = m_quickBar->findChild<QPushButton *>(QLatin1String(q.btn)))
      {
        bool negated = false;
        for (const auto &c : m_query.group.conditions)
          if (c.dim == q.d && c.negate)
            negated = true;
        if (negated)
          btn->setToolTip(tr("已开启，但存在反向条件——两者同时生效时结果为其补集"));
      }
  }
  if (m_filterBar)
  {
    // 维度词表（下拉候选）——行快照 effectiveType 口径（覆盖类型可选）。
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
    m_tagCloud->setCloud(m_tags.tagCloud(), m_query.activeTag);
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

// ---- P2 W1：旧控件 = 编辑器/镜像（不再当第二事实源）--------------------------
// 用户编辑（textChanged/currentIndexChanged）→ setPositiveValue 只动首个正向
// 条件；本镜像只在刷新/替换后把状态写回控件（QSignalBlocker 防回环），控件
// 为空时绝不删除否定/其他来源条件（原 clearDim 路径的语义缺口）。
void DataListPanel::mirrorQueryIntoLegacyControls()
{
  using namespace paleo::dataops;
  auto *search = findChild<QLineEdit *>(QStringLiteral("assetSearchEdit"));
  auto *typeFilter = findChild<QComboBox *>(QStringLiteral("assetTypeFilter"));
  const auto firstPositive = [this](FilterDim d) {
    for (const FilterCondition &c : m_query.group.conditions)
      if (c.dim == d && !c.negate)
        return c.value;
    return QString();
  };
  if (search)
  {
    const QString v = firstPositive(FilterDim::Search);
    if (search->text().trimmed() != v)
    {
      const QSignalBlocker b(search);
      search->setText(v);
    }
  }
  if (typeFilter)
  {
    const QString v = firstPositive(FilterDim::Type);
    if (typeFilter->currentData().toString() != v)
    {
      const QSignalBlocker b(typeFilter);
      const int idx = typeFilter->findData(v);
      typeFilter->setCurrentIndex(idx >= 0 ? idx : 0);
    }
  }
}

void DataListPanel::wireLegacyFilterControls(QLineEdit *search, QComboBox *typeFilter)
{
  if (!search || !typeFilter)
    return;
  connect(search, &QLineEdit::textChanged, this, [this](const QString &text) {
    // 只编辑首个正向 Search 条件；否定条件/其他维度/同维度多值保留。刷新
    // 期间的镜像回写走 QSignalBlocker，不会进本处理器（无反馈环）。
    if (m_query.setPositiveValue(paleo::dataops::FilterDim::Search, text.trimmed()))
      commitQuery();
  });
  connect(typeFilter, &QComboBox::currentIndexChanged, this, [this] {
    auto *tf = qobject_cast<QComboBox *>(sender());
    if (!tf)
      return;
    if (m_query.setPositiveValue(paleo::dataops::FilterDim::Type,
                                 tf->currentData().toString()))
      commitQuery();
  });
}

void DataListPanel::commitQuery()
{
  // 统一提交面：镜像（防被删条件从旧输入框回填）→ AND/OR/快捷态同步 →
  // 过滤 → chip/词表刷新。
  mirrorQueryIntoLegacyControls();
  if (m_filterBar)
    m_filterBar->setOrMode(m_query.group.orMode);
  applyListFilter();
  refreshChipBar();
  refreshTagCloud();
}

void DataListPanel::applyFilterGroup(const paleo::dataops::FilterGroup &g)
{
  // 兼容面：替换条件组（FilterGroup 不含列约束 → 一并清除；活动标签保留，
  // 与旧实现语义一致）。镜像后旧输入框不会把已删除的条件带回来。
  m_query.group = g;
  m_query.columns.clear();
  commitQuery();
}

void DataListPanel::applyQuery(const paleo::dataops::query::DataQuery &q)
{
  m_query = q;
  commitQuery();
}

paleo::dataops::query::QueryStateParse
DataListPanel::tryApplyQueryStateString(const QString &s)
{
  // P2 W4：先解析、看诊断，再决定是否替换——坏串不清空当前查询/选区，
  // 且失败要被看见（不能看起来像成功应用了原意）。
  auto parsed = paleo::dataops::query::parseQueryStateString(s);
  if (parsed.valid)
  {
    m_query = parsed.query;
    commitQuery();
  }
  else
  {
    emit statusMessage(tr("过滤器链接未应用：%1").arg(parsed.diagnostic));
  }
  return parsed;
}

void DataListPanel::setFilterFromStateString(const QString &s)
{
  // 兼容面：合法旧串/新串照常整组替换；坏串保持现状（旧行为会清空过滤）。
  tryApplyQueryStateString(s);
}

// ---- P2 W3：列漏斗提交面 ------------------------------------------------------
void DataListPanel::setColumnConstraint(paleo::dataops::query::ColumnKey k,
                                        const QStringList &values)
{
  m_query.setColumnValues(k, values);
  commitQuery();
}

void DataListPanel::clearColumnConstraint(paleo::dataops::query::ColumnKey k)
{
  if (!m_query.column(k))
    return;
  m_query.clearColumn(k);
  commitQuery();
}

QStringList
DataListPanel::columnCandidatesFor(paleo::dataops::query::ColumnKey k) const
{
  return m_query.columnCandidates(k, m_rows);
}

void DataListPanel::openColumnFunnel(int tableColumn)
{
  using namespace paleo::dataops::query;
  ColumnKey k;
  if (tableColumn == 0)
    k = ColumnKey::Name;
  else if (tableColumn == 1)
    k = ColumnKey::Type;
  else if (tableColumn == 2)
    k = ColumnKey::Entity;
  else
    return;
  const QStringList candidates = m_query.columnCandidates(k, m_rows);
  if (candidates.isEmpty())
    return;
  QStringList checked;
  if (const ColumnConstraint *cur = m_query.column(k))
    checked = cur->values;
  else
    checked = candidates;
  bool accepted = false;
  // 键盘可达（QMenu 原生导航/空格勾选/Esc 取消）；取消不提交。
  const QStringList keep =
      paleo::dataops::ColumnValuePicker::exec(candidates, checked, this, &accepted);
  if (!accepted)
    return;
  if (keep == candidates)
    clearColumnConstraint(k); // 全选 = 不限（确定行为，规范化）
  else
    setColumnConstraint(k, keep); // 空集 = 显式零命中
}
