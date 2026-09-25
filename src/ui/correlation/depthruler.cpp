#include "depthruler.h"

#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <cmath>

// Wave-base stub: tick math (niceStep/major/minor/labels) is final-quality;
// subtask D lands the polished paint() (mono tnum labels, grid, unit tag).

DepthRuler::DepthRuler()
{
  setAcceptedMouseButtons(Qt::NoButton);
}

void DepthRuler::setRange(float depthMin, float depthMax)
{
  if (depthMax < depthMin)
    qSwap(depthMin, depthMax);
  m_min = depthMin;
  m_max = qMax(depthMax, depthMin + 1e-3f);
  m_stepDirty = true;
  prepareGeometryChange();
}

void DepthRuler::setUnit(Unit unit)
{
  m_unit = unit;
  m_stepDirty = true;
  update();
}

void DepthRuler::setHeight(qreal px)
{
  m_height = qMax(px, 20.0);
  prepareGeometryChange();
}

double DepthRuler::niceStep(double span, int targetMajors)
{
  if (span <= 0.0 || targetMajors <= 0)
    return 1.0;
  const double raw = span / targetMajors;
  const double mag = std::pow(10.0, std::floor(std::log10(raw)));
  const double norm = raw / mag;
  double nice;
  if (norm < 1.5)       nice = 1.0;
  else if (norm < 3.5)  nice = 2.0;
  else if (norm < 7.5)  nice = 5.0;
  else                  nice = 10.0;
  return nice * mag;
}

double DepthRuler::majorStep() const
{
  if (m_stepDirty)
  {
    const double conv = (m_unit == Unit::Feet) ? 3.280839895 : 1.0;
    m_majorStep = niceStep((m_max - m_min) * conv);
    m_stepDirty = false;
  }
  return m_majorStep;
}

QList<double> DepthRuler::majorDepths() const
{
  QList<double> out;
  const double conv = (m_unit == Unit::Feet) ? 3.280839895 : 1.0;
  const double step = majorStep();
  const double lo = m_min * conv, hi = m_max * conv;
  for (double v = std::ceil(lo / step) * step; v <= hi + step * 1e-9; v += step)
    out.append(v);
  return out;
}

QList<double> DepthRuler::minorDepths() const
{
  QList<double> out;
  const double conv = (m_unit == Unit::Feet) ? 3.280839895 : 1.0;
  const double minor = majorStep() / 5.0;
  const double lo = m_min * conv, hi = m_max * conv;
  const auto majors = majorDepths();
  for (double v = std::ceil(lo / minor) * minor; v <= hi + minor * 1e-9; v += minor)
  {
    bool isMajor = false;
    for (double m : majors)
      if (std::abs(m - v) < minor * 1e-3)
      {
        isMajor = true;
        break;
      }
    if (!isMajor)
      out.append(v);
  }
  return out;
}

QString DepthRuler::labelText(double depth) const
{
  return QString::number(depth, 'f', std::abs(majorStep()) < 1.0 ? 1 : 0);
}

qreal DepthRuler::yForDepth(float depth) const
{
  const double t = (static_cast<double>(depth) - m_min) / (m_max - m_min);
  return t * m_height;
}

QRectF DepthRuler::boundingRect() const
{
  return QRectF(0.0, 0.0, m_labelWidth, m_height);
}

void DepthRuler::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget)
{
  Q_UNUSED(option); Q_UNUSED(widget);
  if (!painter)
    return;

  const QColor textMuted(QStringLiteral("#5D6E80"));
  const QColor border(QStringLiteral("#DFE5EC"));
  painter->setPen(QPen(border, 1.0));
  for (double d : majorDepths())
  {
    const qreal y = yForDepth(static_cast<float>(
        d / ((m_unit == Unit::Feet) ? 3.280839895 : 1.0)));
    painter->drawLine(QPointF(m_labelWidth - 8.0, y), QPointF(m_labelWidth, y));
  }
  QFont f = painter->font();
  f.setPointSize(7);
  painter->setFont(f);
  painter->setPen(QPen(textMuted, 1.0));
  for (double d : majorDepths())
  {
    const qreal y = yForDepth(static_cast<float>(
        d / ((m_unit == Unit::Feet) ? 3.280839895 : 1.0)));
    painter->drawText(QPointF(2.0, y - 3.0), labelText(d));
  }
}
