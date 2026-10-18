// 层：QGIS 封装
#include "paleomapsource.h"
#include <QFileInfo>
#include <QSet>
#include <cmath>
#include <qgsvectorlayer.h>
#include <qgsrasterlayer.h>
#include <qgsgeometry.h>
namespace PaleoMapSource {
QVariantMap inspect(const QString &path, QString *error) {
  const auto extension = QFileInfo(path).suffix().toLower();
  if (QStringList{"pdf", "svg"}.contains(extension)) return {{"source_type", "document"}};
  if (QStringList{"shp", "gpkg", "geojson", "json"}.contains(extension)) {
    QgsVectorLayer layer(path, "source", "ogr");
    if (!layer.isValid()) { if (error) *error = QObject::tr("矢量图件无法读取：%1").arg(path); return {}; }
    QVariantMap result{{"source_type", "vector"}, {"spatial", layer.crs().isValid()}, {"crs", layer.crs().toWkt()},
        {"feature_count", layer.featureCount()}, {"polygon", layer.geometryType() == Qgis::GeometryType::Polygon}};
    return result;
  }
  QgsRasterLayer layer(path, "source", "gdal");
  if (!layer.isValid()) { if (error) *error = QObject::tr("栅格图件无法读取：%1").arg(path); return {}; }
  return {{"source_type", "raster"}, {"spatial", layer.crs().isValid()}, {"crs", layer.crs().toWkt()}};
}
double surveyArea(const QVector<QVector<QPair<double, double>>> &polygons) {
  QVector<QgsGeometry> shapes;
  for (const auto &points : polygons) {
    QgsPolylineXY ring; for (const auto &p : points) {
      if (!std::isfinite(p.first) || !std::isfinite(p.second)) { ring.clear(); break; }
      ring << QgsPointXY(p.first, p.second);
    }
    if (ring.size() < 3) continue;
    if (ring.first() != ring.last()) ring << ring.first();
    const auto geometry = QgsGeometry::fromPolygonXY({ring});
    if (geometry.isGeosValid() && geometry.area() > 0) shapes << geometry;
  }
  if (shapes.isEmpty()) return -1;
  const auto combined = QgsGeometry::unaryUnion(shapes);
  return combined.isNull() ? -1 : combined.area() / 1000000.;
}
}
