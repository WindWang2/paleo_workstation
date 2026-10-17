// 层：QGIS 封装
#include "facieshierarchyrenderer.h"
#include "faciesmaplegend.h"
#include "../domain/facieshierarchy.h"
#include "../domain/faciescatalog.h"
#include "mappingartifactwriter.h"
#include <QUndoStack>
#include <QJsonDocument>
#include <qgscolorramp.h>
#include <qgsdatadefinedsizelegend.h>
#include <qgscategorizedsymbolrenderer.h>
#include <qgsexception.h>
#include <qgsexpression.h>
#include <qgsgeometrycollection.h>
#include <qgsmergedfeaturerenderer.h>
#include <qgsmarkersymbol.h>
#include <qgsmarkersymbollayer.h>
#include <qgspalettedrasterrenderer.h>
#include <qgsrasterlayer.h>
#include <qgssymbol.h>
#include <qgsvectorlayer.h>
namespace {
// 地图过滤的命中结果必须包含祖先 key，否则 QGIS 会删掉没有自身几何的父标题。
class HierarchyCategories final : public QgsCategorizedSymbolRenderer {
public:
  using QgsCategorizedSymbolRenderer::QgsCategorizedSymbolRenderer;
  HierarchyCategories *clone() const override {
    auto *copy = new HierarchyCategories(classAttribute(), categories());
    if (sourceSymbol()) copy->setSourceSymbol(sourceSymbol()->clone());
    if (sourceColorRamp()) copy->setSourceColorRamp(sourceColorRamp()->clone());
    if (dataDefinedSizeLegend()) copy->setDataDefinedSizeLegend(new QgsDataDefinedSizeLegend(*dataDefinedSizeLegend()));
    copyRendererData(copy);
    return copy;
  }
  QSet<QString> legendKeysForFeature(const QgsFeature &feature, QgsRenderContext &context) const override {
    auto keys = QgsCategorizedSymbolRenderer::legendKeysForFeature(feature, context);
    const auto leaves = keys;
    for (const auto &key : leaves) {
      const auto path = QJsonDocument::fromJson(key.toUtf8()).toVariant().toStringList();
      for (int i = 1; i < path.size(); ++i)
        keys.insert(QString::fromUtf8(QJsonDocument::fromVariant(path.mid(0, i)).toJson(QJsonDocument::Compact)));
    }
    return keys;
  }
};
}
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
  const auto present = codes(layer);
  QVariantList styled;
  for (const auto &v : schema) styled << FaciesCatalog::appearance(v.toMap());
  auto entries = FaciesHierarchy::legend(styled, level, present);
  if (auto *v = qobject_cast<QgsVectorLayer *>(layer);
      v && v->geometryType() == Qgis::GeometryType::Point)
    for (auto &entry : entries) {
      auto row = FaciesCatalog::appearance(entry.toMap());
      if (v->fields().indexOf("facies_intervals") >= 0 && row.value("key") == "unknown")
        row.insert("name", QObject::tr("未预测"));
      if (!QColor(row.value("color").toString()).isValid())
        row.insert("color", "#9AA7B4");
      entry = row;
    }
  if (qobject_cast<QgsRasterLayer *>(layer))
    for (auto &entry : entries) {
      auto row = entry.toMap();
      row.insert("texture", QString()); // 栅格使用纯色调色板，图例不能额外画微相纹理。
      entry = row;
    }
  return entries;
}
QVariantList apply(QgsMapLayer *layer, const QVariantList &schema,
                   const QString &level) {
  MappingArtifactWriter::applyFaciesStyle(layer, schema);
  const auto entries = legend(layer, schema, level);
  if (auto *v = qobject_cast<QgsVectorLayer *>(layer)) {
    const bool wellResult = v->fields().indexOf("facies_intervals") >= 0;
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
      // 测井点克隆已配置好的纹理圆点，避免 setColor 把白色外圈也改掉。
      if (!wellResult)
        symbol->setColor(QColor(row.value("color").toString()));
      if (v->geometryType() == Qgis::GeometryType::Point)
        if (auto *marker = dynamic_cast<QgsMarkerSymbol *>(symbol))
          if (auto *simple = dynamic_cast<QgsSimpleMarkerSymbolLayer *>(marker->symbolLayer(0))) {
            simple->setStrokeColor(QColor(QStringLiteral("#FFFFFF")));
            simple->setStrokeWidth(0.6);
          }
      // setColor 会把描边层一并刷成相色——补回边缘描边（相面每个相的
      // 边缘要可见，2026-10-11 用户口径）。
      if (v->geometryType() == Qgis::GeometryType::Polygon)
        MappingArtifactWriter::applyFaciesEdge(
            symbol, QColor(row.value("color").toString()));
      categories << QgsRendererCategory(row.value("key"), symbol,
                                        row.value("name").toString(), true, row.value("key").toString());
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
        new HierarchyCategories(keyExpression, categories);
    v->setRenderer(v->geometryType() == Qgis::GeometryType::Polygon
                       ? static_cast<QgsFeatureRenderer *>(
                             new QgsMergedFeatureRenderer(renderer))
                       : renderer);
    v->setCustomProperty("paleo/faciesDisplayExpression", nameExpression);
    MappingArtifactWriter::applyFaciesLabels(
        v, v->customProperty("paleo/faciesLabelMode",
                             MappingArtifactWriter::defaultFaciesLabelMode(v))
               .toInt());
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
  FaciesMapLegend::install(layer);
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
