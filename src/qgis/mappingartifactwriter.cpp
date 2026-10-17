// 层：QGIS 封装
#include "mappingartifactwriter.h"
#include "mappingfeatureidentity.h"
#include "../catalog/datacatalog.h"
#include "../domain/faciescatalog.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <gdal.h>
#include <qgsattributetableconfig.h>
#include <qgscategorizedsymbolrenderer.h>
#include <qgscoordinatetransform.h>
#include <qgsexpression.h>
#include <qgsfillsymbol.h>
#include <qgsfillsymbollayer.h>
#include <qgsgeometry.h>
#include <qgslinesymbol.h>
#include <qgsmarkersymbol.h>
#include <qgsmarkersymbollayer.h>
#include <qgspalettedrasterrenderer.h>
#include <qgspallabeling.h>
#include <qgsproject.h>
#include <qgsproperty.h>
#include <qgsrasterlayer.h>
#include <qgsspatialindex.h>
#include <qgsvectordataprovider.h>
#include <qgsvectorfilewriter.h>
#include <qgsvectorlayer.h>
#include <qgsvectorlayerlabeling.h>

#include <qgstextbuffersettings.h>
#include <qgstextformat.h>
namespace MappingArtifactWriter {
void restoreRasterCrs(QgsMapLayer *layer) {
  auto *raster = qobject_cast<QgsRasterLayer *>(layer);
  if (!raster)
    return;
  GDALDatasetH ds =
      GDALOpen(raster->source().toUtf8().constData(), GA_ReadOnly);
  if (!ds)
    return;
  const char *stored = GDALGetMetadataItem(ds, "PALEO_CRS_WKT", nullptr);
  auto wkt = stored ? QString::fromUtf8(stored) : QString();
  GDALClose(ds);
  // 老层位 TIFF 未保存原始 WKT，GeoTIFF 将 EDATUM 降成 Unknown。
  // Paleo 命名的局部网格仍采用工程约定，恢复后才能匹配显式配准操作。
  if (wkt.isEmpty() && raster->crs().type() == Qgis::CrsType::Engineering &&
      raster->crs().toWkt().contains(QStringLiteral("Paleo local engineering grid")))
    wkt = DataCatalog::localGridCrsWkt();
  const auto crs = QgsCoordinateReferenceSystem::fromWkt(wkt);
  if (!wkt.isEmpty() && crs.isValid())
    raster->setCrs(crs);
}

void restoreVectorCrs(QgsMapLayer *layer) {
  auto *vector = qobject_cast<QgsVectorLayer *>(layer);
  if (!vector)
    return;
  // 矢量无 PALEO_CRS_WKT 元数据通道（GPKG srs_def 只能写 WKT1 LOCAL_CS，
  // EDATUM 名必丢），只能按图层名恢复。已是规范 WKT 时跳过 setCrs——
  // QgsMapLayer::setCrs 无条件发 crsChanged，重复调用会多余触发下游重算。
  const auto canonical = QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt());
  if (vector->crs().type() == Qgis::CrsType::Engineering &&
      vector->crs() != canonical && canonical.isValid() &&
      vector->crs().toWkt().contains(QStringLiteral("Paleo local engineering grid")))
    vector->setCrs(canonical);
}

bool raster(const QString &path, const QVector<int> &cells, int cols, int rows,
            const QRectF &extent, QString *error) {
  return raster(path, cells, cols, rows, extent, QString(), error);
}

bool raster(const QString &path, const QVector<int> &cells, int cols, int rows,
            const QRectF &extent, const QString &crsWkt, QString *error) {
  if (cells.size() != cols * rows || extent.isEmpty()) {
    if (error)
      *error = QObject::tr("预测网格为空或尺寸不匹配");
    return false;
  }
  GDALAllRegister();
  auto *driver = GDALGetDriverByName("GTiff");
  GDALDatasetH ds = driver ? GDALCreate(driver, path.toUtf8().constData(), cols,
                                        rows, 1, GDT_Int32, nullptr)
                           : nullptr;
  if (!ds) {
    if (error)
      *error = QObject::tr("无法创建预测栅格");
    return false;
  }
  double transform[] = {
      extent.left(),          extent.width() / cols, 0, extent.bottom(), 0,
      -extent.height() / rows};
  const QString effectiveCrs = !crsWkt.trimmed().isEmpty() ? crsWkt : DataCatalog::localGridCrsWkt();
  bool ok =
      GDALSetGeoTransform(ds, transform) == CE_None &&
      GDALSetProjection(
          ds, effectiveCrs.toUtf8().constData()) == CE_None;
  // GeoTIFF's keys cannot represent a named engineering datum. Preserve the
  // exact CRS in the TIFF metadata as well, so QGIS needs no sidecar file.
  ok = ok &&
       GDALSetMetadataItem(ds, "PALEO_CRS_WKT",
                           effectiveCrs.toUtf8().constData(),
                           nullptr) == CE_None;
  auto band = GDALGetRasterBand(ds, 1);
  GDALSetRasterNoDataValue(band, -9999);
  ok = ok && GDALRasterIO(band, GF_Write, 0, 0, cols, rows,
                          const_cast<int *>(cells.constData()), cols, rows,
                          GDT_Int32, 0, 0) == CE_None;
  GDALClose(ds);
  if (!ok && error)
    *error = QObject::tr("写入预测栅格失败");
  return ok;
}
bool points(const QString &path, const QVariantList &points, QString *error) {
  QJsonArray features;
  for (const auto &v : points) {
    auto props = v.toMap();
    if (props.value(MappingFeatureIdentity::UidField).toString().trimmed().isEmpty())
      props.insert(MappingFeatureIdentity::UidField,
                   QUuid::createUuid().toString(QUuid::WithoutBraces));
    const double x = props.take("x").toDouble(), y = props.take("y").toDouble();
    features.append(QJsonObject{
        {"type", "Feature"},
        {"geometry",
         QJsonObject{{"type", "Point"}, {"coordinates", QJsonArray{x, y}}}},
        {"properties", QJsonObject::fromVariantMap(props)}});
  }
  QJsonObject root{
      {"type", "FeatureCollection"},
      {"features", features},
      {"crs",
       QJsonObject{{"type", "name"},
                   {"properties",
                    QJsonObject{{"name", DataCatalog::localGridCrsWkt()}}}}}};
  QSaveFile file(path);
  const auto bytes = QJsonDocument(root).toJson();
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
      !file.commit()) {
    if (error)
      *error = file.errorString();
    return false;
  }
  return true;
}
bool vectorSnapshot(QgsVectorLayer *layer, const QString &path,
                    QString *error) {
  if (!layer || !layer->isValid()) {
    if (error)
      *error = QObject::tr("矢量图层无效");
    return false;
  }
  const auto sourcePath = QFileInfo(layer->source().section(QLatin1Char('|'), 0, 0)).canonicalFilePath();
  const auto destinationPath = QFileInfo(path).canonicalFilePath();
  if (!sourcePath.isEmpty() && sourcePath == destinationPath) {
    if (error) *error = QObject::tr("图件快照必须使用新文件，不能覆盖来源版本");
    return false;
  }
  QgsVectorFileWriter::SaveVectorOptions options;
  options.driverName = QStringLiteral("GPKG");
  options.layerName = QStringLiteral("features");
  options.fileEncoding = QStringLiteral("UTF-8");
  const bool saved = QgsVectorFileWriter::writeAsVectorFormatV3(
             layer, path, QgsCoordinateTransformContext(), options, error) ==
         QgsVectorFileWriter::NoError;
  if (!saved) return false;
  // Identity normalization applies to the new snapshot only. In particular,
  // exporting a legacy or published layer never modifies its source provider.
  bool identified = false;
  {
    QgsVectorLayer snapshot(path + QStringLiteral("|layername=features"),
                            QStringLiteral("snapshot"), QStringLiteral("ogr"));
    identified = MappingFeatureIdentity::ensure(&snapshot, error);
  }
  if (!identified) QFile::remove(path);
  return identified;
}
QVariantList constraintGeometries(const QString &path, QString *error) {
  QgsVectorLayer layer(path, QStringLiteral("constraints"),
                       QStringLiteral("ogr"));
  if (!layer.isValid()) {
    if (error)
      *error = QObject::tr(
          "请选择有效的线矢量文件（GeoPackage / GeoJSON / Shapefile）");
    return {};
  }
  const auto crs =
      QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt());
  if (!layer.crs().isValid() || layer.crs() != crs) {
    if (error)
      *error = QObject::tr("约束线必须采用本工程的局部米制坐标；请先完成配准");
    return {};
  }
  QVariantList records;
  auto iterator = layer.getFeatures();
  QgsFeature f;
  while (iterator.nextFeature(f)) {
    const auto geometry = f.geometry();
    if (geometry.isEmpty() || geometry.type() != Qgis::GeometryType::Line ||
        !geometry.isGeosValid()) {
      if (error)
        *error = QObject::tr("约束文件包含无效几何，未导入");
      return {};
    }
    records.append(geometry.asWkt());
  }
  if (records.isEmpty() && error)
    *error = QObject::tr("约束文件没有线要素");
  return records;
}
bool syncFaciesAttributes(QgsVectorLayer *layer, const QVariantList &facies,
                          QString *error) {
  if (!layer || layer->fields().indexOf("facies_code") < 0)
    return true;
  if (!MappingFeatureIdentity::ensure(layer, error))
    return false;
  const bool own = !layer->isEditable();
  if (own && !layer->startEditing()) {
    if (error)
      *error = QObject::tr("无法编辑相属性");
    return false;
  }
  layer->beginEditCommand(QObject::tr("同步相、亚相、微相属性"));
  bool ok = true;
  for (const auto &name :
       {"facies_name", "subfacies", "microfacies", "facies_label", "texture"})
    if (layer->fields().indexOf(name) < 0)
      ok = layer->addAttribute(QgsField(name, QMetaType::Type::QString)) && ok;
  if (layer->fields().indexOf("facies_intervals") >= 0) {
    if (layer->fields().indexOf("predicted_facies") < 0)
      ok = layer->addAttribute(QgsField("predicted_facies", QMetaType::Type::QString)) && ok;
    for (const auto &name : {"dominant_facies", "predicted_dominant", "dominant_thickness_md", "dominant_ratio", "facies_ratio", "facies_composition", "tie"})
      if (const int index = layer->fields().indexOf(name); index >= 0)
        ok = layer->deleteAttribute(index) && ok;
  }
  auto it = layer->getFeatures();
  QgsFeature feature;
  while (ok && it.nextFeature(feature)) {
    // #419：迭代中嵌套 getFeature 对部分 provider 会重置游标/跳行——
    // 改后的 facies_code 从写入集本身追踪，不再回读图层。
    QVariant code = feature.attribute("facies_code");
    if (layer->fields().indexOf("facies_intervals") >= 0) {
      QVariantMap source;
      for (int i = 0; i < feature.fields().size(); ++i)
        source.insert(feature.fields().at(i).name(), feature.attribute(i));
      // 属性表直接改编码时，空值状态也同步到最终预测相。
      source.insert("prediction_status", code.isNull()
          ? QStringLiteral("未预测") : QStringLiteral("已修订"));
      const auto final = FaciesCatalog::formationPrediction(source, facies);
      for (auto a = final.cbegin(); a != final.cend(); ++a) {
        const int index = layer->fields().indexOf(a.key());
        if (index >= 0) ok = layer->changeAttributeValue(feature.id(), index, a.value()) && ok;
        if (a.key() == QLatin1String("facies_code"))
          code = a.value();
      }
    }
    const auto attrs =
        FaciesCatalog::attributes(facies, code);
    for (auto a = attrs.cbegin(); a != attrs.cend(); ++a)
      ok = layer->changeAttributeValue(
               feature.id(), layer->fields().indexOf(a.key()),
               code.isNull() && layer->fields().indexOf("facies_intervals") >= 0
                   ? (a.key() == "facies_label" ? QVariant(QObject::tr("未预测")) : QVariant()) : a.value()) && ok;
  }
  if (!ok) {
    layer->destroyEditCommand();
    if (own)
      layer->rollBack();
    if (error)
      *error = QObject::tr("同步相属性失败");
    return false;
  }
  layer->endEditCommand();
  if (own && !layer->commitChanges()) {
    if (error)
      *error = layer->commitErrors().join("；");
    layer->rollBack();
    return false;
  }
  return true;
}
int defaultFaciesLabelMode(const QgsVectorLayer *layer) {
  return layer && layer->geometryType() == Qgis::GeometryType::Polygon ? 2 : 3;
}
void applyFaciesLabels(QgsVectorLayer *layer, int mode) {
  if (!layer || (layer->geometryType() != Qgis::GeometryType::Polygon &&
                 layer->geometryType() != Qgis::GeometryType::Point))
    return;
  mode = std::clamp(mode, 0, 3);
  layer->setCustomProperty("paleo/faciesLabelMode", mode);
  if (!mode) {
    layer->setLabelsEnabled(false);
    layer->triggerRepaint();
    return;
  }
  auto field = [layer](const QString &name) {
    return layer->fields().indexOf(name) < 0
               ? QStringLiteral("NULL")
               : QStringLiteral("nullif(trim(to_string(%1)), '')")
                     .arg(QgsExpression::quotedColumnRef(name));
  };
  const QString identifier =
      QStringLiteral("coalesce(%1, %2, %3, to_string($id))")
          .arg(field("name"), field("id"), field("fid"));
  const QString defaultFacies =
      QStringLiteral("coalesce(%1, %2, %3, %4, %5)")
          .arg(field("facies_label"), field("microfacies"), field("subfacies"),
               field("facies_name"),
               QgsExpression::quotedString(QObject::tr("其他 / 未分类")));
  QString facies = layer->customProperty("paleo/faciesDisplayExpression", defaultFacies).toString();
  if (layer->fields().indexOf("prediction_status") >= 0)
    facies = QStringLiteral("CASE WHEN \"prediction_status\" = %1 THEN %1 ELSE (%2) END")
        .arg(QgsExpression::quotedString(QObject::tr("未预测")), facies);
  QgsPalLayerSettings settings;
  if (layer->fields().indexOf("facies_intervals") >= 0)
    settings.priority = 10;
  settings.isExpression = true;
  settings.fieldName =
      mode == 1   ? identifier
      : mode == 2 ? facies
                  : QStringLiteral("%1 || '\n' || %2").arg(identifier, facies);
  settings.placement = layer->geometryType() == Qgis::GeometryType::Point
                           ? Qgis::LabelPlacement::OrderedPositionsAroundPoint
                           : Qgis::LabelPlacement::OverPoint;
  QgsTextFormat format;
  format.setFont(QFont(QStringLiteral("Noto Sans SC"), 9));
  format.setSize(9);
  format.setSizeUnit(Qgis::RenderUnit::Points);
  format.setColor(QColor("#24303E"));
  QgsTextBufferSettings buffer;
  buffer.setEnabled(true);
  buffer.setSize(.8);
  buffer.setColor(Qt::white);
  format.setBuffer(buffer);
  settings.setFormat(format);
  // 文字标注不画底衬，直接压在相面上（2026-10-11 用户口径「标注不需要
  // 背景」）。字号随画布比例尺自适应：@map_scale 数据定义（1:2.5万≈15pt、
  // 1:50万≈6pt），与 qgisstyleservice 井位标记同一写法，缩放时标注跟随
  // 图面大小变化而不是固定屏幕点号。
  settings.dataDefinedProperties().setProperty(
      QgsPalLayerSettings::Property::Size,
      QgsProperty::fromExpression(QStringLiteral(
          "clamp(6.0, scale_linear(@map_scale, 25000, 500000, 15.0, 6.0), "
          "15.0)")));
  layer->setLabeling(new QgsVectorLayerSimpleLabeling(settings));
  layer->setLabelsEnabled(true);
  layer->triggerRepaint();
}

void applyFaciesEdge(QgsSymbol *symbol, const QColor &fillColor) {
  if (!symbol || symbol->type() != Qgis::SymbolType::Fill)
    return;
  // 相面边缘：填充色同系加深一档描边（数据符号色，不入 UI token）。
  // MergedFeatureRenderer 先把同相图斑合成一体化再画——描边落在每个相
  // 块的外轮廓上，正是「每个相的边缘」。
  const QColor edge = fillColor.darker(160);
  bool stroked = false;
  for (int i = 0; i < symbol->symbolLayerCount(); ++i) {
    auto *fill =
        dynamic_cast<QgsSimpleFillSymbolLayer *>(symbol->symbolLayer(i));
    if (!fill)
      continue;
    fill->setStrokeStyle(Qt::SolidLine);
    fill->setStrokeColor(edge);
    fill->setStrokeWidth(0.3);
    fill->setStrokeWidthUnit(Qgis::RenderUnit::Millimeters);
    stroked = true;
  }
  if (!stroked) {
    // SVG 纹理填充不含描边层——追加一个只画轮廓的符号层（透明填充）。
    auto *outline = new QgsSimpleFillSymbolLayer();
    outline->setBrushStyle(Qt::NoBrush);
    outline->setStrokeStyle(Qt::SolidLine);
    outline->setStrokeColor(edge);
    outline->setStrokeWidth(0.3);
    outline->setStrokeWidthUnit(Qgis::RenderUnit::Millimeters);
    symbol->appendSymbolLayer(outline);
  }
}

void applyFaciesStyle(QgsMapLayer *layer, const QVariantList &facies) {
  if (auto *vector = qobject_cast<QgsVectorLayer *>(layer)) {
    if (vector->fields().indexOf("facies_code") < 0)
      return;
    // Older immutable products can expose names in their attribute tables
    // without rewriting their source files. Copies materialize these fields.
    const auto fallbackAttributes = FaciesCatalog::attributes({}, QVariant());
    for (auto a = fallbackAttributes.cbegin(); a != fallbackAttributes.cend();
         ++a) {
      if (vector->fields().indexOf(a.key()) >= 0 ||
          vector->customProperty("paleoLayerId")
              .toString()
              .startsWith("draft."))
        continue;
      QString expression = QStringLiteral("CASE ");
      for (const auto &v : facies) {
        auto f = v.toMap();
        expression +=
            QStringLiteral("WHEN \"facies_code\" = %1 THEN %2 ")
                .arg(QgsExpression::quotedValue(f.value("code")),
                     QgsExpression::quotedValue(
                         FaciesCatalog::attributes(facies, f.value("code"))
                             .value(a.key())));
      }
      expression += QStringLiteral("ELSE %1 END")
                        .arg(QgsExpression::quotedValue(a.value()));
      if (facies.isEmpty())
        expression = QgsExpression::quotedValue(a.value());
      vector->addExpressionField(expression,
                                 QgsField(a.key(), QMetaType::Type::QString));
    }
    QgsCategoryList categories;
    for (const auto &value : facies) {
      const auto f = FaciesCatalog::appearance(value.toMap());
      QgsSymbol *symbol = QgsSymbol::defaultSymbol(vector->geometryType());
      if (!symbol)
        continue;
      const bool point = vector->geometryType() == Qgis::GeometryType::Point;
      // 预测点与相面、井道共用分类的纹理底色；圆点保留白色描边。
      const auto faciesColor = QColor(f.value("color").toString());
      symbol->setColor(faciesColor.isValid() ? faciesColor : QColor(QStringLiteral("#9AA7B4")));
      const auto texture =
          FaciesCatalog::fillPath(f.value("texture").toString());
      if (!texture.isEmpty() && !point) {
        if (vector->geometryType() == Qgis::GeometryType::Polygon)
          symbol->changeSymbolLayer(0, new QgsSVGFillSymbolLayer(texture, 10));
      }
      // 相面除填充外还要能看见每个相的边缘（2026-10-11 用户口径）。
      if (vector->geometryType() == Qgis::GeometryType::Polygon)
        applyFaciesEdge(symbol, QColor(f.value("color").toString()));
      if (point) {
        auto *marker = static_cast<QgsMarkerSymbol *>(symbol);
        marker->setSize(3.2);
        if (!texture.isEmpty()) {
          auto *fill = new QgsFillSymbol();
          auto *pattern = new QgsSVGFillSymbolLayer(texture, 2.0);
          pattern->setColor(faciesColor);
          fill->changeSymbolLayer(0, pattern);
          auto *outline = new QgsSimpleFillSymbolLayer();
          outline->setBrushStyle(Qt::NoBrush);
          outline->setStrokeColor(Qt::white);
          outline->setStrokeWidth(0.3);
          outline->setStrokeWidthUnit(Qgis::RenderUnit::Millimeters);
          fill->appendSymbolLayer(outline);
          auto *circle = new QgsFilledMarkerSymbolLayer(Qgis::MarkerShape::Circle, 3.2);
          circle->setSubSymbol(fill);
          marker->changeSymbolLayer(0, circle);
        }
        if (auto *simple =
                dynamic_cast<QgsSimpleMarkerSymbolLayer *>(marker->symbolLayer(0))) {
          simple->setStrokeColor(QColor(QStringLiteral("#FFFFFF")));
          simple->setStrokeWidth(0.6);
        }
      }
      categories.append(QgsRendererCategory(f.value("code"), symbol,
                                            f.value("name").toString()));
    }
    // A default category includes both NULL and codes missing from the schema.
    auto *fallback = QgsSymbol::defaultSymbol(vector->geometryType());
    if (fallback) {
      const bool point = vector->geometryType() == Qgis::GeometryType::Point;
      fallback->setColor(QColor(QStringLiteral("#9AA7B4")));
      if (vector->geometryType() == Qgis::GeometryType::Polygon)
        applyFaciesEdge(fallback, QColor(QStringLiteral("#9AA7B4")));
      if (point) {
        auto *marker = static_cast<QgsMarkerSymbol *>(fallback);
        marker->setSize(3.2);
        if (auto *simple =
                dynamic_cast<QgsSimpleMarkerSymbolLayer *>(marker->symbolLayer(0))) {
          simple->setStrokeColor(QColor(QStringLiteral("#FFFFFF")));
          simple->setStrokeWidth(0.6);
        }
      }
      categories.append(QgsRendererCategory(QVariant(), fallback,
                                            vector->fields().indexOf("prediction_status") >= 0
                                                ? QObject::tr("未预测") : QObject::tr("其他 / 未分类")));
    }
    vector->setRenderer(
        new QgsCategorizedSymbolRenderer("facies_code", categories));
    applyFaciesLabels(vector,
                      vector->customProperty("paleo/faciesLabelMode",
                                             defaultFaciesLabelMode(vector))
                          .toInt());
    const QStringList names{"facies_code", "facies_name",  "subfacies",
                            "microfacies", "facies_label", "texture", "facies_evidence", "prediction_status", "prediction_reason", "predicted_facies"};
    const QStringList labels{QObject::tr("相编码"),   QObject::tr("相"),
                             QObject::tr("亚相"),     QObject::tr("微相"),
                             QObject::tr("类别名称"), QObject::tr("纹理"), QObject::tr("解释证据"), QObject::tr("预测状态"), QObject::tr("未预测原因"), QObject::tr("预测相")};
    for (int i = 0; i < names.size(); ++i)
      if (int index = vector->fields().indexOf(names[i]); index >= 0)
        vector->setFieldAlias(index, labels[i]);
    if (vector->fields().indexOf("facies_intervals") >= 0) {
      // 原版 QGIS 属性表只展示一列最终相；历史图件不改写源文件。
      if (vector->fields().indexOf("predicted_facies") < 0)
        vector->addExpressionField("CASE WHEN \"facies_code\" IS NULL THEN NULL ELSE \"facies_label\" END",
                                  QgsField("predicted_facies", QMetaType::Type::QString));
      vector->setFieldAlias(vector->fields().indexOf("predicted_facies"), QObject::tr("预测相"));
      if (!vector->customProperty("paleo/finalPredictionTableConfigured").toBool()) {
        auto config = vector->attributeTableConfig();
        config.update(vector->fields());
        auto columns = config.columns();
        const QSet<QString> hidden{
            "facies_name", "subfacies", "microfacies", "facies_label", "texture",
            "facies_intervals", "predicted_intervals", "formation_intervals",
            "dominant_facies", "predicted_dominant", "dominant_thickness_md",
            "dominant_ratio", "facies_ratio", "facies_composition", "tie"};
        for (auto &column : columns)
          if (hidden.contains(column.name)) column.hidden = true;
        config.setColumns(columns);
        vector->setAttributeTableConfig(config);
        vector->setCustomProperty("paleo/finalPredictionTableConfigured", true);
      }
    }
  } else if (auto *raster = qobject_cast<QgsRasterLayer *>(layer)) {
    QgsPalettedRasterRenderer::ClassData classes;
    QSet<int> defined;
    for (const auto &value : facies) {
      const auto f = value.toMap();
      defined.insert(f.value("code").toInt());
      classes.append(QgsPalettedRasterRenderer::Class(
          f.value("code").toInt(), QColor(f.value("color").toString()),
          f.value("name").toString()));
    }
    for (const auto &entry : QgsPalettedRasterRenderer::classDataFromRaster(
             raster->dataProvider(), 1))
      if (!defined.contains(int(entry.value)))
        classes.append(QgsPalettedRasterRenderer::Class(
            entry.value, QColor("#9AA7B4"),
            QObject::tr("其他（%1）").arg(entry.value)));
    raster->setRenderer(
        new QgsPalettedRasterRenderer(raster->dataProvider(), 1, classes));
  }
  if (layer)
    layer->triggerRepaint();
}
} // namespace MappingArtifactWriter

#include <algorithm>
#include <cmath>
#include <limits>
#include <qgsrasterblock.h>
#include <qgsrasterdataprovider.h>

bool MappingArtifactWriter::composeRaster(
    const QString &path, const QList<QgsMapLayer *> &layers,
    const QVariantList &facies, const QList<double> &thresholds,
    const QSet<QString> &continuous, QString *error,
    const std::function<bool()> &cancelled,
    const std::function<void(double)> &progress, ComposeReceipt *receipt) {
  auto reject = [error](const QString &s) {
    if (error)
      *error = s;
    return false;
  };
  if (layers.isEmpty() || facies.isEmpty())
    return reject(QObject::tr("请选择编图输入与相分类"));
  QList<int> codes;
  for (const auto &f : facies)
    codes << f.toMap().value("code").toInt();
  if (!continuous.isEmpty() &&
      (thresholds.size() != codes.size() - 1 ||
       !std::is_sorted(thresholds.begin(), thresholds.end()) ||
       std::adjacent_find(thresholds.begin(), thresholds.end()) !=
           thresholds.end()))
    return reject(
        QObject::tr("连续单因素需要按相类别顺序填写 N−1 个严格递增分相阈值"));
  for (double t : thresholds)
    if (!std::isfinite(t))
      return reject(QObject::tr("分相阈值必须为有限数值"));
  QgsRectangle extent;
  int cols = 64, rows = 64;
  const auto crs =
      QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt());
  for (auto *l : layers) {
    if (!l || !l->isValid() || l->crs() != crs)
      return reject(QObject::tr("所有编图输入须使用本工程局部米制坐标"));
    if (auto *r = qobject_cast<QgsRasterLayer *>(l)) {
      extent = r->extent();
      cols = r->width();
      rows = r->height();
      break;
    }
  }
  if (extent.isEmpty()) {
    for (auto *l : layers)
      extent.combineExtentWith(l->extent());
    if (extent.width() < 1 || extent.height() < 1)
      extent.grow(100);
  }
  if (cols <= 0 || rows <= 0 || qint64(cols) * rows > 4 * 1024 * 1024)
    return reject(QObject::tr("编图网格超过 400 万像元，请先裁剪输入范围"));
  QVector<int> output(cols * rows, -9999);
  if (receipt) {
    receipt->winningCells = QList<qint64>(layers.size(), 0);
    receipt->totalCells = qint64(cols) * rows;
    receipt->decidedCells = 0;
  }
  int layerIndex = 0;
  for (auto *l : layers) {
    if (cancelled && cancelled())
      return reject(QObject::tr("已取消"));
    const double baseProgress = 95.0 * layerIndex++ / layers.size();
    if (progress)
      progress(baseProgress);
    if (l->crs() != crs)
      return reject(QObject::tr("编图输入坐标不一致，请先配准"));
    if (auto *r = qobject_cast<QgsRasterLayer *>(l)) {
      std::unique_ptr<QgsRasterBlock> block(
          r->dataProvider()->block(1, extent, cols, rows));
      if (!block || !block->isValid())
        return reject(QObject::tr("无法读取编图栅格"));
      const bool numeric = continuous.contains(l->id());
      for (int i = 0; i < output.size(); ++i) {
        if (i % 4096 == 0) {
          if (cancelled && cancelled())
            return reject(QObject::tr("已取消"));
          if (progress)
            progress(baseProgress + 95.0 * i / output.size() / layers.size());
        }
        if (block->isNoData(i))
          continue;
        const double v = block->value(i);
        if (!std::isfinite(v))
          continue;
        const int code = numeric ? codes[std::upper_bound(thresholds.begin(),
                                                          thresholds.end(), v) -
                                         thresholds.begin()]
                                 : int(std::round(v));
        if (!numeric && (std::abs(v - code) > 1e-6 || !codes.contains(code)))
          return reject(QObject::tr(
              "相栅格含当前层位未定义的编码；请统一相分类或将其作为参考图"));
        if (output[i] == -9999) {
          output[i] = code;
          if (receipt)
            ++receipt->winningCells[layerIndex - 1];
        }
      }
    } else if (auto *v = qobject_cast<QgsVectorLayer *>(l)) {
      if (v->geometryType() != Qgis::GeometryType::Point ||
          v->fields().indexOf("facies_code") < 0)
        return reject(QObject::tr("自动融合支持相栅格、单因素栅格与预测相点；已"
                                  "有相面请用“复制底图并编辑”"));
      QgsSpatialIndex index;
      QHash<QgsFeatureId, int> pointCodes;
      QgsFeatureId pointId = 0;
      QgsFeature f;
      auto it = v->getFeatures();
      while (it.nextFeature(f)) {
        if (cancelled && cancelled())
          return reject(QObject::tr("已取消"));
        if (f.attribute("facies_code").isNull())
          continue;
        if (f.geometry().isEmpty())
          continue;
        int code = f.attribute("facies_code").toInt();
        if (!codes.contains(code))
          return reject(QObject::tr("相点含当前层位未定义的编码"));
        auto g = f.geometry().centroid();
        QgsFeature indexed(++pointId);
        indexed.setGeometry(g);
        index.addFeature(indexed);
        pointCodes.insert(pointId, code);
      }
      if (pointCodes.isEmpty())
        return reject(QObject::tr("预测相点为空"));
      // Categorical nearest-neighbour: never interpolate numeric facies codes.
      for (int y = 0; y < rows; ++y) {
        if (cancelled && cancelled())
          return reject(QObject::tr("已取消"));
        if (progress)
          progress(baseProgress + 95.0 * y / rows / layers.size());
        for (int x = 0; x < cols; ++x) {
          int i = y * cols + x;
          if (output[i] != -9999)
            continue;
          QgsPointXY p(extent.xMinimum() + (x + .5) * extent.width() / cols,
                       extent.yMaximum() - (y + .5) * extent.height() / rows);
          const auto nearest = index.nearestNeighbor(p, 1);
          const int code =
              nearest.isEmpty()
                      ? -9999
                      : pointCodes.value(
                            *std::min_element(nearest.cbegin(), nearest.cend()),
                            -9999);
          if (code != -9999 && receipt)
            ++receipt->winningCells[layerIndex - 1];
          output[i] = code;
        }
      }
    }
  }
  if (cancelled && cancelled())
    return reject(QObject::tr("已取消"));
  if (progress)
    progress(95);
  if (receipt)
    for (const auto cell : output)
      if (cell != -9999)
        ++receipt->decidedCells;
  return raster(path, output, cols, rows,
                QRectF(extent.xMinimum(), extent.yMinimum(), extent.width(),
                       extent.height()),
                error);
}
