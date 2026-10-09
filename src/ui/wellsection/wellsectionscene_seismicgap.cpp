// 层：视图
// wellsectionscene_seismicgap — GapItem 井间缝：地震底图（TVD 域逐行精确
// 反解——方向 98 治本后行级 LUT 绕开已删）+ 层段带/高亮带/分层连线 +
// hover/右键断开重连。零改动拆分（方向 98）。
#include "wellsectionscene.h"
#include "wellsectionscene_internal.h"

#include "ui/paleotheme.h"

#include <QLineF>
#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionGraphicsItem>

#include <QtNumeric>
#include <cmath>

namespace wellsectionui {

GapItem::GapItem(RenderState *st, int index) : m_st(st), m_index(index)
{
  setFlag(QGraphicsItem::ItemUsesExtendedStyleOption);
  // 连线拾取：hover 高亮/提示 + 右键断开重连（左键留给列选中）。
  setAcceptHoverEvents(true);
  setAcceptedMouseButtons(Qt::RightButton);
}

QRectF GapItem::boundingRect() const
{
  return {m_st->columnRight(m_index), 0.0, m_st->gapWidth(m_index),
          m_st->sceneHeight()};
}

void GapItem::paint(QPainter *p, const QStyleOptionGraphicsItem *option,
                    QWidget *)
{
  const wellsection::Well &a = m_st->wells[m_index];
  const wellsection::Well &b = m_st->wells[m_index + 1];
  const double x0 = m_st->columnRight(m_index);
  const double x1 = m_st->columnLeft(m_index + 1);
  const QRectF gapRect(x0, 0.0, x1 - x0, m_st->sceneHeight());
  const QRectF exposed = option->exposedRect.intersected(gapRect);
  if (exposed.isEmpty())
    return;

  const wellsection::SeismicGap *gap =
      m_index < m_st->strip.gaps.size() ? &m_st->strip.gaps[m_index] : nullptr;

  p->setClipRect(exposed);

  // 1. 地震底图（按时深关系逐像素自适应采样）。
  const bool seismicDrawn = m_st->seismicOn && gap && gap->valid();
  if (seismicDrawn)
    paintSeismic(p, exposed, *gap);

  // 连线分 Effective/断开两组：断开画细虚线示意（可右键重连），不参与
  // 层段带/高亮带的闭合几何。
  const QVector<wellsection::Link> allLks = wellsection::links(a, b);
  QVector<wellsection::Link> lks;   // 连接中的
  QVector<wellsection::Link> broken; // 用户断开的
  for (const auto &lk : allLks) {
    if (wellsection::linkConnected(m_st->linkOverrides, a.id, b.id, lk.name))
      lks.push_back(lk);
    else
      broken.push_back(lk);
  }
  // 地震底图上描线先铺一圈纸面光晕，连线不被波形吃掉。
  const auto stroke = [&](const QPainterPath &path, const QPen &pen) {
    if (seismicDrawn)
    {
      QColor halo = m_st->theme.paper;
      halo.setAlpha(200);
      p->strokePath(path, QPen(halo, pen.widthF() + 2.5));
    }
    p->strokePath(path, pen);
  };

  // 2. 层段带（zoneFill）：单一闭合子路径——连线 k 正向 + 右端竖边 +
  //    连线 k+1 反向（connectPath 衔接；addPath 会各自按弦闭合，填成
  //    弦—曲线细楔而非整带）。任一端 y 换算不出（坏表井 TVD 域）→ 跳过
  //    该带（不出带不伪造）。
  if (m_st->theme.zoneFill && lks.size() >= 2)
  {
    for (int k = 0; k + 1 < lks.size(); ++k)
    {
      QPainterPath top = linkPath(lks[k]);
      QPainterPath bot = linkPath(lks[k + 1]);
      if (top.isEmpty() || bot.isEmpty())
        continue;
      QPainterPath band = top;
      band.connectPath(bot.toReversed());
      band.closeSubpath();
      QColor c = wellsection::zoneColor(m_st->zoneOrder.indexOf(lks[k].name));
      c.setAlpha(140);
      p->fillPath(band, c);
    }
  }

  // 高亮段的两条边界连线。
  const wellsection::Link *la = nullptr, *lb = nullptr;
  if (!m_st->activeTop.isEmpty())
    for (const auto &lk : lks)
    {
      if (lk.name == m_st->activeTop)
        la = &lk;
      if (lk.name == m_st->baseTop)
        lb = &lk;
    }

  // 3. 高亮段带：active 路径 + base 反向路径闭合成单一子路径（端点
  //    换算不出 → 跳过，同上）。
  if (la && lb)
  {
    QPainterPath top = linkPath(*la);
    QPainterPath bot = linkPath(*lb);
    if (!top.isEmpty() && !bot.isEmpty())
    {
      QPainterPath band = top;
      band.connectPath(bot.toReversed());
      band.closeSubpath();
      QColor hc = highlightColor();
      hc.setAlpha(55);
      p->fillPath(band, hc);
    }
  }

  // 4. 分层连线（S 形贝塞尔或直线）；高亮两界的名字不画灰线——
  //    随后只画主色 2px 边界，避免灰线与蓝线并肩。hover 中的连线加粗。
  for (const auto &lk : lks)
  {
    if (&lk == la || &lk == lb)
      continue;
    QPen pen(m_st->theme.link, m_st->theme.linkWidth);
    if (lk.name == m_hoverLink)
      pen.setWidthF(pen.widthF() + 1.2);
    stroke(linkPath(lk), pen);
  }
  // 4b. 断开的连线：细虚线示意断点（右键可重连）。
  for (const auto &lk : broken)
  {
    QPen pen(m_st->theme.frame, 1.0, Qt::DashLine);
    if (lk.name == m_hoverLink)
      pen.setWidthF(pen.widthF() + 1.0);
    stroke(linkPath(lk), pen);
  }
  // 5. 高亮边界（主色 2px，沿同一连线几何）。
  const QPen hiPen(highlightColor(), 2.0);
  if (la)
    stroke(linkPath(*la), hiPen);
  if (lb)
    stroke(linkPath(*lb), hiPen);

  // 6. 不可绘原因（地震开且缝无效；tooltip 在面板侧挂，不在 paint 里改）。
  if (m_st->seismicOn && gap && !gap->valid() && !gap->reason.isEmpty())
  {
    QFont f = p->font();
    f.setPointSize(PaleoTheme::tokens().labelPt);
    p->setFont(f);
    p->setPen(QColor(QStringLiteral("#7A7A7A")));
    const QRectF tr2(x0 + 4, qMax(24.0, exposed.top() + 8.0),
                     gapRect.width() - 8, 60);
    p->drawText(tr2, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                gap->reason);
  }
}

QPainterPath GapItem::linkPath(const wellsection::Link &lk) const
{
  const double x0 = m_st->columnRight(m_index);
  const double x1 = m_st->columnLeft(m_index + 1);
  const double yl = m_st->yForMd(m_index, lk.leftMd);
  const double yr = m_st->yForMd(m_index + 1, lk.rightMd);
  // 坏表井 TVD 域：端点换算不出 → 空路径（描边/填充自然跳过，不把 NaN
  // 喂给光栅化器）。
  if (!std::isfinite(yl) || !std::isfinite(yr))
    return QPainterPath();
  QPainterPath path(QPointF(x0, yl));
  if (m_st->theme.curvedLinks)
  {
    const double midX = (x0 + x1) * 0.5;
    path.cubicTo(QPointF(midX, yl), QPointF(midX, yr), QPointF(x1, yr));
  }
  else
    path.lineTo(x1, yr);
  return path;
}

// 连线命中：路径上采样最近距离（贝塞尔重几何不必精确解析），阈值 6px。
QString GapItem::hitLink(const QPointF &pt) const
{
  const wellsection::Well &a = m_st->wells[m_index];
  const wellsection::Well &b = m_st->wells[m_index + 1];
  const double x0 = m_st->columnRight(m_index);
  const double x1 = m_st->columnLeft(m_index + 1);
  QString best;
  double bestDist = 6.0;
  for (const auto &lk : wellsection::links(a, b))
  {
    const double yl = m_st->yForMd(m_index, lk.leftMd);
    const double yr = m_st->yForMd(m_index + 1, lk.rightMd);
    const bool curved = m_st->theme.curvedLinks;
    QPointF prev(x0, yl);
    for (int s = 1; s <= 16; ++s)
    {
      const double t = double(s) / 16.0;
      QPointF cur;
      if (curved)
      {
        const double mt = 1.0 - t;
        const double mx = (x0 + x1) * 0.5;
        cur.setX(mt * mt * mt * x0 + 3.0 * mt * mt * t * mx +
                 3.0 * mt * t * t * mx + t * t * t * x1);
        cur.setY(mt * mt * mt * yl + 3.0 * mt * mt * t * yl +
                 3.0 * mt * t * t * yr + t * t * t * yr);
      }
      else
      {
        cur.setX(x0 + (x1 - x0) * t);
        cur.setY(yl + (yr - yl) * t);
      }
      // 线段 prev→cur 到点的距离（点到线段投影）。
      const QPointF d = cur - prev;
      const double len2 = QPointF::dotProduct(d, d);
      double u = len2 > 0.0
                     ? QPointF::dotProduct(pt - prev, d) / len2
                     : 0.0;
      u = qBound(0.0, u, 1.0);
      const QPointF proj = prev + u * d;
      const double dist = QLineF(pt, proj).length();
      if (dist < bestDist)
      {
        bestDist = dist;
        best = lk.name;
      }
      prev = cur;
    }
  }
  return best;
}

void GapItem::hoverMoveEvent(QGraphicsSceneHoverEvent *e)
{
  const QString name = hitLink(e->pos());
  if (name == m_hoverLink)
    return; // 无变化不重画
  m_hoverLink = name;
  update();
  if (m_linkHover)
  {
    if (name.isEmpty())
      m_linkHover(QString());
    else
    {
      const wellsection::Well &a = m_st->wells[m_index];
      const wellsection::Well &b = m_st->wells[m_index + 1];
      const bool connected = wellsection::linkConnected(
          m_st->linkOverrides, a.id, b.id, name);
      m_linkHover(QObject::tr("连线 %1：%2 ↔ %3（%4，右键%5）")
                      .arg(name, a.name, b.name,
                           connected ? QObject::tr("已连接") : QObject::tr("已断开"),
                           connected ? QObject::tr("断开") : QObject::tr("重连")));
    }
  }
  QGraphicsItem::hoverMoveEvent(e);
}

void GapItem::hoverLeaveEvent(QGraphicsSceneHoverEvent *e)
{
  if (!m_hoverLink.isEmpty())
  {
    m_hoverLink.clear();
    update();
    if (m_linkHover)
      m_linkHover(QString());
  }
  QGraphicsItem::hoverLeaveEvent(e);
}

void GapItem::mousePressEvent(QGraphicsSceneMouseEvent *e)
{
  if (e->button() == Qt::RightButton)
  {
    const QString name = hitLink(e->pos());
    if (!name.isEmpty() && m_linkMenu)
    {
      const wellsection::Well &a = m_st->wells[m_index];
      const wellsection::Well &b = m_st->wells[m_index + 1];
      m_linkMenu(m_index, name,
                 wellsection::linkConnected(m_st->linkOverrides, a.id, b.id,
                                            name));
      e->accept();
      return;
    }
  }
  QGraphicsItem::mousePressEvent(e);
}

void GapItem::paintSeismic(QPainter *p, const QRectF &exposed,
                           const wellsection::SeismicGap &gap)
{
  const wellsection::Well &a = m_st->wells[m_index];
  const wellsection::Well &b = m_st->wells[m_index + 1];
  const double offA = m_st->offsets.value(m_index, 0.0);
  const double offB = m_st->offsets.value(m_index + 1, 0.0);
  const double x0 = m_st->columnRight(m_index);
  const double gapW = m_st->gapWidth(m_index);

  // 设备分辨率渲染：exposed 场景区 → 设备像素 rect。
  const QTransform xf = p->deviceTransform();
  const QRect devRect = xf.mapRect(exposed).toAlignedRect();
  if (devRect.isEmpty())
    return;

  const bool cacheHit =
      !m_img.isNull() && m_imgRect == devRect &&
      m_imgKeyLayout == m_st->layoutVersion &&
      m_imgKeyStrip == m_st->stripVersion &&
      m_imgKeyTheme == m_st->theme.id;
  if (!cacheHit)
  {
    // 256 级 LUT：灰阶 +峰黑→白；RWB 正红/零白/负蓝。
    const bool gray = m_st->theme.seismicGray;
    QColor lut[256];
    for (int i = 0; i < 256; ++i)
    {
      if (gray)
        lut[i] = QColor::fromRgb(255 - i, 255 - i, 255 - i);
      else if (i >= 128)
      {
        const double t = (i - 128) / 127.0;
        lut[i] = QColor::fromRgbF(1.0, 1.0 - 0.784 * t, 1.0 - 0.843 * t);
      }
      else
      {
        const double t = i / 127.0;
        lut[i] = QColor::fromRgbF(0.082 + 0.918 * t, 0.396 + 0.604 * t,
                                  0.753 + 0.247 * t);
      }
    }
    const int alpha = int(m_st->theme.seismicOpacity * 255 + 0.5);
    const float clip = qMax(1e-6f, m_st->strip.clip);
    const QTransform inv = xf.inverted();
    // TVD 域：显示深（垂深）先经井斜反解回 MD 再走时深（直井恒等；坏表
    // → NaN 整行透明）。tvdToMd 已治本（deviationsurvey 构造预建段缓存 +
    // 站点二分，方向 98）：逐行直调即精确反解，旧的「每 8 行取 knot、行间
    // 线性内插」行级 LUT 绕开（亚米~米级内插误差）随之删除——全幅重算
    // 路径逐行 O(段数+100·log 站数)，不再需要近似快路径。
    const bool tvdDomain = m_st->domain == wellsection::DepthDomain::TVD;
    // 行号 → 该井 MD（TVD 域 = 井斜精确反解；MD 域 = 显示深+偏移直通）。
    const auto mdAtRow = [&](int dy, double off, const wellsection::Well &w) {
      const double sy = inv.map(QPointF(0, devRect.top() + dy + 0.5)).y();
      const double display = m_st->displayAtY(sy);
      return tvdDomain ? w.mdOf(display + off) : display + off;
    };

    m_img = QImage(devRect.size(), QImage::Format_ARGB32_Premultiplied);
    m_img.fill(Qt::transparent);
    for (int dy = 0; dy < devRect.height(); ++dy)
    {
      const double mdA = mdAtRow(dy, offA, a);
      const double mdB = mdAtRow(dy, offB, b);
      const double tA = (a.timeDepth && std::isfinite(mdA))
                            ? a.timeDepth->twtAt(mdA)
                            : qQNaN();
      const double tB = (b.timeDepth && std::isfinite(mdB))
                            ? b.timeDepth->twtAt(mdB)
                            : qQNaN();
      QRgb *line = reinterpret_cast<QRgb *>(m_img.scanLine(dy));
      for (int dx = 0; dx < devRect.width(); ++dx)
      {
        const double sx =
            inv.map(QPointF(devRect.left() + dx + 0.5, 0)).x();
        const double f = (sx - x0) / gapW;
        double twt = qQNaN();
        if (std::isfinite(tA) && std::isfinite(tB))
          twt = tA + (tB - tA) * f;
        const float amp = gap.sampleAt(f, twt);
        if (!std::isfinite(amp))
        {
          line[dx] = qRgba(0, 0, 0, 0);
          continue;
        }
        const double norm = qBound(-1.0, double(amp) / clip, 1.0);
        QColor c = lut[int((norm + 1.0) * 127.5)];
        c.setAlpha(alpha);
        line[dx] = c.rgba();
      }
    }
    m_imgRect = devRect;
    m_imgKeyLayout = m_st->layoutVersion;
    m_imgKeyStrip = m_st->stripVersion;
    m_imgKeyTheme = m_st->theme.id;
  }
  p->drawImage(exposed, m_img);
}

} // namespace wellsectionui
