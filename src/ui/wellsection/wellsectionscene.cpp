// 层：视图
// token 例外：DESIGN 数据符号例外：纸面地层/岩性图未知值的灰色占位符号。（tools/ui-token-exceptions.json 精确计数）。
// wellsectionscene（主 TU，方向 98 拆分后）：共享 helper + RenderState +
// 吸顶版头 HeaderWidget + 视图 View。ColumnItem 在 _columns/_images、
// 井间缝在 _seismicgap、断层投绘在 _faults（同族 TU）。
#include "wellsectionscene.h"

#include "domain/faciesclassification.h"
#include "domain/seismic/nicestep.h"
#include "services/imagelod.h"
#include "ui/paleotheme.h"
#include "ui/wellcomposite/patterncatalog.h"

#include <QApplication>
#include <QGraphicsScene>
#include <QGraphicsSceneEvent>
#include <QLineF>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmapCache>
#include <QPointer>
#include <QScrollBar>
#include <QStyleOptionGraphicsItem>
#include <QThread>
#include <QWheelEvent>

#include <QtNumeric>
#include <algorithm>
#include <cmath>
#include <memory>
#include <numeric>
#include <utility>

namespace wellsectionui {

QString tvdBadgeText(const wellsection::Well &w,
                     wellsection::DepthDomain domain)
{
  if (domain != wellsection::DepthDomain::TVD)
    return QString(); // 角标是 TVD 域专属诚实面，MD 域不出现
  switch (w.tvdStatus())
  {
    case wellsection::TvdStatus::NoSurvey:
      return QObject::tr("TVD 不可用（无测斜）");
    case wellsection::TvdStatus::BrokenSurvey:
      return QObject::tr("TVD 不可用（井斜表损坏）");
    case wellsection::TvdStatus::Surveyed:
      break;
  }
  return QString();
}

QString depthTrackCaption(const wellsection::TrackSpec &tr,
                          wellsection::DepthDomain domain,
                          const wellsection::Datum &datum)
{
  QString title = tr.displayTitle();
  if (tr.kind == wellsection::TrackKind::Depth)
  {
    if (domain == wellsection::DepthDomain::TVD)
    {
      // 拉平语义不随域切换丢失（R1-2 L1）：TVD×拉平题注双口径词。
      if (datum.mode == wellsection::DatumMode::Elevation)
        title = QObject::tr("海拔垂深/m");
      else if (datum.mode == wellsection::DatumMode::Flatten)
        title = QObject::tr("拉平·垂深/m");
      else
        title = QObject::tr("垂深/m");
    }
    else if (datum.mode == wellsection::DatumMode::Elevation)
      title = QObject::tr("海拔/m");
    else if (datum.mode == wellsection::DatumMode::Flatten)
      title = QObject::tr("拉平/m");
  }
  return title;
}

QString hoverReadoutText(const wellsection::Well &w,
                         wellsection::DepthDomain domain, double md,
                         const QString &zoneName)
{
  QString text;
  if (!std::isfinite(md))
    // 坏表井 TVD 域：读数换算不出——如实说明，不出「nan」。
    text = w.name + QObject::tr(" · 井斜不可用，TVD 域无深度读数");
  else
  {
    text = w.name + QStringLiteral(" · MD ") +
           QString::number(md, 'f', 1) + QStringLiteral(" m");
    if (domain == wellsection::DepthDomain::TVD)
    {
      // 无测斜井 TVD 读数是恒等值（= 按井深绘制），出数值会与角标
      // 「TVD 不可用（无测斜）」抵触——如实注明口径（坏表井走上方
      // 无读数分支，措辞同款）。
      if (w.tvdStatus() == wellsection::TvdStatus::NoSurvey)
        text += QObject::tr(" · TVD 不可用（无测斜，按井深绘制）");
      else
        text += QStringLiteral(" · TVD ") +
                QString::number(w.tvdOf(md), 'f', 1) + QStringLiteral(" m");
    }
  }
  if (!zoneName.isEmpty())
    text += QObject::tr(" · 层段 %1").arg(zoneName);
  return text;
}

QString lithoTrackCaptionText(const wellsection::Well &w,
                              const QString &sourceMnemonic)
{
  if (w.litho.isEmpty())
    return QObject::tr("推断·%1 截断").arg(sourceMnemonic);
  const QString prov = w.litho.first().provenance.trimmed();
  return prov.isEmpty() ? QObject::tr("解释")
                        : QObject::tr("解释·%1").arg(prov);
}

namespace {
constexpr double kNameRowH = 26.0;
constexpr qreal kDragThreshold = 6.0;

QString scaleText(double lo, double hi)
{
  const auto fmt = [](double v) {
    QString s = QString::number(v, 'g', 4);
    return s;
  };
  return QStringLiteral("%1—%2").arg(fmt(lo), fmt(hi));
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

// 题注行数上限（R1-4）：版头高取全井题注行数最大——单井长 provenance
// 不得拖高全部列。超限截断 + 行尾省略号；计数（headerHeight）与绘制
// （paintContents 岩性题注）共用本函数，口径单源。
QStringList wrapCaptionClamped(const QString &text, double w,
                               const QFontMetricsF &fm)
{
  QStringList lines = wrapCaption(text, w, fm);
  if (lines.size() <= 4)
    return lines;
  lines = lines.mid(0, 4);
  lines.last() = fm.elidedText(lines.last() + QStringLiteral("…"),
                               Qt::ElideRight, int(w));
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
  double left = margin;
  for (int i = 0; i < wells.size(); ++i)
  {
    if (x < left)
      return -1;
    if (x <= left + cw)
      return i;
    left += cw + gapWidth(i);
  }
  return -1;
}

double RenderState::sceneWidth() const
{
  if (wells.isEmpty())
    return 2.0 * margin;
  double w = 2.0 * margin + wells.size() * columnWidth();
  for (int i = 0; i + 1 < wells.size(); ++i)
    w += gapWidth(i);
  return w;
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
  f.setPointSize(PaleoTheme::tokens().labelPt);
  const QFontMetricsF fm(f);
  const auto scaleOf = [](const wellsection::CurveStyle &c) {
    return scaleText(c.min, c.max);
  };
  int lines = 1;
  for (const wellsection::TrackSpec &tr : m_st->tpl.tracks)
  {
    const double w = qBound(24, tr.width, 200) - 4.0;
    int n;
    if (tr.kind == wellsection::TrackKind::Curve && tr.curves.size() >= 2)
    {
      // N 曲线子格各自行数折算（子格高度为格高 1/N，行数上取整折 N 倍）。
      n = 0;
      for (const auto &cs : tr.curves)
        n += qCeil(double(wrapCaption(cs.label.isEmpty() ? cs.mnemonic
                                                         : cs.label,
                                     w, fm)
                               .size() +
                           wrapCaption(scaleOf(cs), w, fm)
                               .size()) /
                   double(tr.curves.size()));
    }
    else
    {
      n = wrapCaption(tr.displayTitle(), w, fm).size();
      if (tr.kind == wellsection::TrackKind::Curve && !tr.curves.isEmpty())
        n += wrapCaption(scaleOf(tr.curves.first()), w, fm).size();
      else if (tr.kind == wellsection::TrackKind::Lithology)
      {
        // 题注随行来源标注按井变化（方向 69）——行数取全井最大（有上限，
        // 超限 elide 见 wrapCaptionClamped），防裁切、不拖高全部列。
        int capLines = 1;
        for (const auto &well : m_st->wells)
          capLines = qMax(capLines, wrapCaptionClamped(
                                         lithoTrackCaptionText(
                                             well, tr.sourceMnemonic),
                                         w, fm)
                                        .size());
        n += capLines;
      }
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
  cellFont.setPointSize(PaleoTheme::tokens().labelPt);
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

    // 上行：井名（选中 = 主色字 + 2px 主色下划线）。TVD 域下无测斜/坏表
    // 井如实加角标小字（名行两行排布，角标用语义色——DESIGN warning/error
    // 文字位变体；MD 域不出现这些标记）。
    const wellsection::Well &w = m_st->wells[i];
    QString title = w.name.endsWith(QStringLiteral("井"))
                        ? w.name
                        : w.name + QObject::tr(" 井");
    const bool sel = (i == m_st->selected);
    const QString badge = tvdBadgeText(w, m_st->domain);
    p->setFont(nameFont);
    p->setPen(sel ? PaleoTheme::tokens().primary : m_st->theme.text);
    if (badge.isEmpty())
    {
      p->drawText(QRectF(colX, 0, cw, kNameRowH - 2),
                  Qt::AlignCenter | Qt::TextSingleLine, title);
    }
    else
    {
      const double nameH = (kNameRowH - 2) * 0.62;
      p->drawText(QRectF(colX, 0, cw, nameH),
                  Qt::AlignCenter | Qt::TextSingleLine, title);
      p->setFont(cellFont);
      const QColor badgeCol =
          w.tvdStatus() == wellsection::TvdStatus::BrokenSurvey
              ? PaleoTheme::tokens().errorText
              : PaleoTheme::tokens().warningText;
      p->setPen(badgeCol);
      p->drawText(QRectF(colX, nameH, cw, kNameRowH - 2 - nameH),
                  Qt::AlignCenter | Qt::TextSingleLine, badge);
    }
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
      if (tr.kind == wellsection::TrackKind::Curve && tr.curves.size() >= 2)
      {
        // 多曲线道：N 个等分子格，各自标名+刻度（用曲线色，不重叠）。
        const int n = tr.curves.size();
        const double subH = cell.height() / n;
        for (int c = 0; c < n; ++c)
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
        // 深度道题注随基准面模式与深度域（TVD 域 = 垂深/海拔垂深）——
        // 与 depthCaptionText() 同一口径，导出图随版头携带域标签。
        QString title = depthTrackCaption(tr, m_st->domain, m_st->datum);
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
          // 口径诚实（方向 69 来源标注）：解释段带资产来源；回落 GR
          // 二分 → 「推断·<曲线> 截断」。题注级分色：推断灰 / 解释正文色。
          // 行数与 headerHeight 同走 wrapCaptionClamped（≤4 行 + elide）。
          ls += wrapCaptionClamped(
              lithoTrackCaptionText(w, tr.sourceMnemonic), twIn, cfm);
          col = w.litho.isEmpty() ? QColor(QStringLiteral("#7A7A7A"))
                                  : m_st->theme.text;
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
                          ? m_st->columnLeft(at) -
                                m_st->gapWidth(qMax(0, at - 1)) * 0.5
                          : m_st->columnRight(at) + m_st->gapWidth(at) * 0.5;
    p->fillRect(QRectF(ix - 1 - xOffset, 0, 3, HH),
                PaleoTheme::tokens().primary);
  }
}

void HeaderWidget::paintEvent(QPaintEvent *)
{
  QPainter p(this);
  paintContents(&p, m_scrollX);
}

QString HeaderWidget::depthCaptionText() const
{
  for (const wellsection::TrackSpec &tr : m_st->tpl.tracks)
    if (tr.kind == wellsection::TrackKind::Depth)
      return depthTrackCaption(tr, m_st->domain, m_st->datum);
  return QString();
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
  const double md = m_st->mdOfDisplay(col, m_st->displayAtY(sp.y()));
  QString zoneName;
  if (std::isfinite(md))
  {
    const double bottomMd = m_st->mdOfDisplay(col, m_st->window.base);
    if (std::isfinite(bottomMd))
    {
      const auto zs = wellsection::zones(w, bottomMd);
      for (const auto &z : zs)
        if (md >= z.topMd && md < z.baseMd)
        {
          zoneName = z.name;
          break;
        }
    }
  }
  const QString text = hoverReadoutText(w, m_st->domain, md, zoneName);
  // 先派 base（连线 hoverLeave 清空提示）再上报——否则从连线移入井柱的
  // 一拍里旧提示会覆盖井读数。
  QGraphicsView::mouseMoveEvent(e);
  emit hoverChanged(text);
}

void View::leaveEvent(QEvent *e)
{
  emit hoverChanged(QString());
  QGraphicsView::leaveEvent(e);
}

} // namespace wellsectionui
