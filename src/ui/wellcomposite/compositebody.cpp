// 层：视图
// 综合图画布·道体渲染与手势（绘制/滚轮缩放/橡皮筋/标志层编辑拖拽/右键菜单）——自 wellcompositecanvas.cpp 拆出（方向 66，行为零变更）
#include "wellcompositecanvas.h"
#include "ui/common/wheelzoom.h"
#include "../paleotheme.h"
#include <QApplication>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QToolTip>
#include <QWheelEvent>
#include <cmath>

namespace WellComposite
{
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

  p.fillRect(rect(), PaleoTheme::tokens(PaleoTheme::Theme::Light).surface);

  // D7.5 空态：无道可画时给出原因而非空白
  if (m_canvas->trackCount() == 0)
  {
    p.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).textMuted);
    QFont hintFont = p.font();
    hintFont.setPointSize(PaleoTheme::tokens().bodyPt);
    p.setFont(hintFont);
    p.drawText(rect(), Qt::AlignCenter,
               tr("未加载井数据\n从数据页打开 LAS 曲线或综合柱状图 XML 后此处渲染道"));
    return;
  }

  const double topDepth = m_canvas->scrollDepth();
  const double visibleDepthSpan = height() / m_canvas->pxPerMeter();
  const double bottomDepth = topDepth + visibleDepthSpan;

  qreal curX = -m_canvas->hScrollOffset();
  const QList<int> vis = m_canvas->visibleTrackIndices();
  for (int vi = 0; vi < vis.size(); ++vi)
  {
    const auto t = m_canvas->tracks().at(vis.at(vi)); // 拷贝 shared_ptr：tracks() 按值返回
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
      gf.setPointSize(PaleoTheme::tokens().labelPt);
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

    // 名签（左端小旗）。#482：旗宽用 QFontMetrics 实测——原「字符数×7」对
    // CJK（每字两倍宽）与字号变化都失真，文字被 ElideRight 无谓截断或旗子
    // 虚胖。
    QFont mf = p.font();
    mf.setPointSize(PaleoTheme::tokens().labelPt);
    p.setFont(mf);
    const QString label = markers.at(i).second;
    const QFontMetricsF mfMetrics(mf);
    const QRectF flagRect(0, y - 8, qMax<qreal>(34.0, mfMetrics.horizontalAdvance(label) + 12.0), 15);
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

    // 右端文字旗：#482 旗宽 QFontMetrics 实测（ElideRight 兜底）；#540-6 旗底禁交互主色装饰底——墨色 text 底 + surface 字
    QFont pf = p.font();
    pf.setPointSize(PaleoTheme::tokens().labelPt);
    p.setFont(pf);
    const QString text = pins.at(i).text.isEmpty() ? tr("钉注") : pins.at(i).text;
    const QFontMetricsF pfMetrics(pf);
    const qreal flagW = qMin<qreal>(150.0, pfMetrics.horizontalAdvance(text) + 18.0);
    const QRectF flagRect(width() - flagW - 2, y + 1, flagW, 14);
    p.fillRect(flagRect, PaleoTheme::tokens(PaleoTheme::Theme::Light).text);
    p.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).surface);
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
    lf.setPointSize(PaleoTheme::tokens().labelPt);
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
    font = PaleoTheme::monoFont(font.pointSize());
    font.setStyleHint(QFont::TypeWriter);
    font.setPointSize(PaleoTheme::tokens().labelPt);
    p.setFont(font);

    const QRectF badgeRect(2, crossY - 8, 48, 16);
    p.fillRect(badgeRect, PaleoTheme::tokens(PaleoTheme::Theme::Light).text);
    p.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).surface);
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
    p.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).border);
    p.drawRoundedRect(cardRect, PaleoTheme::tokens().radiusSm, PaleoTheme::tokens().radiusSm);

    QFont cardFont = p.font();
    cardFont = PaleoTheme::monoFont(cardFont.pointSize());
    cardFont.setStyleHint(QFont::TypeWriter);
    cardFont.setPointSize(PaleoTheme::tokens().labelPt);
    p.setFont(cardFont);

    const double ppm = m_canvas->pxPerMeter();
    const double metersPerCm = (ppm > 1e-4) ? (37.79528 / ppm) : 5.0;
    const QString mCmStr = (metersPerCm < 1.0) ? QString::number(metersPerCm, 'f', 2)
                           : (metersPerCm < 10.0) ? QString::number(metersPerCm, 'f', 1)
                                                  : QString::number(metersPerCm, 'f', 0);
    const QString scaleText = tr("比例尺: %1 (1cm≈%2m)")
                                  .arg(m_canvas->scaleRatio(), mCmStr);
    p.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).text);
    p.drawText(QRectF(cardRect.left() + 8, cardRect.top() + 3, cardW - 16, 15),
               Qt::AlignLeft | Qt::AlignVCenter, scaleText);

    // 当前视口深度范围与跨度
    const QString rangeText = tr("[%1~%2m] 跨度:%3m")
                                  .arg(QString::number(topDepth, 'f', 1))
                                  .arg(QString::number(bottomDepth, 'f', 1))
                                  .arg(QString::number(visibleDepthSpan, 'f', 1));
    p.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).textMuted);
    p.drawText(QRectF(cardRect.left() + 8, cardRect.top() + 19, cardW - 16, 15),
               Qt::AlignLeft | Qt::AlignVCenter, rangeText);
    p.restore();
  }
}

void WellCompositeBody::wheelEvent(QWheelEvent *event)
{
  if (!m_canvas) return;
  const int delta = event->angleDelta().y();
  if (delta == 0) { event->ignore(); return; }

  // 测井可视化统一惯例：裸滚轮 = 上下滚动深度；Ctrl+滚轮 = 调整深度比例尺。
  if (event->modifiers() & Qt::ControlModifier)
  {
    // Ctrl+Shift ×5 加速缩放
    const double factor = paleo::ui::wheelZoomFactor(
        delta, (event->modifiers() & Qt::ShiftModifier) ? 7.0 : 1.2);
    // D2.4 锚点：鼠标模式取光标深度，中心模式取视口中点
    const double anchor = m_canvas->wheelZoomAtMouse()
                              ? m_canvas->yToDepth(event->position().y())
                              : m_canvas->yToDepth(height() * 0.5);
    m_canvas->setZoomFactor(m_canvas->zoomFactor() * factor, anchor);
    event->accept();
    return;
  }

  // 每档滚动视口可见深度的 1/8（任意比例尺下手感一致）；Shift ×5 快速翻页
  double step = m_canvas->visibleDepthSpan() / 8.0 * (delta / 120.0);
  if (event->modifiers() & Qt::ShiftModifier)
    step *= 5.0;
  m_canvas->setScrollDepth(m_canvas->scrollDepth() - step);
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
      m_canvas->m_markerDragName = m_canvas->markerLines().at(hit).second;
      emit m_canvas->markerSelected(m_canvas->m_markerDragName);
      m_canvas->m_markerDragOrigDepth = m_canvas->markerLines().at(hit).first;
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
    // setMarkerLines 按深度重排序：拖过相邻线后索引换人——按名字重定位
    // 被拖线，否则下一帧移动的是邻居、松手上报错对象。
    int idx = -1;
    for (int i = 0; i < markers.size(); ++i)
      if (markers.at(i).second == m_canvas->m_markerDragName)
      {
        idx = i;
        break;
      }
    if (idx >= 0)
    {
      double newDepth = m_canvas->yToDepth(event->pos().y()) + m_canvas->m_markerDragGrabOffset;
      // D3.1 吸附到整刻度（6px 阈值内才贴）
      const double gridStep = DepthTools::niceStepFor(m_canvas->pxPerMeter());
      const double thresholdM = 6.0 / m_canvas->pxPerMeter();
      newDepth = DepthTools::snapDepth(newDepth, {}, thresholdM, false, true, gridStep);
      markers[idx].first = qBound(m_canvas->minDepth(), newDepth, m_canvas->maxDepth());
      m_canvas->m_markerDragIndex = idx; // 拖拽高亮跟随真实被拖线
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

  ImageDepthItem image;
  if (imageAt(event->pos(), &image))
  {
    setToolTip(tr("双击放大查看照片"));
    setCursor(Qt::PointingHandCursor);
  }
  else if (const int trackIdx = m_canvas->trackIndexAtX(event->pos().x()); trackIdx >= 0)
  {
    setToolTip(m_canvas->toolTipFor(trackIdx, d));
    setCursor(Qt::ArrowCursor);
  }
  else
  {
    setToolTip(QString());
    setCursor(Qt::ArrowCursor);
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
    m_canvas->m_markerDragIndex = -1;
    for (const auto &m : markers)
      if (m.second == m_canvas->m_markerDragName)
      {
        emit m_canvas->markerMoved(m.second, m.first);
        break;
      }
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
  // Qt 的双击序列前面已有一次 press；结束漫游，防止非模态窗口打开后残留拖拽。
  m_isPanning = false;
  ImageDepthItem image;
  if (event->button() == Qt::LeftButton && imageAt(event->pos(), &image))
  {
    emit m_canvas->imagePreviewRequested(image);
    event->accept();
    return;
  }

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

bool WellCompositeBody::imageAt(const QPoint &pos, ImageDepthItem *item) const
{
  if (!m_canvas || !rect().contains(pos)) return false;
  const int trackIndex = m_canvas->trackIndexAtX(pos.x());
  if (trackIndex < 0) return false;
  const auto track = std::dynamic_pointer_cast<ImageTrack>(m_canvas->tracks().at(trackIndex));
  if (!track) return false;
  const int visibleIndex = m_canvas->visibleTrackIndices().indexOf(trackIndex);
  QRectF bodyRect = m_canvas->trackBodyRect(visibleIndex);
  bodyRect.setHeight(height());
  const int index = track->imageIndexAt(pos, bodyRect, m_canvas->scrollDepth(), m_canvas->pxPerMeter());
  if (index < 0) return false;
  if (item) *item = track->items().at(index);
  return true;
}

void WellCompositeBody::openTrackContextMenu(const QPoint &pos, int trackIndex)
{
  m_lastContextMenuTrack = trackIndex;
  const auto track = m_canvas->tracks().at(trackIndex); // 拷贝 shared_ptr：tracks() 按值返回，后续跨 exec()
  QMenu menu(this);
  menu.setObjectName(QStringLiteral("wellCompositeTrackMenu"));
  ImageDepthItem image;
  QAction *actImage = nullptr;
  if (imageAt(mapFromGlobal(pos), &image))
  {
    actImage = menu.addAction(tr("放大查看照片…"));
    menu.addSeparator();
  }

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

  if (picked == actImage)
  {
    emit m_canvas->imagePreviewRequested(image);
  }
  else if (picked == actHide)
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
