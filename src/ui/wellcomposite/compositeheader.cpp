// 层：视图
// 综合图画布·置顶道头（D1.3 拖拽换位 / D1.4 分隔线调宽 / D7.2 焦点环）——自 wellcompositecanvas.cpp 拆出（方向 66，行为零变更）
#include "wellcompositecanvas.h"
#include "../paleotheme.h"
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

namespace WellComposite
{
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
  p.fillRect(rect(), m_canvas->editMode() ? PaleoTheme::tokens(PaleoTheme::Theme::Light).warningBg : PaleoTheme::tokens(PaleoTheme::Theme::Light).surfaceAlt);
  p.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).border);
  p.drawLine(rect().bottomLeft(), rect().bottomRight());

  const QList<int> vis = m_canvas->visibleTrackIndices();
  qreal curX = -m_canvas->hScrollOffset();
  for (int vi = 0; vi < vis.size(); ++vi)
  {
    const auto t = m_canvas->tracks().at(vis.at(vi)); // 拷贝 shared_ptr：tracks() 按值返回
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
    f.setPointSize(PaleoTheme::tokens().labelPt);
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

} // namespace WellComposite
