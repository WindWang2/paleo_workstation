// 层：QGIS 封装
#include "facieshierarchyrenderer.h"
#include "../domain/facieshierarchy.h"
#include "mappingartifactwriter.h"
#include <QUndoStack>
#include <qgscategorizedsymbolrenderer.h>
#include <qgsexception.h>
#include <qgsexpression.h>
#include <qgsgeometrycollection.h>
#include <qgsmergedfeaturerenderer.h>
#include <qgspalettedrasterrenderer.h>
#include <qgsrasterlayer.h>
#include <qgssymbol.h>
#include <qgsvectorlayer.h>
namespace FaciesHierarchyRenderer {
QVariantList codes(QgsMapLayer *layer) {
  QVariantList result;
  if (auto *v = qobject_cast<QgsVectorLayer *>(layer)) {
    auto it = v->getFeatures(
        QgsFeatureRequest()
            .setFlags(Qgis::FeatureRequestFlag::NoGeometry)
            .setSubsetOfAttributes(QStringList{"facies_code"}, v->fields()));
    QgsFeature f;
    while (it.nextFeature(f))
      result << f.attribute("facies_code");
  } else if (auto *r = qobject_cast<QgsRasterLayer *>(layer))
    for (const auto &c :
         QgsPalettedRasterRenderer::classDataFromRaster(r->dataProvider(), 1))
      result << c.value;
  return result;
}
QVariantList legend(QgsMapLayer *layer, const QVariantList &schema,
                    const QString &level) {
  auto entries = FaciesHierarchy::legend(schema, level, codes(layer));
  if (auto *v = qobject_cast<QgsVectorLayer *>(layer);
      v && v->geometryType() == Qgis::GeometryType::Point)
    for (auto &entry : entries) {
      auto row = entry.toMap();
      row.insert("color", "#24303E");
      row.insert("texture", QString());
      entry = row;
    }
  return entries;
}
QVariantList apply(QgsMapLayer *layer, const QVariantList &schema,
                   const QString &level) {
  MappingArtifactWriter::applyFaciesStyle(layer, schema);
  const auto entries = legend(layer, schema, level);
  if (auto *v = qobject_cast<QgsVectorLayer *>(layer)) {
    auto *base = dynamic_cast<QgsCategorizedSymbolRenderer *>(v->renderer());
    if (!base)
      return entries;
    QgsCategoryList categories;
    QString keyExpression = "CASE ", nameExpression = "CASE ";
    bool hasCodes = false;
    for (const auto &entry : entries) {
      const auto row = entry.toMap();
      QgsSymbol *symbol = nullptr;
      for (const auto &c : base->categories())
        if (c.value().toString() == row.value("code").toString()) {
          symbol = c.symbol()->clone();
          break;
        }
      if (!symbol)
        symbol = QgsSymbol::defaultSymbol(v->geometryType());
      symbol->setColor(QColor(row.value("color").toString()));
      categories << QgsRendererCategory(row.value("key"), symbol,
                                        row.value("name").toString());
      for (const auto &code : row.value("codes").toList()) {
        const auto when = QStringLiteral("WHEN \"facies_code\" = %1 THEN ")
                              .arg(QgsExpression::quotedValue(code));
        keyExpression +=
            when + QgsExpression::quotedValue(row.value("key")) + " ";
        nameExpression +=
            when + QgsExpression::quotedValue(row.value("name")) + " ";
        hasCodes = true;
      }
    }
    keyExpression += "ELSE 'unknown' END";
    nameExpression +=
        "ELSE " + QgsExpression::quotedString(QObject::tr("其他 / 未分类")) +
        " END";
    if (!hasCodes) {
      keyExpression = "'unknown'";
      nameExpression =
          QgsExpression::quotedString(QObject::tr("其他 / 未分类"));
    }
    auto *renderer =
        new QgsCategorizedSymbolRenderer(keyExpression, categories);
    v->setRenderer(v->geometryType() == Qgis::GeometryType::Polygon
                       ? static_cast<QgsFeatureRenderer *>(
                             new QgsMergedFeatureRenderer(renderer))
                       : renderer);
    v->setCustomProperty("paleo/faciesDisplayExpression", nameExpression);
    MappingArtifactWriter::applyFaciesLabels(
        v, v->customProperty("paleo/faciesLabelMode", 3).toInt());
  } else if (auto *r = qobject_cast<QgsRasterLayer *>(layer)) {
    auto *renderer = dynamic_cast<QgsPalettedRasterRenderer *>(r->renderer());
    if (renderer) {
      auto classes = renderer->classes();
      for (auto &c : classes)
        for (const auto &entry : entries) {
          const auto row = entry.toMap();
          for (const auto &code : row.value("codes").toList())
            if (code.toInt() == int(c.value)) {
              c.color = QColor(row.value("color").toString());
              c.label = row.value("name").toString();
            }
        }
      r->setRenderer(
          new QgsPalettedRasterRenderer(r->dataProvider(), 1, classes));
    }
  }
  layer->setCustomProperty("paleo/faciesResolvedLevel", level);
  layer->setCustomProperty("paleo/faciesDisplayLegend", entries);
  layer->triggerRepaint();
  return entries;
}
void watchUndo(QgsVectorLayer *layer, QObject *context,
               const std::function<void()> &callback) {
  QObject::connect(layer->undoStack(), &QUndoStack::indexChanged, context,
                   [context, callback] {
                     QMetaObject::invokeMethod(context, callback,
                                               Qt::QueuedConnection);
                   });
}
bool validateTopology(QgsVectorLayer *v, QString *error) {
  if (!v || v->geometryType() != Qgis::GeometryType::Polygon)
    return true;
  auto *collection = new QgsGeometryCollection;
  QgsGeometry coverage(collection);
  auto it = v->getFeatures();
  QgsFeature f;
  while (it.nextFeature(f)) {
    if (f.geometry().isEmpty() || !f.geometry().isGeosValid()) {
      if (error)
        *error = QObject::tr("相面含空几何或无效几何，请修复后保存");
      return false;
    }
    collection->addGeometry(f.geometry().constGet()->clone());
  }
  if (collection->numGeometries() < 2)
    return true;
  try {
    if (coverage.validateCoverage(0) == Qgis::CoverageValidityResult::Valid)
      return true;
  } catch (const QgsNotSupportedException &) {
    if (error)
      *error = QObject::tr("当前 QGIS/GEOS 不支持相面覆盖拓扑检查");
    return false;
  }
  if (error)
    *error = QObject::tr("相、亚相、微相共用的面覆盖存在重叠或共边不匹配；请撤"
                         "销或修复边界后保存");
  return false;
}
} // namespace FaciesHierarchyRenderer
