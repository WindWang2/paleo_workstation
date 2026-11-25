// 层：功能
#include "mappingworkbench.h"
#include "../domain/singlefactorrequest.h"
#include "../services/singlefactordef.h"
#include "../qgis/faciesmapboundary.h"
#include "../qgis/qgislayerservice.h"

namespace {
bool isStudyBoundary(const LayerDeclaration &layer) {
  return layer.type == "vector" && (layer.layerId == "survey.area" || layer.layerId.startsWith("boundary."));
}
}

QVariantList MappingWorkbench::mapBoundaries() const {
  QVariantList result;
  if (!m_layers) return result;
  for (const auto &decl : m_layers->declared())
    if (isStudyBoundary(decl))
      result << QVariantMap{{"id", decl.layerId}, {"name", decl.title}};
  return result;
}

QVariantMap MappingWorkbench::prepareMapBoundary(const QString &id, QString *error) const {
  if (!m_layers || !isStudyBoundary(m_layers->declaration(id))) {
    if (error) *error = tr("所选工区边界已失效。");
    return {};
  }
  auto *layer = m_layers->instantiate(id, error);
  if (!layer) return {};
  auto boundary = FaciesMapBoundary::inspect(layer, error);
  if (!boundary.isEmpty()) boundary.insert("layer", id);
  return boundary;
}

bool MappingWorkbench::seedMapBoundary(const QString &draft, const QVariantMap &boundary, int code, QString *error) {
  return FaciesMapBoundary::seed(m_layers ? m_layers->layer(draft) : nullptr, boundary, code, error);
}

QVariantList MappingWorkbench::baseMapCandidates(const QString &horizon) const {
  QVariantList out;
  for (const auto &value : products(horizon)) {
    auto row = value.toMap(); if (row.value("horizon").toString() != horizon) continue;
    const auto metadata = row.value("parameters").toMap(); const auto kind = row.value("kind").toString();
    QString category, title, action;
    if (kind == "existing_paleomap" && metadata.value("spatial").toBool()) { category = "existing"; title = tr("现有古地理图"); }
    else if (paleo::singlefactor::isAnalysisFactorRaster(kind, metadata.value("value_source").toString())) {
      const auto factor = metadata.value("factor_id").toString(); category = "factor:" + factor;
      title = tr("单因素／%1").arg(SingleFactorRegistry::titleFor(factor)); action = "compose";
    } else if (kind == "seismic_prediction") { category = "seismic"; title = tr("地震相图"); }
    else if (QStringList{"facies_polygons", "composed_facies", "edited_facies"}.contains(kind)) { category = "facies"; title = tr("其他相图"); }
    else continue;
    if (action.isEmpty() && !metadata.value("facies").toList().isEmpty()) action = row.value("type") == "raster" ? "polygonize" : "copy";
    row.insert("category", category); row.insert("category_title", title); row.insert("start_action", action);
    row.insert("reference_only", action.isEmpty()); out << row;
  }
  return out;
}
