// 层：视图
#include "wellsectionscene.h"

#include "domain/seismic/nicestep.h"
#include "ui/paleotheme.h"

#include <QApplication>
#include <QGraphicsScene>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QStyleOptionGraphicsItem>
#include <QWheelEvent>

#include <QtNumeric>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace wellsectionui {

namespace {
constexpr double kNameRowH = 26.0;
constexpr qreal kDragThreshold = 6.0;

// 高亮一律取 UI token primary（不随图件主题；与层位 chip 同语义）。
QColor highlightColor() { return PaleoTheme::tokens().primary; }

QString scaleText(double lo, double hi)
{
  const auto fmt = [](double v) {
    QString s = QString::number(v, 'g', 4);
    return s;
  };
  return QStringLiteral("%1—%2").arg(fmt(lo), fmt(hi));
}

// 曲线值 → 道内 x 归一 [0,1]（logScale 取 log10）。
double curveNorm(const wellsection::CurveStyle &c, double v)
{
  double lo = c.min, hi = c.max;
  if (c.logScale)
  {
    if (v <= 0 || lo <= 0 || hi <= 0)
      return qQNaN();
    lo = std::log10(lo);
    hi = std::log10(hi);
    v = std::log10(v);
  }
  if (hi <= lo)
    return qQNaN();
  return (v - lo) / (hi - lo);
}

// 岩性纹色：浅色底上用 frame 画纹，否则同色加深。
QColor patternColor(const QColor &base, const QColor &dark)
{
  return base.lightness() > 230 ? dark : base.darker(160);
}

// 题注换行：行宽过 w 时在 '/' 前断开（「深度」/「/m」），空格处亦可断
// （空格挂前段尾，合行时不丢——「GR 推断」→「GR 」/「推断」）；
// 单段仍宽才右省略。
QStringList wrapCaption(const QString &text, double w, const QFontMetricsF &fm)
{
  QStringList segs;
  int pos = 0;
  for (int i = 1; i < text.size(); ++i)
    if (text.at(i) == QLatin1Char('/'))
    {
      segs << text.mid(pos, i - pos);
      pos = i;
    }
    else if (text.at(i) == QLatin1Char(' '))
    {
      segs << text.mid(pos, i - pos + 1);
      pos = i + 1;
    }
  segs << text.mid(pos);
  QStringList lines;
  QString cur;
  for (const QString &s : segs)
  {
    if (!cur.isEmpty() && fm.horizontalAdvance(cur + s) > w)
    {
      lines << cur;
      cur = s;
    }
    else
      cur += s;
  }
  if (!cur.isEmpty())
    lines << cur;
  for (QString &l : lines)
    if (fm.horizontalAdvance(l) > w)
      l = fm.elidedText(l, Qt::ElideRight, int(w));
  return lines;
}
} // namespace

// ---------------------------------------------------------------------------
// RenderState
// ---------------------------------------------------------------------------
int RenderState::columnAtX(double x) const
{
  if (wells.isEmpty())
    return -1;
  const double cw = columnWidth();
  const double rel = x - margin;
  if (rel < 0)
    return -1;
  const int i = int(rel / (cw + gapPx));
  if (i < 0 || i >= wells.size())
    return -1;
  return (rel - i * (cw + gapPx) <= cw) ? i : -1;
}

double RenderState::sceneWidth() const
{
  if (wells.isEmpty())
    return 2.0 * margin;
  return 2.0 * margin + wells.size() * columnWidth() +
         (wells.size() - 1) * gapPx;
}

// ---------------------------------------------------------------------------
// ColumnItem — 单井整列
// ---------------------------------------------------------------------------
ColumnItem::ColumnItem(RenderState *st, int index) : m_st(st), m_index(index)
{
  setFlag(QGraphicsItem::ItemUsesExtendedStyleOption);
}

QRectF ColumnItem::boundingRect() const
{
  return {m_st->columnLeft(m_index), 0.0, m_st->columnWidth(),
          m_st->sceneHeight()};
}

void ColumnItem::paint(QPainter *p, const QStyleOptionGraphicsItem *option,
                       QWidget *)
{
  const wellsection::Well &w = m_st->wells[m_index];
  const double off = m_st->offsets.value(m_index, 0.0);
  const QRectF colRect = boundingRect();
  const QRectF exposed = option->exposedRect.intersected(colRect);
  if (exposed.isEmpty())
    return;

  p->fillRect(colRect, m_st->theme.paper);
  p->setClipRect(exposed);

  const double left = colRect.left();
  const wellsection::Interval hi =
      m_st->activeTop.isEmpty()
          ? wellsection::Interval{}
          : wellsection::formationInterval(w, m_st->activeTop, m_st->baseTop);

  // 各道内容
  double x = left;
  int trackIdx = 0;
  for (const wellsection::TrackSpec &tr : m_st->tpl.tracks)
  {
    const double tw = qBound(24, tr.width, 200);
    const QRectF trRect(x, 0.0, tw, m_st->sceneHeight());
    switch (tr.kind)
    {
      case wellsection::TrackKind::Zone:
      {
        // 高亮段底色在层段道内先铺（标签在其上）。
        if (hi.hasBase())
        {
          const double hy0 = m_st->yForMd(m_index, hi.topMd);
          const double hy1 = m_st->yForMd(m_index, hi.baseMd);
          QColor hc = highlightColor();
          hc.setAlpha(55);
          p->fillRect(QRectF(trRect.left(), hy0, tw, hy1 - hy0), hc);
        }
        paintZoneTrack(p, trRect, exposed);
        break;
      }
      case wellsection::TrackKind::Curve:
        paintCurveTrack(p, trRect, exposed, tr, trackIdx);
        break;
      case wellsection::TrackKind::Depth:
        paintDepthTrack(p, trRect, exposed);
        break;
      case wellsection::TrackKind::Lithology:
        paintLithologyTrack(p, trRect, exposed, tr);
        break;
    }
    x += tw;
    ++trackIdx;
  }

  // 分层界线：可见顶各一条，横贯整列。
  p->setPen(QPen(m_st->theme.link, 1.0));
  for (const wellsection::Top &t : w.tops)
  {
    const double y = m_st->yForDisplay(t.md - off);
    if (y >= exposed.top() - 1 && y <= exposed.bottom() + 1)
      p->drawLine(QPointF(left, y), QPointF(colRect.right(), y));
  }

  // 高亮段边界线（2px 主色，横贯整列）。
  if (hi.valid())
  {
    p->setPen(QPen(highlightColor(), 2.0));
    const double hy0 = m_st->yForMd(m_index, hi.topMd);
    if (hy0 >= exposed.top() - 2 && hy0 <= exposed.bottom() + 2)
      p->drawLine(QPointF(left, hy0), QPointF(colRect.right(), hy0));
    if (hi.hasBase())
    {
      const double hy1 = m_st->yForMd(m_index, hi.baseMd);
      if (hy1 >= exposed.top() - 2 && hy1 <= exposed.bottom() + 2)
        p->drawLine(QPointF(left, hy1), QPointF(colRect.right(), hy1));
    }
  }

  // 外框 + 道分隔线（最上层，保证边界清爽）。
  p->setClipping(false);
  p->setPen(QPen(m_st->theme.frame, 1.0));
  p->drawRect(colRect);
  x = left;
  for (int i = 0; i + 1 < m_st->tpl.tracks.size(); ++i)
  {
    x += qBound(24, m_st->tpl.tracks.at(i).width, 200);
    p->drawLine(QPointF(x, colRect.top()), QPointF(x, colRect.bottom()));
  }
}

void ColumnItem::paintZoneTrack(QPainter *p, const QRectF &trackRect,
                                const QRectF &exposed)
{
  const wellsection::Well &w = m_st->wells[m_index];
  const double off = m_st->offsets.value(m_index, 0.0);
  const auto zs = wellsection::zones(w, m_st->window.base + off);
  QFont f = p->font();
  f.setPointSize(PaleoTheme::kBodyPt);
  const bool zoneFill = m_st->theme.zoneFill;
  for (const wellsection::Zone &z : zs)
  {
    const double y0 = m_st->yForMd(m_index, z.topMd);
    const double y1 = m_st->yForMd(m_index, z.baseMd);
    if (y1 < exposed.top() || y0 > exposed.bottom())
      continue;
    const QRectF band(trackRect.left(), y0, trackRect.width(), y1 - y0);
    if (zoneFill)
    {
      QColor c = wellsection::zoneColor(m_st->zoneOrder.indexOf(z.name));
      c.setAlpha(140); // ≈55%
      p->fillRect(band, c);
    }
    if (y1 - y0 < 14.0)
      continue; // 带太薄不写名
    const bool hot = !m_st->activeTop.isEmpty() && z.name == m_st->activeTop;
    QFont zf = f;
    zf.setBold(hot);
    p->setFont(zf);
    p->setPen(hot ? highlightColor() : m_st->theme.text);
    p->drawText(band, Qt::AlignCenter | Qt::TextSingleLine, z.name);
  }
  p->setFont(f);
}

void ColumnItem::paintCurveTrack(QPainter *p, const QRectF &trackRect,
                                 const QRectF &exposed,
                                 const wellsection::TrackSpec &tr, int trackIdx)
{
  const wellsection::Well &w = m_st->wells[m_index];
  const double innerL = trackRect.left() + 2.0;
  const double innerW = trackRect.width() - 4.0;
  if (innerW <= 1.0)
    return;

  if (m_pathVersion != m_st->curveVersion)
  {
    m_pathCache.clear();
    m_sandCache.clear();
    m_pathVersion = m_st->curveVersion;
  }

  for (int ci = 0; ci < tr.curves.size(); ++ci)
  {
    const wellsection::CurveStyle &cs = tr.curves.at(ci);
    const wellsection::Curve *curve = w.curve(cs.mnemonic);
    if (!curve || curve->depths.size() < 2)
      continue;

    // 路径缓存键：道序 × 曲线序。
    const quint64 pathKey = (quint64(trackIdx) << 8) | quint64(ci);
    QPainterPath path = m_pathCache.value(pathKey);
    QPainterPath sand;
    const bool buildSand = tr.sandFill && ci == 0;
    if (buildSand)
      sand = m_sandCache.value(pathKey);
    if (path.isEmpty())
    {
      // 按深度升序的取样序。
      const int n = curve->depths.size();
      QVector<int> order(n);
      std::iota(order.begin(), order.end(), 0);
      std::sort(order.begin(), order.end(), [&](int a, int b) {
        return curve->depths[a] < curve->depths[b];
      });
      const double cutNorm = curveNorm(cs, tr.cutoff);
      const double xCut = std::isfinite(cutNorm)
                              ? innerL + cutNorm * innerW
                              : qQNaN(); // 砂充填边界 x

      // 逐像素行 min/max 抽稀：一行的样点收成一段横条，行间以折线
      // 相连（连续 zigzag，不走 moveTo 断笔）。
      int prevRow = INT_MIN;
      double rowMinX = 0.0, rowMaxX = 0.0;
      bool rowHasSand = false, rowAllSand = true, pathStarted = false;
      const auto flush = [&] {
        if (prevRow == INT_MIN)
          return;
        const double y = prevRow + 0.5;
        if (!pathStarted)
        {
          path.moveTo(rowMinX, y);
          pathStarted = true;
        }
        else
          path.lineTo(rowMinX, y);
        path.lineTo(rowMaxX, y);
        if (buildSand && rowHasSand)
        {
          const double fx = rowAllSand ? rowMaxX : xCut;
          if (std::isfinite(fx) && fx > innerL + 0.2)
            sand.addRect(innerL, y - 0.5, fx - innerL, 1.0);
        }
      };
      for (int k = 0; k < n; ++k)
      {
        const double md = curve->depths[order[k]];
        const double v = curve->values[order[k]];
        const double nx =
            (std::isfinite(md) && std::isfinite(v)) ? curveNorm(cs, v) : qQNaN();
        if (!std::isfinite(nx))
        {
          flush();
          prevRow = INT_MIN;
          pathStarted = false; // 缺样断线
          continue;
        }
        const double sx = qBound(innerL, innerL + nx * innerW, innerL + innerW);
        const int row = int(m_st->yForMd(m_index, md));
        if (row != prevRow)
        {
          flush();
          prevRow = row;
          rowMinX = rowMaxX = sx;
          rowHasSand = v < tr.cutoff;
          rowAllSand = rowHasSand;
        }
        else
        {
          rowMinX = qMin(rowMinX, sx);
          rowMaxX = qMax(rowMaxX, sx);
          if (v < tr.cutoff)
            rowHasSand = true;
          else
            rowAllSand = false;
        }
      }
      flush();
      m_pathCache.insert(pathKey, path);
      if (buildSand)
        m_sandCache.insert(pathKey, sand);
    }

    // 砂充填在曲线之下。
    if (buildSand && !sand.isEmpty())
    {
      p->save();
      p->setClipRect(trackRect.intersected(exposed));
      if (m_st->theme.sand.alpha() == 0)
      {
        // 简洁主题：不充底色也不描行边（逐行矩形描边会成黑杠），
        // 纸面上只铺一层稀疏黑点阵。
        p->fillPath(sand, QBrush(m_st->theme.frame, Qt::Dense6Pattern));
      }
      else
      {
        p->fillPath(sand, m_st->theme.sand);
        p->fillPath(sand, QBrush(m_st->theme.sandDots, Qt::Dense6Pattern));
      }
      p->restore();
    }

    p->save();
    p->setClipRect(trackRect.intersected(exposed));
    p->setPen(QPen(cs.color.isValid() ? cs.color : m_st->theme.text, 1.2));
    p->setBrush(Qt::NoBrush);
    p->drawPath(path);
    p->restore();
  }
}

void ColumnItem::paintDepthTrack(QPainter *p, const QRectF &trackRect,
                                 const QRectF &exposed)
{
  const double off = m_st->offsets.value(m_index, 0.0);
  // 可见真实 MD 范围（显示深 + 偏移回 MD）。
  const double mdLo = m_st->displayAtY(exposed.top()) + off;
  const double mdHi = m_st->displayAtY(exposed.bottom()) + off;
  if (mdHi - mdLo < 1e-3)
    return;
  const double yTop = m_st->yForMd(m_index, mdLo);
  const double yBot = m_st->yForMd(m_index, mdHi);
  const int target = qMax(4, int((yBot - yTop) / 60.0));
  // 海拔模式：轴标 = kb − MD（补心海拔基准，向上为正）——刻度取整在
  // 海拔空间做，井深/拉平模式照旧在 MD 空间取整。
  const auto ticks = m_st->datum.mode == wellsection::DatumMode::Elevation
                         ? seismic::NiceStep::GenerateTicks(
                               m_st->wells[m_index].kb - mdHi,
                               m_st->wells[m_index].kb - mdLo, yBot, yTop,
                               target, QStringLiteral("%.0f"), true)
                         : seismic::NiceStep::GenerateTicks(
                               mdLo, mdHi, yTop, yBot, target,
                               QStringLiteral("%.0f"), true);

  QFont mono = PaleoTheme::monoFont();
  mono.setPointSize(PaleoTheme::kLabelPt);
  p->setFont(mono);
  const QFontMetricsF fm(mono);
  const QColor faint = m_st->theme.frame;
  const double cx = trackRect.center().x();
  for (const auto &t : ticks)
  {
    const double y = t.pixelPos;
    if (t.isMajor)
    {
      p->setPen(QPen(faint, 0.6));
      p->drawLine(QPointF(trackRect.left() + 1, y),
                  QPointF(trackRect.right() - 1, y));
      const QString label = t.label;
      const QRectF tr2(trackRect.left() + 1, y - fm.height() * 0.62,
                       trackRect.width() - 2, fm.height());
      // 文本框出图体边界（顶部/底部裁切）则不写标签，刻线照画。
      if (tr2.top() >= 0.0 && tr2.bottom() <= m_st->sceneHeight())
      {
        // 标签衬纸底色，刻线不穿字。
        p->fillRect(tr2, m_st->theme.paper);
        p->setPen(m_st->theme.text);
        p->drawText(tr2, Qt::AlignHCenter | Qt::AlignVCenter, label);
      }
    }
    else
    {
      p->setPen(QPen(faint, 0.5));
      p->drawLine(QPointF(cx - 3, y), QPointF(cx + 3, y));
    }
  }
}

void ColumnItem::paintLithologyTrack(QPainter *p, const QRectF &trackRect,
                                     const QRectF &exposed,
                                     const wellsection::TrackSpec &tr)
{
  const wellsection::Well &w = m_st->wells[m_index];
  const wellsection::Curve *src = w.curve(tr.sourceMnemonic);
  if (!src)
    return;
  const auto intervals = wellsection::inferSandShale(*src, tr.cutoff);
  const double w2 = trackRect.width();
  for (const wellsection::LithoInterval &iv : intervals)
  {
    const double y0 = m_st->yForMd(m_index, iv.topMd);
    const double y1 = m_st->yForMd(m_index, iv.baseMd);
    if (y1 < exposed.top() || y0 > exposed.bottom())
      continue;
    if (iv.sand)
    {
      const QRectF r(trackRect.left(), y0, w2, y1 - y0);
      const QColor base = m_st->theme.lithoSand;
      p->fillRect(r, base);
      p->fillRect(r, QBrush(patternColor(base, m_st->theme.frame),
                            Qt::Dense6Pattern));
    }
    else
    {
      const QRectF r(trackRect.left(), y0, w2 * 0.6, y1 - y0);
      const QColor base = m_st->theme.lithoShale;
      p->fillRect(r, base);
      p->fillRect(r, QBrush(patternColor(base, m_st->theme.frame),
                            Qt::HorPattern));
    }
  }
}

// ---------------------------------------------------------------------------
// GapItem — 井间缝
// ---------------------------------------------------------------------------
GapItem::GapItem(RenderState *st, int index) : m_st(st), m_index(index)
{
  setFlag(QGraphicsItem::ItemUsesExtendedStyleOption);
}

QRectF GapItem::boundingRect() const
{
  return {m_st->columnRight(m_index), 0.0, m_st->gapPx,
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

  const auto lks = wellsection::links(a, b);
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
  //    弦—曲线细楔而非整带）。
  if (m_st->theme.zoneFill && lks.size() >= 2)
  {
    for (int k = 0; k + 1 < lks.size(); ++k)
    {
      QPainterPath band = linkPath(lks[k]);
      band.connectPath(linkPath(lks[k + 1]).toReversed());
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

  // 3. 高亮段带：active 路径 + base 反向路径闭合成单一子路径。
  if (la && lb)
  {
    QPainterPath band = linkPath(*la);
    band.connectPath(linkPath(*lb).toReversed());
    band.closeSubpath();
    QColor hc = highlightColor();
    hc.setAlpha(55);
    p->fillPath(band, hc);
  }

  // 4. 分层连线（S 形贝塞尔或直线）；高亮两界的名字不画灰线——
  //    随后只画主色 2px 边界，避免灰线与蓝线并肩。
  for (const auto &lk : lks)
  {
    if (&lk == la || &lk == lb)
      continue;
    stroke(linkPath(lk), QPen(m_st->theme.link, m_st->theme.linkWidth));
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
    f.setPointSize(PaleoTheme::kLabelPt);
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

void GapItem::paintSeismic(QPainter *p, const QRectF &exposed,
                           const wellsection::SeismicGap &gap)
{
  const wellsection::Well &a = m_st->wells[m_index];
  const wellsection::Well &b = m_st->wells[m_index + 1];
  const double offA = m_st->offsets.value(m_index, 0.0);
  const double offB = m_st->offsets.value(m_index + 1, 0.0);
  const double x0 = m_st->columnRight(m_index);
  const double gapW = m_st->gapPx;

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

    m_img = QImage(devRect.size(), QImage::Format_ARGB32_Premultiplied);
    m_img.fill(Qt::transparent);
    const QTransform inv = xf.inverted();
    for (int dy = 0; dy < devRect.height(); ++dy)
    {
      const double sy = inv.map(QPointF(0, devRect.top() + dy + 0.5)).y();
      const double display = m_st->displayAtY(sy);
      const double tA = a.timeDepth ? a.timeDepth->twtAt(display + offA)
                                    : qQNaN();
      const double tB = b.timeDepth ? b.timeDepth->twtAt(display + offB)
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

// ---------------------------------------------------------------------------
// HeaderWidget — 吸顶版头
// ---------------------------------------------------------------------------
HeaderWidget::HeaderWidget(RenderState *st, QWidget *parent)
    : QWidget(parent), m_st(st)
{
  setFixedHeight(headerHeight());
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  setMouseTracking(false);
  setAccessibleName(tr("连井剖面版头"));
}

void HeaderWidget::relayout()
{
  setFixedHeight(headerHeight());
  update();
}

int HeaderWidget::headerHeight() const
{
  // 题注行高 = 各道换行后最大行数 × lineSpacing + 8；版头 = 名行 + 题注行，
  // 不低于 kHeight。
  QFont f;
  f.setPointSize(PaleoTheme::kLabelPt);
  const QFontMetricsF fm(f);
  const auto scaleOf = [](const wellsection::CurveStyle &c) {
    return scaleText(c.min, c.max);
  };
  int lines = 1;
  for (const wellsection::TrackSpec &tr : m_st->tpl.tracks)
  {
    const double w = qBound(24, tr.width, 200) - 4.0;
    int n;
    if (tr.kind == wellsection::TrackKind::Curve && tr.curves.size() == 2)
    {
      n = 0;
      for (const auto &cs : tr.curves)
        n += wrapCaption(cs.label.isEmpty() ? cs.mnemonic : cs.label, w, fm)
                 .size() +
             wrapCaption(scaleOf(cs), w, fm).size();
    }
    else
    {
      n = wrapCaption(tr.displayTitle(), w, fm).size();
      if (tr.kind == wellsection::TrackKind::Curve && !tr.curves.isEmpty())
        n += wrapCaption(scaleOf(tr.curves.first()), w, fm).size();
      else if (tr.kind == wellsection::TrackKind::Lithology)
        n += wrapCaption(tr.sourceMnemonic + QObject::tr(" 推断"), w, fm)
                 .size();
    }
    lines = qMax(lines, n);
  }
  const int h = int(std::ceil(kNameRowH + lines * fm.lineSpacing() + 8));
  return qMax(kHeight, h);
}

void HeaderWidget::setScrollOffset(int x)
{
  if (m_scrollX == x)
    return;
  m_scrollX = x;
  update();
}

int HeaderWidget::columnAtX(double sceneX) const
{
  return m_st->columnAtX(sceneX);
}

void HeaderWidget::paintContents(QPainter *p, double xOffset) const
{
  const double W = m_st->sceneWidth();
  const int HH = headerHeight();
  p->fillRect(QRectF(0, 0, W, HH), m_st->theme.paper);

  QFont nameFont = p->font();
  nameFont.setPointSize(PaleoTheme::kTitlePt);
  QFont cellFont = nameFont;
  cellFont.setPointSize(PaleoTheme::kLabelPt);
  const QFontMetricsF cfm(cellFont);

  const auto scaleOf = [](const wellsection::CurveStyle &c) {
    return scaleText(c.min, c.max);
  };
  const double lineH = cfm.lineSpacing();
  // 竖向居中逐行画（行已按格宽换行/省略）。
  const auto drawLines = [&](const QRectF &r, const QStringList &ls,
                             const QColor &col) {
    p->setPen(col);
    double y = r.top() + (r.height() - lineH * ls.size()) * 0.5;
    for (const QString &l : ls)
    {
      p->drawText(QRectF(r.left(), y, r.width(), lineH),
                  Qt::AlignHCenter | Qt::AlignVCenter | Qt::TextSingleLine, l);
      y += lineH;
    }
  };

  for (int i = 0; i < m_st->wells.size(); ++i)
  {
    const double colX = m_st->columnLeft(i) - xOffset;
    const double cw = m_st->columnWidth();
    if (colX + cw < 0 || colX > W)
      continue;

    // 上行：井名（选中 = 主色字 + 2px 主色下划线）。
    const wellsection::Well &w = m_st->wells[i];
    QString title = w.name.endsWith(QStringLiteral("井"))
                        ? w.name
                        : w.name + QObject::tr(" 井");
    const bool sel = (i == m_st->selected);
    p->setFont(nameFont);
    p->setPen(sel ? PaleoTheme::tokens().primary : m_st->theme.text);
    p->drawText(QRectF(colX, 0, cw, kNameRowH - 2),
                Qt::AlignCenter | Qt::TextSingleLine, title);
    if (sel)
      p->fillRect(QRectF(colX + 4, kNameRowH - 2, cw - 8, 2),
                  PaleoTheme::tokens().primary);

    // 下行：逐道标题/刻度格（格高随换行行数自适应，由 headerHeight 定）。
    double x = colX;
    for (const wellsection::TrackSpec &tr : m_st->tpl.tracks)
    {
      const double tw = qBound(24, tr.width, 200);
      const QRectF cell(x, kNameRowH + 2, tw, HH - kNameRowH - 4);
      const double twIn = tw - 4.0; // 换行可用宽
      p->setPen(QPen(m_st->theme.frame, 1));
      p->drawRect(cell);
      p->save();
      p->setClipRect(cell.adjusted(1, 1, -1, -1));
      p->setFont(cellFont);
      if (tr.kind == wellsection::TrackKind::Curve && tr.curves.size() == 2)
      {
        // 双曲线道：上下两个等分子格，各自标名+刻度（用曲线色，不重叠）。
        const double subH = cell.height() / 2;
        for (int c = 0; c < 2; ++c)
        {
          const wellsection::CurveStyle &cs = tr.curves[c];
          const QRectF sub(cell.left(), cell.top() + c * subH, cell.width(),
                           subH);
          QStringList ls =
              wrapCaption(cs.label.isEmpty() ? cs.mnemonic : cs.label, twIn,
                          cfm);
          ls += wrapCaption(scaleOf(cs), twIn, cfm);
          drawLines(sub, ls, cs.color.isValid() ? cs.color : m_st->theme.text);
        }
      }
      else
      {
        // 深度道题注随基准面模式（海拔模式轴标是 kb−MD）。
        QString title = tr.displayTitle();
        if (tr.kind == wellsection::TrackKind::Depth &&
            m_st->datum.mode == wellsection::DatumMode::Elevation)
          title = QObject::tr("海拔/m");
        if (tr.kind == wellsection::TrackKind::Depth &&
            m_st->datum.mode == wellsection::DatumMode::Flatten)
          title = QObject::tr("拉平/m");
        QStringList ls = wrapCaption(title, twIn, cfm);
        QColor col = m_st->theme.text;
        if (tr.kind == wellsection::TrackKind::Curve && !tr.curves.isEmpty())
        {
          const wellsection::CurveStyle &cs = tr.curves.first();
          ls += wrapCaption(scaleOf(cs), twIn, cfm);
          col = cs.color.isValid() ? cs.color : m_st->theme.text;
        }
        else if (tr.kind == wellsection::TrackKind::Lithology)
        {
          ls += wrapCaption(tr.sourceMnemonic + QObject::tr(" 推断"), twIn,
                            cfm);
          col = QColor(QStringLiteral("#7A7A7A"));
        }
        drawLines(cell, ls, col);
      }
      p->restore();
      x += tw;
    }
  }
  // 拖排插入位标记：插入点落在第 m_insertAt 列的左缝中心。
  if (m_dragging && m_insertAt >= 0)
  {
    const int at = qMin(m_insertAt, m_st->wells.size() - 1);
    const double ix = at == m_insertAt
                          ? m_st->columnLeft(at) - m_st->gapPx * 0.5
                          : m_st->columnRight(at) + m_st->gapPx * 0.5;
    p->fillRect(QRectF(ix - 1 - xOffset, 0, 3, HH),
                PaleoTheme::tokens().primary);
  }
}

void HeaderWidget::paintEvent(QPaintEvent *)
{
  QPainter p(this);
  paintContents(&p, m_scrollX);
}

void HeaderWidget::mousePressEvent(QMouseEvent *e)
{
  const double sceneX = e->pos().x() + m_scrollX;
  const int col = columnAtX(sceneX);
  if (e->button() == Qt::LeftButton)
  {
    m_pressCol = col;
    m_pressPos = e->pos();
    m_dragging = false;
    m_insertAt = -1;
  }
  else if (e->button() == Qt::RightButton && col >= 0)
  {
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("wellSectionHeaderMenu"));
    QAction *act = menu->addAction(tr("从剖面移除"));
    connect(act, &QAction::triggered, this,
            [this, col] { emit removeRequested(col); });
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(e->globalPosition().toPoint());
  }
  QWidget::mousePressEvent(e);
}

void HeaderWidget::mouseMoveEvent(QMouseEvent *e)
{
  if (m_pressCol >= 0 && (e->buttons() & Qt::LeftButton))
  {
    const qreal dx = e->pos().x() - m_pressPos.x();
    if (!m_dragging && qAbs(dx) > kDragThreshold)
      m_dragging = true;
    if (m_dragging)
    {
      // 插入位 = 拖出列中心越过多少其它列中心。
      const double cw = m_st->columnWidth();
      const double cx = m_st->columnLeft(m_pressCol) + dx + cw * 0.5;
      int ins = 0;
      for (int i = 0; i < m_st->wells.size(); ++i)
      {
        if (i == m_pressCol)
          continue;
        if (m_st->columnLeft(i) + cw * 0.5 < cx)
          ++ins;
      }
      if (ins != m_insertAt)
      {
        m_insertAt = ins;
        update();
      }
    }
  }
  QWidget::mouseMoveEvent(e);
}

void HeaderWidget::mouseReleaseEvent(QMouseEvent *e)
{
  if (e->button() == Qt::LeftButton && m_pressCol >= 0)
  {
    if (m_dragging && m_insertAt >= 0)
      emit reorderRequested(m_pressCol, m_insertAt);
    else
      emit wellClicked(m_pressCol);
  }
  m_pressCol = -1;
  m_dragging = false;
  m_insertAt = -1;
  update();
  QWidget::mouseReleaseEvent(e);
}

// ---------------------------------------------------------------------------
// View
// ---------------------------------------------------------------------------
View::View(RenderState *st, QGraphicsScene *scene, QWidget *parent)
    : QGraphicsView(scene, parent), m_st(st)
{
  setFrameShape(QFrame::NoFrame);
  setRenderHint(QPainter::Antialiasing);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  // 场景窄于视口时钉左上（默认 AlignCenter 会居中 → 井柱相对版头漂移；
  // 且 scrollbar value==mapToScene(0,0).x() 的不变量是版头同步的前提）。
  setAlignment(Qt::AlignLeft | Qt::AlignTop);
  setTransformationAnchor(QGraphicsView::NoAnchor);
  setResizeAnchor(QGraphicsView::NoAnchor);
  viewport()->setMouseTracking(true);
}

void View::wheelEvent(QWheelEvent *e)
{
  const int delta = e->angleDelta().y();
  if (delta == 0)
  {
    QGraphicsView::wheelEvent(e);
    return;
  }
  const double f = delta > 0 ? 1.15 : 1.0 / 1.15;
  const auto mods = e->modifiers();
  if ((mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier))
  {
    emit gapZoomRequested(f);
    e->accept();
    return;
  }
  if (mods & Qt::ControlModifier)
  {
    const QPoint vp = e->position().toPoint();
    const double anchorDepth = m_st->displayAtY(mapToScene(vp).y());
    emit depthZoomRequested(f, anchorDepth, vp.y());
    e->accept();
    return;
  }
  QScrollBar *bar = (mods & Qt::ShiftModifier) ? horizontalScrollBar()
                                             : verticalScrollBar();
  bar->setValue(bar->value() - delta);
  e->accept();
}

void View::resizeEvent(QResizeEvent *e)
{
  QGraphicsView::resizeEvent(e);
  emit viewportResized();
}

void View::mousePressEvent(QMouseEvent *e)
{
  if (e->button() == Qt::LeftButton)
  {
    const int col = m_st->columnAtX(mapToScene(e->pos()).x());
    if (col >= 0)
      emit columnClicked(col);
  }
  QGraphicsView::mousePressEvent(e);
}

void View::mouseMoveEvent(QMouseEvent *e)
{
  const QPointF sp = mapToScene(e->pos());
  const int col = m_st->columnAtX(sp.x());
  if (col < 0 || col >= m_st->wells.size())
  {
    emit hoverChanged(QString());
    QGraphicsView::mouseMoveEvent(e);
    return;
  }
  const wellsection::Well &w = m_st->wells[col];
  const double off = m_st->offsets.value(col, 0.0);
  const double md = m_st->displayAtY(sp.y()) + off;
  QString zoneName;
  const auto zs = wellsection::zones(w, m_st->window.base + off);
  for (const auto &z : zs)
    if (md >= z.topMd && md < z.baseMd)
    {
      zoneName = z.name;
      break;
    }
  QString text = w.name + QStringLiteral(" · MD ") +
                 QString::number(md, 'f', 1) + QStringLiteral(" m");
  if (!zoneName.isEmpty())
    text += tr(" · 层段 %1").arg(zoneName);
  emit hoverChanged(text);
  QGraphicsView::mouseMoveEvent(e);
}

void View::leaveEvent(QEvent *e)
{
  emit hoverChanged(QString());
  QGraphicsView::leaveEvent(e);
}

} // namespace wellsectionui
