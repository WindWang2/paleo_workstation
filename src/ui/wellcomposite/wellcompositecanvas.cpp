// 层：视图
#include "wellcompositecanvas.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QWheelEvent>
#include <cmath>

namespace WellComposite
{

// ----------------------------------------------------------------------------
// WellCompositeCanvas 主控件
// ----------------------------------------------------------------------------
WellCompositeCanvas::WellCompositeCanvas(QWidget *parent)
  : QWidget(parent)
{
  setMouseTracking(true);

  m_header = new WellCompositeHeader(this);
  m_body = new WellCompositeBody(this);
  m_vScrollBar = new QScrollBar(Qt::Vertical, this);
  m_hScrollBar = new QScrollBar(Qt::Horizontal, this);

  // 滚动条样式
  const QString sbStyle = QStringLiteral(
      "QScrollBar:vertical { width: 10px; background: #F5F7FA; margin: 0; }"
      "QScrollBar::handle:vertical { background: #CFD8DC; border-radius: 5px; min-height: 20px; }"
      "QScrollBar::handle:vertical:hover { background: #90A4AE; }"
      "QScrollBar:horizontal { height: 10px; background: #F5F7FA; margin: 0; }"
      "QScrollBar::handle:horizontal { background: #CFD8DC; border-radius: 5px; min-width: 20px; }"
      "QScrollBar::handle:horizontal:hover { background: #90A4AE; }"
      "QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }");
  m_vScrollBar->setStyleSheet(sbStyle);
  m_hScrollBar->setStyleSheet(sbStyle);

  connect(m_vScrollBar, &QScrollBar::valueChanged, this, [this](int val) {
    m_scrollDepth = m_minDepth + val / pxPerMeter();
    updateAll();
    notifyViewportChanged();
  });

  connect(m_hScrollBar, &QScrollBar::valueChanged, this, [this](int val) {
    m_hScrollOffset = val;
    updateAll();
  });

  m_scrollDepth = m_minDepth;
}

void WellCompositeCanvas::addTrack(const std::shared_ptr<WellTrack> &track)
{
  if (!track) return;
  m_tracks.append(track);
  syncScrollBars();
  updateAll();
}

void WellCompositeCanvas::insertTrack(int index, const std::shared_ptr<WellTrack> &track)
{
  if (!track) return;
  const int idx = qBound(0, index, static_cast<int>(m_tracks.size()));
  m_tracks.insert(idx, track);
  syncScrollBars();
  updateAll();
}

void WellCompositeCanvas::removeTrack(int index)
{
  if (index >= 0 && index < m_tracks.size())
  {
    m_tracks.removeAt(index);
    syncScrollBars();
    updateAll();
  }
}

void WellCompositeCanvas::clearTracks()
{
  m_tracks.clear();
  syncScrollBars();
  updateAll();
}

void WellCompositeCanvas::setTracks(const QList<std::shared_ptr<WellTrack>> &tracks)
{
  m_tracks = tracks;
  for (auto &t : m_tracks)
  {
    if (t && t->type() == TrackType::DepthScale)
    {
      auto dst = std::dynamic_pointer_cast<DepthScaleTrack>(t);
      if (dst) dst->setScaleRatio(m_scaleRatio);
    }
  }
  syncScrollBars();
  updateAll();
}

void WellCompositeCanvas::setDepthRange(double minDepth, double maxDepth)
{
  if (maxDepth <= minDepth)
    maxDepth = minDepth + 100.0;

  m_minDepth = minDepth;
  m_maxDepth = maxDepth;
  m_scrollDepth = qBound(m_minDepth, m_scrollDepth, m_maxDepth);
  syncScrollBars();
  updateAll();
  notifyViewportChanged();
}

void WellCompositeCanvas::setScrollDepth(double depth)
{
  const double visibleSpan = (m_body ? m_body->height() : 100) / pxPerMeter();
  const double maxScroll = qMax(m_minDepth, m_maxDepth - visibleSpan);
  m_scrollDepth = qBound(m_minDepth, depth, maxScroll);

  if (m_vScrollBar)
  {
    const int val = static_cast<int>(std::round((m_scrollDepth - m_minDepth) * pxPerMeter()));
    m_vScrollBar->blockSignals(true);
    m_vScrollBar->setValue(val);
    m_vScrollBar->blockSignals(false);
  }
  updateAll();
  notifyViewportChanged();
}

double WellCompositeCanvas::visibleDepthSpan() const
{
  const double bodyH = (m_body && m_body->height() > 10) ? m_body->height() : 100.0;
  return bodyH / pxPerMeter();
}

void WellCompositeCanvas::notifyViewportChanged()
{
  emit viewportChanged(visibleTopDepth(), visibleBottomDepth(), visibleDepthSpan());
}

double WellCompositeCanvas::pxPerMeter() const
{
  return qMax(0.01, m_basePxPerMeter * m_zoomFactor);
}

double WellCompositeCanvas::depthToY(double depth) const
{
  return (depth - m_scrollDepth) * pxPerMeter();
}

double WellCompositeCanvas::yToDepth(double y) const
{
  return m_scrollDepth + y / pxPerMeter();
}

qreal WellCompositeCanvas::totalTracksWidth() const
{
  qreal w = 0.0;
  for (const auto &t : m_tracks)
    if (t && t->isVisible())
      w += t->width();
  return w;
}

QString WellCompositeCanvas::calculateScaleRatioString() const
{
  const double ppm = pxPerMeter();
  if (ppm <= 1e-4) return QStringLiteral("1:5000");
  int denom = qRound(3779.528 / ppm);
  if (denom < 10) denom = 10;
  else if (denom < 50) denom = (denom + 2) / 5 * 5;
  else if (denom < 200) denom = (denom + 5) / 10 * 10;
  else if (denom < 1000) denom = (denom + 12) / 25 * 25;
  else denom = (denom + 25) / 50 * 50;
  return QStringLiteral("1:%1").arg(denom);
}

void WellCompositeCanvas::setZoomFactor(double factor, double anchorDepth)
{
  const double oldFactor = m_zoomFactor;
  m_zoomFactor = qBound(0.2, factor, 20.0);

  if (std::abs(m_zoomFactor - oldFactor) < 1e-4)
    return;

  // 保持鼠标所在锚点深度在屏幕视觉上不动
  if (anchorDepth >= m_minDepth && anchorDepth <= m_maxDepth && m_body)
  {
    const double anchorY = (anchorDepth - m_scrollDepth) * (m_basePxPerMeter * oldFactor);
    m_scrollDepth = anchorDepth - anchorY / pxPerMeter();
  }

  m_scaleRatio = calculateScaleRatioString();
  for (auto &t : m_tracks)
  {
    if (t && t->type() == TrackType::DepthScale)
    {
      auto dst = std::dynamic_pointer_cast<DepthScaleTrack>(t);
      if (dst) dst->setScaleRatio(m_scaleRatio);
    }
  }

  syncScrollBars();
  updateAll();
  emit scaleRatioChanged(m_scaleRatio);
  emit zoomChanged(m_zoomFactor);
  notifyViewportChanged();
}

void WellCompositeCanvas::zoomIn()
{
  const double centerDepth = yToDepth(m_body ? m_body->height() * 0.5 : 0.0);
  setZoomFactor(m_zoomFactor * 1.3, centerDepth);
}

void WellCompositeCanvas::zoomOut()
{
  const double centerDepth = yToDepth(m_body ? m_body->height() * 0.5 : 0.0);
  setZoomFactor(m_zoomFactor / 1.3, centerDepth);
}

void WellCompositeCanvas::resetZoom()
{
  const double bodyH = (m_body && m_body->height() > 50) ? m_body->height() : qMax(100.0, height() - m_headerHeight - 10.0);
  const double totalSpan = qMax(1.0, m_maxDepth - m_minDepth);

  if (m_baseScaleRatio == tr("自适应") || m_baseScaleRatio == QStringLiteral("自适应"))
  {
    m_basePxPerMeter = bodyH / totalSpan;
    m_zoomFactor = 1.0;
    m_scaleRatio = calculateScaleRatioString();
  }
  else
  {
    if (m_baseScaleRatio == QStringLiteral("1:200"))
      m_basePxPerMeter = 3779.528 / 200.0;
    else if (m_baseScaleRatio == QStringLiteral("1:500"))
      m_basePxPerMeter = 3779.528 / 500.0;
    else if (m_baseScaleRatio == QStringLiteral("1:1000"))
      m_basePxPerMeter = 3779.528 / 1000.0;
    else if (m_baseScaleRatio == QStringLiteral("1:2000"))
      m_basePxPerMeter = 3779.528 / 2000.0;
    else if (m_baseScaleRatio.startsWith(QLatin1String("1:")))
    {
      bool ok = false;
      int denom = m_baseScaleRatio.mid(2).toInt(&ok);
      if (ok && denom > 0)
        m_basePxPerMeter = 3779.528 / denom;
      else
        m_basePxPerMeter = 3779.528 / 500.0;
    }
    else
      m_basePxPerMeter = 3779.528 / 500.0;

    m_zoomFactor = 1.0;
    m_scaleRatio = m_baseScaleRatio;
  }

  m_scrollDepth = m_minDepth;
  for (auto &t : m_tracks)
  {
    if (t && t->type() == TrackType::DepthScale)
    {
      auto dst = std::dynamic_pointer_cast<DepthScaleTrack>(t);
      if (dst) dst->setScaleRatio(m_scaleRatio);
    }
  }
  syncScrollBars();
  updateAll();
  emit scaleRatioChanged(m_scaleRatio);
  emit zoomChanged(m_zoomFactor);
  notifyViewportChanged();
}

void WellCompositeCanvas::setScaleRatio(const QString &ratioStr)
{
  m_baseScaleRatio = ratioStr;
  const double bodyH = (m_body && m_body->height() > 50) ? m_body->height() : qMax(100.0, height() - m_headerHeight - 10.0);
  const double totalSpan = qMax(1.0, m_maxDepth - m_minDepth);

  if (ratioStr == tr("自适应") || ratioStr == QStringLiteral("自适应"))
  {
    m_basePxPerMeter = bodyH / totalSpan;
    m_zoomFactor = 1.0;
    m_scaleRatio = calculateScaleRatioString();
  }
  else
  {
    if (ratioStr == QStringLiteral("1:200"))
      m_basePxPerMeter = 3779.528 / 200.0;
    else if (ratioStr == QStringLiteral("1:500"))
      m_basePxPerMeter = 3779.528 / 500.0;
    else if (ratioStr == QStringLiteral("1:1000"))
      m_basePxPerMeter = 3779.528 / 1000.0;
    else if (ratioStr == QStringLiteral("1:2000"))
      m_basePxPerMeter = 3779.528 / 2000.0;
    else if (ratioStr.startsWith(QLatin1String("1:")))
    {
      bool ok = false;
      int denom = ratioStr.mid(2).toInt(&ok);
      if (ok && denom > 0)
        m_basePxPerMeter = 3779.528 / denom;
      else
        m_basePxPerMeter = 3779.528 / 500.0;
    }
    else
      m_basePxPerMeter = 3779.528 / 500.0;

    m_zoomFactor = 1.0;
    m_scaleRatio = ratioStr;
  }

  for (auto &t : m_tracks)
  {
    if (t && t->type() == TrackType::DepthScale)
    {
      auto dst = std::dynamic_pointer_cast<DepthScaleTrack>(t);
      if (dst) dst->setScaleRatio(m_scaleRatio);
    }
  }
  syncScrollBars();
  updateAll();
  emit scaleRatioChanged(m_scaleRatio);
  emit zoomChanged(m_zoomFactor);
  notifyViewportChanged();
}

void WellCompositeCanvas::setHoverDepth(double depth)
{
  m_hoverDepth = depth;
  updateAll();
  emit depthHovered(depth);
}

void WellCompositeCanvas::updateAll()
{
  if (m_header) m_header->update();
  if (m_body) m_body->update();
}

void WellCompositeCanvas::syncScrollBars()
{
  if (!m_body || !m_vScrollBar || !m_hScrollBar)
    return;

  const double bodyH = m_body->height();
  const double bodyW = m_body->width();
  const double totalH = (m_maxDepth - m_minDepth) * pxPerMeter();
  const double totalW = totalTracksWidth();

  // 垂直深度滚动条
  if (totalH > bodyH)
  {
    m_vScrollBar->setVisible(true);
    m_vScrollBar->setRange(0, static_cast<int>(std::round(totalH - bodyH)));
    m_vScrollBar->setPageStep(static_cast<int>(std::round(bodyH)));
  }
  else
  {
    m_vScrollBar->setVisible(false);
    m_scrollDepth = m_minDepth;
  }

  // 水平多道滚动条
  if (totalW > bodyW)
  {
    m_hScrollBar->setVisible(true);
    m_hScrollBar->setRange(0, static_cast<int>(std::round(totalW - bodyW)));
    m_hScrollBar->setPageStep(static_cast<int>(std::round(bodyW)));
  }
  else
  {
    m_hScrollBar->setVisible(false);
    m_hScrollOffset = 0.0;
  }
}

void WellCompositeCanvas::resizeEvent(QResizeEvent *event)
{
  QWidget::resizeEvent(event);

  const int sbSize = 10;
  const int w = width();
  const int h = height();

  const int headerH = static_cast<int>(m_headerHeight);
  const int bodyH = qMax(10, h - headerH - sbSize);
  const int bodyW = qMax(10, w - sbSize);

  m_header->setGeometry(0, 0, bodyW, headerH);
  m_body->setGeometry(0, headerH, bodyW, bodyH);
  m_vScrollBar->setGeometry(bodyW, headerH, sbSize, bodyH);
  m_hScrollBar->setGeometry(0, headerH + bodyH, bodyW, sbSize);

  if (m_scaleRatio == tr("自适应") || m_scaleRatio == QStringLiteral("自适应"))
  {
    const double totalSpan = qMax(1.0, m_maxDepth - m_minDepth);
    m_basePxPerMeter = bodyH / totalSpan;
  }

  syncScrollBars();
  notifyViewportChanged();
}

// ----------------------------------------------------------------------------
// WellCompositeHeader: 置顶道头
// ----------------------------------------------------------------------------
WellCompositeHeader::WellCompositeHeader(WellCompositeCanvas *canvas)
  : QWidget(canvas), m_canvas(canvas)
{
}

void WellCompositeHeader::paintEvent(QPaintEvent * /*event*/)
{
  if (!m_canvas) return;

  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setRenderHint(QPainter::TextAntialiasing, true);

  p.fillRect(rect(), QColor(QStringLiteral("#F5F7FA")));
  p.setPen(QColor(QStringLiteral("#DFE5EC")));
  p.drawLine(rect().bottomLeft(), rect().bottomRight());

  qreal curX = -m_canvas->hScrollOffset();
  for (const auto &t : m_canvas->tracks())
  {
    if (!t || !t->isVisible())
      continue;

    const qreal tw = t->width();
    const QRectF trackHeadRect(curX, 0, tw, height());

    if (trackHeadRect.right() >= 0 && trackHeadRect.left() <= width())
    {
      t->paintHeader(p, trackHeadRect, m_canvas->hoverDepth());
    }

    curX += tw;
  }
}

// ----------------------------------------------------------------------------
// WellCompositeBody: 道体渲染与手势交互
// ----------------------------------------------------------------------------
WellCompositeBody::WellCompositeBody(WellCompositeCanvas *canvas)
  : QWidget(canvas), m_canvas(canvas)
{
  setMouseTracking(true);
}

void WellCompositeBody::paintEvent(QPaintEvent * /*event*/)
{
  if (!m_canvas) return;

  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setRenderHint(QPainter::TextAntialiasing, true);

  p.fillRect(rect(), QColor(QStringLiteral("#FFFFFF")));

  const double topDepth = m_canvas->scrollDepth();
  const double visibleDepthSpan = height() / m_canvas->pxPerMeter();
  const double bottomDepth = topDepth + visibleDepthSpan;

  qreal curX = -m_canvas->hScrollOffset();
  for (const auto &t : m_canvas->tracks())
  {
    if (!t || !t->isVisible())
      continue;

    const qreal tw = t->width();
    const QRectF trackBodyRect(curX, 0, tw, height());

    if (trackBodyRect.right() >= 0 && trackBodyRect.left() <= width())
    {
      t->paintBody(p, trackBodyRect, topDepth, bottomDepth, m_canvas->pxPerMeter());
    }

    curX += tw;
  }

  // 绘制横跨全道的十字准星虚线与深度气泡
  const double hDepth = m_canvas->hoverDepth();
  if (hDepth >= topDepth && hDepth <= bottomDepth)
  {
    const qreal crossY = (hDepth - topDepth) * m_canvas->pxPerMeter();

    p.save();
    // 准星横线
    p.setPen(QPen(QColor(QStringLiteral("#1B73D0")), 1.0, Qt::DashLine));
    p.drawLine(QPointF(0, crossY), QPointF(width(), crossY));

    // 准星深度 Badge（深色小气泡）
    const QString dStr = QStringLiteral("%1 m").arg(QString::number(hDepth, 'f', 1));
    QFont font = p.font();
    font.setFamily(QStringLiteral("JetBrains Mono, monospace"));
    font.setPointSize(7);
    p.setFont(font);

    const QRectF badgeRect(2, crossY - 8, 48, 16);
    p.fillRect(badgeRect, QColor(QStringLiteral("#24303E")));
    p.setPen(QColor(QStringLiteral("#FFFFFF")));
    p.drawText(badgeRect, Qt::AlignCenter, dStr);

    p.restore();
  }

  // 依据 DESIGN.md §140：画布右下角白底半透明位置与比例尺图例卡片
  if (width() > 240 && height() > 120)
  {
    p.save();
    const qreal cardW = 195.0;
    const qreal cardH = 38.0;
    const QRectF cardRect(width() - cardW - 10, height() - cardH - 10, cardW, cardH);

    p.fillRect(cardRect, QColor(255, 255, 255, 235));
    p.setPen(QColor(QStringLiteral("#DFE5EC")));
    p.drawRoundedRect(cardRect, 4.0, 4.0);

    QFont cardFont = p.font();
    cardFont.setFamily(QStringLiteral("JetBrains Mono, monospace"));
    cardFont.setPointSize(7);
    p.setFont(cardFont);

    // 比例尺与每厘米米数换算
    const double ppm = m_canvas->pxPerMeter();
    const double metersPerCm = (ppm > 1e-4) ? (37.79528 / ppm) : 5.0;
    const QString mCmStr = (metersPerCm < 1.0) ? QString::number(metersPerCm, 'f', 2)
                           : (metersPerCm < 10.0) ? QString::number(metersPerCm, 'f', 1)
                                                  : QString::number(metersPerCm, 'f', 0);
    const QString scaleText = QStringLiteral("比例尺: %1 (1cm≈%2m)")
                                  .arg(m_canvas->scaleRatio(), mCmStr);
    p.setPen(QColor(QStringLiteral("#24303E")));
    p.drawText(QRectF(cardRect.left() + 8, cardRect.top() + 3, cardW - 16, 15),
               Qt::AlignLeft | Qt::AlignVCenter, scaleText);

    // 当前视口深度范围与跨度
    const QString rangeText = QStringLiteral("[%1~%2m] 跨度:%3m")
                                  .arg(QString::number(topDepth, 'f', 1))
                                  .arg(QString::number(bottomDepth, 'f', 1))
                                  .arg(QString::number(visibleDepthSpan, 'f', 1));
    p.setPen(QColor(QStringLiteral("#5D6E80")));
    p.drawText(QRectF(cardRect.left() + 8, cardRect.top() + 19, cardW - 16, 15),
               Qt::AlignLeft | Qt::AlignVCenter, rangeText);
    p.restore();
  }
}

void WellCompositeBody::wheelEvent(QWheelEvent *event)
{
  if (!m_canvas) return;

  const double mouseDepth = m_canvas->yToDepth(event->position().y());

  // 支持滚轮缩放深度（以光标处深度为锚点）
  const double factor = event->angleDelta().y() > 0 ? 1.2 : 0.8333;
  m_canvas->setZoomFactor(m_canvas->zoomFactor() * factor, mouseDepth);

  event->accept();
}

void WellCompositeBody::mousePressEvent(QMouseEvent *event)
{
  if (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton)
  {
    m_isPanning = true;
    m_lastMousePos = event->pos();
    setCursor(Qt::ClosedHandCursor);
    event->accept();
    return;
  }
  QWidget::mousePressEvent(event);
}

void WellCompositeBody::mouseMoveEvent(QMouseEvent *event)
{
  if (!m_canvas) return;

  if (m_isPanning)
  {
    const int dy = event->pos().y() - m_lastMousePos.y();
    const int dx = event->pos().x() - m_lastMousePos.x();
    m_lastMousePos = event->pos();

    // 纵向拖拽平移深度
    const double deltaDepth = -dy / m_canvas->pxPerMeter();
    m_canvas->setScrollDepth(m_canvas->scrollDepth() + deltaDepth);

    // 横向拖拽平移井道
    const double totalW = m_canvas->totalTracksWidth();
    const double maxH = qMax(0.0, totalW - width());
    m_canvas->m_hScrollOffset = qBound(0.0, m_canvas->m_hScrollOffset - dx, maxH);

    m_canvas->syncScrollBars();
    m_canvas->updateAll();
    m_canvas->notifyViewportChanged();
    event->accept();
    return;
  }

  // 更新当前悬停深度
  const double d = m_canvas->yToDepth(event->pos().y());
  m_canvas->setHoverDepth(d);

  event->accept();
}

void WellCompositeBody::mouseReleaseEvent(QMouseEvent *event)
{
  if (m_isPanning)
  {
    m_isPanning = false;
    setCursor(Qt::ArrowCursor);
    event->accept();
    return;
  }
  QWidget::mouseReleaseEvent(event);
}

void WellCompositeBody::mouseDoubleClickEvent(QMouseEvent *event)
{
  if (!m_canvas) return;
  // 双击一键复位缩放
  m_canvas->resetZoom();
  event->accept();
}

void WellCompositeBody::leaveEvent(QEvent * /*event*/)
{
  if (m_canvas)
    m_canvas->setHoverDepth(-1.0);
}

} // namespace WellComposite