// 层：视图
#include "wellcompositecanvas.h"
#include "../paleotheme.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QToolTip>
#include <QWheelEvent>
#include <cmath>

#include "trackops.h"

namespace WellComposite
{

// ----------------------------------------------------------------------------
// WellCompositeCanvas 主控件
// ----------------------------------------------------------------------------
WellCompositeCanvas::WellCompositeCanvas(QWidget *parent)
  : QWidget(parent)
{
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus); // D7.1/D7.2 键盘导航需要画布可聚焦

  m_header = new WellCompositeHeader(this);
  m_body = new WellCompositeBody(this);
  m_vScrollBar = new QScrollBar(Qt::Vertical, this);
  m_hScrollBar = new QScrollBar(Qt::Horizontal, this);

  // 滚动条样式（chrome，跟随主题）
  const auto sbStyleBuilder = [] {
    const auto &t = PaleoTheme::tokens();
    return QStringLiteral(
               "QScrollBar:vertical { width: 10px; background: %1; margin: 0; }"
               "QScrollBar::handle:vertical { background: %2; border-radius: 5px; min-height: 20px; }"
               "QScrollBar::handle:vertical:hover { background: %3; }"
               "QScrollBar:horizontal { height: 10px; background: %1; margin: 0; }"
               "QScrollBar::handle:horizontal { background: %2; border-radius: 5px; min-width: 20px; }"
               "QScrollBar::handle:horizontal:hover { background: %3; }"
               "QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }")
        .arg(t.surfaceAlt.name(), t.border.name(), t.textDisabled.name());
  };
  PaleoTheme::applyThemedStyleSheet(m_vScrollBar, sbStyleBuilder);
  PaleoTheme::applyThemedStyleSheet(m_hScrollBar, sbStyleBuilder);

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
    if (m_focusTrackIndex >= m_tracks.size())
      m_focusTrackIndex = m_tracks.isEmpty() ? -1 : m_tracks.size() - 1;
    syncScrollBars();
    updateAll();
    emit trackRemoved(index);
  }
}

void WellCompositeCanvas::clearTracks()
{
  m_tracks.clear();
  m_focusTrackIndex = -1;
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

void WellCompositeCanvas::moveTrack(int fromIndex, int toIndex)
{
  if (fromIndex < 0 || fromIndex >= m_tracks.size())
    return;
  if (toIndex < 0 || toIndex >= m_tracks.size() || fromIndex == toIndex)
    return;
  m_tracks.move(fromIndex, toIndex);
  syncScrollBars();
  updateAll();
  emit trackOrderChanged();
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

bool WellCompositeCanvas::clampViewportToLimits(double *scrollDepth, double pxPerMeterTarget) const
{
  if (!scrollDepth)
    return false;
  // D2.8 只在视口几何可信赖（≥80px，已显示）时施加跨度上下限；隐藏/测试
  // 小几何下钳制会把缩放因子反压到荒谬值
  const double bodyH = (m_body && m_body->height() >= 80) ? m_body->height() : 0.0;
  double span = bodyH / qMax(0.01, pxPerMeterTarget);

  bool clamped = false;
  if (bodyH < 80.0)
  {
    // 仅做滚动边界夹取
    const double sp = visibleDepthSpan();
    const double maxScroll0 = qMax(m_minDepth, m_maxDepth - sp);
    const double sd0 = qBound(m_minDepth, *scrollDepth, maxScroll0);
    if (std::abs(sd0 - *scrollDepth) > 1e-9)
    {
      *scrollDepth = sd0;
      return true;
    }
    return false;
  }
  // D2.8 上限：不放大到超出整井跨度
  const double total = qMax(1.0, m_maxDepth - m_minDepth);
  const double maxSpan = m_maxVisibleSpan > 0.0 ? qMin(m_maxVisibleSpan, total * 3.0) : total;
  if (span > maxSpan)
  {
    span = maxSpan;
    clamped = true;
  }
  // D2.8 下限：最小显示段（缺省 10m）
  if (span < m_minVisibleSpan)
  {
    span = m_minVisibleSpan;
    clamped = true;
  }

  const double maxScroll = qMax(m_minDepth, m_maxDepth - span);
  const double sd = qBound(m_minDepth, *scrollDepth, maxScroll);
  if (std::abs(sd - *scrollDepth) > 1e-9)
    clamped = true;
  *scrollDepth = sd;
  return clamped;
}

double WellCompositeCanvas::applySnap(double rawDepth) const
{
  if (!m_snapEnabled)
    return rawDepth;
  const double thresholdM = m_snapThresholdPx / pxPerMeter();
  const double gridStep = DepthTools::niceStepFor(pxPerMeter());
  return DepthTools::snapDepth(rawDepth, m_markers, thresholdM, true, m_snapToGrid, gridStep);
}

void WellCompositeCanvas::setScrollDepth(double depth, bool userDriven)
{
  const double visibleSpan = (m_body ? m_body->height() : 100) / pxPerMeter();
  const double maxScroll = qMax(m_minDepth, m_maxDepth - visibleSpan);
  m_scrollDepth = qBound(m_minDepth, depth, maxScroll);

  if (userDriven)
    m_scrollDepth = applySnap(m_scrollDepth);

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

QList<int> WellCompositeCanvas::visibleTrackIndices() const
{
  QList<int> idx;
  for (int i = 0; i < m_tracks.size(); ++i)
    if (m_tracks.at(i) && m_tracks.at(i)->isVisible())
      idx << i;
  return idx;
}

int WellCompositeCanvas::visibleTrackCount() const
{
  return visibleTrackIndices().size();
}

QRectF WellCompositeCanvas::trackBodyRect(int visibleIndex) const
{
  const QList<int> vis = visibleTrackIndices();
  if (visibleIndex < 0 || visibleIndex >= vis.size())
    return QRectF();
  qreal curX = -m_hScrollOffset;
  for (int vi = 0; vi < visibleIndex; ++vi)
    curX += m_tracks.at(vis.at(vi))->width();
  return QRectF(curX, 0, m_tracks.at(vis.at(visibleIndex))->width(), 1.0);
}

int WellCompositeCanvas::trackIndexAtX(qreal x) const
{
  const QList<int> vis = visibleTrackIndices();
  qreal curX = -m_hScrollOffset;
  for (int vi = 0; vi < vis.size(); ++vi)
  {
    const qreal w = m_tracks.at(vis.at(vi))->width();
    if (x >= curX && x < curX + w)
      return vis.at(vi);
    curX += w;
  }
  return -1;
}

int WellCompositeCanvas::splitterIndexAtX(qreal x) const
{
  // 道间分隔线 = 可见道累积边界（首道左边界不算）
  const QList<int> vis = visibleTrackIndices();
  qreal curX = -m_hScrollOffset;
  for (int vi = 0; vi < vis.size(); ++vi)
  {
    const qreal w = m_tracks.at(vis.at(vi))->width();
    const qreal boundary = curX + w;
    if (vi < vis.size() - 1 && std::abs(x - boundary) <= 5.0)
      return vi; // 左侧道可见序号
    curX += w;
  }
  return -1;
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
  const double oldPpm = pxPerMeter();
  double newFactor = qMax(0.01, factor);

  // D2.4：锚点深度在变焦前后屏幕上保持不动
  double anchorY = 0.0;
  const bool hasAnchor = anchorDepth >= m_minDepth && anchorDepth <= m_maxDepth && m_body;
  if (hasAnchor)
    anchorY = (anchorDepth - m_scrollDepth) * oldPpm;

  const double newPpm = qMax(0.01, m_basePxPerMeter * newFactor);
  double newScroll = hasAnchor ? anchorDepth - anchorY / newPpm : m_scrollDepth;

  // D2.8：视口跨度上下限修正（必要时同时收 zoom 因子）——视口可信赖时才生效
  const double bodyH = (m_body && m_body->height() >= 80) ? m_body->height() : 0.0;
  if (bodyH >= 80.0)
  {
    const double total = qMax(1.0, m_maxDepth - m_minDepth);
    const double maxSpan = m_maxVisibleSpan > 0.0 ? qMin(m_maxVisibleSpan, total * 3.0) : total;
    double span = bodyH / newPpm;
    if (span > maxSpan)
    {
      span = maxSpan;
      newFactor = bodyH / (span * m_basePxPerMeter);
    }
    else if (span < m_minVisibleSpan)
    {
      span = m_minVisibleSpan;
      newFactor = bodyH / (span * m_basePxPerMeter);
    }
  }
  m_zoomFactor = newFactor;

  if (std::abs(m_zoomFactor - oldFactor) < 1e-6 && std::abs(newScroll - m_scrollDepth) < 1e-9)
    return;

  m_scrollDepth = newScroll;
  clampViewportToLimits(&m_scrollDepth, pxPerMeter());

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

// 比例尺字符串 → 基准 px/m（H2 重构：单一换算函数，两处调用收口）
static double ppmForScaleRatio(const QString &ratioStr, double bodyH, double totalSpan)
{
  if (ratioStr == QStringLiteral("自适应") || ratioStr == QStringLiteral("Adaptive"))
    return totalSpan > 0.0 ? bodyH / totalSpan : 3779.528 / 500.0;
  if (ratioStr.startsWith(QLatin1String("1:")))
  {
    bool ok = false;
    const int denom = ratioStr.mid(2).toInt(&ok);
    if (ok && denom > 0)
      return 3779.528 / denom;
  }
  return 3779.528 / 500.0;
}

void WellCompositeCanvas::resetZoom()
{
  const double bodyH = (m_body && m_body->height() > 50) ? m_body->height() : qMax(100.0, height() - m_headerHeight - 10.0);
  const double totalSpan = qMax(1.0, m_maxDepth - m_minDepth);

  m_basePxPerMeter = ppmForScaleRatio(m_baseScaleRatio, bodyH, totalSpan);
  m_zoomFactor = 1.0;
  m_scaleRatio = m_baseScaleRatio == QStringLiteral("自适应") ? calculateScaleRatioString() : m_baseScaleRatio;

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

  m_basePxPerMeter = ppmForScaleRatio(ratioStr, bodyH, totalSpan);
  m_zoomFactor = 1.0;
  m_scaleRatio = ratioStr == QStringLiteral("自适应") ? calculateScaleRatioString() : ratioStr;

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

void WellCompositeCanvas::setPins(const QList<DepthPin> &pins)
{
  m_pins = pins;
  updateAll();
}

void WellCompositeCanvas::setMarkerLines(const QVector<DepthTools::MarkerLine> &markers)
{
  m_markers = markers;
  std::sort(m_markers.begin(), m_markers.end(),
            [](const DepthTools::MarkerLine &a, const DepthTools::MarkerLine &b) {
              return a.first < b.first;
            });
  updateAll();
}

void WellCompositeCanvas::setEditMode(bool on)
{
  if (m_editMode == on)
    return;
  m_editMode = on;
  if (!on)
    m_markerDragIndex = -1;
  updateAll(); // D3.14 道头着色 + 工具条由面板监听 editModeChanged
  emit editModeChanged(m_editMode);
}

void WellCompositeCanvas::setDepthUnitLabel(const QString &label)
{
  for (auto &t : m_tracks)
  {
    if (t && t->type() == TrackType::DepthScale)
    {
      auto dst = std::dynamic_pointer_cast<DepthScaleTrack>(t);
      if (dst) dst->setDepthUnitLabel(label);
    }
  }
  updateAll();
}

void WellCompositeCanvas::setTwtLabels(const QVector<QPair<double, QString>> &twtAtDepth)
{
  for (auto &t : m_tracks)
  {
    if (t && t->type() == TrackType::DepthScale)
    {
      auto dst = std::dynamic_pointer_cast<DepthScaleTrack>(t);
      if (dst) dst->setTwtLabels(twtAtDepth);
    }
  }
  updateAll();
}

void WellCompositeCanvas::setHighContrast(bool on)
{
  for (auto &t : m_tracks)
  {
    if (t && t->type() == TrackType::DepthScale)
    {
      auto dst = std::dynamic_pointer_cast<DepthScaleTrack>(t);
      if (dst) dst->setHighContrast(on);
    }
  }
  updateAll();
}

QString WellCompositeCanvas::toolTipFor(int trackIndex, double depth) const
{
  if (trackIndex < 0 || trackIndex >= m_tracks.size())
    return QString();
  const auto &t = m_tracks.at(trackIndex);
  return t ? t->trackToolTip(depth) : QString();
}

QString WellCompositeCanvas::trackCsvAt(int trackIndex) const
{
  if (trackIndex < 0 || trackIndex >= m_tracks.size() || !m_tracks.at(trackIndex))
    return QString();
  return TrackOps::exportTrackCsv(*m_tracks.at(trackIndex));
}

void WellCompositeCanvas::setFocusTrackIndex(int index)
{
  if (index >= m_tracks.size())
    index = m_tracks.isEmpty() ? -1 : m_tracks.size() - 1;
  if (m_focusTrackIndex == index)
    return;
  m_focusTrackIndex = index;
  updateAll(); // D7.2 焦点环重绘
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

  if (m_scaleRatio == QStringLiteral("自适应"))
  {
    const double totalSpan = qMax(1.0, m_maxDepth - m_minDepth);
    m_basePxPerMeter = bodyH / totalSpan;
  }

  syncScrollBars();
  notifyViewportChanged();
}

// D7.1 全键盘导航：Tab/Backtab 道间焦点、Enter 配置、Esc 取消、方向键滚深度、+/- 缩放
void WellCompositeCanvas::keyPressEvent(QKeyEvent *event)
{
  switch (event->key())
  {
  case Qt::Key_Escape:
    // 逐级取消：先取消进行中的道头拖拽/橡皮筋/标志层拖拽
    if (m_headerDragIndex >= 0)
    {
      m_headerDragCancelled = true;
      m_headerDragIndex = -1;
      m_headerDragInsertIndex = -1;
      updateAll();
    }
    else if (m_rubberBandActive)
    {
      m_rubberBandActive = false;
      m_rubberBandRect = QRect();
      updateAll();
    }
    else if (m_markerDragIndex >= 0)
    {
      m_markerDragIndex = -1;
      updateAll();
    }
    event->accept();
    return;
  case Qt::Key_Up:
    setScrollDepth(scrollDepth() - visibleDepthSpan() * 0.1, true);
    event->accept();
    return;
  case Qt::Key_Down:
    setScrollDepth(scrollDepth() + visibleDepthSpan() * 0.1, true);
    event->accept();
    return;
  case Qt::Key_Plus:
  case Qt::Key_Equal:
    zoomIn();
    event->accept();
    return;
  case Qt::Key_Minus:
    zoomOut();
    event->accept();
    return;
  case Qt::Key_Enter:
  case Qt::Key_Return:
    if (m_focusTrackIndex >= 0)
    {
      emit trackConfigRequested(m_focusTrackIndex);
      event->accept();
      return;
    }
    break;
  case Qt::Key_Tab:
    if (!m_tracks.isEmpty())
    {
      int next = m_focusTrackIndex + (event->modifiers() & Qt::ShiftModifier ? -1 : 1);
      if (next < 0)
        next = m_tracks.size() - 1;
      if (next >= m_tracks.size())
        next = 0;
      setFocusTrackIndex(next);
      event->accept();
      return;
    }
    break;
  default:
    break;
  }
  QWidget::keyPressEvent(event);
}

void WellCompositeCanvas::setZoomSpanLimits(double minSpanMeters, double maxSpanMeters)
{
  m_minVisibleSpan = qMax(0.1, minSpanMeters);
  m_maxVisibleSpan = qMax(0.0, maxSpanMeters);
  // 立即按新上下限夹取当前视口（视口可信赖时）
  double sd = m_scrollDepth;
  const double bodyH = (m_body && m_body->height() >= 80) ? m_body->height() : 0.0;
  if (bodyH >= 80.0)
  {
    double span = bodyH / pxPerMeter();
    const double total = qMax(1.0, m_maxDepth - m_minDepth);
    const double maxSpan = m_maxVisibleSpan > 0.0 ? qMin(m_maxVisibleSpan, total * 3.0) : total;
    if (span > maxSpan || span < m_minVisibleSpan)
    {
      span = qBound(m_minVisibleSpan, span, maxSpan);
      setZoomFactor(bodyH / (span * m_basePxPerMeter), -1.0);
    }
  }
  clampViewportToLimits(&sd, pxPerMeter());
  m_scrollDepth = sd;
  syncScrollBars();
  updateAll();
  notifyViewportChanged();
}

// ----------------------------------------------------------------------------
// WellCompositeHeader: 置顶道头（D1.3 拖拽换位 / D1.4 分隔线 / D7.2 焦点环）
// ----------------------------------------------------------------------------
WellCompositeHeader::WellCompositeHeader(WellCompositeCanvas *canvas)
  : QWidget(canvas), m_canvas(canvas)
{
  setMouseTracking(true);
}

WellCompositeHeader::HitKind WellCompositeHeader::hitTest(const QPoint &pos, int *trackVisibleIndex) const
{
  if (!m_canvas)
    return HitKind::None;
  const qreal x = pos.x();

  // 分隔线优先（5px 热区）
  const int split = m_canvas->splitterIndexAtX(x);
  if (split >= 0)
  {
    if (trackVisibleIndex)
      *trackVisibleIndex = split;
    return HitKind::Splitter;
  }

  const QList<int> vis = m_canvas->visibleTrackIndices();
  qreal curX = -m_canvas->hScrollOffset();
  for (int vi = 0; vi < vis.size(); ++vi)
  {
    const qreal w = m_canvas->tracks().at(vis.at(vi))->width();
    if (x >= curX && x < curX + w)
    {
      if (trackVisibleIndex)
        *trackVisibleIndex = vi;
      return HitKind::TrackTitle;
    }
    curX += w;
  }
  return HitKind::None;
}

void WellCompositeHeader::paintEvent(QPaintEvent * /*event*/)
{
  if (!m_canvas) return;

  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setRenderHint(QPainter::TextAntialiasing, true);

  // D3.14 编辑模式视觉区分：道头底色染橙
  p.fillRect(rect(), m_canvas->editMode() ? QColor(255, 244, 224) : QColor(QStringLiteral("#F5F7FA")));
  p.setPen(QColor(QStringLiteral("#DFE5EC")));
  p.drawLine(rect().bottomLeft(), rect().bottomRight());

  const QList<int> vis = m_canvas->visibleTrackIndices();
  qreal curX = -m_canvas->hScrollOffset();
  for (int vi = 0; vi < vis.size(); ++vi)
  {
    const auto &t = m_canvas->tracks().at(vis.at(vi));
    const qreal tw = t->width();
    const QRectF trackHeadRect(curX, 0, tw, height());

    if (trackHeadRect.right() >= 0 && trackHeadRect.left() <= width())
    {
      t->paintHeader(p, trackHeadRect, m_canvas->hoverDepth());

      // D7.2 焦点环：焦点道 2px primary 描边（DESIGN.md focus-ring，不用虚线框）
      if (vis.at(vi) == m_canvas->focusTrackIndex())
      {
        p.setPen(QPen(QColor(QStringLiteral("#1B73D0")), 2.0));
        p.setBrush(Qt::NoBrush);
        p.drawRect(trackHeadRect.adjusted(1, 1, -1, -1));
      }
    }
    curX += tw;
  }

  // D1.3 拖拽换位投影指示线：目标插入位置的蓝色竖线 + 拖起道标题幽灵
  if (m_canvas->isHeaderDragActive())
  {
    qreal insertX = -m_canvas->hScrollOffset();
    const int insertIdx = m_canvas->headerDragInsertIndex();
    const QList<int> visIdx = m_canvas->visibleTrackIndices();
    const int clamped = qBound(0, insertIdx, visIdx.size());
    for (int i = 0; i < clamped; ++i)
      insertX += m_canvas->tracks().at(visIdx.at(i))->width();

    p.setPen(QPen(QColor(QStringLiteral("#1B73D0")), 2.0));
    p.drawLine(QPointF(insertX, 0), QPointF(insertX, height()));

    // 拖起道半透明标题胶囊跟随（视觉反馈）
    const QString dragTitle = m_canvas->tracks().at(
        visIdx.value(qBound(0, m_canvas->m_headerDragIndex, visIdx.size() - 1)))->title();
    QFont f = p.font();
    f.setPointSize(8);
    f.setBold(true);
    p.setFont(f);
    const QRectF ghost(qBound(0.0, insertX + 4.0, qMax(0.0, width() - 110.0)), 4, 106, 18);
    p.fillRect(ghost, QColor(27, 115, 208, 200));
    p.setPen(Qt::white);
    p.drawText(ghost.adjusted(4, 0, -4, 0), Qt::AlignVCenter | Qt::ElideRight, dragTitle);
  }

  // D1.4 分隔线 hover 提示（拖拽热区视觉化）
  if (m_draggingSplitter && m_splitterTrackIndex >= 0 && m_splitterTrackIndex + 1 < vis.size())
  {
    qreal bx = -m_canvas->hScrollOffset();
    for (int i = 0; i <= m_splitterTrackIndex; ++i)
      bx += m_canvas->tracks().at(vis.at(i))->width();
    p.setPen(QPen(QColor(QStringLiteral("#1B73D0")), 1.5));
    p.drawLine(QPointF(bx, 0), QPointF(bx, height()));
  }
}

void WellCompositeHeader::mousePressEvent(QMouseEvent *event)
{
  if (!m_canvas) return;
  if (event->button() != Qt::LeftButton)
  {
    QWidget::mousePressEvent(event);
    return;
  }

  int vi = -1;
  const HitKind kind = hitTest(event->pos(), &vi);

  if (kind == HitKind::Splitter)
  {
    m_draggingSplitter = true;
    m_splitterTrackIndex = vi;
    const QList<int> vis = m_canvas->visibleTrackIndices();
    m_splitterStartWidth = m_canvas->tracks().at(vis.at(vi))->width();
    setCursor(Qt::SizeHorCursor);
    event->accept();
    return;
  }

  if (kind == HitKind::TrackTitle)
  {
    // D1.3 拖起：进入换位会话（移动阈值后才真正视为拖拽）
    m_canvas->m_headerDragIndex = vi;
    m_canvas->m_headerDragInsertIndex = vi;
    m_canvas->m_headerDragCancelled = false;
    event->accept();
    return;
  }

  QWidget::mousePressEvent(event);
}

void WellCompositeHeader::mouseMoveEvent(QMouseEvent *event)
{
  if (!m_canvas) return;

  if (m_draggingSplitter)
  {
    // D1.4 拖拽调宽：以按住位置为基准的水平位移改左道宽（下限 24px）
    const QList<int> vis = m_canvas->visibleTrackIndices();
    if (m_splitterTrackIndex >= 0 && m_splitterTrackIndex < vis.size())
    {
      const int trackIdx = vis.at(m_splitterTrackIndex);
      qreal left = -m_canvas->hScrollOffset();
      for (int i = 0; i < m_splitterTrackIndex; ++i)
        left += m_canvas->tracks().at(vis.at(i))->width();
      const qreal newW = qMax<qreal>(24.0, event->pos().x() - left);
      m_canvas->tracks().at(trackIdx)->setWidth(newW);
      m_canvas->syncScrollBars();
      m_canvas->updateAll();
      emit m_canvas->trackWidthChanged(trackIdx);
    }
    event->accept();
    return;
  }

  if (m_canvas->isHeaderDragActive())
  {
    // D1.3 投影插入位：按各道中心 x 计算插入序（松手提交）
    const QList<int> vis = m_canvas->visibleTrackIndices();
    qreal curX = -m_canvas->hScrollOffset();
    int insert = vis.size();
    for (int vi = 0; vi < vis.size(); ++vi)
    {
      const qreal w = m_canvas->tracks().at(vis.at(vi))->width();
      if (event->pos().x() < curX + w * 0.5)
      {
        insert = vi;
        break;
      }
      curX += w;
    }
    m_canvas->m_headerDragInsertIndex = insert;
    update();
    event->accept();
    return;
  }

  // hover 光标语义
  int vi = -1;
  const HitKind kind = hitTest(event->pos(), &vi);
  setCursor(kind == HitKind::Splitter ? Qt::SizeHorCursor : Qt::ArrowCursor);
  QWidget::mouseMoveEvent(event);
}

void WellCompositeHeader::mouseReleaseEvent(QMouseEvent *event)
{
  if (!m_canvas) return;

  if (m_draggingSplitter)
  {
    m_draggingSplitter = false;
    m_splitterTrackIndex = -1;
    setCursor(Qt::ArrowCursor);
    update();
    event->accept();
    return;
  }

  if (m_canvas->isHeaderDragActive())
  {
    // D1.3 松手提交：可见序 → 绝对序换算后 moveTrack
    const int fromVis = m_canvas->m_headerDragIndex;
    const int insertVis = m_canvas->m_headerDragInsertIndex;
    m_canvas->m_headerDragIndex = -1;

    const QList<int> vis = m_canvas->visibleTrackIndices();
    if (fromVis >= 0 && fromVis < vis.size())
    {
      const int fromAbs = vis.at(fromVis);
      int insertAbs = insertVis >= vis.size() ? m_canvas->trackCount()
                                              : vis.at(qBound(0, insertVis, vis.size() - 1));
      // moveTrack 语义：移除 fromAbs 后插入到 insertAbs 之前
      int toAbs = insertAbs;
      if (insertAbs > fromAbs)
        --toAbs;
      if (toAbs >= m_canvas->trackCount())
        m_canvas->insertTrack(m_canvas->trackCount(),
                              m_canvas->tracks().at(fromAbs));
      else
        m_canvas->moveTrack(fromAbs, toAbs);
    }
    m_canvas->m_headerDragInsertIndex = -1;
    m_canvas->updateAll();
    event->accept();
    return;
  }

  QWidget::mouseReleaseEvent(event);
}

void WellCompositeHeader::keyPressEvent(QKeyEvent *event)
{
  // Esc 取消拖拽透传给画布统一处理
  if (event->key() == Qt::Key_Escape && m_canvas)
  {
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(m_canvas, &esc);
    update();
    event->accept();
    return;
  }
  QWidget::keyPressEvent(event);
}

// ----------------------------------------------------------------------------
// WellCompositeBody: 道体渲染与手势交互
// ----------------------------------------------------------------------------
WellCompositeBody::WellCompositeBody(WellCompositeCanvas *canvas)
  : QWidget(canvas), m_canvas(canvas)
{
  setMouseTracking(true);
}

int WellCompositeBody::markerHitTest(qreal y, double *lineDepth) const
{
  if (!m_canvas)
    return -1;
  const auto &markers = m_canvas->markerLines();
  for (int i = 0; i < markers.size(); ++i)
  {
    const double ly = m_canvas->depthToY(markers.at(i).first);
    if (std::abs(ly - y) <= 4.0)
    {
      if (lineDepth)
        *lineDepth = markers.at(i).first;
      return i;
    }
  }
  return -1;
}

void WellCompositeBody::paintEvent(QPaintEvent * /*event*/)
{
  if (!m_canvas) return;

  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setRenderHint(QPainter::TextAntialiasing, true);

  p.fillRect(rect(), QColor(QStringLiteral("#FFFFFF")));

  // D7.5 空态：无道可画时给出原因而非空白
  if (m_canvas->trackCount() == 0)
  {
    p.setPen(QColor(QStringLiteral("#5D6E80")));
    QFont hintFont = p.font();
    hintFont.setPointSize(10);
    p.setFont(hintFont);
    p.drawText(rect(), Qt::AlignCenter,
               QStringLiteral("未加载井数据\n从数据页打开 LAS 曲线或综合柱状图 XML 后此处渲染道"));
    return;
  }

  const double topDepth = m_canvas->scrollDepth();
  const double visibleDepthSpan = height() / m_canvas->pxPerMeter();
  const double bottomDepth = topDepth + visibleDepthSpan;

  qreal curX = -m_canvas->hScrollOffset();
  const QList<int> vis = m_canvas->visibleTrackIndices();
  for (int vi = 0; vi < vis.size(); ++vi)
  {
    const auto &t = m_canvas->tracks().at(vis.at(vi));
    const qreal tw = t->width();
    const QRectF trackBodyRect(curX, 0, tw, height());

    if (trackBodyRect.right() >= 0 && trackBodyRect.left() <= width())
    {
      t->paintBody(p, trackBodyRect, topDepth, bottomDepth, m_canvas->pxPerMeter());
    }
    curX += tw;
  }

  // ---- D2.12 标志层间距 gap 高亮（相邻线距 > 阈值时琥珀带 + 标注）----
  if (m_canvas->gapThresholdMeters() > 0.0 && m_canvas->markerLines().size() >= 2)
  {
    const auto gaps = DepthTools::gapSegments(m_canvas->markerLines(), m_canvas->gapThresholdMeters());
    for (const auto &g : gaps)
    {
      if (g.bottom < topDepth || g.top > bottomDepth)
        continue;
      const qreal y0 = m_canvas->depthToY(g.top);
      const qreal y1 = m_canvas->depthToY(g.bottom);
      QRectF bandRect(0, y0, width(), y1 - y0);
      p.fillRect(bandRect, QColor(242, 153, 0, 26));
      p.setPen(QPen(QColor(242, 153, 0, 140), 1.0, Qt::DotLine));
      p.drawRect(bandRect);
      QFont gf = p.font();
      gf.setPointSize(7);
      p.setFont(gf);
      p.setPen(QColor(QStringLiteral("#B45309")));
      p.drawText(bandRect.adjusted(60, 2, -4, -2), Qt::AlignRight | Qt::AlignTop,
                 QStringLiteral("Δ%1m").arg(QString::number(g.span, 'f', 0)));
    }
  }

  // ---- 标志层线（横贯虚线 + 名签；编辑模式加粗且可拖 D3.1）----
  const auto &markers = m_canvas->markerLines();
  for (int i = 0; i < markers.size(); ++i)
  {
    const double d = markers.at(i).first;
    if (d < topDepth || d > bottomDepth)
      continue;
    const qreal y = m_canvas->depthToY(d);
    const bool dragging = (m_canvas->m_markerDragIndex == i);

    p.setPen(QPen(dragging ? QColor(QStringLiteral("#E53935"))
                           : QColor(QStringLiteral("#B45309")),
                  dragging ? 2.0 : 1.2, Qt::DashLine));
    p.drawLine(QPointF(0, y), QPointF(width(), y));

    // 名签（左端小旗）
    QFont mf = p.font();
    mf.setPointSize(7);
    p.setFont(mf);
    const QString label = markers.at(i).second;
    const QRectF flagRect(0, y - 8, qMax<qreal>(34.0, label.size() * 7.0 + 8), 15);
    p.fillRect(flagRect, QColor(dragging ? 253 : 255, dragging ? 235 : 244, dragging ? 235 : 224, 225));
    p.setPen(QColor(QStringLiteral("#92400E")));
    p.drawText(flagRect.adjusted(3, 0, -3, 0), Qt::AlignVCenter | Qt::ElideRight, label);
  }

  // ---- D2.3 深度标注钉渲染（钉线 + 文字旗，随缩放滚动）----
  const auto &pins = m_canvas->pins();
  for (int i = 0; i < pins.size(); ++i)
  {
    const double d = pins.at(i).depth;
    if (d < topDepth || d > bottomDepth)
      continue;
    const qreal y = m_canvas->depthToY(d);
    p.setPen(QPen(QColor(QStringLiteral("#1B73D0")), 1.0, Qt::DotLine));
    p.drawLine(QPointF(0, y), QPointF(width(), y));

    // 右端文字旗
    QFont pf = p.font();
    pf.setPointSize(7);
    p.setFont(pf);
    const QString text = pins.at(i).text.isEmpty() ? tr("钉注") : pins.at(i).text;
    const qreal flagW = qMin<qreal>(150.0, text.size() * 7.0 + 14);
    const QRectF flagRect(width() - flagW - 2, y + 1, flagW, 14);
    p.fillRect(flagRect, QColor(27, 115, 208, 215));
    p.setPen(Qt::white);
    p.drawText(flagRect.adjusted(4, 0, -4, 0), Qt::AlignVCenter | Qt::ElideRight, text);
  }

  // ---- D2.2 橡皮筋区间带 ----
  if (m_canvas->isRubberBandActive() && !m_canvas->rubberBandRect().isNull())
  {
    const QRect rb = m_canvas->rubberBandRect().normalized();
    p.fillRect(rb, QColor(27, 115, 208, 30));
    p.setPen(QPen(QColor(QStringLiteral("#1B73D0")), 1.0, Qt::DashLine));
    p.drawRect(rb);

    // 顶底深度标签
    const double dTop = m_canvas->yToDepth(rb.top());
    const double dBot = m_canvas->yToDepth(rb.bottom());
    QFont lf = p.font();
    lf.setPointSize(7);
    p.setFont(lf);
    p.setPen(QColor(QStringLiteral("#1B73D0")));
    p.drawText(rb.adjusted(4, -14, -4, -2), Qt::AlignLeft,
               QStringLiteral("%1~%2m (%3m)")
                   .arg(QString::number(dTop, 'f', 1), QString::number(dBot, 'f', 1),
                        QString::number(std::abs(dBot - dTop), 'f', 1)));
  }

  // 绘制横跨全道的十字准星虚线与深度气泡
  const double hDepth = m_canvas->hoverDepth();
  if (hDepth >= topDepth && hDepth <= bottomDepth)
  {
    const qreal crossY = (hDepth - topDepth) * m_canvas->pxPerMeter();

    p.save();
    p.setPen(QPen(QColor(QStringLiteral("#1B73D0")), 1.0, Qt::DashLine));
    p.drawLine(QPointF(0, crossY), QPointF(width(), crossY));

    const QString dStr = QStringLiteral("%1 m").arg(QString::number(hDepth, 'f', 1));
    QFont font = p.font();
    font.setFamilies({QStringLiteral("JetBrains Mono"), QStringLiteral("monospace")});
    font.setStyleHint(QFont::TypeWriter);
    font.setPointSize(8);
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
    cardFont.setFamilies({QStringLiteral("JetBrains Mono"), QStringLiteral("monospace")});
    cardFont.setStyleHint(QFont::TypeWriter);
    cardFont.setPointSize(8);
    p.setFont(cardFont);

    const double ppm = m_canvas->pxPerMeter();
    const double metersPerCm = (ppm > 1e-4) ? (37.79528 / ppm) : 5.0;
    const QString mCmStr = (metersPerCm < 1.0) ? QString::number(metersPerCm, 'f', 2)
                           : (metersPerCm < 10.0) ? QString::number(metersPerCm, 'f', 1)
                                                  : QString::number(metersPerCm, 'f', 0);
    const QString scaleText = tr("比例尺: %1 (1cm≈%2m)")
                                  .arg(m_canvas->scaleRatio(), mCmStr);
    p.setPen(QColor(QStringLiteral("#24303E")));
    p.drawText(QRectF(cardRect.left() + 8, cardRect.top() + 3, cardW - 16, 15),
               Qt::AlignLeft | Qt::AlignVCenter, scaleText);

    // 当前视口深度范围与跨度
    const QString rangeText = tr("[%1~%2m] 跨度:%3m")
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

  // D2.10 滚轮修饰：Shift ×5 加速、Ctrl ×0.2 微调
  double factor = event->angleDelta().y() > 0 ? 1.2 : 0.8333;
  if (event->modifiers() & Qt::ShiftModifier)
    factor = factor > 1.0 ? 7.0 : 0.14;
  else if (event->modifiers() & Qt::ControlModifier)
    factor = factor > 1.0 ? 1.04 : 0.96;

  // D2.4 锚点：鼠标模式取光标深度，中心模式取视口中点
  const double anchor = m_canvas->wheelZoomAtMouse()
                            ? m_canvas->yToDepth(event->position().y())
                            : m_canvas->yToDepth(height() * 0.5);

  m_canvas->setZoomFactor(m_canvas->zoomFactor() * factor, anchor);
  event->accept();
}

void WellCompositeBody::mousePressEvent(QMouseEvent *event)
{
  if (!m_canvas) return;

  // D3.1 编辑模式：标志层线按下即进入拖拽（优先于平移）
  if (m_canvas->editMode() && event->button() == Qt::LeftButton)
  {
    const int hit = markerHitTest(event->pos().y());
    if (hit >= 0)
    {
      m_canvas->m_markerDragIndex = hit;
      m_canvas->m_markerDragGrabOffset =
          m_canvas->markerLines().at(hit).first - m_canvas->yToDepth(event->pos().y());
      event->accept();
      return;
    }
  }

  if (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton)
  {
    // D2.2 Shift+左键 = 橡皮筋区间选取
    if (event->button() == Qt::LeftButton && (event->modifiers() & Qt::ShiftModifier))
    {
      m_shiftRubber = true;
      m_canvas->m_rubberBandActive = true;
      m_canvas->m_rubberBandRect = QRect(event->pos(), QSize(0, 0));
      update();
      event->accept();
      return;
    }

    m_isPanning = true;
    m_lastMousePos = event->pos();
    m_pressPos = event->pos();
    setCursor(Qt::ClosedHandCursor);
    event->accept();
    return;
  }
  QWidget::mousePressEvent(event);
}

void WellCompositeBody::mouseMoveEvent(QMouseEvent *event)
{
  if (!m_canvas) return;

  // D3.1 标志层拖拽
  if (m_canvas->m_markerDragIndex >= 0)
  {
    auto markers = m_canvas->markerLines();
    const int idx = m_canvas->m_markerDragIndex;
    if (idx >= 0 && idx < markers.size())
    {
      double newDepth = m_canvas->yToDepth(event->pos().y()) + m_canvas->m_markerDragGrabOffset;
      // D3.1 吸附到整刻度（6px 阈值内才贴）
      const double gridStep = DepthTools::niceStepFor(m_canvas->pxPerMeter());
      const double thresholdM = 6.0 / m_canvas->pxPerMeter();
      newDepth = DepthTools::snapDepth(newDepth, {}, thresholdM, false, true, gridStep);
      markers[idx].first = qBound(m_canvas->minDepth(), newDepth, m_canvas->maxDepth());
      m_canvas->setMarkerLines(markers);
    }
    update();
    event->accept();
    return;
  }

  // D2.2 橡皮筋
  if (m_shiftRubber && m_canvas->isRubberBandActive())
  {
    m_canvas->m_rubberBandRect.setBottomRight(event->pos());
    update();
    event->accept();
    return;
  }

  if (m_isPanning)
  {
    const int dy = event->pos().y() - m_lastMousePos.y();
    const int dx = event->pos().x() - m_lastMousePos.x();
    m_lastMousePos = event->pos();

    // 纵向拖拽平移深度
    const double deltaDepth = -dy / m_canvas->pxPerMeter();
    m_canvas->setScrollDepth(m_canvas->scrollDepth() + deltaDepth, true);

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

  // 更新当前悬停深度 + D1.9 道 tooltip
  const double d = m_canvas->yToDepth(event->pos().y());
  m_canvas->setHoverDepth(d);

  const int trackIdx = m_canvas->trackIndexAtX(event->pos().x());
  if (trackIdx >= 0)
  {
    const QString tip = m_canvas->toolTipFor(trackIdx, d);
    if (!tip.isEmpty())
      setToolTip(tip);
  }

  event->accept();
}

void WellCompositeBody::mouseReleaseEvent(QMouseEvent *event)
{
  if (!m_canvas) return;

  // D3.1 标志层拖拽松手：吸附后提交（面板接 markerMoved 走 undo 栈与写回）
  if (m_canvas->m_markerDragIndex >= 0)
  {
    const auto markers = m_canvas->markerLines();
    const int idx = m_canvas->m_markerDragIndex;
    m_canvas->m_markerDragIndex = -1;
    if (idx >= 0 && idx < markers.size())
      emit m_canvas->markerMoved(markers.at(idx).second, markers.at(idx).first);
    update();
    event->accept();
    return;
  }

  // D2.2 橡皮筋松手 → 区间统计
  if (m_shiftRubber && m_canvas->isRubberBandActive())
  {
    m_shiftRubber = false;
    const QRect rb = m_canvas->rubberBandRect().normalized();
    m_canvas->m_rubberBandActive = false;
    m_canvas->m_rubberBandRect = QRect();
    update();

    if (std::abs(rb.height()) >= 6)
    {
      double d0 = m_canvas->yToDepth(rb.top());
      double d1 = m_canvas->yToDepth(rb.bottom());
      if (d0 > d1)
        std::swap(d0, d1);
      emit m_canvas->intervalSelected(d0, d1);
    }
    event->accept();
    return;
  }

  if (m_isPanning)
  {
    m_isPanning = false;
    if (event->button() == Qt::LeftButton &&
        (event->pos() - m_pressPos).manhattanLength() < 4)
      emit m_canvas->depthClicked(m_canvas->yToDepth(event->pos().y()));
    setCursor(Qt::ArrowCursor);
    event->accept();
    return;
  }
  QWidget::mouseReleaseEvent(event);
}

void WellCompositeBody::mouseDoubleClickEvent(QMouseEvent *event)
{
  if (!m_canvas) return;

  // D2.3 深度标尺道上双击 = 加标注钉意图（面板弹输入框并持久化 sidecar）
  const int trackIdx = m_canvas->trackIndexAtX(event->pos().x());
  if (trackIdx >= 0 && m_canvas->tracks().at(trackIdx)->type() == TrackType::DepthScale)
  {
    emit m_canvas->pinCreateRequested(m_canvas->yToDepth(event->pos().y()));
    event->accept();
    return;
  }

  // 双击一键复位缩放
  m_canvas->resetZoom();
  event->accept();
}

void WellCompositeBody::leaveEvent(QEvent * /*event*/)
{
  if (m_canvas)
    m_canvas->setHoverDepth(-1.0);
}

// D1.5 道右键菜单：隐藏/复制/导出 CSV/配置/跳深度/删除
void WellCompositeBody::contextMenuEvent(QContextMenuEvent *event)
{
  if (!m_canvas) return;

  // D2.3 标注钉右键（编辑文字/删除/跳转）
  {
    const auto &pins = m_canvas->pins();
    const double clickDepth = m_canvas->yToDepth(event->pos().y());
    for (int i = 0; i < pins.size(); ++i)
    {
      if (std::abs(pins.at(i).depth - clickDepth) * m_canvas->pxPerMeter() <= 4.0)
      {
        QMenu pinMenu(this);
        QAction *actEdit = pinMenu.addAction(tr("编辑钉注…"));
        QAction *actJump = pinMenu.addAction(tr("跳转到该深度"));
        QAction *actDel = pinMenu.addAction(tr("删除钉注"));
        QAction *pickedPin = pinMenu.exec(event->globalPos());
        if (pickedPin == actEdit)
          emit m_canvas->pinEditRequested(i);
        else if (pickedPin == actJump)
          m_canvas->setScrollDepth(pins.at(i).depth);
        else if (pickedPin == actDel)
        {
          QList<DepthPin> next = pins;
          DepthTools::removePinAt(&next, i);
          m_canvas->setPins(next);
        }
        event->accept();
        return;
      }
    }
  }

  // D3.2 标志层右键 → 转发给面板（改名/删除/插入）
  if (m_canvas->editMode())
  {
    double lineDepth = 0.0;
    const int hit = markerHitTest(event->pos().y(), &lineDepth);
    if (hit >= 0)
    {
      emit m_canvas->markerContextMenuRequested(m_canvas->markerLines().at(hit).second, lineDepth);
      event->accept();
      return;
    }
  }

  const int trackIdx = m_canvas->trackIndexAtX(event->pos().x());
  if (trackIdx < 0)
  {
    QWidget::contextMenuEvent(event);
    return;
  }

  openTrackContextMenu(event->globalPos(), trackIdx);
  event->accept();
}

void WellCompositeBody::openTrackContextMenu(const QPoint &pos, int trackIndex)
{
  m_lastContextMenuTrack = trackIndex;
  const auto &track = m_canvas->tracks().at(trackIndex);
  QMenu menu(this);
  menu.setObjectName(QStringLiteral("wellCompositeTrackMenu"));

  QAction *actHide = menu.addAction(tr("隐藏该道"));
  QAction *actDuplicate = menu.addAction(tr("复制该道"));
  QAction *actCsv = menu.addAction(tr("导出该道 CSV…"));
  menu.addSeparator();
  QAction *actGoto = menu.addAction(tr("跳转到深度…"));
  QAction *actConfig = menu.addAction(tr("道配置…"));
  menu.addSeparator();
  QAction *actRemove = menu.addAction(tr("删除该道"));
  actRemove->setEnabled(track->type() != TrackType::DepthScale); // 标尺道不可删

  QAction *picked = menu.exec(pos);
  if (!picked)
    return;

  if (picked == actHide)
  {
    track->setVisible(false);
    m_canvas->syncScrollBars();
    m_canvas->updateAll();
    emit m_canvas->trackVisibilityChanged(trackIndex);
  }
  else if (picked == actDuplicate)
  {
    emit m_canvas->trackDuplicationRequested(trackIndex);
  }
  else if (picked == actCsv)
  {
    emit m_canvas->trackCsvRequested(trackIndex);
  }
  else if (picked == actGoto)
  {
    emit m_canvas->gotoDepthRequested();
  }
  else if (picked == actConfig)
  {
    emit m_canvas->trackConfigRequested(trackIndex);
  }
  else if (picked == actRemove)
  {
    m_canvas->removeTrack(trackIndex);
  }
}

void WellCompositeBody::beginRubberBand(const QPoint &pos)
{
  if (!m_canvas) return;
  m_canvas->m_rubberBandActive = true;
  m_canvas->m_rubberBandRect = QRect(pos, QSize(0, 0));
  update();
}

void WellCompositeBody::updateRubberBand(const QPoint &pos)
{
  if (!m_canvas || !m_canvas->isRubberBandActive()) return;
  m_canvas->m_rubberBandRect.setBottomRight(pos);
  update();
}

void WellCompositeBody::endRubberBand()
{
  if (!m_canvas || !m_canvas->isRubberBandActive()) return;
  const QRect rb = m_canvas->rubberBandRect().normalized();
  m_canvas->m_rubberBandActive = false;
  m_canvas->m_rubberBandRect = QRect();
  update();
  if (std::abs(rb.height()) >= 6)
  {
    double d0 = m_canvas->yToDepth(rb.top());
    double d1 = m_canvas->yToDepth(rb.bottom());
    if (d0 > d1)
      std::swap(d0, d1);
    emit m_canvas->intervalSelected(d0, d1);
  }
}

void WellCompositeBody::keyPressEvent(QKeyEvent *event)
{
  // 子件键盘事件透传画布（D7.1 統一键盘导航入口）
  if (m_canvas)
  {
    QApplication::sendEvent(m_canvas, event);
    if (event->isAccepted())
      return;
  }
  QWidget::keyPressEvent(event);
}

} // namespace WellComposite
