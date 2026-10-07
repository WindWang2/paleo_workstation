// 层：视图
#include "datalist.h"
#include "datalistops.h"
#include "../paleotheme.h"
#include "../paleoicons.h"
#include "../../catalog/datacatalog.h"
#include "../../catalog/realizationset.h"
#include "../../services/previewdoc.h"
#include "datanavtree.h"
#include "dataops/dataopsmodel.h"
#include "dataopsviews.h"

#include <algorithm>
#include <functional>
#include <QInputDialog>
#include <QLineEdit>
#include <QScrollBar>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>

using namespace paleo::pagesinternal;

namespace {

// 展开态快照键：沿父链拼路径（根→本节点），单节 = 类型|assetId|entityId|sub，
// 无 id 的分类组补去计数尾巴的文本（"测井 (20 井)" → "测井"）。父链区分
// 跨井同名分支（如各井的「岩心照片」组）——重建后按键找回同一节点，
// catalog.changed 触发的全量刷新不抹掉用户的展开，也不串井。
QString navExpandedKey(const QTreeWidgetItem *it)
{
  QStringList parts;
  for (const QTreeWidgetItem *p = it; p; p = p->parent())
  {
    const QString id = p->data(0, Qt::UserRole).toString();
    const QString eid = p->data(0, Qt::UserRole + 1).toString();
    const QString type = p->data(0, Qt::UserRole + 2).toString();
    const QString sub = p->data(0, Qt::UserRole + 3).toString();
    QString part = type + QLatin1Char('|') + id + QLatin1Char('|') + eid +
                   QLatin1Char('|') + sub;
    if (id.isEmpty() && eid.isEmpty())
      part += QLatin1Char('|') + p->text(0).section(QStringLiteral(" ("), 0, 0);
    parts.prepend(part);
  }
  return parts.join(QStringLiteral("\x1f"));
}

} // namespace

void DataListPanel::refreshAssetTree()
{
  if (!m_tree)
    return;
  // 快照当前展开态与滚动位，函数尾还原——refreshAssetTable/catalog.changed
  // 引起的重建不得把树跳回一级收拢态。
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
  PreviewDocService *svc = m_doc;
  if (!svc)
    return;
  DataCatalog *cat = svc->catalog();
  if (!cat)
    return;

  // 0. 测区 (Survey Area) — 首项显示，双击打开测区全景地图 (QGIS 画布)
  auto *surveyRoot = new QTreeWidgetItem(m_tree);
  surveyRoot->setText(0, tr("测区"));
  surveyRoot->setText(1, tr("工区全景"));
  surveyRoot->setData(0, Qt::UserRole, QStringLiteral("survey_area"));
  surveyRoot->setData(0, Qt::UserRole + 2, QStringLiteral("survey_area"));
  surveyRoot->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconPolygonLayer.svg")));
  surveyRoot->setExpanded(false);

  auto *surveyMapItem = new QTreeWidgetItem(surveyRoot);
  surveyMapItem->setText(0, tr("工区全景地图"));
  surveyMapItem->setText(1, tr("QGIS地图画布"));
  surveyMapItem->setData(0, Qt::UserRole, QStringLiteral("survey_area"));
  surveyMapItem->setData(0, Qt::UserRole + 2, QStringLiteral("survey_area"));
  surveyMapItem->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mActionZoomFullExtent.svg")));

  // 1. 井 (Wells)——显示名走实体改写表（D4.1），排序按 D2.5 记忆键。
  QVector<CatalogEntity> wells = cat->entities(QStringLiteral("well"));
  // 软删实体（撤销新建等）不显示。
  const auto wellsVisible = [this](const QVector<CatalogEntity> &in) {
    QVector<CatalogEntity> out;
    for (const CatalogEntity &e : in)
      if (!m_recycle.isRemoved(e.id))
        out.append(e);
    return out;
  };
  wells = wellsVisible(wells);
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
  auto *wellRoot = new QTreeWidgetItem(m_tree);
  wellRoot->setText(0, tr("测井 (%1 井)").arg(wells.size()));
  wellRoot->setText(1, tr("井位 / 测井曲线 / 分层 / 时深"));
  wellRoot->setData(0, Qt::UserRole + 2, QStringLiteral("category"));
  wellRoot->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconPointLayer.svg")));
  wellRoot->setExpanded(false);

  for (const CatalogEntity &w : wells)
  {
    auto *wellItem = new QTreeWidgetItem(wellRoot);
    wellItem->setText(0, m_entityOv.displayName(w));
    wellItem->setData(0, Qt::UserRole + 1, w.id);
    wellItem->setData(0, Qt::UserRole + 2, QStringLiteral("well"));
    wellItem->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconPointLayer.svg")));
    if (w.hasSurface)
    {
      wellItem->setText(1, QStringLiteral("X: %1, Y: %2").arg(QString::number(w.surfaceX, 'f', 1)).arg(QString::number(w.surfaceY, 'f', 1)));
      wellItem->setFont(1, PaleoTheme::monoFont()); // 坐标列走 mono 数字面（DESIGN.md）
    }

    // 获取该井所有关联资产并按角色序排：测井曲线 -> 井分层 -> 时深关系 -> 井身/井位
    const QVector<EntityAssetLink> wLinks = cat->linksForEntity(w.id);
    const auto roleOrder = [](const QString &r) {
      if (r == QLatin1String("well_log")) return 0;
      if (r == QLatin1String("tops")) return 1;
      if (r == QLatin1String("time_depth")) return 2;
      if (r == QLatin1String("well_head")) return 3;
      return 4;
    };
    QVector<EntityAssetLink> sortedLinks = wLinks;
    std::stable_sort(sortedLinks.begin(), sortedLinks.end(),
                     [roleOrder](const EntityAssetLink &a, const EntityAssetLink &b) {
                       const int ra = roleOrder(a.role);
                       const int rb = roleOrder(b.role);
                       if (ra != rb)
                         return ra < rb;
                       if (a.ordinal != b.ordinal)
                         return a.ordinal < b.ordinal;
                       return false;
                     });

    QVector<EntityAssetLink> coreLinks; // core 角色链接——收尾建分组分支用
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
      auto *sub = new QTreeWidgetItem(wellItem);
      sub->setData(0, Qt::UserRole, a.id);
      sub->setData(0, Qt::UserRole + 1, w.id);
      sub->setData(0, Qt::UserRole + 2, l.role);

      QString roleDisplay = l.role;
      QString detailDisplay;
      if (l.role == QLatin1String("well_log"))
      {
        roleDisplay = tr("测井曲线");
        detailDisplay = a.displayName;
        sub->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconLineLayer.svg")));
      }
      else if (l.role == QLatin1String("tops"))
      {
        roleDisplay = tr("井分层");
        detailDisplay = a.displayName;
        sub->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mActionOpenTable.svg")));
      }
      else if (l.role == QLatin1String("time_depth"))
      {
        roleDisplay = tr("时深关系");
        detailDisplay = a.displayName;
        sub->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mActionOpenTable.svg")));
      }
      else if (l.role == QLatin1String("well_head"))
      {
        roleDisplay = tr("井身/井位");
        detailDisplay = a.displayName;
        sub->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconPointLayer.svg")));
      }
      else
      {
        sub->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mActionOpenTable.svg")));
      }
      if (l.role == QLatin1String("well_log") && !l.unresolved)
      {
        const QString kind = l.isPrimary ? tr("主文件") : tr("成员");
        sub->setText(0, tr("%1 · %2 (%3)").arg(a.displayName, kind, roleDisplay));
        sub->setText(1, tr("%1 · %2").arg(kind, a.displayName));
      }
      else
      {
        sub->setText(0, QStringLiteral("%1 (%2)").arg(a.displayName, roleDisplay));
        sub->setText(1, detailDisplay);
      }
      if (l.unresolved)
      {
        PaleoTheme::setItemTextColor(sub, 0, PaleoTheme::ItemTextColor::Warning); // 待复核色（现取随主题）
        sub->setText(1, tr("未决关联"));
      }
    }

    // 「岩心照片 (N)」分组分支（L3，无 id 的 category 节点；点击只展开/收拢）。
    // 照片叶（L4）保留 assetId/wellId/core 角色三元——激活、拖放、过滤语义
    // 与原平铺叶完全一致。
    if (!coreLinks.isEmpty())
    {
      auto *branch = new QTreeWidgetItem(wellItem);
      branch->setData(0, Qt::UserRole + 2, QStringLiteral("category"));
      branch->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconRaster.svg")));
      int added = 0;
      for (const EntityAssetLink &l : coreLinks)
      {
        const CatalogAsset a = cat->assetById(l.assetId);
        if (a.id.isEmpty())
          continue;
        auto *leaf = new QTreeWidgetItem(branch);
        leaf->setData(0, Qt::UserRole, a.id);
        leaf->setData(0, Qt::UserRole + 1, w.id);
        leaf->setData(0, Qt::UserRole + 2, l.role);
        leaf->setText(0, a.displayName);
        leaf->setText(1, tr("岩心照片"));
        leaf->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconRaster.svg")));
        if (l.unresolved)
        {
          PaleoTheme::setItemTextColor(leaf, 0, PaleoTheme::ItemTextColor::Warning);
          leaf->setText(1, tr("未决关联"));
        }
        ++added;
      }
      if (added == 0)
        delete branch;
      else
        branch->setText(0, tr("岩心照片 (%1)").arg(added));
    }
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
    auto *compBranch = new QTreeWidgetItem(wellRoot);
    compBranch->setText(0, tr("综合柱状图 (%1)").arg(compositeAssets.size()));
    compBranch->setText(1, tr("多井道地质综合柱状图 (ResFormStar 规范)"));
    compBranch->setData(0, Qt::UserRole + 2, QStringLiteral("category"));
    compBranch->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconLineLayer.svg")));
    compBranch->setExpanded(true);

    for (const CatalogAsset &a : compositeAssets)
    {
      auto *it = new QTreeWidgetItem(compBranch);
      it->setText(0, a.displayName);
      it->setText(1, tr("多井道地质综合柱状图")); // 道数/曲线数未解析，不臆造
      it->setData(0, Qt::UserRole, a.id);
      it->setData(0, Qt::UserRole + 2, QStringLiteral("composite_log"));
      it->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconLineLayer.svg")));
    }
  }

  if (!logAssets.isEmpty())
  {
    auto *curveBranch = new QTreeWidgetItem(wellRoot);
    curveBranch->setText(0, tr("未关联曲线 (%1)").arg(logAssets.size()));
    curveBranch->setText(1, tr("无已决井链接的 LAS（拖到井节点挂接）"));
    curveBranch->setData(0, Qt::UserRole + 2, QStringLiteral("category"));
    curveBranch->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconLineLayer.svg")));
    curveBranch->setExpanded(true);

    for (const CatalogAsset &a : logAssets)
    {
      auto *it = new QTreeWidgetItem(curveBranch);
      it->setText(0, a.displayName);
      it->setText(1, tr("测井曲线 (GR/AC/DEN/电阻率等)"));
      it->setData(0, Qt::UserRole, a.id);
      it->setData(0, Qt::UserRole + 2, QStringLiteral("well_log"));
      it->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconLineLayer.svg")));
    }
  }

  // 3. 地震 (Seismic)
  QList<CatalogAsset> seisAssets;
  for (const CatalogAsset &a : cat->assets())
    if (assetVisible(a) && a.type == QLatin1String("seismic"))
      seisAssets.append(a);
  auto *seismicRoot = new QTreeWidgetItem(m_tree);
  seismicRoot->setText(0, tr("地震 (%1)").arg(seisAssets.size()));
  seismicRoot->setText(1, tr("三维地震体 / 层位解释"));
  seismicRoot->setData(0, Qt::UserRole + 2, QStringLiteral("category"));
  seismicRoot->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconPolygonLayer.svg")));
  seismicRoot->setExpanded(false);

  const QVector<CatalogEntity> surveys = cat->entities(QStringLiteral("seismic_survey"));
  CatalogEntity survey;
  if (!surveys.isEmpty())
    survey = surveys.front();

  for (const CatalogAsset &a : seisAssets)
  {
    auto *seisItem = new QTreeWidgetItem(seismicRoot);
    seisItem->setText(0, a.displayName);
    seisItem->setData(0, Qt::UserRole, a.id);
    seisItem->setData(0, Qt::UserRole + 2, QStringLiteral("seismic_volume"));
    seisItem->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconPolygonLayer.svg")));
    if (!survey.id.isEmpty() && survey.inlineMax > survey.inlineMin)
    {
      seisItem->setText(1, QStringLiteral("Inline %1–%2 · Xline %3–%4 · %5ms")
          .arg(int(survey.inlineMin)).arg(int(survey.inlineMax))
          .arg(int(survey.xlineMin)).arg(int(survey.xlineMax))
          .arg(survey.sampleIntervalUs / 1000.0, 0, 'f', 1));
      
      auto *inl = new QTreeWidgetItem(seisItem);
      inl->setText(0, tr("Inline 剖面 (主测线 %1–%2)").arg(int(survey.inlineMin)).arg(int(survey.inlineMax)));
      inl->setText(1, tr("双击预览剖面"));
      inl->setData(0, Qt::UserRole, a.id);
      inl->setData(0, Qt::UserRole + 2, QStringLiteral("seismic_line"));
      inl->setData(0, Qt::UserRole + 3, QStringLiteral("inline"));
      inl->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconLineLayer.svg")));

      auto *xl = new QTreeWidgetItem(seisItem);
      xl->setText(0, tr("Crossline 剖面 (联络线 %1–%2)").arg(int(survey.xlineMin)).arg(int(survey.xlineMax)));
      xl->setText(1, tr("双击预览剖面"));
      xl->setData(0, Qt::UserRole, a.id);
      xl->setData(0, Qt::UserRole + 2, QStringLiteral("seismic_line"));
      xl->setData(0, Qt::UserRole + 3, QStringLiteral("crossline"));
      xl->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconLineLayer.svg")));
      seisItem->setExpanded(true);
    }
    else
    {
      seisItem->setText(1, tr("SEG-Y 地震数据"));
    }
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
  auto *auxRoot = new QTreeWidgetItem(m_tree);
  auxRoot->setText(0, tr("辅助资料 (%1)").arg(auxAssets.size()));
  auxRoot->setText(1, tr("参考相图 / 文档 / 图片"));
  auxRoot->setData(0, Qt::UserRole + 2, QStringLiteral("category"));
  auxRoot->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mActionFolder.svg")));
  auxRoot->setExpanded(false);

  auto *faciesBranch = new QTreeWidgetItem(auxRoot);
  faciesBranch->setText(0, tr("参考相图"));
  faciesBranch->setData(0, Qt::UserRole + 2, QStringLiteral("category"));
  faciesBranch->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconPolygonLayer.svg")));
  faciesBranch->setExpanded(true);

  auto *docBranch = new QTreeWidgetItem(auxRoot);
  docBranch->setText(0, tr("参考资料"));
  docBranch->setData(0, Qt::UserRole + 2, QStringLiteral("category"));
  docBranch->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mActionFolder.svg")));
  docBranch->setExpanded(true);

  for (const CatalogAsset &a : auxAssets)
  {
    const bool isGeo = a.displayName.endsWith(QLatin1String(".geojson"), Qt::CaseInsensitive) || a.type == QLatin1String("boundary");
    QTreeWidgetItem *parent = isGeo ? faciesBranch : docBranch;
    auto *it = new QTreeWidgetItem(parent);
    it->setText(0, a.displayName);
    it->setData(0, Qt::UserRole, a.id);
    it->setData(0, Qt::UserRole + 2, QStringLiteral("auxiliary"));
    it->setIcon(0, PaleoIcons::qgisTheme(isGeo ? QStringLiteral("mIconPolygonLayer.svg") : QStringLiteral("mActionOpenTable.svg")));
    if (isGeo)
      it->setText(1, tr("GeoJSON 矢量相图"));
    else if (a.displayName.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive))
      it->setText(1, tr("PDF 文档"));
    else if (a.displayName.endsWith(QLatin1String(".pptx"), Qt::CaseInsensitive))
      it->setText(1, tr("PPT 演示文稿"));
    else if (a.displayName.endsWith(QLatin1String(".png"), Qt::CaseInsensitive))
      it->setText(1, tr("图像"));
    else if (a.displayName.endsWith(QLatin1String(".xml"), Qt::CaseInsensitive))
      it->setText(1, tr("XML 数据表"));
    else
      it->setText(1, a.type);
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
    auto *plannedRoot = new QTreeWidgetItem(m_tree);
    plannedRoot->setText(0, tr("计划井 (%1)").arg(plannedVisible.size()));
    plannedRoot->setText(1, tr("布井候选（不进实井计算）"));
    plannedRoot->setData(0, Qt::UserRole + 2, QStringLiteral("category"));
    plannedRoot->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconPointLayer.svg")));
    plannedRoot->setExpanded(false);
    for (const CatalogEntity &p : plannedVisible)
    {
      auto *item = new QTreeWidgetItem(plannedRoot);
      item->setText(0, m_entityOv.displayName(p));
      item->setData(0, Qt::UserRole + 1, p.id);
      item->setData(0, Qt::UserRole + 2, QStringLiteral("planned"));
      item->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconPointLayer.svg")));
      if (p.hasSurface)
      {
        item->setText(1, QStringLiteral("X: %1, Y: %2")
                                 .arg(QString::number(p.surfaceX, 'f', 1))
                                 .arg(QString::number(p.surfaceY, 'f', 1)));
        item->setFont(1, PaleoTheme::monoFont());
      }
    }
  }

  // 地震解释层位归入地震分类，资产仍保持 horizon 类型与层序界面关联。
  QList<CatalogAsset> horAssets;
  for (const CatalogAsset &a : cat->assets())
    if (assetVisible(a) && a.type == QLatin1String("horizon"))
      horAssets.append(a);
  std::sort(horAssets.begin(), horAssets.end(), [](const CatalogAsset &a, const CatalogAsset &b) {
    return naturalNameSort(a.displayName, b.displayName);
  });
  auto *horRoot = new QTreeWidgetItem(seismicRoot);
  horRoot->setText(0, tr("层位 (%1)").arg(horAssets.size()));
  horRoot->setText(1, tr("解释层位数据"));
  horRoot->setData(0, Qt::UserRole + 2, QStringLiteral("category"));
  horRoot->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mActionOpenTable.svg")));
  horRoot->setExpanded(false);

  for (const CatalogAsset &a : horAssets)
  {
    auto *hItem = new QTreeWidgetItem(horRoot);
    hItem->setText(0, a.displayName);
    hItem->setData(0, Qt::UserRole, a.id);
    hItem->setData(0, Qt::UserRole + 2, QStringLiteral("horizon"));
    hItem->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mActionOpenTable.svg")));
    // 网格规格不在 catalog 里（按文件名臆造 411×641 已回收）——如实标类型。
    hItem->setText(1, tr("层位网格"));
  }

  // 6. 不确定性集合（方向 47）：realization_set 资产 → 父集合→成员树，
  //    成员 index 升序 + 缺号位如实列「缺席」；统计面挂为集合子节点
  //   （成员叶激活 = realizationMemberRequested，统计叶 = realizationStatRequested）。
  {
    const auto sets = paleo::realization::enumerateSets(*cat);
    if (!sets.isEmpty())
    {
      auto *setRoot = new QTreeWidgetItem(m_tree);
      setRoot->setText(0, tr("不确定性集合 (%1)").arg(sets.size()));
      setRoot->setText(1, tr("realization 集合 · 成员等概率实现"));
      setRoot->setData(0, Qt::UserRole + 2, QStringLiteral("category"));
      setRoot->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconRaster.svg")));
      setRoot->setExpanded(true);
      for (const auto &set : sets)
      {
        auto *setItem = new QTreeWidgetItem(setRoot);
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
        setItem->setText(0, setText);
        setItem->setText(1, set.members.size() == 1 ? tr("单成员 · 无不确定性")
                                                  : tr("realization 集合"));
        setItem->setData(0, Qt::UserRole, set.setId); // 集合资产 id
        setItem->setData(0, Qt::UserRole + 2, QStringLiteral("realization_set"));
        setItem->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconRaster.svg")));
        setItem->setExpanded(true);
        // 成员叶按 declaredCount 全列——缺号位展示为缺席（禁选）。
        for (int idx = 0; idx < set.declaredCount; ++idx)
        {
          const QString vid = paleo::realization::memberVersionId(set, idx);
          auto *leaf = new QTreeWidgetItem(setItem);
          leaf->setText(0, vid.isEmpty() ? tr("成员 #%1（缺席）").arg(idx)
                                       : tr("成员 #%1").arg(idx));
          leaf->setData(0, Qt::UserRole + 2, QStringLiteral("realization_member"));
          leaf->setData(0, Qt::UserRole + 3, set.setId);
          leaf->setData(0, Qt::UserRole + 4, idx);
          leaf->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconRaster.svg")));
          if (vid.isEmpty())
          {
            leaf->setFlags(leaf->flags() & ~Qt::ItemIsEnabled);
            leaf->setText(1, tr("成员栅格缺席——集合不完整"));
          }
          else
            leaf->setText(1, tr("成员版本 %1").arg(vid.left(8)));
        }
        // 统计面子节点（token → 口径词与图签同源）。
        for (const auto &s : paleo::realization::statSurfaces(*cat, set.setId))
        {
          auto *leaf = new QTreeWidgetItem(setItem);
          const QString label = paleo::realization::statisticDisplayLabel(s.token);
          leaf->setText(0, tr("%1 · %2 成员").arg(label.isEmpty() ? s.token : label).arg(s.memberCount));
          leaf->setData(0, Qt::UserRole + 2, QStringLiteral("realization_stat"));
          leaf->setData(0, Qt::UserRole + 3, set.setId);
          leaf->setData(0, Qt::UserRole + 4, s.token);
          leaf->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconRaster.svg")));
          leaf->setText(1, tr("统计面 %1").arg(s.token));
        }
      }
    }
  }

  // 7. 标签分组（D2.4 组织面 + D3.5 拖放目标）：每个标签一个叶节点，
  //    拖资产到标签节点 = 打标签；点击标签 = 过滤。
  const auto tagCloud = m_tags.tagCloud();
  if (!tagCloud.isEmpty())
  {
    auto *tagRoot = new QTreeWidgetItem(m_tree);
    tagRoot->setText(0, tr("标签 (%1)").arg(tagCloud.size()));
    tagRoot->setText(1, tr("点击过滤 / 拖资产来打标签"));
    tagRoot->setData(0, Qt::UserRole + 2, QStringLiteral("tag_group"));
    tagRoot->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mActionFolder.svg")));
    tagRoot->setExpanded(false);
    for (const auto &tc : tagCloud)
    {
      auto *leaf = new QTreeWidgetItem(tagRoot);
      leaf->setText(0, QStringLiteral("%1 ×%2").arg(tc.first).arg(tc.second));
      leaf->setData(0, Qt::UserRole + 2, QStringLiteral("tag_leaf"));
      leaf->setData(0, Qt::UserRole + 3, tc.first);
      leaf->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mActionOpenTable.svg")));
      // 点击标签节点 = 按标签过滤（与标签云同语义）。
    }
  }

  // 还原展开态与滚动位（快照见函数头）——无匹配键的项保持构建默认态。
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
