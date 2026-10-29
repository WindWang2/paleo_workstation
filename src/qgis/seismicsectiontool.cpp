// 层：QGIS 封装
#include "seismicsectiontool.h"

#include <QCursor>
#include <QKeyEvent>
#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>
#include <qgssnappingconfig.h>
#include <qgssnappingutils.h>
#include <qgsvertexmarker.h>

#include "catalog/datacatalog.h"
#include "projectmapreference.h"

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
  m_snapWells.clear();
  if (!canvas())
  {
    m_snapWells = wells;
    return;
  }
  // 井口坐标是局部测网米制值（DataCatalog::localGridCrsWkt），而吸附比较与
  // 落点都发生在画布 CRS 上——配准工程（画布 CRS ≠ 测网）下不投影就是「度
  // vs 米」的跨框架比较：吸附静默永不命中，数值巧合命中时落点双重错位
  // （#795；SeismicMapLink::onSectionPathCaptured 还会从画布 CRS 变换回测
  // 网）。故注入时按画布目标 CRS 整体投影，投影失败的井移出吸附集；未配准
  // 工程（画布 CRS 与测网同源或无效）原样直通。
  const auto destination = canvas()->mapSettings().destinationCrs();
  const auto source = QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt());
  if (!destination.isValid() || destination == source ||
      destination.toWkt() == source.toWkt())
  {
    m_snapWells = wells;
    return;
  }
  for (const auto &well : wells)
  {
    QgsPointXY projected;
    if (!paleo::mapreference::transformPoint(canvas(), QgsPointXY(well.x, well.y), source,
                                             destination, &projected))
      continue;
    wellsection::Well snap = well;
    snap.x = projected.x();
    snap.y = projected.y();
    m_snapWells << snap;
  }
}

void SeismicSectionTool::activate()
{
  m_points.clear();
  m_vertexWellIds.clear();
  clearSnapFeedback();
  if (m_rubberBand)
    m_rubberBand->reset(Qgis::GeometryType::Line);
  QgsMapTool::activate();
}

void SeismicSectionTool::deactivate()
{
  m_points.clear();
  m_vertexWellIds.clear();
  clearSnapFeedback();
  if (m_rubberBand)
    m_rubberBand->reset(Qgis::GeometryType::Line);
  QgsMapTool::deactivate();
}

double SeismicSectionTool::snapToleranceMapUnits() const
{
  // 容差取画布 QGIS 吸附设置，单位按 QgsSnappingConfig::units() 分支：像素
  // 容差经 mapUnitsPerPixel 折算；工程（地图）单位已是本函数要回的地图单位
  // 口径；层单位没有参考层可依（吸附井集不属任何图层），按工程单位处理。
  // 未启用/未配置时回退 12 像素。
  double tolerance = 12.0;
  auto units = Qgis::MapToolUnit::Pixels;
  if (canvas() && canvas()->snappingUtils())
  {
    const QgsSnappingConfig cfg = canvas()->snappingUtils()->config();
    if (cfg.enabled() && cfg.tolerance() > 0)
    {
      tolerance = cfg.tolerance();
      units = cfg.units();
    }
  }
  if (units == Qgis::MapToolUnit::Pixels)
    return tolerance * (canvas() ? canvas()->mapUnitsPerPixel() : 1.0);
  return tolerance;
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
    // 吸附命中在落点处重算，不复用 canvasMoveEvent 的缓存：程序化点击（测试
    // 注入）或指针瞬移可能不产生中间的 move 事件，缓存会给出过期命中（首点
    // 永不吸附）。
    const QgsPointXY pressed = e->mapPoint();
    const int hovered = m_snapWells.isEmpty()
                            ? -1
                            : wellsection::nearestWellWithin(m_snapWells, pressed.x(),
                                                             pressed.y(),
                                                             snapToleranceMapUnits());
    if (hovered >= 0)
    {
      const wellsection::Well &w = m_snapWells.at(hovered);
      // 连续同井忽略：不产生重复顶点与重复井序项
      if (m_vertexWellIds.isEmpty() || m_vertexWellIds.last() != w.id)
      {
        m_points.append(QgsPointXY(w.x, w.y));
        m_vertexWellIds.append(w.id);
      }
    }
    else
    {
      m_points.append(pressed);
      m_vertexWellIds.append(QString());
    }
    redraw();
  }
  else if (e->button() == Qt::RightButton)
  {
    if (m_points.size() >= 2)
    {
      const QVector<QgsPointXY> captured = m_points;
      // 井序去重（保首次出现序）：连续同井在落点处已忽略，但折线回环
      // （A→B→A）会重复收集——同一口井在一条剖面上只能有一个位置，
      // 否则「≥2 口井」判定与剖面井序都会被虚增。
      QStringList wellIds;
      for (const QString &id : m_vertexWellIds)
        if (!id.isEmpty() && !wellIds.contains(id))
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
  // 吸附命中在事件内现算并即用：press 路径同样复算（程序化点击或指针瞬移
  // 可能没有中间 move），不存在需要跨事件缓存的消费者。
  const int hovered = m_snapWells.isEmpty()
                          ? -1
                          : wellsection::nearestWellWithin(m_snapWells, pt.x(), pt.y(),
                                                           snapToleranceMapUnits());
  QgsPointXY hover = pt;
  if (hovered >= 0)
  {
    const wellsection::Well &w = m_snapWells.at(hovered);
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
