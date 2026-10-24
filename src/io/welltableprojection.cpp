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
  if (heads->isEmpty() || destination.isEmpty() || sameNativeCrs(source, destination)) return true;
  if (source.isEmpty()) {
    if (error) *error = QObject::tr("井点坐标系未知，不能转换到工程坐标系");
    return false;
  }
  OGRSpatialReference from, to;
  if (from.SetFromUserInput(source.toUtf8().constData()) != OGRERR_NONE || to.SetFromUserInput(destination.toUtf8().constData()) != OGRERR_NONE) { if (error) *error = QObject::tr("井点坐标系无效"); return false; }
  from.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER); to.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  std::unique_ptr<OGRCoordinateTransformation, decltype(&OCTDestroyCoordinateTransformation)> ct(OGRCreateCoordinateTransformation(&from, &to), OCTDestroyCoordinateTransformation);
  if (!ct) { if (error) *error = QObject::tr("无法转换井点坐标系"); return false; }
  for (auto &h : *heads)
    if (!ct->Transform(1, &h.x, &h.y) || !std::isfinite(h.x) || !std::isfinite(h.y)) { if (error) *error = QObject::tr("井 %1 的坐标无法转换到工程坐标系").arg(h.name); return false; }
  return true;
}
}
