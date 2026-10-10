// 层：视图
// wellsectionscene_columns — ColumnItem 本体：整列纸面/道框/分层界线与
// 各道内容（层段/曲线/深度/岩性/相）+ 图片锚双击拾取。图片道 LOD 缓存
// 在 wellsectionscene_images.cpp（同族 TU）。零改动拆分（方向 98）。
#include "wellsectionscene.h"
#include "wellsectionscene_internal.h"

#include "domain/faciesclassification.h"
#include "domain/seismic/nicestep.h"
#include "ui/paleotheme.h"
#include "ui/wellcomposite/patterncatalog.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmapCache>
#include <QStyleOptionGraphicsItem>

#include <QtNumeric>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace wellsectionui {

namespace {
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
} // namespace

// ---------------------------------------------------------------------------
// ColumnItem — 单井整列
// ---------------------------------------------------------------------------
ColumnItem::ColumnItem(RenderState *st, int index)
    : m_st(st), m_index(index),
      m_imageDecodeHost(std::make_shared<ImageDecodeHost>())
{
  m_imageDecodeHost->self = this;
  setFlag(QGraphicsItem::ItemUsesExtendedStyleOption);
  // 图片锚双击拾取（左键）；未命中在 handler 里 ignore——列选中在
  // View::mousePressEvent（先于场景转发发生），不被吞。
  setAcceptedMouseButtons(Qt::LeftButton);
}

ColumnItem::~ColumnItem()
{
  // 队列里的解码结果还拿着 shared_ptr；先摘掉 self，回调不再碰本项。
  if (m_imageDecodeHost)
    m_imageDecodeHost->self = nullptr;
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
        // 高亮段底色在层段道内先铺（标签在其上）；坏表井 TVD 域换算
        // 不出 y → 不铺（不出段不伪造）。
        if (hi.hasBase())
        {
          const double hy0 = m_st->yForMd(m_index, hi.topMd);
          const double hy1 = m_st->yForMd(m_index, hi.baseMd);
          if (std::isfinite(hy0) && std::isfinite(hy1))
          {
            QColor hc = highlightColor();
            hc.setAlpha(55);
            p->fillRect(QRectF(trRect.left(), hy0, tw, hy1 - hy0), hc);
          }
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
      case wellsection::TrackKind::Facies:
        paintFaciesTrack(p, trRect, exposed);
        break;
      case wellsection::TrackKind::Image:
        paintImageTrack(p, trRect, exposed, p->worldTransform());
        break;
    }
    x += tw;
    ++trackIdx;
  }

  // 分层界线：可见顶各一条，横贯整列（y 经当前深度域映射）。
  p->setPen(QPen(m_st->theme.link, 1.0));
  for (const wellsection::Top &t : w.tops)
  {
    const double y = m_st->yForMd(m_index, t.md);
    if (std::isfinite(y) && y >= exposed.top() - 1 && y <= exposed.bottom() + 1)
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
  // 窗口底（显示深）→ 该井 MD（TVD 域经井斜反解；坏表 → NaN → 不出段）。
  const double bottomMd = m_st->mdOfDisplay(m_index, m_st->window.base);
  if (!std::isfinite(bottomMd))
    return;
  const auto zs = wellsection::zones(w, bottomMd);
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
        // 坏表井 TVD 域换算不出 y → 同缺样断笔（不伪造 int(NaN) 的行号）。
        const double yD = m_st->yForMd(m_index, md);
        if (!std::isfinite(yD))
        {
          flush();
          prevRow = INT_MIN;
          pathStarted = false;
          continue;
        }
        const double sx = qBound(innerL, innerL + nx * innerW, innerL + innerW);
        const int row = int(yD);
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
  // 可见域深范围（显示深 + 域偏移；TVD 域显示深即垂深）。
  const double off = m_st->offsets.value(m_index, 0.0);
  const double dLo = m_st->displayAtY(exposed.top()) + off;
  const double dHi = m_st->displayAtY(exposed.bottom()) + off;
  // 坏表井 + TVD 域：偏移/换算 NaN → 无刻度可画（不喂 NaN 给取整器）。
  if (!std::isfinite(dLo) || !std::isfinite(dHi) || dHi - dLo < 1e-3)
    return;
  const double yTop = m_st->yForDisplay(dLo - off);
  const double yBot = m_st->yForDisplay(dHi - off);
  const int target = qMax(4, int((yBot - yTop) / 60.0));
  // 海拔模式：轴标 = kb − 域深（补心海拔基准，向上为正；TVD 域即海拔
  // 垂深 TVDSS）——刻度取整在海拔空间做，其余模式在域深空间取整。
  const auto ticks = m_st->datum.mode == wellsection::DatumMode::Elevation
                         ? seismic::NiceStep::GenerateTicks(
                               m_st->wells[m_index].kb - dHi,
                               m_st->wells[m_index].kb - dLo, yBot, yTop,
                               target, QStringLiteral("%.0f"), true)
                         : seismic::NiceStep::GenerateTicks(
                               dLo, dHi, yTop, yBot, target,
                               QStringLiteral("%.0f"), true);

  QFont mono = PaleoTheme::monoFont();
  mono.setPointSize(PaleoTheme::tokens().labelPt);
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
  const double w2 = trackRect.width();
  // 解释岩性段（catalog 资产）优先——工程解释成果按词面取工程图式花纹；
  // 未命中词表 → 中性灰底 + 原词面（不吞数据、不伪造图式）。
  if (!w.litho.isEmpty())
  {
    QFont f = p->font();
    f.setPointSize(PaleoTheme::kLabelPt);
    const QFontMetricsF fm(f);
    for (const wellsection::LithoSegment &seg : w.litho)
    {
      const double y0 = m_st->yForMd(m_index, seg.topMd);
      const double y1 = m_st->yForMd(m_index, seg.baseMd);
      if (!std::isfinite(y0) || !std::isfinite(y1))
        continue; // 坏表井 TVD 域换算不出——不出段不伪造
      if (y1 < exposed.top() || y0 > exposed.bottom())
        continue;
      const QRectF band(trackRect.left(), y0, w2, y1 - y0);
      WellComposite::PatternDef def;
      if (WellComposite::PatternCatalog::lookupLithology(seg.litho, &def))
      {
        p->fillRect(band, def.bg);
        // 花纹 tile 按 (key,bg,fg) 缓存——重绘不再逐段重造 QPixmap。
        const QString cacheKey = QStringLiteral("paleo-ws-litho-%1-%2-%3")
                                     .arg(def.key, def.bg.name(), def.fg.name());
        QPixmap pm;
        if (!QPixmapCache::find(cacheKey, &pm))
        {
          pm = WellComposite::PatternCatalog::createLithoPattern(def.key,
                                                                def.bg, def.fg);
          if (!pm.isNull())
            QPixmapCache::insert(cacheKey, pm);
        }
        if (!pm.isNull())
          p->fillRect(band, QBrush(pm));
      }
      else
      {
        // 未命中词表：中性灰底 + 原词面（不吞数据、不伪造图式）。
        p->fillRect(band, QColor(217, 222, 228));
      }
      p->setPen(QPen(m_st->theme.frame, 0.5));
      p->drawRect(band);
      if (band.height() >= fm.height() + 4 && band.width() > 18)
      {
        p->setFont(f);
        p->setPen(m_st->theme.text);
        p->drawText(band.adjusted(1, 0, -1, 0),
                    Qt::AlignCenter | Qt::TextWordWrap, seg.litho);
      }
    }
    return;
  }
  // GR 推断回落（题注标「推断」，不混充解释成果）。domain 内置 fallback
  // provider 包装 inferSandShale——渲染期回落，workflow 不预挂推断段。
  const wellsection::Curve *src = w.curve(tr.sourceMnemonic);
  if (!src)
    return;
  const wellsection::GrCutoffLithologyProvider fallback(*src, tr.cutoff);
  const auto segments = fallback.lithologyFor(QString());
  for (const wellsection::LithoSegment &seg : segments)
  {
    const bool sand = seg.litho == wellsection::GrCutoffLithologyProvider::sandWord();
    const double y0 = m_st->yForMd(m_index, seg.topMd);
    const double y1 = m_st->yForMd(m_index, seg.baseMd);
    if (!std::isfinite(y0) || !std::isfinite(y1))
      continue; // 坏表井 TVD 域同口径：不出段不伪造
    if (y1 < exposed.top() || y0 > exposed.bottom())
      continue;
    if (sand)
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

void ColumnItem::paintFaciesTrack(QPainter *p, const QRectF &trackRect,
                                  const QRectF &exposed)
{
  const wellsection::Well &w = m_st->wells[m_index];
  if (w.facies.isEmpty())
    return;
  QFont f = p->font();
  f.setPointSize(PaleoTheme::kLabelPt);
  const QFontMetricsF fm(f);
  for (const wellsection::FaciesSegment &seg : w.facies)
  {
    const double y0 = m_st->yForMd(m_index, seg.topMd);
    const double y1 = m_st->yForMd(m_index, seg.baseMd);
    if (!std::isfinite(y0) || !std::isfinite(y1))
      continue; // 坏表井 TVD 域同口径：不出段不伪造
    if (y1 < exposed.top() || y0 > exposed.bottom())
      continue;
    const QRectF band(trackRect.left(), y0, trackRect.width(), y1 - y0);
    // 12 色 Wheel（crossplot 数据符号色同源）+ 描边，段高足够时标类号。
    const auto cc = paleo::crossplot::classColor(seg.classId);
    QColor fill(cc.red, cc.green, cc.blue, 200);
    p->fillRect(band, fill);
    p->setPen(QPen(m_st->theme.frame, 0.5));
    p->drawRect(band);
    if (band.height() >= fm.height() + 4 && band.width() > 14)
    {
      p->setFont(f);
      p->setPen(m_st->theme.text);
      p->drawText(band, Qt::AlignCenter | Qt::TextSingleLine,
                  QString::number(seg.classId));
    }
  }
}

// 双击图片锚 → 激活回调（面板接：编辑锚深对话框）。命中取本帧绘制
// 矩形（item 坐标）；空白处不拦截——列的其它交互（选中/工具）不受影响。
void ColumnItem::mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event)
{
  if (!m_imageActivate)
  {
    event->ignore();
    return;
  }
  const wellsection::Well &w = m_st->wells.value(m_index);
  for (const ImageHit &hit : m_imageHits)
  {
    if (hit.anchorIdx < 0 || hit.anchorIdx >= w.images.size())
      continue;
    if (hit.rect.contains(event->pos()))
    {
      m_imageActivate(w.id, w.images.at(hit.anchorIdx));
      return;
    }
  }
  event->ignore();
}

} // namespace wellsectionui
