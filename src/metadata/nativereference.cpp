// 层：数据
#include "nativereference.h"
#include <ogr_spatialref.h>
#include <cmath>
#include <algorithm>
#include <memory>
bool validNativeCrs(const QString &crs)
{
  OGRSpatialReference srs;
  return !crs.isEmpty() && srs.SetFromUserInput(crs.toUtf8().constData()) == OGRERR_NONE &&
         (srs.IsProjected() || srs.IsGeographic());
}
bool sameNativeCrs(const QString &first, const QString &second)
{
  OGRSpatialReference a, b;
  if (a.SetFromUserInput(first.toUtf8().constData()) != OGRERR_NONE || b.SetFromUserInput(second.toUtf8().constData()) != OGRERR_NONE) return false;
  a.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER); b.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  return a.IsSame(&b);
}
QString metricNativeCrs(const QString &source, double x, double y)
{
  OGRSpatialReference crs;
  if (crs.SetFromUserInput(source.toUtf8().constData()) != OGRERR_NONE || !crs.IsGeographic()) return source;
  double lon = 0, lat = 0;
  if (!nativeToGeographic(source, x, y, &lon, &lat) || lat < -80 || lat > 84) return QStringLiteral("EPSG:3857");
  const int zone = std::clamp(static_cast<int>(std::floor((lon + 180) / 6)) + 1, 1, 60);
  return QStringLiteral("EPSG:%1").arg((lat >= 0 ? 32600 : 32700) + zone);
}
bool nativeToGeographic(const QString &crs, double x, double y, double *lon, double *lat)
{
  OGRSpatialReference source, destination;
  if (source.SetFromUserInput(crs.toUtf8().constData()) != OGRERR_NONE || destination.importFromEPSG(4326) != OGRERR_NONE) return false;
  source.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  destination.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  std::unique_ptr<OGRCoordinateTransformation, decltype(&OCTDestroyCoordinateTransformation)> transform(
      OGRCreateCoordinateTransformation(&source, &destination), OCTDestroyCoordinateTransformation);
  if (!transform || !transform->Transform(1, &x, &y) || !std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 180 || std::abs(y) >= 90) return false;
  if (lon) *lon = x;
  if (lat) *lat = y;
  return true;
}
