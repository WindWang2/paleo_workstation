// 层：视图
#include "depthruler.h"

#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPen>

#include <cmath>

namespace
{
  // DESIGN.md tokens — the only colors this ruler ever paints.
  const QColor kTextMuted(QStringLiteral("#5D6E80"));
  const QColor kBorder(QStringLiteral("#DFE5EC"));

  // Tick geometry (scene px): marks hang off the label column's right edge.
  constexpr qreal kMajorTickLen = 8.0;
  constexpr qreal kMinorTickLen = 4.0;
  constexpr qreal kTickLabelGap = 3.0;   // gap between label column right edge and major tick
  // Half a 7pt label above the first / below the last tick: boundingRect
  // must cover them so scene repaints never clip the end labels.
  constexpr qreal kVerticalPad = 10.0;
} // namespace

DepthRuler::DepthRuler()
{
  setAcceptedMouseButtons(Qt::NoButton);   // chrome: visible, not clickable
}

void DepthRuler::setRange(float depthMin, float depthMax)
{
  if (!qIsFinite(depthMin) || !qIsFinite(depthMax))
  {
    // A NaN window (e.g. empty curve stats) must not poison the mapping —
    // fall back to a sane default window instead of painting NaN coords.
    depthMin = 0.0f;
    depthMax = 100.0f;
  }
  if (depthMax < depthMin)
    qSwap(depthMin, depthMax);
  m_min = depthMin;
  m_max = qMax(depthMax, depthMin + 1e-3f);   // never a zero span
  m_stepDirty = true;
  prepareGeometryChange();
}

void DepthRuler::setUnit(Unit unit)
{
  if (m_unit == unit)
    return;
  m_unit = unit;
  m_stepDirty = true;   // the step re-nices in the new display units
  update();
}

void DepthRuler::setHeight(qreal px)
{
  const qreal h = qMax(px, 20.0);
  if (h == m_height)
    return;
  m_height = h;
  prepareGeometryChange();
}

void DepthRuler::setGridWidth(qreal px)
{
  const qreal w = qMax(px, 0.0);
  if (w == m_gridWidth)
    return;
  prepareGeometryChange();   // grid reach is part of the bounding rect
  m_gridWidth = w;
  update();
}

double DepthRuler::displayFactor() const
{
  return (m_unit == Unit::Feet) ? 3.280839895 : 1.0;
}

double DepthRuler::niceStep(double span, int targetMajors)
{
  if (!std::isfinite(span) || span <= 0.0 || targetMajors <= 0)
    return 1.0;
  const double raw = span / targetMajors;
  const double mag = std::pow(10.0, std::floor(std::log10(raw)));
  const double norm = raw / mag;   // ∈ [1, 10)
  double nice;
  // Thresholds 1.5/3/7.5 (not the textbook 1.5/3.5/7.5) keep span/step < 9
  // at the default target of 6 in EVERY bucket — the "≤ 9 labeled ticks
  // for any range" guardrail asserted in tests:
  //   1×mag → 6·norm < 9    2×mag → 3·norm < 9    5×mag → 1.2·norm < 9
  //   10×mag → 0.6·norm < 6
  if (norm < 1.5)      nice = 1.0;
  else if (norm < 3.0) nice = 2.0;
  else if (norm < 7.5) nice = 5.0;
  else                 nice = 10.0;
  return nice * mag;
}

double DepthRuler::majorStep() const
{
  if (m_stepDirty)
  {
    m_majorStep = niceStep((m_max - m_min) * displayFactor());
    m_stepDirty = false;
  }
  return m_majorStep;
}

QList<double> DepthRuler::majorDepths() const
{
  QList<double> out;
  const double conv = displayFactor();
  const double step = majorStep();
  const double lo = m_min * conv, hi = m_max * conv;
  // Integer-lattice walk (v = n·step) instead of accumulating v += step:
  // immune to fp drift, and the tick count is exactly last-first+1 ≤ 9 by
  // the niceStep threshold tuning. The ±1e-6 nudge (on the quotient, so
  // relative) keeps boundary-exact windows from losing their end tick.
  const qint64 first = static_cast<qint64>(std::ceil(lo / step - 1e-6));
  const qint64 last  = static_cast<qint64>(std::floor(hi / step + 1e-6));
  for (qint64 n = first; n <= last; ++n)
    out.append(n * step);
  return out;
}

QList<double> DepthRuler::minorDepths() const
{
  QList<double> out;
  const double conv = displayFactor();
  const double minor = majorStep() / 5.0;
  const double lo = m_min * conv, hi = m_max * conv;
  const qint64 first = static_cast<qint64>(std::ceil(lo / minor - 1e-6));
  const qint64 last  = static_cast<qint64>(std::floor(hi / minor + 1e-6));
  for (qint64 n = first; n <= last; ++n)
  {
    if (n % 5 == 0)
      continue;   // every 5th lattice point IS a major tick — never both
    out.append(n * minor);
  }
  return out;
}

QString DepthRuler::labelText(double depth) const
{
  const double step = majorStep();
  int decimals = 0;
  if (step > 0.0 && step < 1.0)   // decimal steps (0.1/0.2/0.5×10^k) keep
    decimals = qBound(1, static_cast<int>(std::ceil(-std::log10(step))), 6);
  // A depth column never shows "-0"/"-0.0": negative zero and values that
  // round to zero at the label's precision snap to plain zero — precision
  // itself (and thus column alignment) is untouched.
  if (depth == 0.0)
    depth = 0.0;   // comparison is sign-blind; assignment clears -0
  else if (depth < 0.0 && depth > -std::pow(10.0, -decimals) / 2.0)
    depth = 0.0;
  return QString::number(depth, 'f', decimals);   // integer steps: no ".0"
}

qreal DepthRuler::yForDepth(float depth) const
{
  // Linear map of the window onto [0, height]: min→0 (top), max→height
  // (bottom) — depth increases downward. Depths outside the window
  // extrapolate along the same line.
  const double t = (static_cast<double>(depth) - m_min) /
                   (static_cast<double>(m_max) - m_min);
  return t * m_height;
}

QRectF DepthRuler::boundingRect() const
{
  const qreal w = m_labelWidth + ((m_grid && m_gridWidth > 0.0) ? m_gridWidth : 0.0);
  return QRectF(0.0, -kVerticalPad, w, m_height + 2.0 * kVerticalPad);
}

void DepthRuler::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget)
{
  Q_UNUSED(option); Q_UNUSED(widget);
  if (!painter)
    return;

  const double conv = displayFactor();
  const qreal tickRight = m_labelWidth;
  const auto yOf = [this, conv](double displayDepth) {
    return yForDepth(static_cast<float>(displayDepth / conv));
  };

  painter->setRenderHint(QPainter::Antialiasing, false);    // crisp 1px rules
  painter->setRenderHint(QPainter::TextAntialiasing, true);

  // --- unit tag: small "m"/"ft" heading the label column (8pt, muted) -----
  {
    QFont f = painter->font();
    f.setPointSize(8);
    painter->setFont(f);
    painter->setPen(QPen(kTextMuted, 1.0));
    painter->drawText(QRectF(2.0, 0.0, m_labelWidth - 4.0, 11.0),
                      Qt::AlignLeft | Qt::AlignVCenter,
                      m_unit == Unit::Feet ? QStringLiteral("ft") : QStringLiteral("m"));
  }

  // --- grid: major-tick rules running into the section area. Drawn by this
  // item, but visually they belong UNDER the well columns — the panel owns
  // that stacking (keep this ruler below the column items via z/child order;
  // this paint only guarantees the geometry).
  if (m_grid && m_gridWidth > 0.0)
  {
    painter->setPen(QPen(kBorder, 1.0));
    for (double d : majorDepths())
    {
      const qreal y = yOf(d);
      painter->drawLine(QPointF(tickRight, y), QPointF(tickRight + m_gridWidth, y));
    }
  }

  // --- tick marks: 1px muted, depth increasing downward -------------------
  painter->setPen(QPen(kTextMuted, 1.0));
  for (double d : minorDepths())
  {
    const qreal y = yOf(d);
    painter->drawLine(QPointF(tickRight - kMinorTickLen, y), QPointF(tickRight, y));
  }
  for (double d : majorDepths())
  {
    const qreal y = yOf(d);
    painter->drawLine(QPointF(tickRight - kMajorTickLen, y), QPointF(tickRight, y));
  }

  // --- labels: right-aligned against the ticks; JetBrains Mono 7pt with
  // tnum so the depth digits align down the column (DESIGN.md 数值/深度).
  // The Monospace style hint keeps the fallback fixed-pitch where the
  // vendor font is absent (CI/offscreen).
  QFont mono;
  mono.setFamilies({QStringLiteral("JetBrains Mono")});
  mono.setStyleHint(QFont::Monospace);
  mono.setFeature(QFont::Tag("tnum"), 1);
  mono.setPointSize(7);
  painter->setFont(mono);
  painter->setPen(QPen(kTextMuted, 1.0));

  const QFontMetricsF fm(mono);
  const qreal boxH = fm.height();
  const qreal labelRight = tickRight - kMajorTickLen - kTickLabelGap;
  for (double d : majorDepths())
  {
    const qreal y = yOf(d);
    painter->drawText(QRectF(0.0, y - boxH / 2.0, labelRight, boxH),
                      Qt::AlignRight | Qt::AlignVCenter, labelText(d));
  }
}
