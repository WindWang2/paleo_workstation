// 层：QGIS 封装
#include "qgisstyleservice.h"
#include <qgscategorizedsymbolrenderer.h>
#include <qgspalettedrasterrenderer.h>
#include <qgsrasterlayer.h>
#include <qgssymbol.h>
#include <qgsvectorlayer.h>

QVariantList QgisStyleService::seismicClusterLegend(const QVariantList &clusterIds) {
  QVariantList rows;
  for (const QVariant &value : clusterIds) {
    const int id = value.toInt();
    rows << QVariantMap{{"cluster_id", id}, {"name", tr("簇 %1").arg(id)},
                        {"color", QColor::fromHsv((id * 137) % 360, 156, 211).name()}};
  }
  return rows;
}

void QgisStyleService::applySeismicClusterStyle(QgsMapLayer *layer, const QVariantList &clusterIds) {
  auto *raster = qobject_cast<QgsRasterLayer *>(layer);
  auto *patches = qobject_cast<QgsVectorLayer *>(layer);
  QgsPalettedRasterRenderer::ClassData palette;
  QgsCategoryList categories;
  for (const auto &entry : seismicClusterLegend(clusterIds)) {
    const auto row = entry.toMap();
    const int id = row.value("cluster_id").toInt();
    const QColor color(row.value("color").toString());
    const QString label = row.value("name").toString();
    if (raster) palette.append(QgsPalettedRasterRenderer::Class(id, color, label));
    if (patches) {
      auto *symbol = QgsSymbol::defaultSymbol(patches->geometryType());
      if (!symbol) continue;
      symbol->setColor(color);
      categories.append(QgsRendererCategory(id, symbol, label));
    }
  }
  if (raster) raster->setRenderer(new QgsPalettedRasterRenderer(raster->dataProvider(), 1, palette));
  if (patches) {
    patches->setReadOnly(true);
    patches->setRenderer(new QgsCategorizedSymbolRenderer(QStringLiteral("cluster_id"), categories));
  }
  layer->triggerRepaint();
}
