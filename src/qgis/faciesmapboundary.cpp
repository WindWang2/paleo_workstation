// 层：QGIS 封装
#include "faciesmapboundary.h"
#include "faciespartitioneditor.h"
#include <QCryptographicHash>
#include <QPainter>
#include <qgsfeatureiterator.h>
#include <qgsvectorlayer.h>

namespace FaciesMapBoundary {
QVariantMap inspect(QgsMapLayer *source, QString *error) {
  auto *layer = qobject_cast<QgsVectorLayer *>(source);
  const auto fail = [error](const QString &why) {
    if (error) *error = why;
    return QVariantMap();
  };
  if (!layer || !layer->isValid() || layer->geometryType() != Qgis::GeometryType::Polygon)
    return fail(QObject::tr("请选择已登记的面状工区边界。"));
  const auto crs = layer->crs();
  if (!crs.isValid() || crs.mapUnits() != Qgis::DistanceUnit::Meters)
    return fail(QObject::tr("工区边界需要已知的米制坐标系，请先完成配准。"));
  if (layer->isModified())
    return fail(QObject::tr("工区边界有未提交的修改，请先保存边界。"));
  QVector<QgsGeometry> polygons;
  QgsFeature feature;
  auto it = layer->getFeatures(QgsFeatureRequest().setNoAttributes());
  while (it.nextFeature(feature)) {
    const auto geometry = feature.geometry();
    if (geometry.isEmpty() || !geometry.isGeosValid())
      return fail(QObject::tr("工区边界含有空或无效的面，请先修复。"));
    polygons.append(geometry);
  }
  auto geometry = QgsGeometry::unaryUnion(polygons);
  if (geometry.type() != Qgis::GeometryType::Polygon || geometry.isEmpty() || !geometry.isGeosValid())
    return fail(QObject::tr("无法建立有效的工区覆盖范围。"));
  geometry.normalize();
  const auto wkb = geometry.asWkb();
  const auto crsWkt = crs.toWkt();
  QCryptographicHash hash(QCryptographicHash::Sha256);
  hash.addData(crsWkt.toUtf8());
  hash.addData(wkb);
  return {{"wkb", wkb}, {"crs", crsWkt}, {"crs_label", crs.userFriendlyIdentifier()},
          {"digest", hash.result().toHex()}, {"name", layer->name()}};
}

QImage thumbnail(const QByteArray &wkb, QSize size, const QColor &color) {
  QgsGeometry geometry;
  geometry.fromWkb(wkb);
  const auto bounds = geometry.boundingBox();
  if (bounds.isEmpty() || size.width() < 4 || size.height() < 4) return {};
  QImage image(size, QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  painter.setRenderHint(QPainter::Antialiasing);
  const double scale = qMin((size.width() - 4) / bounds.width(), (size.height() - 4) / bounds.height());
  painter.translate(size.width() * .5, size.height() * .5);
  painter.scale(scale, -scale);
  painter.translate(-bounds.center().x(), -bounds.center().y());
  QPen outline(color);
  outline.setCosmetic(true);
  painter.setPen(outline);
  QColor fill(color);
  fill.setAlpha(48);
  painter.setBrush(fill);
  geometry.draw(painter);
  return image;
}

bool seed(QgsMapLayer *draft, const QVariantMap &boundary, int code, QString *error) {
  auto *layer = qobject_cast<QgsVectorLayer *>(draft);
  if (!layer || layer->featureCount() != 0 ||
      layer->crs() != QgsCoordinateReferenceSystem::fromWkt(boundary.value("crs").toString())) {
    if (error) *error = QObject::tr("工作副本已变化，请重新选择编图范围。");
    return false;
  }
  QgsGeometry geometry;
  geometry.fromWkb(boundary.value("wkb").toByteArray());
  return FaciesPartitionEditor::paint(layer, geometry, code, error);
}
}
