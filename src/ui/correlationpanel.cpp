#include "correlationpanel.h"

#include "../linkage/selectioncontext.h"
#include "../io/lasparser.h"

#include <QGraphicsLineItem>
#include <QGraphicsPathItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsView>
#include <QGridLayout>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QSet>

#include <qgsfillsymbol.h>
#include <qgslinechartplot.h>
#include <qgslinesymbol.h>
#include <qgsmargins.h>
#include <qgsmarkersymbol.h>
#include <qgsplot.h>
#include <qgsrendercontext.h>
#include <qgstextformat.h>

#include <cmath>

// ---------------------------------------------------------------------------
// Scene scaffold: one rounded-rect placeholder column per well (~120px wide,
// full strip height), name label on top, horizon tick near the bottom.
// Colors come from DESIGN.md tokens: surface fill, border stroke, primary for
// the selected state, text/text-muted for labels.
// ---------------------------------------------------------------------------
namespace
{
  constexpr int kWellIdRole = 0;    // QGraphicsItem::data key → well id
  constexpr int kHighlightRole = 1; // QGraphicsItem::data key → bool
  constexpr int kCurveRole = 2;     // QGraphicsItem::data key → owning well id (curve items)

  constexpr qreal kColumnWidth = 120.0;
  constexpr qreal kColumnGap   = 16.0; // spacing.md
  constexpr qreal kMargin      = 12.0;
  constexpr qreal kLabelBand   = 24.0;
  constexpr qreal kSceneHeight = 380.0;
  constexpr qreal kTickLift    = 24.0; // horizon tick sits this far above the bottom edge
  constexpr qreal kRadius      = 4.0;  // rounded.sm
  constexpr qreal kCurvePad    = 8.0;  // curve inset from column side edges, matches tick

  const QColor kSurface(QStringLiteral("#FFFFFF"));
  const QColor kSurfaceAlt(QStringLiteral("#EDF1F5"));
  const QColor kBorder(QStringLiteral("#DFE5EC"));
  const QColor kPrimary(QStringLiteral("#1B73D0"));
  const QColor kText(QStringLiteral("#24303E"));
  const QColor kTextMuted(QStringLiteral("#5D6E80"));

  QPen columnPen(bool highlighted)
  {
    return highlighted ? QPen(kPrimary, 2.0) : QPen(kBorder, 1.0);
  }

  // Scene subclass detects which column a click landed on during normal
  // dispatch — the same path a viewport mouse event takes, so tests exercise
  // it by clicking the viewport (or sending the scene event directly).
  class CorrelationScene : public QGraphicsScene
  {
    public:
      using QGraphicsScene::QGraphicsScene;
      std::function<void(const QString &)> clicked;
      std::function<void(const QString &)> doubleClicked;

    protected:
      void mousePressEvent(QGraphicsSceneMouseEvent *e) override
      {
        QGraphicsScene::mousePressEvent(e);
        if (clicked && e->button() == Qt::LeftButton)
        {
          const QString id = columnIdAt(e->scenePos());
          if (!id.isEmpty())
            clicked(id);
        }
      }
      void mouseDoubleClickEvent(QGraphicsSceneMouseEvent *e) override
      {
        QGraphicsScene::mouseDoubleClickEvent(e);
        if (e->button() == Qt::LeftButton)
        {
          const QString id = columnIdAt(e->scenePos());
          if (!id.isEmpty())
          {
            if (clicked) // QListWidget-style: every press reports a click
              clicked(id);
            if (doubleClicked)
              doubleClicked(id);
          }
        }
      }

    private:
      // Children (label, tick) resolve to their parent column.
      QString columnIdAt(const QPointF &pos) const
      {
        for (QGraphicsItem *it : items(pos))
          for (QGraphicsItem *c = it; c; c = c->parentItem())
          {
            const QVariant v = c->data(kWellIdRole);
            if (v.isValid())
              return v.toString();
          }
        return {};
      }
  };
} // namespace

WellCorrelationPanel::WellCorrelationPanel(SelectionContext *ctx, QWidget *parent)
  : QWidget(parent), m_ctx(ctx)
{
  auto *lay = new QGridLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(0);

  auto *scene = new CorrelationScene(this);
  scene->clicked = [this](const QString &id) { emit wellClicked(id); };
  scene->doubleClicked = [this](const QString &id) { emit wellDoubleClicked(id); };
  m_scene = scene;

  m_view = new QGraphicsView(scene, this);
  m_view->setObjectName(QStringLiteral("correlationView"));
  m_view->setAccessibleName(tr("连井剖面"));
  m_view->setFrameShape(QFrame::NoFrame);
  m_view->setRenderHint(QPainter::Antialiasing);
  m_view->setBackgroundBrush(kSurfaceAlt);
  m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  m_view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  lay->addWidget(m_view, 0, 0);

  // §42.4 empty state — guidance text overlaid on the strip area; stacked in
  // the same grid cell so it never shifts the view's geometry.
  m_emptyLabel = new QLabel(tr("选择井以构建剖面"), this);
  m_emptyLabel->setObjectName(QStringLiteral("emptyLabel"));
  m_emptyLabel->setAlignment(Qt::AlignCenter);
  m_emptyLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
  m_emptyLabel->setStyleSheet(QStringLiteral("color: %1;").arg(kTextMuted.name()));
  lay->addWidget(m_emptyLabel, 0, 0);
  m_emptyLabel->setVisible(true);

  if (m_ctx)
  {
    connect(m_ctx, &SelectionContext::selectionChanged, this,
            [this](const QStringList &ids, const QString &origin) {
              if (origin != QLatin1String("correlation")) // own echo — skip
                applySelection(ids);
            });
  }
}

void WellCorrelationPanel::setWells(const QList<QPair<QString, QString>> &wells)
{
  m_wells = wells;
  rebuildScene();
  if (m_ctx) // selection may predate the wells — honor current state
    applySelection(m_ctx->selectedIds());
}

QString WellCorrelationPanel::wellAt(int index) const
{
  return (index >= 0 && index < m_wells.size()) ? m_wells.at(index).first : QString();
}

void WellCorrelationPanel::reorder(int from, int to)
{
  if (from == to || from < 0 || to < 0 || from >= m_wells.size() || to >= m_wells.size())
    return;
  m_wells.move(from, to);
  rebuildScene();
  if (m_ctx)
    applySelection(m_ctx->selectedIds());
}

bool WellCorrelationPanel::isWellHighlighted(const QString &wellId) const
{
  if (wellId.isEmpty() || !m_scene)
    return false;
  const auto items = m_scene->items();
  for (QGraphicsItem *it : items)
  {
    const QVariant id = it->data(kWellIdRole);
    if (id.isValid() && id.toString() == wellId)
      return it->data(kHighlightRole).toBool();
  }
  return false;
}

void WellCorrelationPanel::setWellCurves(const QString &wellId, const QVector<float> &depths,
                                         const QVector<float> &values, const QString &curveName)
{
  const qsizetype n = qMin(depths.size(), values.size());
  if (wellId.isEmpty() || n == 0)
    m_curves.remove(wellId); // empty input clears the well's curve
  else
    m_curves.insert(wellId, {depths.first(n), values.first(n), curveName});

  rebuildScene();
  if (m_ctx) // rebuild resets pens — restore selection state like reorder()
    applySelection(m_ctx->selectedIds());
}

void WellCorrelationPanel::clearWellCurves()
{
  m_curves.clear();
  rebuildScene();
  if (m_ctx)
    applySelection(m_ctx->selectedIds());
}

int WellCorrelationPanel::curveItemCount(const QString &wellId) const
{
  if (!m_scene)
    return 0;
  int count = 0;
  const auto items = m_scene->items();
  for (QGraphicsItem *it : items)
    if (it->data(kCurveRole).isValid() && it->data(kCurveRole).toString() == wellId)
      ++count;
  return count;
}

bool WellCorrelationPanel::loadWellLas(const QString &wellId, const QString &lasPath,
                                       const QString &curveMnemonic)
{
  QStringList names;
  QList<LasCurve> curves;
  if (!LasParser::parse(lasPath, names, curves) || curves.isEmpty())
    return false;

  const QString want = curveMnemonic.trimmed();
  int idx = -1;
  for (int i = 0; i < names.size(); ++i)
    if (names.at(i).compare(want, Qt::CaseInsensitive) == 0)
    {
      idx = i;
      break;
    }
  if (idx < 0)
    return false;

  const LasCurve &depthCurve = curves.first(); // ~C column 0 is the DEPT index
  const LasCurve &valueCurve = curves.at(idx);

  QVector<float> depths(depthCurve.values.size());
  for (qsizetype i = 0; i < depthCurve.values.size(); ++i)
    depths[i] = static_cast<float>(depthCurve.values.at(i));
  QVector<float> values(valueCurve.values.size());
  for (qsizetype i = 0; i < valueCurve.values.size(); ++i)
    values[i] = static_cast<float>(valueCurve.values.at(i));

  setWellCurves(wellId, depths, values, valueCurve.name);
  return true;
}

// Log tracks are drawn by QGIS's own plot renderer (Qgs2DXyPlot), not
// hand-built polylines — the plot engine owns axis scaling, the shared
// depth axis lands in its y range, and each contiguous run of finite
// samples becomes one QgsXyPlotSeries so NULL gaps stay gaps in the
// rendered track. The plot renders into a per-column QImage which is
// placed as a mouse-transparent pixmap child of the column item, so
// clicks still resolve to the well through the parent chain.
void WellCorrelationPanel::addCurveItems(QGraphicsPathItem *column, const WellCurve &curve,
                                         float depthMin, float depthSpan)
{
  const QRectF band = column->path().boundingRect();
  const qreal x0 = band.x() + kCurvePad;
  const qreal w = band.width() - 2.0 * kCurvePad;
  const QString id = column->data(kWellIdRole).toString();

  // Split samples into contiguous finite runs → one series per run.
  const qsizetype n = qMin(curve.depths.size(), curve.values.size());
  QgsPlotData data; // owns the series once added
  bool any = false;
  float vMin = 0.0f, vMax = 0.0f;
  const auto flushRun = [&](QList<std::pair<double, double>> &pts) {
    if (pts.size() < 2)
    {
      pts.clear();
      return; // a lone sample draws no line — skip it
    }
    auto *s = new QgsXyPlotSeries();
    s->setName(curve.name);
    s->setData(pts);
    data.addSeries(s); // takes ownership
    pts.clear();
  };
  QList<std::pair<double, double>> run;
  for (qsizetype i = 0; i < n; ++i)
  {
    const float d = curve.depths.at(i);
    const float v = curve.values.at(i);
    if (!std::isfinite(d) || !std::isfinite(v))
    {
      flushRun(run);
      continue;
    }
    if (!any) { vMin = vMax = v; any = true; }
    else { vMin = qMin(vMin, v); vMax = qMax(vMax, v); }
    // x = value, y = -depth: Qgs2DXyPlot's y axis grows upward, and a well
    // track wants depth growing downward — negating depth flips it with no
    // axis-inversion API (none exists on QgsPlotAxis in 4.2).
    run.append({static_cast<double>(v), -static_cast<double>(d)});
  }
  flushRun(run);
  if (!any || data.series().isEmpty())
    return;

  // Render target: the column band below the title strip. NB in QGIS 4.2's
  // standalone (non-layout) render path, Qgs2DPlot::setSize is consumed in
  // painter units — so we pass pixels, not millimeters.
  const int pxW = qMax(8, static_cast<int>(w));
  const int pxH = qMax(8, static_cast<int>(band.height()));
  QImage img(QSize(pxW, pxH), QImage::Format_ARGB32_Premultiplied);
  img.fill(Qt::transparent);

  QgsLineChartPlot plot;
  plot.setSize(QSizeF(pxW, pxH));
  plot.setMargins(QgsMargins());                      // tracks fill the column
  plot.setXMinimum(vMin);
  plot.setXMaximum(qMax(vMax, vMin + 1e-6));          // flat curves still render
  const double yMin = -static_cast<double>(depthMin + depthSpan);
  const double yMax = -static_cast<double>(depthMin);
  plot.setYMinimum(yMin);
  plot.setYMaximum(qMax(yMax, yMin + 1e-6));

  // Compact track look: transparent chart chrome, invisible grid lines,
  // ~0-size axis text + huge label interval → interiorPlotArea reserves
  // almost no axis space, so the curve spans the whole column.
  QVariantMap noFill;
  noFill.insert(QStringLiteral("color"), QStringLiteral("0,0,0,0"));
  noFill.insert(QStringLiteral("outline_style"), QStringLiteral("no"));
  plot.setChartBackgroundSymbol(QgsFillSymbol::createSimple(noFill).release());
  plot.setChartBorderSymbol(QgsFillSymbol::createSimple(noFill).release());
  const auto invisibleLine = [] {
    QVariantMap m;
    m.insert(QStringLiteral("color"), QStringLiteral("0,0,0,0"));
    return QgsLineSymbol::createSimple(m).release(); // plot takes ownership
  };
  plot.xAxis().setGridMajorSymbol(invisibleLine());
  plot.xAxis().setGridMinorSymbol(invisibleLine());
  plot.yAxis().setGridMajorSymbol(invisibleLine());
  plot.yAxis().setGridMinorSymbol(invisibleLine());
  QgsTextFormat tiny;
  tiny.setSize(0.5); // shrinks the reserved axis-label space to ~nothing
  plot.xAxis().setTextFormat(tiny);
  plot.yAxis().setTextFormat(tiny);
  plot.xAxis().setLabelInterval(1e30);
  plot.yAxis().setLabelInterval(1e30);

  // Design-token ink: muted track stroke, no point markers.
  plot.setLineSymbolAt(0, QgsLineSymbol::createSimple(
      QVariantMap{{QStringLiteral("color"), kTextMuted.name()},
                  {QStringLiteral("width"), QStringLiteral("0.8")}}).release());
  plot.setMarkerSymbolAt(0, QgsMarkerSymbol::createSimple(
      QVariantMap{{QStringLiteral("color"), QStringLiteral("0,0,0,0")},
                  {QStringLiteral("outline_color"), QStringLiteral("0,0,0,0")}}).release());

  QRect interior;
  {
    QPainter p(&img);
    QgsRenderContext rc = QgsRenderContext::fromQPainter(&p);
    QgsPlotRenderContext pc;
    // Axis decoration space is computed off the render context — query the
    // real content rect so the curve crop fills the column exactly.
    interior = plot.interiorPlotArea(rc, pc, data).toAlignedRect()
               .intersected(img.rect());
    plot.render(rc, pc, data);
  }

  const QImage cropped = interior.isEmpty() ? img : img.copy(interior);
  if (cropped.isNull())
    return;
  auto *item = new QGraphicsPixmapItem(QPixmap::fromImage(cropped), column);
  // Stretch the content crop across the full band when the axis reservation
  // was nonzero — tracks are always full-height in the column.
  item->setOffset(x0, band.y());
  if (cropped.size() != img.size())
  {
    const qreal sx = w / cropped.width();
    const qreal sy = band.height() / cropped.height();
    item->setScale(1.0);
    item->setTransform(QTransform::fromScale(sx, sy));
  }
  item->setData(kCurveRole, id);
  item->setAcceptedMouseButtons(Qt::NoButton); // never intercept well clicks
}

void WellCorrelationPanel::rebuildScene()
{
  m_scene->clear();
  m_emptyLabel->setVisible(m_wells.isEmpty());

  const int n = m_wells.size();
  const qreal width = kMargin * 2 + n * kColumnWidth + (n > 0 ? (n - 1) * kColumnGap : 0);
  m_scene->setSceneRect(0, 0, width, kSceneHeight);

  // Shared depth axis: global min/max depth across every provided curve so
  // all columns stay depth-registered (a shallow track occupies only the
  // top of its column). depthSpan < 0 marks "no axis" → nothing drawn.
  float depthMin = 0.0f, depthSpan = -1.0f;
  {
    bool have = false;
    float depthMax = 0.0f;
    for (const WellCurve &c : m_curves)
      for (const float d : c.depths)
        if (std::isfinite(d))
        {
          if (!have) { depthMin = depthMax = d; have = true; }
          else { depthMin = qMin(depthMin, d); depthMax = qMax(depthMax, d); }
        }
    if (have)
      depthSpan = depthMax - depthMin;
  }

  const qreal colH = kSceneHeight - kLabelBand - kMargin;
  const qreal tickY = kSceneHeight - kMargin - kTickLift;
  for (int i = 0; i < n; ++i)
  {
    const qreal x = kMargin + i * (kColumnWidth + kColumnGap);

    QPainterPath path;
    path.addRoundedRect(QRectF(x, kLabelBand, kColumnWidth, colH), kRadius, kRadius);
    auto *column = m_scene->addPath(path, columnPen(false), QBrush(kSurface));
    column->setData(kWellIdRole, m_wells.at(i).first);
    column->setData(kHighlightRole, false);

    // Well name centered above the column.
    auto *label = new QGraphicsSimpleTextItem(m_wells.at(i).second, column);
    QFont f = label->font();
    f.setPointSize(9); // body token
    label->setFont(f);
    label->setBrush(kText);
    const QRectF lb = label->boundingRect();
    label->setPos(x + (kColumnWidth - lb.width()) / 2.0,
                  kLabelBand - lb.height() - 2.0);

    // Horizon tick placeholder near the bottom edge (activeHorizon binding
    // arrives with real log rendering).
    auto *tick = new QGraphicsLineItem(x + 8, tickY, x + kColumnWidth - 8, tickY, column);
    QPen tickPen(kTextMuted, 1.0, Qt::DashLine);
    tick->setPen(tickPen);

    // Real log curve, when one was provided for this well.
    const auto cit = m_curves.constFind(m_wells.at(i).first);
    if (cit != m_curves.constEnd() && depthSpan >= 0.0f)
    {
      addCurveItems(column, *cit, depthMin, depthSpan);

      // Curve mnemonic as a small caption under the well title.
      if (!cit->name.isEmpty())
      {
        auto *cl = new QGraphicsSimpleTextItem(cit->name, column);
        QFont cf = cl->font();
        cf.setPointSize(8); // caption token
        cl->setFont(cf);
        cl->setBrush(kTextMuted);
        const QRectF cb = cl->boundingRect();
        cl->setPos(x + (kColumnWidth - cb.width()) / 2.0, kLabelBand + 2.0);
      }
    }
  }
}

void WellCorrelationPanel::applySelection(const QStringList &ids)
{
  const QSet<QString> selected(ids.begin(), ids.end());
  const auto items = m_scene->items();
  for (QGraphicsItem *it : items)
  {
    if (!it->data(kWellIdRole).isValid())
      continue; // label/tick children carry no id
    const bool on = selected.contains(it->data(kWellIdRole).toString());
    if (it->data(kHighlightRole).toBool() == on)
      continue;
    it->setData(kHighlightRole, on);
    static_cast<QGraphicsPathItem *>(it)->setPen(columnPen(on));
  }
}
