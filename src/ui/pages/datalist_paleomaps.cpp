// 层：视图
#include "datalist_navspec_internal.h"
#include "../../services/previewdoc.h"
#include "../../catalog/datacatalog.h"
#include "../../domain/mappinghorizons.h"
QVector<DataListPanel::NavNodeSpec> DataListPanel::buildPaleomapNavSpec() const {
  if (!m_doc || !m_doc->catalog()) return {};
  auto *cat = m_doc->catalog(); NavNodeSpec root;
  root.text0 = tr("现有古地理图"); root.type = "category"; root.sub = "paleomaps"; root.icon = "mIconPolygonLayer.svg";
  QMap<QString, QVector<NavNodeSpec>> byHorizon; int count = 0;
  for (const auto &asset : cat->assets()) {
    if (asset.type != "paleogeographic_map" || m_recycle.isRemoved(asset.id)) continue;
    const auto v = cat->currentVersion(asset.id); const auto h = v.extra.value("horizon").toString();
    NavNodeSpec leaf; leaf.id = asset.id; leaf.type = "paleomap"; leaf.text0 = asset.displayName;
    leaf.text1 = tr("v%1 · %2 个配套文件").arg(v.versionNumber).arg(qMax(1, v.extra.value("bundle_members").toList().size()));
    leaf.tooltip0 = tr("层位：%1\n%2").arg(h, v.path); leaf.icon = "mIconPolygonLayer.svg";
    byHorizon[h].append(leaf); ++count;
  }
  auto horizons = mappingHorizons(); for (auto it = byHorizon.begin(); it != byHorizon.end(); ++it) if (!horizons.contains(it.key())) horizons << it.key();
  for (const auto &h : horizons) {
    if (!byHorizon.contains(h)) continue;
    NavNodeSpec group; group.type = "category"; group.sub = "paleomap_horizon_" + h;
    group.text0 = h.isEmpty() ? tr("未指定层位") : h; group.text1 = tr("%1 项").arg(byHorizon[h].size());
    group.icon = "mActionFolder.svg"; group.children = byHorizon[h]; root.children << group;
  }
  root.text1 = tr("%1 项 · 按层位管理").arg(count);
  NavNodeSpec output; output.type = "category"; output.sub = "project_map_outputs"; output.text0 = tr("工区图件输出"); output.icon = "mActionSaveMapAsImage.svg";
  QMap<QString, QVector<NavNodeSpec>> figures;
  for (const auto &asset : cat->assets()) {
    if (m_recycle.isRemoved(asset.id)) continue;
    const auto version = cat->currentVersion(asset.id);
    if (version.stage != "OUTPUT" || version.extra.value("map_exports").toList().isEmpty()) continue;
    const auto h = version.extra.value("horizon").toString(); NavNodeSpec leaf;
    leaf.id = asset.id; leaf.type = "map_output"; leaf.text0 = asset.displayName; leaf.text1 = tr("%1 · v%2").arg(asset.format.toUpper()).arg(version.versionNumber);
    leaf.icon = "mActionSaveMapAsImage.svg"; leaf.tooltip0 = version.path; figures[h] << leaf;
  }
  for (auto it = figures.begin(); it != figures.end(); ++it) { NavNodeSpec group; group.type = "category"; group.sub = "outputs_" + it.key();
    group.text0 = it.key().isEmpty() ? tr("未指定层位") : it.key(); group.children = it.value(); output.children << group;
  }
  return {root, output};
}

DataListPanel::NavNodeSpec DataListPanel::buildOverviewNavSpec() const
{
  NavNodeSpec root;
  root.text0 = tr("工区概况"); root.text1 = tr("看板 / 工区地图");
  root.type = QStringLiteral("category"); root.sub = QStringLiteral("project_overview");
  root.icon = QStringLiteral("mIconPolygonLayer.svg"); root.expanded = true;
  NavNodeSpec dashboard;
  dashboard.text0 = tr("工区看板"); dashboard.text1 = tr("统计与项目进度");
  dashboard.id = QStringLiteral("project_dashboard"); dashboard.type = QStringLiteral("survey_area");
  dashboard.icon = QStringLiteral("mLayoutItemChart.svg");
  root.children.append(dashboard);
  NavNodeSpec map;
  map.text0 = tr("工区全景地图"); map.text1 = tr("QGIS 地图画布");
  map.id = QStringLiteral("survey_area"); map.type = QStringLiteral("survey_area");
  map.icon = QStringLiteral("mActionZoomFullExtent.svg"); root.children.append(map);
  return root;
}
