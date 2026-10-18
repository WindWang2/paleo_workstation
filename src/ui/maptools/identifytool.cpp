// 层：视图
#include "identifytool.h"

#include "identifypopup.h"
#include "../../qgis/previewidentify.h"

#include <QKeyEvent>
#include <QTimer>
#include <QToolButton>
#include <qgsmapcanvas.h>
#include <qgsmaplayer.h>
#include <qgsmapmouseevent.h>
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
} // namespace

PaleoIdentifyTool::PaleoIdentifyTool(QgsMapCanvas *canvas)
  : QgsMapTool(canvas)
{
  m_popup = new IdentifyPopup(canvas);
  m_core = new PreviewIdentifyCore(this);
  m_hover = new QTimer(this);
  m_hover->setSingleShot(true);
  m_hover->setInterval(80);
  setCursor(Qt::CrossCursor);
  connect(m_hover, &QTimer::timeout, this, [this] { refreshAt(m_pending, false); });
  connect(m_popup, &IdentifyPopup::pointerInside, this, [this](bool inside) { m_overPopup = inside; });
}

PaleoIdentifyTool::~PaleoIdentifyTool() = default;

void PaleoIdentifyTool::canvasMoveEvent(QgsMapMouseEvent *event)
{
  if (m_pinned || m_overPopup)
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
    auto *button = m_popup->findChild<QToolButton *>(
        event->key() == Qt::Key_Left ? QStringLiteral("identifyPrev") : QStringLiteral("identifyNext"));
    if (button && m_popup->isVisible())
      button->click();
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
  m_popup->hide();
  m_popup->setHits({});
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
    if (pin || !m_overPopup)
      hidePopup();
    return;
  }
  m_pinned = pin;
  m_popup->setHits(hits);
  m_popup->showNear(pixel);
}
