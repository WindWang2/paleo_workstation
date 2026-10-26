// 层：数据
#include "welltable.h"
#include "../metadata/nativereference.h"
#include <QObject>
#include <ogr_spatialref.h>
#include <memory>
#include <cmath>
namespace paleo::io {
bool projectWellCoordinates(QVector<WellHeadRecord> *heads, const QString &source, const QString &destination, QString *error)
{
  // Depth-only stratification tables have no horizontal coordinates to project.
  if (heads->isEmpty() || (source.isEmpty() && destination.isEmpty())) return true;
  if (source.isEmpty()) {
    if (error) *error = QObject::tr("井点坐标系未知，不能转换到工程坐标系");
    return false;
  }
  OGRSpatialReference from, to;
  const auto target = destination.isEmpty() ? source : destination;
  if (from.SetFromUserInput(source.toUtf8().constData(), OGRSpatialReference::SET_FROM_USER_INPUT_LIMITATIONS_get()) != OGRERR_NONE || to.SetFromUserInput(target.toUtf8().constData(), OGRSpatialReference::SET_FROM_USER_INPUT_LIMITATIONS_get()) != OGRERR_NONE) { if (error) *error = QObject::tr("井点坐标系无效"); return false; }
  from.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER); to.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  // EPSG:2436 has no zone-number prefix (false easting = 500000 m).
  // Refuse survey prefixes / reversed axes even when no reprojection is needed.
  // Do not infer a different source CRS or silently remove digits.
  if (QString::fromLatin1(from.GetAuthorityCode(nullptr)) == QLatin1String("2436")) {
    QStringList invalid;
    for (const auto &h : *heads)
      if (!std::isfinite(h.x) || !std::isfinite(h.y) || std::abs(h.x) > 2'000'000 || std::abs(h.y) > 10'000'000)
        invalid << QObject::tr("%1（东坐标 %2，北坐标 %3）").arg(h.name).arg(h.x, 0, 'g', 12).arg(h.y, 0, 'g', 12);
    if (!invalid.isEmpty()) {
      if (error) *error = QObject::tr("EPSG:2436 坐标异常：%1。请核实横纵轴对应、带号前缀和来源坐标系；系统不会自动去带号或交换数值。").arg(invalid.join(QStringLiteral("；")));
      return false;
    }
  }
  if (destination.isEmpty() || sameNativeCrs(source, destination)) return true;
  std::unique_ptr<OGRCoordinateTransformation, decltype(&OCTDestroyCoordinateTransformation)> ct(OGRCreateCoordinateTransformation(&from, &to), OCTDestroyCoordinateTransformation);
  if (!ct) { if (error) *error = QObject::tr("无法转换井点坐标系"); return false; }
  for (auto &h : *heads)
    if (!ct->Transform(1, &h.x, &h.y) || !std::isfinite(h.x) || !std::isfinite(h.y)) { if (error) *error = QObject::tr("井 %1 的坐标无法转换到工程坐标系").arg(h.name); return false; }
  return true;
}
}
