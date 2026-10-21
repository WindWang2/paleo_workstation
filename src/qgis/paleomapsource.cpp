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
  // C-11（full-audit-20261020）：角点是 SEG-Y 道头原值（单位随工区 CRS），本层
  // 无 CRS 语境——度制坐标（全部 |x|≤360 且 |y|≤90）下 /1e6 得到的「km²」
  // 是量级失真 1e10 级的伪面积（CRS_ASSUMPTION_AUDIT §2.3 的冻结期量级闸
  // 前置防线）。如实返回 -1，消费端按「未登记边界」兜底呈现。
  bool sawPoint = false, degreeLike = true;
  for (const auto &poly : polygons)
    for (const auto &p : poly) {
      if (!std::isfinite(p.first) || !std::isfinite(p.second)) continue;
      sawPoint = true;
      if (std::abs(p.first) > 360.0 || std::abs(p.second) > 90.0)
        degreeLike = false;
    }
  if (sawPoint && degreeLike) return -1;
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
