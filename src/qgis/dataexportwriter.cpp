// 层：QGIS 封装
#include "dataexportwriter.h"
#include <QDir>
#include <QFileInfo>
#include <gdal.h>
#include <ogr_api.h>
#include <qgscoordinatetransform.h>
#include <qgsproject.h>
#include <qgsrasterdataprovider.h>
#include <qgsrasterfilewriter.h>
#include <qgsrasterlayer.h>
#include <qgsrasterpipe.h>
#include <qgsvectorfilewriter.h>
#include <qgsvectorlayer.h>

namespace PaleoDataExportWriter {
bool writeLayer(QgsMapLayer *layer, const QString &directory, QStringList *files,
                QVariantMap *description, QString *error) {
  if (!layer || !layer->isValid()) { if (error) *error = QObject::tr("图层数据不可用"); return false; }
  if (!QDir().mkpath(directory)) { if (error) *error = QObject::tr("无法创建图层数据目录"); return false; }
  const auto path = QDir(directory).filePath("data.gpkg");
  if (auto *vector = qobject_cast<QgsVectorLayer *>(layer)) {
    QgsVectorFileWriter::SaveVectorOptions options;
    options.driverName = "GPKG"; options.layerName = "data"; options.fileEncoding = "UTF-8";
    if (QgsVectorFileWriter::writeAsVectorFormatV3(vector, path, QgsCoordinateTransformContext(), options, error) != QgsVectorFileWriter::NoError) return false;
    QgsVectorLayer check(path, "check", "ogr");
    if (!check.isValid() || check.featureCount() != vector->featureCount()) {
      if (error) *error = QObject::tr("图层数据回读校验失败"); return false;
    }
    *description = {{"format", "GPKG"}, {"features", vector->featureCount()}, {"crs_wkt", vector->crs().toWkt()}};
    *files << path;
  } else if (auto *raster = qobject_cast<QgsRasterLayer *>(layer)) {
    if (raster->providerType() != "gdal") {
      description->insert("external_reference", true);
      description->insert("source", layer->source());
      return true; // online basemap: retain the declaration and attribution, do not download tiles
    }
    if (!write(raster->source(), directory, paleo::exporting::DataFormat::GeoTiff, files, description, error)) return false;
  } else {
    if (error) *error = QObject::tr("当前图层没有可用的数据写出器：%1").arg(layer->name()); return false;
  }
  bool styleOk = false;
  const auto style = QDir(directory).filePath("style.qml");
  const auto detail = layer->saveNamedStyle(style, styleOk);
  if (!styleOk) { if (error) *error = QObject::tr("样式保存失败：%1").arg(detail); return false; }
  *files << style;
  return true;
}
bool writeLayerById(QgsProject *project, const QString &layerId, const QString &directory,
                    QStringList *files, QVariantMap *description, QString *error) {
  if (!project) {
    if (error)
      *error = QObject::tr("图件工程缺失");
    return false;
  }
  auto *layer = project->mapLayer(layerId);
  if (!layer) {
    if (error)
      *error = QObject::tr("图层缺失：%1").arg(layerId);
    return false;
  }
  if (!writeLayer(layer, directory, files, description, error))
    return false;
  if (description) {
    description->insert("qgis_layer_id", layer->id());
    description->insert("title", layer->name());
  }
  return true;
}
bool write(const QString &source, const QString &destination,
           paleo::exporting::DataFormat format, QStringList *files,
           QVariantMap *description, QString *error,
           const std::function<bool()> &cancel) {
  using F = paleo::exporting::DataFormat;
  const auto fail = [error](const QString &message) { if (error) *error = message; return false; };
  if (cancel && cancel()) return fail(QObject::tr("导出已取消"));
  const auto stem = QDir(destination).filePath("data");
  // 目标目录可能是复用/预置的：先登记既有文件，结束时只收本次写出的产物
  // （调用方对 files 逐项算 sha256 写 manifest，无关文件不能混进来）。
  const QDir destinationDir(destination);
  const QStringList preexisting = destinationDir.entryList(QDir::Files);
  QgsRasterLayer raster(source, QStringLiteral("data"), QStringLiteral("gdal"));
  if (raster.isValid()) {
    if (format != F::Exchange && format != F::GeoTiff)
      return fail(QObject::tr("栅格数值数据请选择 GeoTIFF 或自动交换格式"));
    auto *provider = raster.dataProvider();
    QgsRasterPipe pipe;
    if (!pipe.set(provider->clone())) return fail(QObject::tr("无法创建数值栅格读取管线"));
    QgsRasterFileWriter writer(stem + ".tif");
    writer.setOutputFormat("GTiff");
    writer.setCreationOptions({"COMPRESS=DEFLATE"});
    const auto result = writer.writeRaster(&pipe, provider->xSize(), provider->ySize(),
        raster.extent(), raster.crs(), QgsCoordinateTransformContext());
    if (result != Qgis::RasterFileWriterResult::Success)
      return fail(QObject::tr("GeoTIFF 写出失败（%1）").arg(int(result)));
    QgsRasterLayer check(stem + ".tif", "check", "gdal");
    if (!check.isValid() || check.width() != raster.width() || check.height() != raster.height() ||
        check.bandCount() != raster.bandCount())
      return fail(QObject::tr("GeoTIFF 回读或栅格尺寸校验失败"));
    QVariantList bands;
    for (int band = 1; band <= raster.bandCount(); ++band) {
      bands.append(QVariantMap{{"band", band}, {"type", int(provider->dataType(band))},
          {"has_nodata", provider->sourceHasNoDataValue(band)},
          {"nodata", provider->sourceHasNoDataValue(band) ? QVariant(provider->sourceNoDataValue(band)) : QVariant()}});
    }
    *description = {{"format", "GeoTIFF"}, {"crs_wkt", raster.crs().toWkt()},
        {"width", raster.width()}, {"height", raster.height()}, {"bands", bands},
        {"extent", raster.extent().toString()}, {"values", "numeric"}};
    *files << stem + ".tif";
    return !(cancel && cancel());
  }
  if (format == F::GeoTiff) return fail(QObject::tr("该版本不是可读取的数值栅格"));
  GDALDatasetH dataset = GDALOpenEx(source.toUtf8().constData(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
  if (!dataset) return fail(QObject::tr("该格式没有可用的交换格式写出器，请选择保持原格式"));
  QStringList names;
  for (int i = 0; i < GDALDatasetGetLayerCount(dataset); ++i)
    names << QString::fromUtf8(OGR_L_GetName(GDALDatasetGetLayer(dataset, i)));
  GDALClose(dataset);
  if (names.isEmpty()) return fail(QObject::tr("没有可导出的矢量或表数据"));
  const QString driver = format == F::GeoJson ? "GeoJSON" : format == F::Shapefile ? "ESRI Shapefile" :
      format == F::Csv ? "CSV" : "GPKG";
  const QString extension = format == F::GeoJson ? ".geojson" : format == F::Shapefile ? ".shp" :
      format == F::Csv ? ".csv" : ".gpkg";
  QVariantList layers;
  for (int i = 0; i < names.size(); ++i) {
    if (cancel && cancel()) return fail(QObject::tr("导出已取消"));
    QgsVectorLayer layer(source + "|layername=" + names[i], names[i], "ogr");
    if (!layer.isValid()) return fail(QObject::tr("无法读取子图层：%1").arg(names[i]));
    QgsVectorFileWriter::SaveVectorOptions options;
    options.driverName = driver;
    options.fileEncoding = "UTF-8";
    options.layerName = names[i];
    if (driver == "CSV") options.layerOptions = {"GEOMETRY=AS_WKT", "CREATE_CSVT=YES"};
    if (driver == "GeoJSON") {
      const QgsCoordinateReferenceSystem wgs84("EPSG:4326");
      options.ct = QgsCoordinateTransform(layer.crs(), wgs84, QgsCoordinateTransformContext());
      if (!layer.crs().isValid() || !options.ct.isValid())
        return fail(QObject::tr("GeoJSON 需要有效的经纬度变换；未配准数据请选择 GeoPackage"));
      options.datasourceOptions = {"RFC7946=YES"};
    }
    const auto path = (driver == "GPKG" ? stem : stem + QStringLiteral("_%1").arg(i + 1)) + extension;
    if (driver == "GPKG" && i > 0)
      options.actionOnExistingFile = QgsVectorFileWriter::CreateOrOverwriteLayer;
    QString detail;
    if (QgsVectorFileWriter::writeAsVectorFormatV3(&layer, path, QgsCoordinateTransformContext(), options, &detail) != QgsVectorFileWriter::NoError)
      return fail(QObject::tr("%1 写出失败：%2").arg(names[i], detail));
    QgsVectorLayer check(driver == "GPKG" ? path + "|layername=" + names[i] : path, "check", "ogr");
    if (!check.isValid() || check.featureCount() != layer.featureCount())
      return fail(QObject::tr("%1 回读或记录数校验失败").arg(names[i]));
    QVariantList fields;
    for (const auto &field : layer.fields())
      fields.append(QVariantMap{{"name", field.name()}, {"type", field.typeName()}, {"comment", field.comment()}});
    layers.append(QVariantMap{{"name", names[i]}, {"features", layer.featureCount()},
        {"fields", fields}, {"crs_wkt", check.crs().toWkt()}, {"geometry", int(layer.wkbType())}});
    if (!files->contains(path)) *files << path;
  }
  if (driver == "ESRI Shapefile") description->insert("warning", QObject::tr("Shapefile 可能截短字段名或改变字段类型；无损交换优先使用 GeoPackage"));
  description->insert("format", driver);
  description->insert("layers", layers);
  // Includes .dbf/.shx/.prj/.cpg and other files actually written by the driver.
  // 复用目录里预置的文件不是本次产物，排除（B.12）。
  for (const auto &file : destinationDir.entryList(QDir::Files)) {
    if (preexisting.contains(file))
      continue;
    const auto path = destinationDir.filePath(file);
    if (!files->contains(path)) *files << path;
  }
  return true;
}
}
