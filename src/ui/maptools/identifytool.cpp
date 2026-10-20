// 层：视图
#include "identifytool.h"

#include "identifypopup.h"
#include "../../qgis/previewidentify.h"

#include <QKeyEvent>
#include <QTimer>
#include <QToolButton>

namespace
{
// 悬停识别每层命中上限：密要素区不设每层上限时，单层就能吃满全局 200 条
// 预算，且查询核心对 bbox 内每个候选 fid 各发一次 provider 查询——GUI
// 线程上每次指针停顿（80ms 防抖）都在为整个画布做全量识别（D.20）。
constexpr int kHoverMaxPerLayer = 25;

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

PaleoIdentifyTool::PaleoIdentifyTool(PaleoMapCanvas *canvas)
  : PaleoMapTool(canvas)
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

void PaleoIdentifyTool::canvasMoveEvent(PaleoMapMouseEvent *event)
{
  if (m_pinned || m_overPopup)
    return;
  m_pending = event->pos();
  m_hover->start();
}

void PaleoIdentifyTool::canvasPressEvent(PaleoMapMouseEvent *event)
{
  if (event->button() != Qt::LeftButton)
    return;
  m_hover->stop();
  refreshAt(event->pos(), true);
}

void PaleoIdentifyTool::canvasReleaseEvent(PaleoMapMouseEvent *) {}

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
  PaleoMapTool::keyPressEvent(event);
}

void PaleoIdentifyTool::deactivate()
{
  hidePopup();
  PaleoMapTool::deactivate();
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
  QVector<IdentifyHit> hits;
  // 每层命中上限显式传小值（kHoverMaxPerLayer）：悬停识别跑在 GUI 线程，
  // 不设每层上限时密要素区的单层就能吃满全局预算并逐 fid 查询（D.20）。
  const auto found =
      m_core->identifyPoint(canvas()->layers(), toMapCoordinates(pixel), tol, kHoverMaxPerLayer);
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
