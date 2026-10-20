// 层：视图
#pragma once
#include "datalist.h"

#include "../../catalog/datacatalog.h"

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
