#include "correlationtrack.h"

#include <QPainter>
#include <QVariantMap>

#include <qgsfillsymbol.h>
#include <qgslinechartplot.h>
#include <qgslinesymbol.h>
#include <qgsmargins.h>
#include <qgsmarkersymbol.h>
#include <qgsplot.h>
#include <qgsrendercontext.h>
#include <qgstextformat.h>

#include <cmath>

// Wave-base: data/logic methods are final-quality; render() ports
// correlationpanel.cpp::addCurveItems onto QgsLineChartPlot — QGIS's plot
// engine always draws the polyline, this class never builds a QPainterPath.

CorrelationTrack::CorrelationTrack(const QString &mnemonic, const QString &unit,
                                   const QVector<float> &depths, const QVector<float> &values)
  : m_mnemonic(mnemonic), m_unit(unit), m_depths(depths), m_values(values)
{
}

void CorrelationTrack::setData(const QVector<float> &depths, const QVector<float> &values)
{
  m_depths = depths;
  m_values = values;
}

bool CorrelationTrack::isEmpty() const
{
  return seriesCount() == 0;
}

int CorrelationTrack::seriesCount() const
{
  const qsizetype n = sampleCount();
  int runs = 0;
  qsizetype run = 0;
  for (qsizetype i = 0; i < n; ++i)
  {
    if (std::isfinite(m_depths.at(i)) && std::isfinite(m_values.at(i)))
      ++run;
    else
    {
      if (run >= 2) ++runs;
      run = 0;
    }
  }
  if (run >= 2) ++runs;
  return runs;
}

QPair<float, float> CorrelationTrack::valueRange() const
{
  const qsizetype n = sampleCount();
  float lo = 0.0f, hi = 0.0f;
  bool any = false;
  for (qsizetype i = 0; i < n; ++i)
  {
    if (!std::isfinite(m_depths.at(i)) || !std::isfinite(m_values.at(i)))
      continue;
    if (!any) { lo = hi = m_values.at(i); any = true; }
    else { lo = qMin(lo, m_values.at(i)); hi = qMax(hi, m_values.at(i)); }
  }
  return {lo, hi};
}

QString CorrelationTrack::caption() const
{
  return m_unit.isEmpty() ? m_mnemonic : m_mnemonic + QStringLiteral(" · ") + m_unit;
}

QImage CorrelationTrack::render(int w, int h, float depthMin, float depthMax,
                                float depthOffset) const
{
  if (isEmpty())
    return {};

  // Degenerate strips collapse to 8×8 — the plot needs a real paint target.
  const int pxW = qMax(8, w);
  const int pxH = qMax(8, h);

  // --- decimation ----------------------------------------------------------
  // 50-well sections re-render on every axis change; a 100k-sample LAS must
  // not rebuild a 100k-point series. Uniform stride thins the samples to
  // ~4×pxH plot points (4 per output pixel — below visible stroke error);
  // the first/last sample are always kept. NaN sentinels are always kept:
  // they add no series points, only run boundaries, so gaps survive
  // thinning. Trade-off vs true min/max decimation: a spike narrower than
  // the stride between kept samples can be clipped, and a finite run
  // shorter than the stride can vanish entirely (sub-pixel at 4 pts/px) —
  // accepted for the bounded plot cost the 50-well budget demands.
  const qsizetype n = sampleCount();
  const qsizetype budget = qMax<qsizetype>(4 * pxH, 8);
  const qsizetype stride = qMax<qsizetype>(1, (n + budget - 1) / budget);

  // Split the thinned samples into contiguous finite runs → one series per
  // run, so NULL gaps stay gaps (port of the panel's series logic).
  QgsPlotData data; // owns the series once added
  QList<std::pair<double, double>> run;
  const auto flushRun = [&](QList<std::pair<double, double>> &pts) {
    if (pts.size() < 2)
    {
      pts.clear();
      return; // a lone sample draws no line — skip it
    }
    auto *s = new QgsXyPlotSeries();
    s->setName(m_mnemonic);
    s->setData(pts);
    data.addSeries(s); // takes ownership
    pts.clear();
  };
  const double off = static_cast<double>(depthOffset);
  for (qsizetype i = 0; i < n; ++i)
  {
    const float d = m_depths.at(i);
    const float v = m_values.at(i);
    const bool finite = std::isfinite(d) && std::isfinite(v);
    if (finite && i % stride != 0 && i != n - 1)
      continue; // thinned out
    if (!finite)
    {
      flushRun(run);
      continue;
    }
    // x = value, y = -(d - depthOffset): Qgs2DXyPlot's y axis grows upward
    // and a well track wants depth growing downward — negating display
    // depth flips it (QgsPlotAxis has no inversion API in QGIS 4.2).
    run.append({static_cast<double>(v), off - static_cast<double>(d)});
  }
  flushRun(run);
  if (data.series().isEmpty())
    return {}; // decimation left nothing drawable at this resolution

  // --- axis scaling ----------------------------------------------------------
  // x spans the FULL-sample value range (never the decimated one — thinning
  // must not shift the track's horizontal scale); a flat curve gets an
  // epsilon so a zero-width range still renders. y spans the section-shared
  // window exactly as passed, in display coordinates: the offset applies to
  // the SAMPLES only, so flatten moves the curve within the shared axis —
  // subtracting it from the axis too would cancel it out and flatten would
  // be a no-op (ink lands at (d - depthMin)/(depthMax - depthMin) either way).
  const QPair<float, float> vr = valueRange();
  const double xMin = static_cast<double>(vr.first);
  const double yMin = -static_cast<double>(depthMax);
  const double yMax = -static_cast<double>(depthMin);

  QgsLineChartPlot plot;
  // NB in QGIS 4.2's standalone (non-layout) render path, Qgs2DPlot::setSize
  // is consumed in painter units — so this passes pixels, not millimeters.
  plot.setSize(QSizeF(pxW, pxH));
  plot.setMargins(QgsMargins()); // the curve fills the strip; chrome draws around
  plot.setXMinimum(xMin);
  plot.setXMaximum(qMax(static_cast<double>(vr.second), xMin + 1e-6));
  plot.setYMinimum(yMin);
  plot.setYMaximum(qMax(yMax, yMin + 1e-6));

  // Compact track look (ported from the panel): transparent chart chrome,
  // invisible grid lines, ~0-size axis text + huge label interval → the
  // interior reserves almost no axis space, so the curve spans the strip.
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

  // Data-symbol ink (DESIGN.md: curve colors are data symbols, not UI
  // tokens): m_color stroke at 0.8, no point markers — applied to EVERY
  // series so NaN-split segments keep the track's color.
  const int seriesTotal = static_cast<int>(data.series().size());
  for (int i = 0; i < seriesTotal; ++i)
  {
    plot.setLineSymbolAt(i, QgsLineSymbol::createSimple(
        QVariantMap{{QStringLiteral("color"), m_color.name()},
                    {QStringLiteral("width"), QStringLiteral("0.8")}}).release());
    plot.setMarkerSymbolAt(i, QgsMarkerSymbol::createSimple(
        QVariantMap{{QStringLiteral("color"), QStringLiteral("0,0,0,0")},
                    {QStringLiteral("outline_color"), QStringLiteral("0,0,0,0")}}).release());
  }

  // --- render + crop + stretch ------------------------------------------------
  QImage img(QSize(pxW, pxH), QImage::Format_ARGB32_Premultiplied);
  img.fill(Qt::transparent);
  QRect interior;
  {
    QPainter p(&img);
    QgsRenderContext rc = QgsRenderContext::fromQPainter(&p);
    QgsPlotRenderContext pc;
    // Axis decoration space is computed off the render context — query the
    // real content rect so the curve crop fills the strip exactly.
    interior = plot.interiorPlotArea(rc, pc, data).toAlignedRect()
                   .intersected(img.rect());
    plot.render(rc, pc, data);
  }
  const QImage content = interior.isEmpty() ? img : img.copy(interior);
  if (content.size() == QSize(pxW, pxH))
    return content;
  // Stretch the content crop back over the full strip — a track always
  // spans its whole cell even when the axis reservation was nonzero.
  return content.scaled(pxW, pxH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}
