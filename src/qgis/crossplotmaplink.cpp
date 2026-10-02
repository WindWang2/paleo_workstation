// 层：QGIS 封装
#include "crossplotmaplink.h"
#include "catalog/datacatalog.h"
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
  return true;
}
bool CrossplotMapLink::locate(const SampleSet &s, int sample, QString *e) {
  double x, y;
  if (!coordinate(s, sample, &x, &y, e))
    return false;
  auto extent = m_canvas->extent();
  double w = extent.width(), h = extent.height();
  if (w <= 0 || h <= 0) {
    w = h = 1000;
  }
  m_canvas->setExtent(QgsRectangle(x - w / 2, y - h / 2, x + w / 2, y + h / 2));
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
