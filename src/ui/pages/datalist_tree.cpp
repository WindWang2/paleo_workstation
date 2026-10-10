// 层：视图
#include "datalist.h"
#include "datalistops.h"
#include "../paleotheme.h"
#include "../paleoicons.h"
#include "../../catalog/datacatalog.h"
#include "../../catalog/catalogroles.h"
#include "../../catalog/realizationset.h"
#include "../../services/previewdoc.h"
#include "datanavtree.h"
#include "dataops/dataopsmodel.h"
#include "dataopsviews.h"

#include <algorithm>
#include <functional>

#include <QFont>
#include <QInputDialog>
#include <QLineEdit>
#include <QQueue>
#include <QScrollBar>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>

using namespace paleo::pagesinternal;

namespace {

// 节点身份部件：type|id|eid|sub 四元组——与 navExpandedKey 同源语义；
// 无 id 的分类组（id 与 eid 皆空）补去计数尾巴的文本（"测井 (20 井)" →
// "测井"）区分同名兄弟（综合柱状图 vs 未关联曲线）。增量 reconcile 与
// 展开态快照用同一把钥匙——匹配得到的节点重建后也能找回同一展开态。
QString navKeyPart(const QString &type, const QString &id, const QString &eid,
                   const QString &sub, const QString &text)
{
  QString part = type + QLatin1Char('|') + id + QLatin1Char('|') + eid +
                 QLatin1Char('|') + sub;
  if (id.isEmpty() && eid.isEmpty())
    part += QLatin1Char('|') + text.section(QStringLiteral(" ("), 0, 0);
  return part;
}

QString navItemKey(const QTreeWidgetItem *it)
{
  return navKeyPart(it->data(0, Qt::UserRole + 2).toString(),
                    it->data(0, Qt::UserRole).toString(),
                    it->data(0, Qt::UserRole + 1).toString(),
                    it->data(0, Qt::UserRole + 3).toString(), it->text(0));
}

// 展开态快照键：沿父链拼路径（根→本节点），单节 = navKeyPart。父链区分
// 跨井同名分支（如各井的「岩心照片」组）——重建后按键找回同一节点，
// catalog.changed 触发的全量刷新不抹掉用户的展开，也不串井。
QString navExpandedKey(const QTreeWidgetItem *it)
{
  QStringList parts;
  for (const QTreeWidgetItem *p = it; p; p = p->parent())
    parts.prepend(navItemKey(p));
  return parts.join(QStringLiteral("\x1f"));
}

int countItemNodes(const QTreeWidgetItem *it)
{
  int n = 1;
  for (int i = 0; i < it->childCount(); ++i)
    n += countItemNodes(it->child(i));
  return n;
}

} // namespace

// ---- 方向 92：树蓝图（NavNodeSpec）------------------------------------------------
//
// 纯数据中间层：buildNavTreeSpec() 从 catalog + 面板状态产出整棵树的内容
// 蓝图（无任何 widget 操作）；全量重建（renderNavChildren）与增量 reconcile
// 共用同一 applyNavSpec 落字段——两条通道产物由构造保证一致，增量不漂移。
// 资产级增/删/改落到节点级更新；变更面超阈值回全量兜底（阈值口径见
// datalist.h kNavReconcile* 注释与 ledger 定案）。
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

QVector<DataListPanel::NavNodeSpec> DataListPanel::buildNavTreeSpec() const
{
  QVector<NavNodeSpec> tops;
  PreviewDocService *svc = m_doc;
  if (!svc)
    return tops;
  DataCatalog *cat = svc->catalog();
  if (!cat)
    return tops;

  // 0. 测区 (Survey Area) — 首项显示，双击打开测区全景地图 (QGIS 画布)
  {
    NavNodeSpec surveyRoot;
    surveyRoot.text0 = tr("测区");
    surveyRoot.text1 = tr("工区全景");
    surveyRoot.id = QStringLiteral("survey_area");
    surveyRoot.type = QStringLiteral("survey_area");
    surveyRoot.icon = QStringLiteral("mIconPolygonLayer.svg");
    surveyRoot.expanded = false;

    NavNodeSpec surveyMapItem;
    surveyMapItem.text0 = tr("工区全景地图");
    surveyMapItem.text1 = tr("QGIS地图画布");
    surveyMapItem.id = QStringLiteral("survey_area");
    surveyMapItem.type = QStringLiteral("survey_area");
    surveyMapItem.icon = QStringLiteral("mActionZoomFullExtent.svg");
    surveyRoot.children.append(surveyMapItem);
    tops.append(surveyRoot);
  }

  // 1. 井 (Wells)——显示名走实体改写表（D4.1），排序按 D2.5 记忆键。
  QVector<CatalogEntity> wells = cat->entities(QStringLiteral("well"));
  // 软删实体（撤销新建等）不显示。
  QVector<CatalogEntity> wellsVisible;
  for (const CatalogEntity &e : wells)
    if (!m_recycle.isRemoved(e.id))
      wellsVisible.append(e);
  wells = wellsVisible;
  const paleo::dataops::EntityOverrideStore *ovStore = &m_entityOv;
  std::sort(wells.begin(), wells.end(),
            [ovStore, this](const CatalogEntity &a, const CatalogEntity &b) {
              if (m_treeSort == paleo::dataops::TreeSortKind::Time)
              {
                const paleo::dataops::EntityOverride oa = ovStore->overrideFor(a.id);
                const paleo::dataops::EntityOverride ob = ovStore->overrideFor(b.id);
                return false; // 实体无时间面——保持稳定序
              }
              return naturalNameSort(ovStore->displayName(a), ovStore->displayName(b));
            });

  // 1. 测井（井实体树 + 多井柱状图/未关联曲线子分支）。已挂井的曲线只在
  // 井节点下出现一次（原平铺「测井」组与其重复——去重后平铺侧只收未决）。
  NavNodeSpec wellRoot;
  wellRoot.text0 = tr("测井 (%1 井)").arg(wells.size());
  wellRoot.text1 = tr("井位 / 测井曲线 / 分层 / 时深");
  wellRoot.type = QStringLiteral("category");
  wellRoot.icon = QStringLiteral("mIconPointLayer.svg");
  wellRoot.expanded = false;

  for (const CatalogEntity &w : wells)
  {
    NavNodeSpec wellItem;
    wellItem.text0 = m_entityOv.displayName(w);
    wellItem.eid = w.id;
    wellItem.type = QStringLiteral("well");
    wellItem.icon = QStringLiteral("mIconPointLayer.svg");
    if (w.hasSurface)
    {
      wellItem.text1 = QStringLiteral("X: %1, Y: %2")
                           .arg(QString::number(w.surfaceX, 'f', 1))
                           .arg(QString::number(w.surfaceY, 'f', 1));
      wellItem.monoDetail = true; // 坐标列走 mono 数字面（DESIGN.md）
    }

    // 获取该井所有关联资产并按角色序排（catalogRoles() 单源——well_log →
    // tops → time_depth → well_head → …）；词表外角色恒尾序、收进「未分组」桶。
    const QVector<EntityAssetLink> wLinks = cat->linksForEntity(w.id);
    QVector<EntityAssetLink> sortedLinks = wLinks;
    std::stable_sort(sortedLinks.begin(), sortedLinks.end(),
                     [](const EntityAssetLink &a, const EntityAssetLink &b) {
                       const int ra = catalogRoleOrder(a.role);
                       const int rb = catalogRoleOrder(b.role);
                       if (ra != rb)
                         return ra < rb;
                       if (a.ordinal != b.ordinal)
                         return a.ordinal < b.ordinal;
                       return false;
                     });

    QVector<EntityAssetLink> coreLinks; // core 角色链接——收尾建分组分支用
    QVector<EntityAssetLink> ungroupedLinks; // 词表外角色——「未分组」可见桶（不静默藏匿）
    for (const EntityAssetLink &l : sortedLinks)
    {
      const CatalogAsset a = cat->assetById(l.assetId);
      if (a.id.isEmpty())
        continue;
      // 岩心照片（core 角色）是高频多实例数据——不平铺进井节点，收进
      // 末尾的「岩心照片 (N)」分组分支（L3），照片叶在第四级。
      if (l.role == QLatin1String("core"))
      {
        coreLinks.append(l);
        continue;
      }
      // 词表外角色（工程自定义 role 等）：不与词表角色混排，也不静默藏匿——
      // 收进末尾「未分组」可见桶（叶三元组与平铺叶同构，激活/拖放语义不变）。
      if (!catalogRoleInfo(l.role))
      {
        ungroupedLinks.append(l);
        continue;
      }
      NavNodeSpec sub;
      sub.id = a.id;
      sub.eid = w.id;
      sub.type = l.role;

      const QString roleDisplay = catalogRoleDisplay(l.role);
      QString detailDisplay;
      if (l.role == QLatin1String("well_log"))
      {
        detailDisplay = a.displayName;
        sub.icon = QStringLiteral("mIconLineLayer.svg");
      }
      else if (l.role == QLatin1String("tops"))
      {
        detailDisplay = a.displayName;
        sub.icon = QStringLiteral("mActionOpenTable.svg");
      }
      else if (l.role == QLatin1String("time_depth"))
      {
        detailDisplay = a.displayName;
        sub.icon = QStringLiteral("mActionOpenTable.svg");
      }
      else if (l.role == QLatin1String("well_head"))
      {
        detailDisplay = a.displayName;
        sub.icon = QStringLiteral("mIconPointLayer.svg");
      }
      else
      {
        sub.icon = QStringLiteral("mActionOpenTable.svg");
      }
      if (l.role == QLatin1String("well_log") && !l.unresolved)
      {
        const QString kind = l.isPrimary ? tr("主文件") : tr("成员");
        sub.text0 = tr("%1 · %2 (%3)").arg(a.displayName, kind, roleDisplay);
        sub.text1 = tr("%1 · %2").arg(kind, a.displayName);
      }
      else
      {
        sub.text0 = QStringLiteral("%1 (%2)").arg(a.displayName, roleDisplay);
        sub.text1 = detailDisplay;
      }
      if (l.unresolved)
      {
        sub.warning = true; // 待复核色（现取随主题）
        sub.text1 = tr("未决关联");
      }
      wellItem.children.append(sub);
    }

    // 「岩心照片 (N)」分组分支（L3，无 id 的 category 节点；双击跳井附件
    // 管理面板——方向 79）。照片叶（L4）保留 assetId/wellId/core 角色三元
    // ——激活、拖放、过滤语义与原平铺叶完全一致。
    if (!coreLinks.isEmpty())
    {
      NavNodeSpec branch;
      branch.type = QStringLiteral("category");
      branch.sub = QStringLiteral("well_attachments");
      branch.icon = QStringLiteral("mIconRaster.svg");
      int added = 0;
      for (const EntityAssetLink &l : coreLinks)
      {
        const CatalogAsset a = cat->assetById(l.assetId);
        if (a.id.isEmpty())
          continue;
        NavNodeSpec leaf;
        leaf.id = a.id;
        leaf.eid = w.id;
        leaf.type = l.role;
        leaf.text0 = a.displayName;
        leaf.text1 = tr("岩心照片");
        leaf.icon = QStringLiteral("mIconRaster.svg");
        if (l.unresolved)
        {
          leaf.warning = true;
          leaf.text1 = tr("未决关联");
        }
        branch.children.append(leaf);
        ++added;
      }
      if (added > 0)
      {
        branch.text0 = tr("岩心照片 (%1)").arg(added);
        wellItem.children.append(branch);
      }
    }

    // 「未分组 (N)」分支（方向 92）：词表外角色可见落桶——排序上不与词表
    // 角色混排，也不静默藏匿；角色名如实展示（词表外不臆造翻译）。
    if (!ungroupedLinks.isEmpty())
    {
      NavNodeSpec branch;
      branch.text0 = tr("未分组 (%1)").arg(ungroupedLinks.size());
      branch.text1 = tr("词表外角色（工程自定义）");
      branch.type = QStringLiteral("category");
      branch.sub = QStringLiteral("ungrouped");
      branch.icon = QStringLiteral("mActionFolder.svg");
      for (const EntityAssetLink &l : ungroupedLinks)
      {
        const CatalogAsset a = cat->assetById(l.assetId);
        if (a.id.isEmpty())
          continue;
        NavNodeSpec leaf;
        leaf.id = a.id;
        leaf.eid = w.id;
        leaf.type = l.role;
        leaf.text0 = a.displayName;
        leaf.text1 = l.role;
        leaf.icon = QStringLiteral("mActionOpenTable.svg");
        if (l.unresolved)
        {
          leaf.warning = true;
          leaf.text1 = tr("未决关联");
        }
        branch.children.append(leaf);
      }
      wellItem.children.append(branch);
    }
    wellRoot.children.append(wellItem);
  }

  // 资产挂在 auxiliary 实体（reference 角色）上 = 辅助资料，不进测区井/测井分组。
  // D1.6 软删资产全树不显示。
  const auto assetVisible = [this](const CatalogAsset &a) {
    return !m_recycle.isRemoved(a.id);
  };
  const auto linkedToAuxEntity = [cat](const QString &assetId) {
    for (const EntityAssetLink &l : cat->linksForAsset(assetId))
    {
      if (!l.entityId.isEmpty() &&
          cat->entityById(l.entityId).entityType == QLatin1String("auxiliary"))
        return true;
    }
    return false;
  };
  // 已决挂井（井实体 + 非 unresolved）——这些曲线在井节点下显示，平铺侧不重复。
  const auto linkedToWell = [cat](const QString &assetId) {
    for (const EntityAssetLink &l : cat->linksForAsset(assetId))
    {
      if (!l.unresolved && !l.entityId.isEmpty() &&
          cat->entityById(l.entityId).entityType == QLatin1String("well"))
        return true;
    }
    return false;
  };

  // 1b. 测井组内子分支：多井综合柱状图 + 未关联曲线（挂井曲线在井节点下）。
  QList<CatalogAsset> logAssets;
  QList<CatalogAsset> compositeAssets;
  for (const CatalogAsset &a : cat->assets())
  {
    if (!assetVisible(a))
      continue;
    if (a.type == QLatin1String("well_log") || a.displayName.endsWith(QLatin1String(".las"), Qt::CaseInsensitive))
    {
      if (!linkedToWell(a.id))
        logAssets.append(a);
    }
    else if ((a.displayName.contains(QStringLiteral("柱状图")) ||
              (a.displayName.endsWith(QLatin1String(".xml"), Qt::CaseInsensitive) && a.displayName.contains(QStringLiteral("综合")))) &&
             !linkedToAuxEntity(a.id))
    {
      compositeAssets.append(a);
    }
  }

  std::sort(logAssets.begin(), logAssets.end(), [](const CatalogAsset &a, const CatalogAsset &b) {
    return naturalNameSort(a.displayName, b.displayName);
  });
  std::sort(compositeAssets.begin(), compositeAssets.end(), [](const CatalogAsset &a, const CatalogAsset &b) {
    return naturalNameSort(a.displayName, b.displayName);
  });

  if (!compositeAssets.isEmpty())
  {
    NavNodeSpec compBranch;
    compBranch.text0 = tr("综合柱状图 (%1)").arg(compositeAssets.size());
    compBranch.text1 = tr("多井道地质综合柱状图 (ResFormStar 规范)");
    compBranch.type = QStringLiteral("category");
    compBranch.icon = QStringLiteral("mIconLineLayer.svg");
    compBranch.expanded = true;

    for (const CatalogAsset &a : compositeAssets)
    {
      NavNodeSpec it;
      it.text0 = a.displayName;
      it.text1 = tr("多井道地质综合柱状图"); // 道数/曲线数未解析，不臆造
      it.id = a.id;
      it.type = QStringLiteral("composite_log");
      it.icon = QStringLiteral("mIconLineLayer.svg");
      compBranch.children.append(it);
    }
    wellRoot.children.append(compBranch);
  }

  if (!logAssets.isEmpty())
  {
    NavNodeSpec curveBranch;
    curveBranch.text0 = tr("未关联曲线 (%1)").arg(logAssets.size());
    curveBranch.text1 = tr("无已决井链接的 LAS（拖到井节点挂接）");
    curveBranch.type = QStringLiteral("category");
    curveBranch.icon = QStringLiteral("mIconLineLayer.svg");
    curveBranch.expanded = true;

    for (const CatalogAsset &a : logAssets)
    {
      NavNodeSpec it;
      it.text0 = a.displayName;
      it.text1 = tr("测井曲线 (GR/AC/DEN/电阻率等)");
      it.id = a.id;
      it.type = QStringLiteral("well_log");
      it.icon = QStringLiteral("mIconLineLayer.svg");
      curveBranch.children.append(it);
    }
    wellRoot.children.append(curveBranch);
  }
  tops.append(wellRoot);

  // 3. 地震 (Seismic)
  QList<CatalogAsset> seisAssets;
  for (const CatalogAsset &a : cat->assets())
    if (assetVisible(a) && a.type == QLatin1String("seismic"))
      seisAssets.append(a);
  NavNodeSpec seismicRoot;
  seismicRoot.text0 = tr("地震 (%1)").arg(seisAssets.size());
  seismicRoot.text1 = tr("三维地震体 / 层位解释");
  seismicRoot.type = QStringLiteral("category");
  seismicRoot.icon = QStringLiteral("mIconPolygonLayer.svg");
  seismicRoot.expanded = false;

  const QVector<CatalogEntity> surveys = cat->entities(QStringLiteral("seismic_survey"));
  CatalogEntity survey;
  if (!surveys.isEmpty())
    survey = surveys.front();

  for (const CatalogAsset &a : seisAssets)
  {
    NavNodeSpec seisItem;
    seisItem.text0 = a.displayName;
    seisItem.id = a.id;
    seisItem.type = QStringLiteral("seismic_volume");
    seisItem.icon = QStringLiteral("mIconPolygonLayer.svg");
    if (!survey.id.isEmpty() && survey.inlineMax > survey.inlineMin)
    {
      seisItem.text1 = QStringLiteral("Inline %1–%2 · Xline %3–%4 · %5ms")
          .arg(int(survey.inlineMin)).arg(int(survey.inlineMax))
          .arg(int(survey.xlineMin)).arg(int(survey.xlineMax))
          .arg(survey.sampleIntervalUs / 1000.0, 0, 'f', 1);

      NavNodeSpec inl;
      inl.text0 = tr("Inline 剖面 (主测线 %1–%2)").arg(int(survey.inlineMin)).arg(int(survey.inlineMax));
      inl.text1 = tr("双击预览剖面");
      inl.id = a.id;
      inl.type = QStringLiteral("seismic_line");
      inl.sub = QStringLiteral("inline");
      inl.icon = QStringLiteral("mIconLineLayer.svg");

      NavNodeSpec xl;
      xl.text0 = tr("Crossline 剖面 (联络线 %1–%2)").arg(int(survey.xlineMin)).arg(int(survey.xlineMax));
      xl.text1 = tr("双击预览剖面");
      xl.id = a.id;
      xl.type = QStringLiteral("seismic_line");
      xl.sub = QStringLiteral("crossline");
      xl.icon = QStringLiteral("mIconLineLayer.svg");

      seisItem.children.append(inl);
      seisItem.children.append(xl);
      seisItem.expanded = true;
    }
    else
    {
      seisItem.text1 = tr("SEG-Y 地震数据");
    }
    seismicRoot.children.append(seisItem);
  }

  // 地震解释层位归入地震分类，资产仍保持 horizon 类型与层序界面关联。
  QList<CatalogAsset> horAssets;
  for (const CatalogAsset &a : cat->assets())
    if (assetVisible(a) && a.type == QLatin1String("horizon"))
      horAssets.append(a);
  std::sort(horAssets.begin(), horAssets.end(), [](const CatalogAsset &a, const CatalogAsset &b) {
    return naturalNameSort(a.displayName, b.displayName);
  });
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
      // 网格规格不在 catalog 里（按文件名臆造 411×641 已回收）——如实标类型。
      hItem.text1 = tr("层位网格");
      horRoot.children.append(hItem);
    }
    seismicRoot.children.append(horRoot);
  }
  tops.append(seismicRoot);

  // 4. 成果图件（智能预测/编图产物）：预测栅格、相面、综合相图、编辑
  //     副本。过程快照（input_snapshot/constraint_*）是追溯机器，不进用户面。
  //     双击走通用资产预览（assetActivated），tif 出栅格页、gpkg 出矢量页。
  {
    const auto isMapProduct = [](const CatalogAsset &a, const CatalogVersion &v) {
      return a.type == QLatin1String("seismic_prediction") ||
             a.type == QLatin1String("wells_prediction") ||
             a.type == QLatin1String("composed_facies") ||
             a.type == QLatin1String("facies_polygons") ||
             a.type == QLatin1String("edited_facies") ||
             a.type == QLatin1String("single_factor_raster") ||
             a.type == QLatin1String("contour_lines") ||
             a.type == QLatin1String("single_factor_cartographic_work") ||
             a.type == QLatin1String("single_factor_cartographic_contour") ||
             a.type == QLatin1String("facies_fusion_raster") ||
             v.extra.value(QStringLiteral("mapping_product")).toBool();
    };
    QList<QPair<CatalogAsset, CatalogVersion>> products;
    for (const CatalogAsset &a : cat->assets())
      if (assetVisible(a))
      {
        const auto v = cat->currentVersion(a.id);
        if (isMapProduct(a, v))
          products.append({a, v});
      }
    std::sort(products.begin(), products.end(),
              [](const auto &x, const auto &y) {
                return naturalNameSort(x.first.displayName, y.first.displayName);
              });
    NavNodeSpec productRoot;
    productRoot.text0 = tr("成果图件 (%1)").arg(products.size());
    productRoot.text1 = tr("智能预测与编图产物");
    productRoot.type = QStringLiteral("category");
    productRoot.icon = QStringLiteral("mActionFolder.svg");
    productRoot.expanded = false; // 顶级分组默认收拢（navTree 契约）
    for (const auto &entry : products)
    {
      const auto &a = entry.first;
      const auto &v = entry.second;
      const QString title = v.extra.value("title").toString();
      const QString horizon = v.extra.value("horizon").toString();
      const bool mock = v.extra.value("mock").toBool();
      const bool raster = v.extra.value("layer_type").toString() == QLatin1String("raster") ||
                          a.format == QLatin1String("tif");
      NavNodeSpec item;
      item.text0 = title.isEmpty() ? a.displayName : title;
      QStringList desc;
      if (!horizon.isEmpty())
        desc << horizon;
      desc << (a.type == QLatin1String("wells_prediction") ? tr("测井相预测")
                : raster                              ? tr("相栅格")
                                                      : tr("相矢量"));
      if (mock)
        desc << tr("Mock");
      item.text1 = desc.join(QStringLiteral(" · "));
      item.tooltip0 = v.path;
      item.id = a.id;
      item.type = QStringLiteral("map_product");
      item.icon = raster ? QStringLiteral("mIconRasterLayer.svg")
                         : QStringLiteral("mIconPolygonLayer.svg");
      productRoot.children.append(item);
    }
    tops.append(productRoot);
  }

  // 5. 辅助资料 (Auxiliary)
  QList<CatalogAsset> auxAssets;
  for (const CatalogAsset &a : cat->assets())
  {
    if (!assetVisible(a))
      continue;
    // 挂在辅助实体下的综合柱状图（如参考井 XML）归参考资料，其余柱状图进测井分组。
    const bool isCompositeXml = a.displayName.contains(QStringLiteral("柱状图")) ||
        (a.displayName.endsWith(QLatin1String(".xml"), Qt::CaseInsensitive) && a.displayName.contains(QStringLiteral("综合")));
    if (isCompositeXml && !linkedToAuxEntity(a.id))
      continue;
    if (isCompositeXml)
    {
      auxAssets.append(a);
      continue;
    }

    if (a.type == QLatin1String("boundary") || a.type == QLatin1String("auxiliary") ||
        a.type == QLatin1String("document") || a.type == QLatin1String("reference") ||
        a.displayName.endsWith(QLatin1String(".geojson"), Qt::CaseInsensitive))
      auxAssets.append(a);
  }
  std::sort(auxAssets.begin(), auxAssets.end(), [](const CatalogAsset &a, const CatalogAsset &b) {
    return naturalNameSort(a.displayName, b.displayName);
  });
  {
    NavNodeSpec auxRoot;
    auxRoot.text0 = tr("辅助资料 (%1)").arg(auxAssets.size());
    auxRoot.text1 = tr("参考相图 / 文档 / 图片");
    auxRoot.type = QStringLiteral("category");
    auxRoot.icon = QStringLiteral("mActionFolder.svg");
    auxRoot.expanded = false;

    NavNodeSpec faciesBranch;
    faciesBranch.text0 = tr("参考相图");
    faciesBranch.type = QStringLiteral("category");
    faciesBranch.icon = QStringLiteral("mIconPolygonLayer.svg");
    faciesBranch.expanded = true;

    NavNodeSpec docBranch;
    docBranch.text0 = tr("参考资料");
    docBranch.type = QStringLiteral("category");
    docBranch.icon = QStringLiteral("mActionFolder.svg");
    docBranch.expanded = true;

    for (const CatalogAsset &a : auxAssets)
    {
      const bool isGeo = a.displayName.endsWith(QLatin1String(".geojson"), Qt::CaseInsensitive) || a.type == QLatin1String("boundary");
      NavNodeSpec it;
      it.text0 = a.displayName;
      it.id = a.id;
      it.type = QStringLiteral("auxiliary");
      it.icon = isGeo ? QStringLiteral("mIconPolygonLayer.svg") : QStringLiteral("mActionOpenTable.svg");
      if (isGeo)
        it.text1 = tr("GeoJSON 矢量相图");
      else if (a.displayName.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive))
        it.text1 = tr("PDF 文档");
      else if (a.displayName.endsWith(QLatin1String(".pptx"), Qt::CaseInsensitive))
        it.text1 = tr("PPT 演示文稿");
      else if (a.displayName.endsWith(QLatin1String(".png"), Qt::CaseInsensitive))
        it.text1 = tr("图像");
      else if (a.displayName.endsWith(QLatin1String(".xml"), Qt::CaseInsensitive))
        it.text1 = tr("XML 数据表");
      else
        it.text1 = a.type;
      if (isGeo)
        faciesBranch.children.append(it);
      else
        docBranch.children.append(it);
    }
    auxRoot.children.append(faciesBranch);
    auxRoot.children.append(docBranch);
    tops.append(auxRoot);
  }

  // 1b. 计划井 (Planned：方向34 布井候选)——虚拟部署实体，独立成组
  // 与实井分组隔开；显示名同走实体改写表，软删实体不显示。
  {
    QVector<CatalogEntity> planned = cat->entities(QStringLiteral("planned"));
    QVector<CatalogEntity> plannedVisible;
    for (const CatalogEntity &e : planned)
      if (!m_recycle.isRemoved(e.id) &&
          (!m_plannedVisible || m_plannedVisible(e.id)))
        plannedVisible.append(e);
    std::sort(plannedVisible.begin(), plannedVisible.end(),
              [ovStore](const CatalogEntity &a, const CatalogEntity &b) {
                return naturalNameSort(ovStore->displayName(a), ovStore->displayName(b));
              });
    NavNodeSpec plannedRoot;
    plannedRoot.text0 = tr("计划井 (%1)").arg(plannedVisible.size());
    plannedRoot.text1 = tr("布井候选（不进实井计算）");
    plannedRoot.type = QStringLiteral("category");
    plannedRoot.icon = QStringLiteral("mIconPointLayer.svg");
    plannedRoot.expanded = false;
    for (const CatalogEntity &p : plannedVisible)
    {
      NavNodeSpec item;
      item.text0 = m_entityOv.displayName(p);
      item.eid = p.id;
      item.type = QStringLiteral("planned");
      item.icon = QStringLiteral("mIconPointLayer.svg");
      if (p.hasSurface)
      {
        item.text1 = QStringLiteral("X: %1, Y: %2")
                         .arg(QString::number(p.surfaceX, 'f', 1))
                         .arg(QString::number(p.surfaceY, 'f', 1));
        item.monoDetail = true;
      }
      plannedRoot.children.append(item);
    }
    tops.append(plannedRoot);
  }

  // 6. 不确定性集合（方向 47）：realization_set 资产 → 父集合→成员树，
  //    成员 index 升序 + 缺号位如实列「缺席」；统计面挂为集合子节点
  //   （成员叶激活 = realizationMemberRequested，统计叶 = realizationStatRequested）。
  {
    const auto sets = paleo::realization::enumerateSets(*cat);
    if (!sets.isEmpty())
    {
      NavNodeSpec setRoot;
      setRoot.text0 = tr("不确定性集合 (%1)").arg(sets.size());
      setRoot.text1 = tr("realization 集合 · 成员等概率实现");
      setRoot.type = QStringLiteral("category");
      setRoot.icon = QStringLiteral("mIconRaster.svg");
      setRoot.expanded = true;
      for (const auto &set : sets)
      {
        NavNodeSpec setItem;
        QString setText = tr("%1（%2/%3 成员）")
                              .arg(set.title)
                              .arg(set.members.size())
                              .arg(set.declaredCount);
        if (!set.missingIndices.isEmpty())
        {
          QStringList miss;
          for (const int idx : set.missingIndices)
            miss << QStringLiteral("#%1").arg(idx);
          setText += tr(" · 缺 %1").arg(miss.join(QStringLiteral("、")));
        }
        setItem.text0 = setText;
        setItem.text1 = set.members.size() == 1 ? tr("单成员 · 无不确定性")
                                                : tr("realization 集合");
        setItem.id = set.setId; // 集合资产 id
        setItem.type = QStringLiteral("realization_set");
        setItem.icon = QStringLiteral("mIconRaster.svg");
        setItem.expanded = true;
        // 成员叶按 declaredCount 全列——缺号位展示为缺席（禁选）。
        for (int idx = 0; idx < set.declaredCount; ++idx)
        {
          const QString vid = paleo::realization::memberVersionId(set, idx);
          NavNodeSpec leaf;
          leaf.text0 = vid.isEmpty() ? tr("成员 #%1（缺席）").arg(idx)
                                     : tr("成员 #%1").arg(idx);
          leaf.type = QStringLiteral("realization_member");
          leaf.sub = set.setId;
          leaf.role4 = idx;
          leaf.icon = QStringLiteral("mIconRaster.svg");
          if (vid.isEmpty())
          {
            leaf.enabled = false;
            leaf.text1 = tr("成员栅格缺席——集合不完整");
          }
          else
            leaf.text1 = tr("成员版本 %1").arg(vid.left(8));
          setItem.children.append(leaf);
        }
        // 统计面子节点（token → 口径词与图签同源）。
        for (const auto &s : paleo::realization::statSurfaces(*cat, set.setId))
        {
          NavNodeSpec leaf;
          const QString label = paleo::realization::statisticDisplayLabel(s.token);
          leaf.text0 = tr("%1 · %2 成员").arg(label.isEmpty() ? s.token : label).arg(s.memberCount);
          leaf.type = QStringLiteral("realization_stat");
          leaf.sub = set.setId;
          leaf.role4 = s.token;
          leaf.icon = QStringLiteral("mIconRaster.svg");
          leaf.text1 = tr("统计面 %1").arg(s.token);
          setItem.children.append(leaf);
        }
        setRoot.children.append(setItem);
      }
      tops.append(setRoot);
    }
  }

  // 7. 标签分组（D2.4 组织面 + D3.5 拖放目标）：每个标签一个叶节点，
  //    拖资产到标签节点 = 打标签；点击标签 = 过滤。
  const auto tagCloud = m_tags.tagCloud();
  if (!tagCloud.isEmpty())
  {
    NavNodeSpec tagRoot;
    tagRoot.text0 = tr("标签 (%1)").arg(tagCloud.size());
    tagRoot.text1 = tr("点击过滤 / 拖资产来打标签");
    tagRoot.type = QStringLiteral("tag_group");
    tagRoot.icon = QStringLiteral("mActionFolder.svg");
    tagRoot.expanded = false;
    for (const auto &tc : tagCloud)
    {
      NavNodeSpec leaf;
      leaf.text0 = QStringLiteral("%1 ×%2").arg(tc.first).arg(tc.second);
      leaf.type = QStringLiteral("tag_leaf");
      leaf.sub = tc.first;
      leaf.icon = QStringLiteral("mActionOpenTable.svg");
      // 点击标签节点 = 按标签过滤（与标签云同语义）。
      tagRoot.children.append(leaf);
    }
    tops.append(tagRoot);
  }

  return tops;
}

// 蓝图 → 既有节点的字段全量重放（含清除面：告色/mono/禁选/tooltip/role4
// 每次按 spec 重置——增量更新不残留旧态）。expanded 只对新建项生效。
void DataListPanel::applyNavSpec(QTreeWidgetItem *item, const NavNodeSpec &spec,
                                 bool newItem)
{
  item->setText(0, spec.text0);
  item->setText(1, spec.text1);
  item->setToolTip(0, spec.tooltip0);
  item->setData(0, Qt::UserRole, spec.id);
  item->setData(0, Qt::UserRole + 1, spec.eid);
  item->setData(0, Qt::UserRole + 2, spec.type);
  item->setData(0, Qt::UserRole + 3, spec.sub);
  item->setData(0, Qt::UserRole + 4, spec.role4);
  item->setIcon(0, PaleoIcons::qgisTheme(spec.icon));
  PaleoTheme::setItemTextColor(item, 0, spec.warning
                                            ? PaleoTheme::ItemTextColor::Warning
                                            : PaleoTheme::ItemTextColor::Normal);
  item->setFont(1, spec.monoDetail ? PaleoTheme::monoFont() : QFont());
  Qt::ItemFlags flags = item->flags();
  if (spec.enabled)
    flags |= Qt::ItemIsEnabled;
  else
    flags &= ~Qt::ItemIsEnabled;
  item->setFlags(flags);
  if (newItem)
    item->setExpanded(spec.expanded);
}

// 全量重建的渲染段：按 spec 建整棵树（#270 展开态由 rebuildNavTree 快照还原）。
void DataListPanel::renderNavChildren(QTreeWidgetItem *parent,
                                      const QVector<NavNodeSpec> &specs)
{
  for (const NavNodeSpec &spec : specs)
  {
    auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
    applyNavSpec(item, spec, true);
    renderNavChildren(item, spec.children);
  }
}

// 全量兜底通道：快照展开态与滚动位 → clear → 按蓝图重建 → 还原。
// catalog.changed 触发的重建不得把树跳回一级收拢态（#270 语义原样保留）。
void DataListPanel::rebuildNavTree(const QVector<NavNodeSpec> &specs)
{
  QSet<QString> expandedKeys;
  {
    QTreeWidgetItemIterator it(m_tree);
    while (*it)
    {
      if ((*it)->isExpanded())
        expandedKeys.insert(navExpandedKey(*it));
      ++it;
    }
  }
  const int vpos = m_tree->verticalScrollBar()
                       ? m_tree->verticalScrollBar()->value()
                       : 0;
  m_tree->clear();
  renderNavChildren(nullptr, specs);
  // 还原展开态与滚动位（快照见上）——无匹配键的项保持构建默认态。
  {
    QTreeWidgetItemIterator it(m_tree);
    while (*it)
    {
      if (expandedKeys.contains(navExpandedKey(*it)))
        (*it)->setExpanded(true);
      ++it;
    }
  }
  if (auto *sb = m_tree->verticalScrollBar())
    sb->setValue(qMin(vpos, sb->maximum()));
}

// 蓝图子树的节点总数（阈值计数用：一次插入/删除计全部派生节点）。
int DataListPanel::navCountSpecNodes(const NavNodeSpec &spec)
{
  int n = 1;
  for (const NavNodeSpec &c : spec.children)
    n += navCountSpecNodes(c);
  return n;
}

// 内容差异判定：文本/tooltip/身份四元组有变才算「改」（图标跟随类型走，
// 不单独计）。告色翻转在现有构建逻辑里总伴随 text1 翻转（未决关联文案），
// 文本判定即覆盖。
bool DataListPanel::navSpecDiffers(const QTreeWidgetItem *it, const NavNodeSpec &spec)
{
  return it->text(0) != spec.text0 || it->text(1) != spec.text1 ||
         it->toolTip(0) != spec.tooltip0 ||
         it->data(0, Qt::UserRole) != QVariant(spec.id) ||
         it->data(0, Qt::UserRole + 1) != QVariant(spec.eid) ||
         it->data(0, Qt::UserRole + 2) != QVariant(spec.type) ||
         it->data(0, Qt::UserRole + 3) != QVariant(spec.sub);
}

// 增量 reconcile：按身份键匹配现存子项与 spec 子项——匹配且内容有变 →
// 字段重放（改）；spec 无匹配 → 新建（增）；现存无 spec → 删除（删）；
// 最后按 spec 序归位（纯位移不计变更，选中由 m_selKeep/refreshAssetTable
// 侧统一还原）。apply=false 为纯 diff（计数）；apply=true 落地。
void DataListPanel::reconcileNavChildren(QTreeWidgetItem *parent,
                                         const QVector<NavNodeSpec> &specs,
                                         bool apply, int *changed)
{
  const int existingCount =
      parent ? parent->childCount() : m_tree->topLevelItemCount();
  // 现存子项按身份键索引（同键多发按序消费——survey 根/全景叶等重复键）。
  QHash<QString, QQueue<QTreeWidgetItem *>> byKey;
  for (int i = 0; i < existingCount; ++i)
  {
    QTreeWidgetItem *it = parent ? parent->child(i) : m_tree->topLevelItem(i);
    byKey[navItemKey(it)].enqueue(it);
  }

  QVector<QTreeWidgetItem *> target; // spec 序的目标排列
  QVector<QPair<QTreeWidgetItem *, bool>> expandNew; // 新建项的构建默认展开（插入后施加）
  for (const NavNodeSpec &spec : specs)
  {
    const QString key =
        navKeyPart(spec.type, spec.id, spec.eid, spec.sub, spec.text0);
    QTreeWidgetItem *it = nullptr;
    auto found = byKey.find(key);
    if (found != byKey.end() && !found->isEmpty())
      it = found->dequeue();
    if (!it)
    {
      if (!apply)
      {
        *changed += navCountSpecNodes(spec);
        target.append(nullptr);
        continue;
      }
      it = new QTreeWidgetItem; // 暂无父——归位段按序插入
      applyNavSpec(it, spec, false); // 展开延后到插入完成（见 expandNew）
      expandNew.append({it, spec.expanded});
      ++*changed;
    }
    else if (navSpecDiffers(it, spec))
    {
      if (apply)
        applyNavSpec(it, spec, false);
      ++*changed;
    }
    target.append(it);
    reconcileNavChildren(it, spec.children, apply, changed);
  }

  // 现存未消费项 → 删除（含子树；diff 档按节点数计）。
  for (auto kit = byKey.constBegin(); kit != byKey.constEnd(); ++kit)
  {
    for (QTreeWidgetItem *leftover : kit.value())
    {
      if (apply)
        delete leftover; // QTreeWidgetItem 析构自父摘除
      else
        *changed += countItemNodes(leftover);
    }
  }

  if (!apply)
    return;
  // 归位：i 递增时 0..i-1 已就位；先摘后插保证 want 落在 i。
  for (int i = 0; i < target.size(); ++i)
  {
    QTreeWidgetItem *want = target.at(i);
    QTreeWidgetItem *have =
        parent ? parent->child(i) : m_tree->topLevelItem(i);
    if (have == want)
      continue;
    if (parent)
    {
      parent->removeChild(want); // 非子节点时无害
      parent->insertChild(i, want);
    }
    else
    {
      const int at = m_tree->indexOfTopLevelItem(want);
      if (at >= 0)
        m_tree->takeTopLevelItem(at);
      m_tree->insertTopLevelItem(i, want);
    }
  }
  // 构建默认展开在插入完成后施加——无视图挂靠项的 setExpanded 不保证
  // 随插入带出展开态（等价面：renderNavChildren 在挂靠后施加，语义一致）。
  for (const auto &ne : expandNew)
    ne.first->setExpanded(ne.second);
}

// 增量通道入口：diff → 阈值内落地（真返回）；超阈值回 false（调用方走
// 全量兜底）。零变更也走增量通道（不 clear、不碰展开/滚动——最优路径）。
bool DataListPanel::reconcileNavTree(const QVector<NavNodeSpec> &specs)
{
  int total = 0;
  {
    QTreeWidgetItemIterator it(m_tree);
    while (*it)
    {
      ++total;
      ++it;
    }
  }
  int changed = 0;
  reconcileNavChildren(nullptr, specs, false, &changed);
  const int quota = qMax(kNavReconcileFloor, total / kNavReconcileDivisor);
  if (changed > quota)
    return false;
  reconcileNavChildren(nullptr, specs, true, &changed);
  m_lastTreeRefreshMode =
      changed > 0 ? TreeRefreshMode::Incremental : TreeRefreshMode::Unchanged;
  return true;
}

void DataListPanel::refreshAssetTree()
{
  if (!m_tree)
    return;
  const QVector<NavNodeSpec> specs = buildNavTreeSpec();
  if (reconcileNavTree(specs))
    return;
  rebuildNavTree(specs);
  m_lastTreeRefreshMode = TreeRefreshMode::FullRebuild;
}

void DataListPanel::setTreeSort(paleo::dataops::TreeSortKind kind)
{
  m_treeSort = kind;
  paleo::dataops::rememberTreeSort(kind);
  refreshAssetTable();
}

void DataListPanel::openGroupConfig()
{
  using namespace paleo::dataops;
  if (!m_groupTree)
    return;
  QStringList dims = {tr("按类型"), tr("按实体"), tr("按标签"), tr("按版本")};
  bool ok = false;
  const QString chosen = QInputDialog::getItem(
      this, tr("分组维度"), tr("按什么分组:"), dims, m_groupMode, false, &ok);
  if (!ok)
    return;
  m_groupMode = dims.indexOf(chosen);
  if (m_groupMode < 0)
    m_groupMode = 0;
  m_groupTree->setGroupBy(AssetGroupTree::GroupBy(m_groupMode));
  setViewMode(4);
  // 重灌当前可见行。
  QSet<QString> visible;
  for (const AssetRowInfo &r : m_rows)
    if (!r.removed)
      visible.insert(r.assetId);
  QVector<AssetRowInfo> shown;
  for (const AssetRowInfo &r : m_rows)
    if (visible.contains(r.assetId))
      shown.append(r);
  m_groupTree->loadRows(m_rows.size() > 200 ? m_pageRows : shown);
}

QTreeWidgetItem *DataListPanel::treeItemForAsset(const QString &assetId) const
{
  if (!m_tree)
    return nullptr;
  QTreeWidgetItemIterator it(m_tree);
  while (*it)
  {
    if ((*it)->data(0, Qt::UserRole).toString() == assetId)
      return *it;
    ++it;
  }
  return nullptr;
}

void DataListPanel::applyFilterToTree(const QSet<QString> &visibleIds, bool filtering)
{
  if (!m_tree)
    return;
  const QString needle = [this]() {
    const auto *search = findChild<QLineEdit *>(QStringLiteral("assetSearchEdit"));
    return search ? search->text().trimmed() : QString();
  }();
  // 递归判定：分类/标签组随子命中；资产叶按可见 id 集；测区节点按文本；
  // 井/标签叶按自身文本或子命中。
  std::function<bool(QTreeWidgetItem *)> visit = [&](QTreeWidgetItem *node) -> bool {
    const QString nodeType = node->data(0, Qt::UserRole + 2).toString();
    const QString id = node->data(0, Qt::UserRole).toString();
    const bool selfMatch =
        needle.isEmpty() || node->text(0).contains(needle, Qt::CaseInsensitive) ||
        node->text(1).contains(needle, Qt::CaseInsensitive);
    if (nodeType == QLatin1String("category") || nodeType == QLatin1String("tag_group"))
    {
      bool any = false;
      for (int k = 0; k < node->childCount(); ++k)
        if (visit(node->child(k)))
          any = true;
      node->setHidden(!any);
      return any;
    }
    if (!id.isEmpty() && nodeType != QLatin1String("well"))
    {
      // 资产叶 / 测线 / 测区：资产在可见集；测区/测线随文本。
      const bool ok = selfMatch &&
                      (nodeType == QLatin1String("survey_area") ||
                       visibleIds.contains(id));
      node->setHidden(!ok);
      return ok;
    }
    // 井节点 / 标签叶 / 其它无 id 结构节点。
    bool any = selfMatch;
    for (int k = 0; k < node->childCount(); ++k)
      if (visit(node->child(k)))
        any = true;
    node->setHidden(!any);
    return any;
  };
  for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
  {
    QTreeWidgetItem *cat = m_tree->topLevelItem(i);
    const bool catVisible = visit(cat);
    cat->setHidden(!catVisible);
    if (filtering && catVisible)
      cat->setExpanded(true);
    else if (!filtering && m_treeWasFiltering)
    {
      // 默认收拢（用户契约）只在「清空搜索退出过滤态」那一跳执行——选中联动/
      // 翻页等无过滤重放（applyListFilter 的良性再入）不得收掉用户手动展开
      // 的一级组，否则点击三级节点会把树跳回一级并收拢。
      cat->setExpanded(false);
    }
  }
  m_treeWasFiltering = filtering;
}
