// 层：功能
#include "seismicsectiontool.h"

#include <QCursor>
#include <QKeyEvent>
#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>

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
    redraw();
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

  const auto pt = e->mapPoint();
  redraw(&pt);
}

void SeismicSectionTool::redraw(const QgsPointXY *hover) {
  if (!m_rubberBand)
    return;
  m_rubberBand->reset(Qgis::GeometryType::Line);
  for (const auto &p : m_points)
    m_rubberBand->addPoint(p, false);
  if (hover)
    m_rubberBand->addPoint(*hover, false);
  m_rubberBand->show();
  m_rubberBand->updatePosition();
}
void SeismicSectionTool::keyPressEvent(QKeyEvent *e) {
  if (e->key() == Qt::Key_Escape) {
    m_points.clear();
    redraw();
    if (canvas())
      canvas()->unsetMapTool(this);
  } else if (e->key() == Qt::Key_Backspace || e->key() == Qt::Key_Delete) {
    if (!m_points.isEmpty())
      m_points.removeLast();
    redraw();
  } else
    QgsMapTool::keyPressEvent(e);
}
