// 层：QGIS 封装
#include "seismicsectiontool.h"

#include <QCursor>
#include <QKeyEvent>
#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>
#include <qgssnappingconfig.h>
#include <qgssnappingutils.h>
#include <qgsvertexmarker.h>

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
    m_snapMarker = new QgsVertexMarker(canvas);
    m_snapMarker->setIconType(QgsVertexMarker::ICON_CIRCLE);
    m_snapMarker->setColor(QColor(QStringLiteral("#D08C1B")));
    m_snapMarker->setIconSize(12);
    m_snapMarker->setPenWidth(2);
    m_snapMarker->hide();
  }
}

SeismicSectionTool::~SeismicSectionTool()
{
  // 橡皮带与顶点标记属 canvas scene（决策①，PreviewMapCanvas 同款）：canvas
  // 先死时 delete 即悬空 SIGSEGV（wave/deepen-perf D2 生命周期测试实证）——
  // scene 自会清理自己的项，工具不越权。
}

void SeismicSectionTool::setSnapWells(const QVector<wellsection::Well> &wells)
{
  m_snapWells = wells;
}

void SeismicSectionTool::activate()
{
  m_points.clear();
  m_vertexWellIds.clear();
  m_hoveredWell = -1;
  clearSnapFeedback();
  if (m_rubberBand)
    m_rubberBand->reset(Qgis::GeometryType::Line);
  QgsMapTool::activate();
}

void SeismicSectionTool::deactivate()
{
  m_points.clear();
  m_vertexWellIds.clear();
  m_hoveredWell = -1;
  clearSnapFeedback();
  if (m_rubberBand)
    m_rubberBand->reset(Qgis::GeometryType::Line);
  QgsMapTool::deactivate();
}

double SeismicSectionTool::snapToleranceMapUnits() const
{
  // 容差取画布 QGIS 吸附设置的像素容差；未启用/未配置时回退 12 像素。
  double pixels = 12.0;
  if (canvas() && canvas()->snappingUtils())
  {
    const QgsSnappingConfig cfg = canvas()->snappingUtils()->config();
    if (cfg.enabled() && cfg.tolerance() > 0)
      pixels = cfg.tolerance();
  }
  return pixels * (canvas() ? canvas()->mapUnitsPerPixel() : 1.0);
}

void SeismicSectionTool::clearSnapFeedback()
{
  if (m_snapMarker)
    m_snapMarker->hide();
  if (canvas())
    canvas()->setToolTip(QString()); // QgsMapCanvas 无 unsetToolTip——QWidget 清空口径
}

void SeismicSectionTool::canvasPressEvent(QgsMapMouseEvent *e)
{
  if (e->button() == Qt::LeftButton)
  {
    if (m_hoveredWell >= 0 && m_hoveredWell < m_snapWells.size())
    {
      const wellsection::Well &w = m_snapWells.at(m_hoveredWell);
      // 连续同井忽略：不产生重复顶点与重复井序项
      if (m_vertexWellIds.isEmpty() || m_vertexWellIds.last() != w.id)
      {
        m_points.append(QgsPointXY(w.x, w.y));
        m_vertexWellIds.append(w.id);
      }
    }
    else
    {
      m_points.append(e->mapPoint());
      m_vertexWellIds.append(QString());
    }
    redraw();
  }
  else if (e->button() == Qt::RightButton)
  {
    if (m_points.size() >= 2)
    {
      const QVector<QgsPointXY> captured = m_points;
      QStringList wellIds;
      for (const QString &id : m_vertexWellIds)
        if (!id.isEmpty())
          wellIds << id;
      m_points.clear();
      m_vertexWellIds.clear();
      if (m_rubberBand)
        m_rubberBand->reset(Qgis::GeometryType::Line);
      emit sectionPathCaptured(captured, wellIds);
    }
    else
    {
      m_points.clear();
      m_vertexWellIds.clear();
      if (m_rubberBand)
        m_rubberBand->reset(Qgis::GeometryType::Line);
    }
  }
}

void SeismicSectionTool::canvasMoveEvent(QgsMapMouseEvent *e)
{
  const auto pt = e->mapPoint();
  m_hoveredWell = m_snapWells.isEmpty()
                      ? -1
                      : wellsection::nearestWellWithin(m_snapWells, pt.x(), pt.y(),
                                                       snapToleranceMapUnits());
  QgsPointXY hover = pt;
  if (m_hoveredWell >= 0)
  {
    const wellsection::Well &w = m_snapWells.at(m_hoveredWell);
    hover = QgsPointXY(w.x, w.y);
    if (m_snapMarker)
    {
      m_snapMarker->setCenter(hover);
      m_snapMarker->show();
    }
    if (canvas())
      canvas()->setToolTip(w.name.isEmpty() ? w.id : w.name);
  }
  else
  {
    clearSnapFeedback();
  }
  if (m_points.isEmpty() || !m_rubberBand)
    return;
  redraw(&hover);
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
    m_vertexWellIds.clear();
    m_hoveredWell = -1;
    clearSnapFeedback();
    redraw();
    emit captureCancelled();
    if (canvas())
      canvas()->unsetMapTool(this);
  } else if (e->key() == Qt::Key_Backspace || e->key() == Qt::Key_Delete) {
    if (!m_points.isEmpty())
    {
      m_points.removeLast();
      m_vertexWellIds.removeLast();
    }
    redraw();
  } else
    QgsMapTool::keyPressEvent(e);
}
