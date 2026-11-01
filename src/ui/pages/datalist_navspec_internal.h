// 层：视图
#pragma once
#include "datalist.h"

#include "../../catalog/datacatalog.h"
#include "../../services/wellreadiness.h"
#include "datalistops.h"
#include "dataops/dataopsmodel.h"
#include <QTreeWidgetItem>
#include <algorithm>

inline int countItemNodes(const QTreeWidgetItem *item)
{
  int count = 1;
  for (int i = 0; i < item->childCount(); ++i)
    count += countItemNodes(item->child(i));
  return count;
}

struct DataListPanel::NavNodeSpec
{
  QString id;      // Qt::UserRole（资产/集合 id；分类节点空）
  QString eid;     // Qt::UserRole + 1（井/实体 id）
  QString type;    // Qt::UserRole + 2（节点类型标记）
  QString sub;     // Qt::UserRole + 3（子标记：well_attachments/ungrouped/inline/…）
  QVariant role4;  // Qt::UserRole + 4（realization 成员 idx / 统计 token；无效 = 清除）
  QString text0;
  QString text1;
  QString tooltip0;
  QString icon;    // qgis 主题图标名（PaleoIcons::qgisTheme 输入）
  bool expanded = false;   // 构建默认展开（仅新建项生效；存量项不碰用户态）
  bool monoDetail = false; // text1 走 mono 字体（坐标列，DESIGN.md）
  bool warning = false;    // text0 走警色（未决关联）
  bool enabled = true;     // false = 禁选（realization 缺席成员）
  // text1 列状态着色（井就绪标识：1=Success 曲线+时深齐备 / 2=Warning 缺项；
  // DESIGN.md:117 状态必带文字，语义色唯一出口 ItemTextColor，禁裸色点）。
  int status1 = 0;
  QVector<NavNodeSpec> children;
};

// E.4/E.5：层位分组分支（从 DataListPanel::buildNavTreeSpec 拆出——巨型文件
// 1100>1094 与函数体 679>676 双闸只缩不涨）。地震解释层位归入地震分类，
// 资产仍保持 horizon 类型与层序界面关联；网格规格不在 catalog 里（按文件名
// 臆造 411×641 已回收）——如实标类型。成员内联定义保持 tr() 上下文为
// "DataListPanel"（翻译键不变）。
inline void DataListPanel::appendHorizonNavBranch(NavNodeSpec &seismicRoot,
                                                  const QList<CatalogAsset> &horAssets) const
{
  NavNodeSpec horRoot;
  horRoot.text0 = tr("层位 (%1)").arg(horAssets.size());
  horRoot.text1 = tr("解释层位数据");
  horRoot.type = QStringLiteral("category");
  horRoot.icon = QStringLiteral("mActionOpenTable.svg");
  horRoot.expanded = false;

  for (const CatalogAsset &a : horAssets)
  {
    NavNodeSpec hItem;
    hItem.text0 = a.displayName;
    hItem.id = a.id;
    hItem.type = QStringLiteral("horizon");
    hItem.icon = QStringLiteral("mActionOpenTable.svg");
    hItem.text1 = tr("层位网格");
    horRoot.children.append(hItem);
  }
  seismicRoot.children.append(horRoot);
}

// 井条目的就绪档位（HY 验收 2026-11-01）：曲线+已确认时深双全 → 绿档
//「曲线+时深齐备」；缺项 → 橙档「缺曲线/缺时深」。全量井保留（数据管理
// 面不过滤），下游分析选井面按就绪口径另行过滤（wellreadiness）。
// 从 buildNavTreeSpec 拆出（file_budget 只缩不涨）；tr() 上下文保持
// "DataListPanel"。
inline void DataListPanel::applyWellReadinessBadge(
    NavNodeSpec &item, const paleo::WellReadiness &ready) const
{
  QString readinessText;
  if (ready.analysisReady())
  {
    item.status1 = 1;
    readinessText = tr("曲线+时深齐备");
  }
  else
  {
    QStringList missing;
    if (!ready.hasCurves) missing << tr("缺曲线");
    if (!ready.tdUsable) missing << tr("缺时深");
    item.status1 = 2;
    readinessText = missing.join(QStringLiteral(" · "));
  }
  item.text1 = item.text1.isEmpty()
                   ? readinessText
                   : readinessText + QStringLiteral(" · ") + item.text1;
}

// 井实体的可见收集（软删回收站滤除）+ 记忆排序（D2.5：时间档实体无时间面
// 保持稳定序，名称档按改写显示名自然序）。从 buildNavTreeSpec 拆出。
inline QVector<CatalogEntity> DataListPanel::visibleWellsSortedForNav(
    DataCatalog *cat, const paleo::dataops::EntityOverrideStore *ovStore) const
{
  QVector<CatalogEntity> wells = cat->entities(QStringLiteral("well"));
  QVector<CatalogEntity> wellsVisible;
  for (const CatalogEntity &e : wells)
    if (!m_recycle.isRemoved(e.id))
      wellsVisible.append(e);
  std::sort(wellsVisible.begin(), wellsVisible.end(),
            [this, ovStore](const CatalogEntity &a, const CatalogEntity &b) {
              if (m_treeSort == paleo::dataops::TreeSortKind::Time)
                return false; // 实体无时间面——保持稳定序
              return paleo::pagesinternal::naturalNameSort(ovStore->displayName(a),
                                                    ovStore->displayName(b));
            });
  return wellsVisible;
}
