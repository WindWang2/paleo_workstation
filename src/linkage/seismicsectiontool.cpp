#include "seismicsectiontool.h"

#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>
#include <QCursor>

SeismicSectionTool::SeismicSectionTool(QgsMapCanvas *canvas)
  : QgsMapTool(canvas)
{
  setCursor(QCursor(Qt::CrossCursor));
  if (canvas)
  {
    m_rubberBand = new QgsRubberBand(canvas, Qgis::GeometryType::Line);
    m_rubberBand->setColor(QColor(QStringLiteral("#1B73D0")));
    m_rubberBand->setWidth(2);
    m_rubberBand->setLineStyle(Qt::DashLine);
  }
}

SeismicSectionTool::~SeismicSectionTool()
{
  delete m_rubberBand;
}

void SeismicSectionTool::activate()
{
  m_points.clear();
  if (m_rubberBand)
    m_rubberBand->reset(Qgis::GeometryType::Line);
  QgsMapTool::activate();
}

void SeismicSectionTool::deactivate()
{
  m_points.clear();
  if (m_rubberBand)
    m_rubberBand->reset(Qgis::GeometryType::Line);
  QgsMapTool::deactivate();
}

void SeismicSectionTool::canvasPressEvent(QgsMapMouseEvent *e)
{
  if (e->button() == Qt::LeftButton)
  {
    const QgsPointXY pt = e->mapPoint();
    m_points.append(pt);
    if (m_rubberBand)
    {
      m_rubberBand->addPoint(pt, true);
      m_rubberBand->show();
    }
  }
  else if (e->button() == Qt::RightButton)
  {
    if (m_points.size() >= 2)
    {
      const QVector<QgsPointXY> captured = m_points;
      m_points.clear();
      if (m_rubberBand)
        m_rubberBand->reset(Qgis::GeometryType::Line);
      emit sectionPathCaptured(captured);
    }
    else
    {
      m_points.clear();
      if (m_rubberBand)
        m_rubberBand->reset(Qgis::GeometryType::Line);
    }
  }
}

void SeismicSectionTool::canvasMoveEvent(QgsMapMouseEvent *e)
{
  if (m_points.isEmpty() || !m_rubberBand)
    return;

  // Update transient last point
  m_rubberBand->movePoint(e->mapPoint());
}
