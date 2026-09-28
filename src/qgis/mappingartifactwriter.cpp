// 层：QGIS 封装
#include "mappingartifactwriter.h"
#include "../catalog/datacatalog.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <gdal.h>
#include <qgscategorizedsymbolrenderer.h>
#include <qgscoordinatetransform.h>
#include <qgsfillsymbol.h>
#include <qgsgeometry.h>
#include <qgslinesymbol.h>
#include <qgsmarkersymbol.h>
#include <qgspalettedrasterrenderer.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsvectorfilewriter.h>
#include <qgsvectorlayer.h>

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
  const auto wkt = stored ? QString::fromUtf8(stored) : QString();
  GDALClose(ds);
  const auto crs = QgsCoordinateReferenceSystem::fromWkt(wkt);
  if (!wkt.isEmpty() && crs.isValid())
    raster->setCrs(crs);
}

bool raster(const QString &path, const QVector<int> &cells, int cols, int rows,
            const QRectF &extent, QString *error) {
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
  bool ok =
      GDALSetGeoTransform(ds, transform) == CE_None &&
      GDALSetProjection(
          ds, DataCatalog::localGridCrsWkt().toUtf8().constData()) == CE_None;
  // GeoTIFF's keys cannot represent a named engineering datum. Preserve the
  // exact CRS in the TIFF metadata as well, so QGIS needs no sidecar file.
  ok = ok &&
       GDALSetMetadataItem(ds, "PALEO_CRS_WKT",
                           DataCatalog::localGridCrsWkt().toUtf8().constData(),
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
  QgsVectorFileWriter::SaveVectorOptions options;
  options.driverName = QStringLiteral("GPKG");
  options.layerName = QStringLiteral("features");
  options.fileEncoding = QStringLiteral("UTF-8");
  return QgsVectorFileWriter::writeAsVectorFormatV3(
             layer, path, QgsCoordinateTransformContext(), options, error) ==
         QgsVectorFileWriter::NoError;
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
void applyFaciesStyle(QgsMapLayer *layer, const QVariantList &facies) {
  if (auto *vector = qobject_cast<QgsVectorLayer *>(layer)) {
    if (vector->fields().indexOf(QStringLiteral("facies_code")) < 0)
      return;
    QgsCategoryList categories;
    for (const auto &value : facies) {
      const auto f = value.toMap();
      QgsSymbol *symbol = QgsSymbol::defaultSymbol(vector->geometryType());
      if (!symbol)
        continue;
      symbol->setColor(QColor(f.value("color").toString()));
      categories.append(QgsRendererCategory(f.value("code"), symbol,
                                            f.value("name").toString()));
    }
    vector->setRenderer(new QgsCategorizedSymbolRenderer(
        QStringLiteral("facies_code"), categories));
  } else if (auto *raster = qobject_cast<QgsRasterLayer *>(layer)) {
    QgsPalettedRasterRenderer::ClassData classes;
    for (const auto &value : facies) {
      const auto f = value.toMap();
      classes.append(QgsPalettedRasterRenderer::Class(
          f.value("code").toInt(), QColor(f.value("color").toString()),
          f.value("name").toString()));
    }
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

bool MappingArtifactWriter::composeRaster(const QString &path,
                                          const QList<QgsMapLayer *> &layers,
                                          const QVariantList &facies,
                                          const QList<double> &thresholds,
                                          const QSet<QString> &continuous,
                                          QString *error) {
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
  for (auto *l : layers) {
    if (l->crs() != crs)
      return reject(QObject::tr("编图输入坐标不一致，请先配准"));
    if (auto *r = qobject_cast<QgsRasterLayer *>(l)) {
      std::unique_ptr<QgsRasterBlock> block(
          r->dataProvider()->block(1, extent, cols, rows));
      if (!block || !block->isValid())
        return reject(QObject::tr("无法读取编图栅格"));
      const bool numeric = continuous.contains(l->id());
      for (int i = 0; i < output.size(); ++i) {
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
        if (output[i] == -9999)
          output[i] = code;
      }
    } else if (auto *v = qobject_cast<QgsVectorLayer *>(l)) {
      if (v->geometryType() != Qgis::GeometryType::Point ||
          v->fields().indexOf("facies_code") < 0)
        return reject(QObject::tr("自动融合支持相栅格、单因素栅格与预测相点；已"
                                  "有相面请用“复制底图并编辑”"));
      QList<QPair<QgsPointXY, int>> points;
      QgsFeature f;
      auto it = v->getFeatures();
      while (it.nextFeature(f)) {
        if (f.geometry().isEmpty())
          continue;
        int code = f.attribute("facies_code").toInt();
        if (!codes.contains(code))
          return reject(QObject::tr("相点含当前层位未定义的编码"));
        auto g = f.geometry().centroid();
        points.append({g.asPoint(), code});
      }
      if (points.isEmpty())
        return reject(QObject::tr("预测相点为空"));
      // Categorical nearest-neighbour: never interpolate numeric facies codes.
      for (int y = 0; y < rows; ++y)
        for (int x = 0; x < cols; ++x) {
          int i = y * cols + x;
          if (output[i] != -9999)
            continue;
          QgsPointXY p(extent.xMinimum() + (x + .5) * extent.width() / cols,
                       extent.yMaximum() - (y + .5) * extent.height() / rows);
          double nearest = std::numeric_limits<double>::max();
          int code = -9999;
          for (const auto &entry : points) {
            double d = entry.first.sqrDist(p);
            if (d < nearest) {
              nearest = d;
              code = entry.second;
            }
          }
          output[i] = code;
        }
    }
  }
  return raster(path, output, cols, rows,
                QRectF(extent.xMinimum(), extent.yMinimum(), extent.width(),
                       extent.height()),
                error);
}
