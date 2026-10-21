// 层：功能
#include "projectareasummary.h"
#include "../catalog/datacatalog.h"
#include "../domain/mappinghorizons.h"
#include "../qgis/paleomapsource.h"
#include "../domain/arearules.h"
#include <QFileInfo>
#include <QSet>
#include <cmath>
namespace ProjectAreaSummary {
QVariantMap snapshot(const DataCatalog *cat) {
  if (!cat || !cat->isOpen()) return {};
  const auto wells = cat->entities("well"); int positioned = 0; double minX = 0, minY = 0, maxX = 0, maxY = 0;
  for (const auto &w : wells) if (w.hasSurface && std::isfinite(w.surfaceX) && std::isfinite(w.surfaceY)) {
    if (!positioned++) { minX = maxX = w.surfaceX; minY = maxY = w.surfaceY; }
    minX = qMin(minX, w.surfaceX); maxX = qMax(maxX, w.surfaceX); minY = qMin(minY, w.surfaceY); maxY = qMax(maxY, w.surfaceY);
  }
  const auto surveys = cat->entities("seismic_survey"); QVector<QVector<QPair<double, double>>> polygons;
  for (const auto &s : surveys) if (s.corners.size() >= 3) polygons << s.corners;
  QSet<QString> seen; QMap<QString, QSet<QString>> stages; QMap<QString, int> maps; int mapCount = 0, versions = 0, outputCount = 0;
  for (const auto &a : cat->assets()) {
    const auto current = cat->currentVersion(a.id);
    if (a.type == "paleogeographic_map") { ++mapCount; ++maps[current.extra.value("horizon").toString()]; }
    for (const auto &v : cat->versionsForAsset(a.id)) {
      ++versions; const auto h = v.extra.value("horizon").toString(), kind = v.extra.value("kind").toString();
      if (!h.isEmpty()) seen.insert(h.toUpper());
      if (kind == "wells_prediction") stages[h].insert("well");
      if (kind == "seismic_prediction") stages[h].insert("seismic");
      if (kind == "single_factor_raster" || kind == "single_factor_cartographic_work") stages[h].insert("factor");
      if (kind == "composed_facies" || kind == "edited_facies" || kind == "facies_polygons") stages[h].insert("map");
      if (v.stage == "OUTPUT") {
        ++outputCount;
        const auto exports = v.extra.value("map_exports").toList();
        if (!h.isEmpty()) stages[h].insert("output");
        for (const auto &e : exports) stages[e.toMap().value("context").toMap().value("horizon").toString()].insert("output");
      }
    }
  }
  // 分层位进度只列「本工程里出现过」的层位：工区内置默认名单（竞赛数据集
  // C3…D72）不是通用层位，空工程不应显示。证据 = 已登记层序界面实体、任一
  // 成果版本/出图引用的层位；工程目录自带 project_area.json 显式配置名单时全列。
  for (const auto &sb : cat->entities("sequence_boundary"))
    if (!sb.extra.value("pending").toBool()) seen.insert(sb.name.toUpper());
  for (auto it = stages.cbegin(); it != stages.cend(); ++it) if (!it.value().isEmpty()) seen.insert(it.key().toUpper());
  for (auto it = maps.cbegin(); it != maps.cend(); ++it) seen.insert(it.key().toUpper());
  const auto dir = AreaRules::activeProjectDir();
  const bool configured = !dir.isEmpty() && QFileInfo::exists(AreaRules::configFilePath(dir));
  QVariantList horizons; QVariantMap counts;
  for (const auto &stage : {"well", "seismic", "factor", "map", "output"}) counts.insert(stage, 0);
  for (const auto &h : mappingHorizons()) {
    if (!configured && !seen.contains(h.toUpper())) continue;
    QVariantMap row{{"horizon", h}, {"existing_maps", maps.value(h)}};
    for (const auto &stage : {"well", "seismic", "factor", "map", "output"}) {
      const bool present = stages[h].contains(stage); row.insert(stage, present);
      counts.insert(stage, counts.value(stage).toInt() + int(present));
    }
    horizons << row;
  }
  return {{"wells", wells.size()}, {"positioned_wells", positioned}, {"seismic_surveys", surveys.size()},
      {"assets", cat->assets().size()}, {"versions", versions}, {"paleomaps", mapCount}, {"outputs", outputCount},
      {"survey_area_km2", PaleoMapSource::surveyArea(polygons)},
      // C-11（full-audit-20261020）：井位坐标同样无 CRS 语境——度制（|x|≤360
      // 且 |y|≤90）下 /1e6 是伪面积，如实给 -1（与 surveyArea 同口径）。
      {"well_envelope_km2", positioned > 1
          ? ((std::abs(minX) <= 360. && std::abs(maxX) <= 360. &&
              std::abs(minY) <= 90. && std::abs(maxY) <= 90.)
                 ? -1.
                 : (maxX - minX) * (maxY - minY) / 1000000.)
          : 0.},
      {"horizons", horizons}, {"stage_counts", counts}};
}
}
