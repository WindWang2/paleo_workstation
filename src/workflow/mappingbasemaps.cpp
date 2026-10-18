// 层：功能
#include "mappingworkbench.h"
#include "../domain/singlefactorrequest.h"
#include "../services/singlefactordef.h"
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
    else if (QStringList{"facies_polygons", "composed_facies", "edited_facies", "wells_prediction"}.contains(kind)) { category = "facies"; title = tr("其他相图"); }
    else continue;
    if (action.isEmpty() && !metadata.value("facies").toList().isEmpty()) action = row.value("type") == "raster" ? "polygonize" : "copy";
    row.insert("category", category); row.insert("category_title", title); row.insert("start_action", action);
    row.insert("reference_only", action.isEmpty()); out << row;
  }
  return out;
}
