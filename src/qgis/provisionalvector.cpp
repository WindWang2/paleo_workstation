// 层：QGIS 封装
#include "provisionalvector.h"
#include "../catalog/datacatalog.h"
#include "../io/geojsonaffine.h"

#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QTransform>
#include <memory>
#include <qgsgeometry.h>
#include <qgsvectordataprovider.h>
#include <qgsvectorfilewriter.h>
#include <qgsvectorlayer.h>

namespace {
QTransform affineMatrix(GeoAffineParams params)
{
  const double tx = params.tx;
  const double ty = params.ty;
  params.tx = params.ty = 0.0;
  double xx, xy, yx, yy;
  geoAffineApply(params, 1.0, 0.0, &xx, &xy);
  geoAffineApply(params, 0.0, 1.0, &yx, &yy);
  return QTransform(xx, xy, yx, yy, tx, ty);
}

bool writeFeatures(QgsVectorLayer &layer, const QString &destination,
                   const QTransform &affine, bool sourceIsPackage,
                   int *featureCount, QString *error)
{
  const auto keys = sourceIsPackage ? layer.dataProvider()->pkAttributeIndexes() : QgsAttributeList{};
  const auto sourceFields = layer.fields();
  QgsFields fields;
  QgsAttributeList sourceIndexes;
  for (int i = 0; i < sourceFields.size(); ++i)
  {
    if (keys.contains(i))
      continue;
    fields.append(sourceFields.at(i));
    sourceIndexes.append(i);
  }

  // GeoPackage's internal columns must not overwrite attributes such as fid or geom.
  const auto freeName = [&fields](QString name) {
    while (fields.lookupField(name) >= 0)
      name.prepend(QLatin1Char('_'));
    return name;
  };
  QgsVectorFileWriter::SaveVectorOptions options;
  options.driverName = QStringLiteral("GPKG");
  options.layerName = QStringLiteral("features");
  options.fileEncoding = QStringLiteral("UTF-8");
  options.layerOptions = {
      QStringLiteral("FID=%1").arg(freeName(QStringLiteral("fid"))),
      QStringLiteral("GEOMETRY_NAME=%1").arg(freeName(QStringLiteral("geom")))};
  const auto crs = QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt());
  std::unique_ptr<QgsVectorFileWriter> writer(QgsVectorFileWriter::create(
      destination, fields, layer.wkbType(), crs, QgsCoordinateTransformContext(), options));
  if (!writer || writer->hasError() != QgsVectorFileWriter::NoError)
  {
    if (error)
      *error = writer ? writer->errorMessage() : QObject::tr("无法创建临时配准图件");
    return false;
  }

  auto iterator = layer.getFeatures();
  QgsFeature sourceFeature;
  int count = 0;
  while (iterator.nextFeature(sourceFeature))
  {
    auto geometry = sourceFeature.geometry();
    if (!geometry.isEmpty() &&
        (geometry.transform(affine) != Qgis::GeometryOperationResult::Success ||
         !geometry.boundingBox().isFinite()))
    {
      if (error)
        *error = QObject::tr("无法变换临时配准要素的坐标");
      return false;
    }
    QgsFeature converted(fields);
    converted.setGeometry(geometry);
    QgsAttributes attributes;
    attributes.reserve(sourceIndexes.size());
    for (int index : sourceIndexes)
      attributes.append(sourceFeature.attribute(index));
    converted.setAttributes(attributes);
    if (!writer->addFeature(converted))
    {
      if (error)
        *error = writer->lastError();
      return false;
    }
    ++count;
  }
  if (count == 0 || !writer->flushBuffer())
  {
    if (error)
      *error = count == 0 ? QObject::tr("矢量没有要素") : writer->lastError();
    return false;
  }
  if (featureCount)
    *featureCount = count;
  return true;
}
}

bool ProvisionalVector::write(const QString &source, const QString &destination,
                              const GeoAffineParams &params, int *featureCount, QString *error)
{
  if (!geoAffineParamsValid(params, error))
    return false;
  const QFileInfo sourceFile(source.section(QLatin1Char('|'), 0, 0));
  constexpr qint64 maxBytes = 500LL * 1024 * 1024;
  if (sourceFile.size() > maxBytes || QFile::exists(destination))
  {
    if (error)
      *error = QObject::tr("临时配准源超过 500 MB，或目标文件已存在");
    return false;
  }
  QgsVectorLayer layer(source, QStringLiteral("provisional-source"), QStringLiteral("ogr"));
  if (!layer.isValid())
  {
    if (error)
      *error = QObject::tr("无法读取临时配准的源矢量");
    return false;
  }
  const bool ok = writeFeatures(layer, destination, affineMatrix(params),
                                sourceFile.suffix().compare(QLatin1String("gpkg"), Qt::CaseInsensitive) == 0,
                                featureCount, error);
  if (!ok)
    QFile::remove(destination);
  return ok;
}
