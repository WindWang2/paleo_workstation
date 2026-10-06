// 层：功能
#include "seismicmaplink.h"
#include "domain/seismic/sgycoordinatemapper.h"
#include "domain/seismic/sgyvolume.h"
#include "selectioncontext.h"

#include <QSignalBlocker>
#include <cmath>

#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfeaturerequest.h>
#include <qgsfields.h>
#include <qgsmapcanvas.h>
#include <qgsvectorlayer.h>
#include <qgsvertexmarker.h>

SeismicMapLink::SeismicMapLink(QgsMapCanvas *canvas, SelectionContext *ctx, QObject *parent)
  : QObject(parent), m_canvas(canvas), m_ctx(ctx)
{
  if (m_ctx)
    connect(m_ctx, &SelectionContext::selectionChanged,
            this, &SeismicMapLink::onContextSelection);
  if (m_canvas)
  {
    connect(m_canvas.data(), &QObject::destroyed, this, [this]() {
      m_cursorMarker = nullptr;
    });
  }
}

SeismicMapLink::~SeismicMapLink()
{
  if (m_canvas)
  {
    delete m_cursorMarker;
  }
}

void SeismicMapLink::setSeismicLayer(QgsVectorLayer *lineLayer, const QString &idField)
{
  if (m_layer == lineLayer && m_idField == idField)
    return;
  if (m_layer)
    disconnect(m_layer, nullptr, this, nullptr);

  m_layer = lineLayer;
  m_idField = idField;
  if (!m_layer)
    return;

  connect(m_layer, &QgsVectorLayer::selectionChanged, this,
          [this](const QgsFeatureIds &, const QgsFeatureIds &, bool) {
            if (!m_ctx || !m_layer)
              return;
            if (m_ctx->broadcasting())
              return;
            const int idx = m_layer->fields().indexOf(m_idField);
            if (idx < 0)
              return;

            const QgsFeatureIds sel = m_layer->selectedFeatureIds();
            if (sel.isEmpty())
            {
              m_ctx->clear(QStringLiteral("seismic_map"));
              return;
            }

            QgsFeatureRequest req;
            req.setFilterFids(sel);
            req.setSubsetOfAttributes(QgsAttributeList{idx});
            req.setFlags(Qgis::FeatureRequestFlag::NoGeometry);

            QStringList lineIds;
            QgsFeature f;
            QgsFeatureIterator it = m_layer->getFeatures(req);
            while (it.nextFeature(f))
              lineIds << f.attribute(idx).toString();
            m_ctx->setSelection(lineIds, QStringLiteral("seismic_map"));
          });
}

QgsVectorLayer *SeismicMapLink::seismicLayer() const
{
  return m_layer.data();
}

void SeismicMapLink::setGridGeometry(const SurveyGridGeometry &geom)
{
  m_gridGeom = geom;
}

void SeismicMapLink::setActiveVolume(std::shared_ptr<const seismic::SgyVolume> volume)
{
  m_volume = volume;
  m_gridGeom = {};
  if (volume && volume->Index()) {
    const auto mapper = seismic::SgyCoordinateMapper::Fit(*volume->Index());
    if (mapper.valid()) {
      const auto &f = mapper.fit();
      m_gridGeom.valid = true;
      m_gridGeom.a = f.b;
      m_gridGeom.b = f.a;
      m_gridGeom.c = f.e;
      m_gridGeom.d = f.d;
      m_gridGeom.p1x = f.c;
      m_gridGeom.p1y = f.f;
      m_gridGeom.inlineMin = volume->InlineMin();
      m_gridGeom.inlineMax = volume->InlineMax();
      m_gridGeom.xlineMin = volume->XlineMin();
      m_gridGeom.xlineMax = volume->XlineMax();
    }
  }
  emit sectionVolumeChanged(volume);
}

void SeismicMapLink::requestSectionCapture()
{
  // R4 信号化（方向 49）：工具归壳持有（SeismicSectionTool 是 QgsMapTool
  // 派生的 widget 机械，qgis 封装域），这里只发意图——壳订阅后创建/激活
  // 工具，并把 sectionPathCaptured 接回 triggerSectionFromMapPolyline。
  emit sectionCaptureRequested();
}

void SeismicMapLink::triggerSectionFromMapPolyline(const QVector<QgsPointXY> &mapPoints, const QString &title)
{
  if (mapPoints.size() < 2)
  {
    emit sectionExtractedFromMap(false, tr("折线至少需要两个点"));
    return;
  }

  if (!m_volume || !m_gridGeom.valid)
  {
    emit sectionExtractedFromMap(false, tr("未加载有效地震体或测网几何"));
    return;
  }

  if (m_canvas && m_canvas->mapSettings().destinationCrs().isGeographic()) {
    emit sectionExtractedFromMap(
        false,
        tr("当前地图使用经纬度，请先切换到与井口及地震一致的米制坐标系"));
    return;
  }
  std::vector<glm::ivec2> pathPoints;
  std::vector<glm::dvec2> mapPolyline;
  for (const auto &pt : mapPoints)
  {
    int inl = 0, xl = 0;
    QString reason;
    if (!std::isfinite(pt.x()) || !std::isfinite(pt.y()) ||
        !m_gridGeom.xyToInlineXline(pt.x(), pt.y(), &inl, &xl, &reason)) {
      emit sectionExtractedFromMap(
          false,
          tr("节点位于地震范围外，保留完整路径后请重新绘制：%1").arg(reason));
      return;
    }
    if (!pathPoints.empty() && pathPoints.back() == glm::ivec2(inl, xl))
      continue;
    pathPoints.push_back({inl, xl});
    // The extractor uses the snapped grid path, so overlays use that same path.
    double x, y;
    m_gridGeom.inlineXlineToXy(inl, xl, &x, &y);
    mapPolyline.push_back({x, y});
  }

  if (pathPoints.size() < 2)
  {
    emit sectionExtractedFromMap(false, tr("拾取的折线点位于地震工区有效范围外"));
    return;
  }

  // 壳订阅：折线已换算成测线序——剖面 dock 走异步提取 + show/raise。
  emit sectionExtractRequested(
      m_volume, pathPoints,
      title.isEmpty() ? tr("地图折线剖面") : title, mapPolyline);
  emit sectionExtractedFromMap(true, QString());
}

void SeismicMapLink::onContextSelection(const QStringList &ids, const QString &origin)
{
  if (!m_layer || origin == QLatin1String("seismic_map"))
    return;

  const int idx = m_layer->fields().indexOf(m_idField);
  if (idx < 0)
    return;

  QgsFeatureIds fids;
  QgsFeatureRequest req;
  req.setSubsetOfAttributes(QgsAttributeList{idx});
  req.setFlags(Qgis::FeatureRequestFlag::NoGeometry);
  QgsFeature f;
  QgsFeatureIterator it = m_layer->getFeatures(req);
  while (it.nextFeature(f))
    if (ids.contains(f.attribute(idx).toString()))
      fids.insert(f.id());

  {
    const QSignalBlocker blocker(m_layer);
    m_layer->selectByIds(fids);
  }
  m_layer->triggerRepaint();
}

void SeismicMapLink::onSectionTraceHovered(
    int, double, double, float, double mapX, double mapY)
{
  if (!m_canvas)
    return;

  if (!std::isfinite(mapX) || !std::isfinite(mapY)) {
    if (m_cursorMarker)
      m_cursorMarker->hide();
    return;
  }

  if (!m_cursorMarker)
  {
    m_cursorMarker = new QgsVertexMarker(m_canvas);
    m_cursorMarker->setIconType(QgsVertexMarker::ICON_CROSS);
    m_cursorMarker->setColor(QColor(QStringLiteral("#1B73D0")));
    m_cursorMarker->setIconSize(14);
    m_cursorMarker->setPenWidth(2);
  }

  m_cursorMarker->setCenter(QgsPointXY(mapX, mapY));
  m_cursorMarker->show();
}

void SeismicMapLink::onSectionTraceClicked(
    int, double, double, float, double mapX, double mapY)
{
  if (!m_canvas)
    return;

  if (std::isfinite(mapX) && std::isfinite(mapY)) {
    m_canvas->setCenter(QgsPointXY(mapX, mapY));
    m_canvas->refresh();
  }
}

void SeismicMapLink::onSectionPathCaptured(const QVector<QgsPointXY> &points)
{
  triggerSectionFromMapPolyline(points, tr("地图拾取任意剖面"));
}
