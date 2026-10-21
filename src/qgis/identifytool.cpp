// 层：QGIS 封装
#include "identifytool.h"

#include "previewidentify.h"

#include <QKeyEvent>
#include <QTimer>
#include <qgsmapcanvas.h>
#include <qgsmaplayer.h>
#include <qgsmapmouseevent.h>
#include <qgsmaptool.h>
#include <qgsrasterlayer.h>
#include <qgsvectorlayer.h>

namespace
{
IdentifyHit hitFrom(const PreviewIdentifyResult &result)
{
  IdentifyHit hit;
  hit.title = result.layerName.isEmpty() ? QObject::tr("未命名图层") : result.layerName;
  if (result.isRaster)
  {
    hit.subtitle = result.rasterValid ? QObject::tr("栅格值") : QObject::tr("无栅格值");
    if (result.rasterValid)
    {
      hit.fields.append({QObject::tr("最近邻"), QString::number(result.rasterValueNearest, 'g', 8)});
      hit.fields.append({QObject::tr("双线性"), QString::number(result.rasterValueBilinear, 'g', 8)});
    }
    return hit;
  }
  hit.subtitle = QObject::tr("要素 %1").arg(result.featureId);
  for (auto it = result.attributes.cbegin(); it != result.attributes.cend() && hit.fields.size() < 6; ++it)
  {
    if (!it.value().isValid() || it.value().isNull())
      continue;
    const QString text = it.value().toString();
    if (text.isEmpty())
      continue;
    hit.fields.append({it.key(), text});
  }
  return hit;
}

class PaleoIdentifyTool : public QgsMapTool
{
public:
  PaleoIdentifyTool(QgsMapCanvas *canvas, IdentifyPresenter *presenter);

  void canvasMoveEvent(QgsMapMouseEvent *event) override;
  void canvasPressEvent(QgsMapMouseEvent *event) override;
  void canvasReleaseEvent(QgsMapMouseEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
  void deactivate() override;

private:
  void refreshAt(const QPoint &pixel, bool pin);
  void hidePopup();

  IdentifyPresenter *m_popup = nullptr;
  PreviewIdentifyCore *m_core = nullptr;
  QTimer *m_hover = nullptr;
  QPoint m_pending;
  bool m_pinned = false;
};

PaleoIdentifyTool::PaleoIdentifyTool(QgsMapCanvas *canvas, IdentifyPresenter *presenter)
  : QgsMapTool(canvas)
  , m_popup(presenter)
{
  m_core = new PreviewIdentifyCore(this);
  m_hover = new QTimer(this);
  m_hover->setSingleShot(true);
  m_hover->setInterval(80);
  setCursor(Qt::CrossCursor);
  connect(m_hover, &QTimer::timeout, this, [this] { refreshAt(m_pending, false); });
}

void PaleoIdentifyTool::canvasMoveEvent(QgsMapMouseEvent *event)
{
  if (m_pinned || m_popup->pointerOverHits())
    return;
  m_pending = event->pos();
  m_hover->start();
}

void PaleoIdentifyTool::canvasPressEvent(QgsMapMouseEvent *event)
{
  if (event->button() != Qt::LeftButton)
    return;
  m_hover->stop();
  refreshAt(event->pos(), true);
}

void PaleoIdentifyTool::canvasReleaseEvent(QgsMapMouseEvent *) {}

void PaleoIdentifyTool::keyPressEvent(QKeyEvent *event)
{
  if (event->key() == Qt::Key_Escape)
  {
    hidePopup();
    event->accept();
    return;
  }
  if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)
  {
    if (m_popup->hitsShown())
      m_popup->stepHit(event->key() == Qt::Key_Left ? -1 : 1);
    event->accept();
    return;
  }
  QgsMapTool::keyPressEvent(event);
}

void PaleoIdentifyTool::deactivate()
{
  hidePopup();
  QgsMapTool::deactivate();
}

void PaleoIdentifyTool::hidePopup()
{
  m_hover->stop();
  m_pinned = false;
  m_popup->dismissHits();
}

void PaleoIdentifyTool::refreshAt(const QPoint &pixel, bool pin)
{
  if (!canvas())
    return;
  const double tol = qMax(canvas()->mapUnitsPerPixel() * 8.0, 0.0);
  const QgsPointXY point = toMapCoordinates(pixel);
  QVector<IdentifyHit> hits;
  const auto found = m_core->identifyPoint(canvas()->layers(), point, tol);
  hits.reserve(found.size());
  for (const PreviewIdentifyResult &result : found)
    hits.append(hitFrom(result));
  if (hits.isEmpty())
  {
    if (pin || !m_popup->pointerOverHits())
      hidePopup();
    return;
  }
  m_pinned = pin;
  m_popup->presentHits(hits, pixel);
}
} // namespace

QgsMapTool *createIdentifyTool(QgsMapCanvas *canvas, IdentifyPresenter *presenter)
{
  return new PaleoIdentifyTool(canvas, presenter);
}
