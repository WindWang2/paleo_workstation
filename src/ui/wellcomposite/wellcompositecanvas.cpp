// 层：视图
// token 例外：DESIGN 地质文档纸面例外：纸面上的地层编辑/拾取/深度范围标记与透明承载层；标准纸面中性色已转 Light token。（tools/ui-token-exceptions.json 精确计数）。
#include "wellcompositecanvas.h"
#include "../paleotheme.h"

#include <QKeyEvent>
#include <QResizeEvent>
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
    return PaleoTheme::metricStyleSheet(QStringLiteral(
               "QScrollBar:vertical { width: 10px; background: %1; margin: 0; }"
               "QScrollBar::handle:vertical { background: %2; border-radius: {rounded.sm}px; min-height: 20px; }"
               "QScrollBar::handle:vertical:hover { background: %3; }"
               "QScrollBar:horizontal { height: 10px; background: %1; margin: 0; }"
               "QScrollBar::handle:horizontal { background: %2; border-radius: {rounded.sm}px; min-width: 20px; }"
               "QScrollBar::handle:horizontal:hover { background: %3; }"
               "QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }"))
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
  refreshScaleTracks();
}

void WellCompositeCanvas::refreshScaleTracks()
{
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
  refreshScaleTracks();
}

void WellCompositeCanvas::setScaleRatio(const QString &ratioStr)
{
  m_baseScaleRatio = ratioStr;
  const double bodyH = (m_body && m_body->height() > 50) ? m_body->height() : qMax(100.0, height() - m_headerHeight - 10.0);
  const double totalSpan = qMax(1.0, m_maxDepth - m_minDepth);

  m_basePxPerMeter = ppmForScaleRatio(ratioStr, bodyH, totalSpan);
  m_zoomFactor = 1.0;
  m_scaleRatio = ratioStr == QStringLiteral("自适应") ? calculateScaleRatioString() : ratioStr;

  refreshScaleTracks();
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

} // namespace WellComposite
