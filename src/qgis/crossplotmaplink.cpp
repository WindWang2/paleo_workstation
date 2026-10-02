// 层：QGIS 封装
#include "crossplotmaplink.h"
#include "catalog/datacatalog.h"
#include <cmath>
#include <qgscoordinatetransform.h>
#include <qgsgeometry.h>
#include <qgsmapcanvas.h>
#include <qgsproject.h>
#include <qgsrectangle.h>
#include <qgsrubberband.h>
namespace paleo::crossplot {
CrossplotMapLink::CrossplotMapLink(QgsMapCanvas *c, QObject *p)
    : QObject(p), m_canvas(c) {}
CrossplotMapLink::~CrossplotMapLink() { clear(); }
void CrossplotMapLink::clear() {
  delete m_band;
  m_band = nullptr;
}
bool CrossplotMapLink::coordinate(const SampleSet &s, int index, double *x,
                                  double *y, QString *e) const {
  auto fail = [&](const QString &t) {
    if (e)
      *e = t;
    return false;
  };
  if (!m_canvas || index < 0 || index >= s.locations.size() ||
      !s.locations[index].hasXY)
    return fail(tr("样本没有可定位的地图坐标"));
  const auto &loc = s.locations[index];
  if (!std::isfinite(loc.x) || !std::isfinite(loc.y))
    return fail(tr("样本坐标不是有限值"));
  auto source = QgsCoordinateReferenceSystem::fromWkt(
      s.grid.crs.isEmpty() ? DataCatalog::localGridCrsWkt() : s.grid.crs);
  if (!source.isValid())
    return fail(tr("样本 CRS 无效"));
  QgsPointXY point(loc.x, loc.y);
  try {
    if (source != m_canvas->mapSettings().destinationCrs())
      point = QgsCoordinateTransform(source,
                                     m_canvas->mapSettings().destinationCrs(),
                                     QgsProject::instance())
                  .transform(point);
  } catch (const QgsCsException &ex) {
    return fail(ex.what());
  }
  *x = point.x();
  *y = point.y();
  if (!std::isfinite(*x) || !std::isfinite(*y))
    return fail(tr("坐标转换没有返回有限位置"));
  return true;
}
bool CrossplotMapLink::locate(const SampleSet &s, int sample, QString *e) {
  double x, y;
  if (!coordinate(s, sample, &x, &y, e))
    return false;
  // A well gets a 1km window in the local engineering grid. Raster locations
  // get a 16-pixel window. Transform its corners as well as its center so
  // projected/geographic destination units never inherit a metre span.
  const double dx =
      s.grid.spatial
          ? 8 * (std::abs(s.grid.transform[1]) + std::abs(s.grid.transform[2]))
          : 500;
  const double dy =
      s.grid.spatial
          ? 8 * (std::abs(s.grid.transform[4]) + std::abs(s.grid.transform[5]))
          : 500;
  SampleSet footprint;
  footprint.grid = s.grid;
  for (double sx : {-dx, dx})
    for (double sy : {-dy, dy}) {
      auto loc = s.locations[sample];
      loc.x += sx;
      loc.y += sy;
      footprint.locations << loc;
    }
  QgsRectangle extent(x, y, x, y);
  for (int i = 0; i < footprint.locations.size(); ++i) {
    double px, py;
    if (!coordinate(footprint, i, &px, &py, e))
      return false;
    extent.combineExtentWith(px, py);
  }
  m_canvas->setExtent(extent);
  m_canvas->refresh();
  return true;
}
bool CrossplotMapLink::highlight(const SampleSet &s,
                                 const QVector<int> &indices, QString *error) {
  clear();
  if (indices.isEmpty())
    return true;
  if (!m_canvas) {
    if (error)
      *error = tr("地图画布不可用");
    return false;
  }
  const auto source = QgsCoordinateReferenceSystem::fromWkt(
      s.grid.crs.isEmpty() ? DataCatalog::localGridCrsWkt() : s.grid.crs);
  if (!source.isValid()) {
    if (error)
      *error = tr("样本 CRS 无效");
    return false;
  }
  QgsCoordinateTransform transform(
      source, m_canvas->mapSettings().destinationCrs(), QgsProject::instance());
  QgsMultiPointXY points;
  points.reserve(indices.size());
  try {
    for (int index : indices) {
      if (index < 0 || index >= s.locations.size() ||
          !s.locations[index].hasXY) {
        if (error)
          *error = tr("选区含不可定位样本");
        return false;
      }
      const auto &loc = s.locations[index];
      QgsPointXY point(loc.x, loc.y);
      if (source != m_canvas->mapSettings().destinationCrs())
        point = transform.transform(point);
      points << point;
    }
  } catch (const QgsCsException &ex) {
    if (error)
      *error = ex.what();
    return false;
  }
  m_band = new QgsRubberBand(m_canvas, Qgis::GeometryType::Point);
  m_band->setColor(m_canvas->palette().highlight().color());
  m_band->setWidth(2);
  m_band->setIcon(QgsRubberBand::ICON_CIRCLE);
  m_band->setIconSize(4);
  m_band->setToGeometry(QgsGeometry::fromMultiPointXY(points), nullptr);
  return true;
}
} // namespace paleo::crossplot
