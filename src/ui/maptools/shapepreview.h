// 层：视图
#pragma once
#include "capturehelpers.h"
#include <qgscompoundcurve.h>
#include <qgscircle.h>
#include <qgsellipse.h>
#include <qgsgeometry.h>
#include <qgsrubberband.h>
#include <qgssnapindicator.h>
#include <qgsmapmouseevent.h>
#include <memory>

// 规则形预览和提交共用同一份地图 CRS 几何，避免预览/落盘轮廓分叉。
class PaleoShapePreview
{
  public:
    explicit PaleoShapePreview(QgsMapCanvas *canvas)
      : m_canvas(canvas), m_band(std::make_unique<QgsRubberBand>(canvas, Qgis::GeometryType::Polygon)),
        m_snap(std::make_unique<QgsSnapIndicator>(canvas))
    {
      m_band->setLineStyle(Qt::DashLine);
    }
    static QgsGeometry geometry(const QString &kind, const QgsCompoundCurve *curve,
                                 const QgsVectorLayer *layer, const QgsMapCanvas *canvas,
                                 const QgsPointXY *cursor)
    {
      if (!curve)
        return {};
      std::unique_ptr<QgsCurve> copy(curve->clone());
      if (!CaptureHelpers::transformToCanvas(*copy, layer, canvas))
        return {};
      QgsPointSequence points;
      copy->points(points);
      const int minimum = kind == QLatin1String("ellipse") ? 2 : 1;
      if (points.size() < minimum)
        return {};
      QgsPoint endpoint;
      if (cursor)
        endpoint = QgsPoint(cursor->x(), cursor->y());
      else if (points.size() > minimum)
        endpoint = points.at(minimum);
      else
        return {};
      if (kind == QLatin1String("rect"))
      {
        if (qgsDoubleNear(points.first().x(), endpoint.x()) || qgsDoubleNear(points.first().y(), endpoint.y()))
          return {};
        return QgsGeometry::fromRect(QgsRectangle(QgsPointXY(points.first()), QgsPointXY(endpoint)));
      }
      if (kind == QLatin1String("circle"))
      {
        const double radius = points.first().distance(endpoint);
        if (!(radius > 0))
          return {};
        return QgsGeometry(QgsCircle(points.first(), radius).toPolygon(24));
      }
      const QgsEllipse ellipse = QgsEllipse::fromCenter2Points(points.first(), points.at(1), endpoint);
      if (ellipse.isEmpty())
        return {};
      return QgsGeometry(ellipse.toPolygon(36));
    }
    void update(const QString &kind, const QgsCompoundCurve *curve, const QgsVectorLayer *layer,
                 QgsMapMouseEvent *event)
    {
      m_snap->setMatch(event->mapPointMatch());
      const QgsPointXY point = event->mapPoint();
      m_geometry = geometry(kind, curve, layer, m_canvas, &point);
      m_band->setToGeometry(m_geometry, nullptr);
    }
    QgsGeometry current() const { return m_geometry; }
    void clear()
    {
      m_geometry = {};
      m_band->reset(Qgis::GeometryType::Polygon);
      m_snap->setMatch(QgsPointLocator::Match());
    }
  private:
    QgsMapCanvas *m_canvas;
    std::unique_ptr<QgsRubberBand> m_band;
    std::unique_ptr<QgsSnapIndicator> m_snap;
    QgsGeometry m_geometry;
};
