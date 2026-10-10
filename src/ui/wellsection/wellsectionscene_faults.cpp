// 层：视图
// wellsectionscene_faults — FaultOverlayItem 断层投绘覆盖项（全幅）：
// trace 沿井路径分数映射列中心 x，深度经缝内插基准面偏移映射 y。
// 零改动拆分（方向 98）。
#include "wellsectionscene.h"

#include "ui/paleotheme.h"

#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionGraphicsItem>

#include <QtNumeric>
#include <cmath>

namespace wellsectionui {

FaultOverlayItem::FaultOverlayItem(RenderState *st) : m_st(st)
{
  setFlag(QGraphicsItem::ItemUsesExtendedStyleOption);
  setZValue(5); // 断层线压在连线/地震之上
}

QRectF FaultOverlayItem::boundingRect() const
{
  return {0.0, 0.0, m_st->sceneWidth(), m_st->sceneHeight()};
}

double FaultOverlayItem::xForAlong(double along) const
{
  const int n = m_st->wells.size();
  if (n < 2 || m_st->pathFractions.size() != n)
    return qQNaN();
  const double cw = m_st->columnWidth();
  if (along <= m_st->pathFractions.first())
    return m_st->columnLeft(0) + cw * 0.5;
  if (along >= m_st->pathFractions.last())
    return m_st->columnLeft(n - 1) + cw * 0.5;
  for (int i = 0; i + 1 < n; ++i)
    if (along <= m_st->pathFractions[i + 1])
    {
      const double f = (along - m_st->pathFractions[i]) /
                       (m_st->pathFractions[i + 1] - m_st->pathFractions[i]);
      return m_st->columnLeft(i) + cw * 0.5 +
             f * (m_st->columnLeft(i + 1) - m_st->columnLeft(i));
    }
  return qQNaN();
}

double FaultOverlayItem::offsetAtX(double x) const
{
  const int n = m_st->wells.size();
  if (n == 0)
    return 0.0;
  if (n == 1)
    return m_st->offsets.value(0, 0.0);
  const double cw = m_st->columnWidth();
  for (int i = 0; i + 1 < n; ++i)
  {
    const double xl = m_st->columnLeft(i) + cw * 0.5;
    const double xr = m_st->columnLeft(i + 1) + cw * 0.5;
    if (x <= xr || i + 2 == n)
    {
      const double f = qBound(0.0, (x - xl) / (xr - xl), 1.0);
      return m_st->offsets.value(i, 0.0) +
             f * (m_st->offsets.value(i + 1, 0.0) -
                  m_st->offsets.value(i, 0.0));
    }
  }
  return m_st->offsets.last();
}

void FaultOverlayItem::paint(QPainter *p,
                             const QStyleOptionGraphicsItem *option, QWidget *)
{
  if (!m_st->faultsOn || m_st->faultTraces.isEmpty())
    return;
  const QRectF exposed = option->exposedRect.intersected(boundingRect());
  if (exposed.isEmpty())
    return;
  p->setClipRect(exposed);
  QFont f = p->font();
  f.setPointSize(PaleoTheme::kLabelPt);
  for (const wellsection::FaultTrace &trace : m_st->faultTraces)
  {
    QPainterPath path;
    bool started = false;
    double firstY = 0.0, firstX = 0.0;
    for (const wellsection::FaultTracePoint &pt : trace.points)
    {
      const double x = xForAlong(pt.along);
      if (!std::isfinite(x))
        continue;
      const double y = m_st->yForDisplay(pt.depth - offsetAtX(x));
      if (!std::isfinite(y))
        continue;
      if (!started)
      {
        path.moveTo(x, y);
        started = true;
        firstX = x;
        firstY = y;
      }
      else
        path.lineTo(x, y);
    }
    if (!started)
      continue;
    // 纸面光晕打底（地震底图上保持可读），再描断层主色。
    QColor halo = m_st->theme.paper;
    halo.setAlpha(190);
    p->strokePath(path, QPen(halo, 4.0));
    p->strokePath(path, QPen(m_st->theme.fault, 1.8));
    // 断层名（首个样点旁）。
    if (firstY > exposed.top() + 10 && firstY < exposed.bottom() - 4)
    {
      p->setFont(f);
      p->setPen(m_st->theme.fault);
      p->drawText(QRectF(firstX - 40, firstY - 16, 80, 14),
                  Qt::AlignHCenter | Qt::AlignVCenter | Qt::TextSingleLine,
                  trace.faultName);
    }
  }
}

} // namespace wellsectionui
