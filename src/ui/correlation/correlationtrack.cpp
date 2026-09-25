#include "correlationtrack.h"

#include <cmath>

// Wave-base stub: data/logic methods are final-quality; render() is
// hollow until the track subtask lands the QgsLineChartPlot port.

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
  Q_UNUSED(w); Q_UNUSED(h); Q_UNUSED(depthMin); Q_UNUSED(depthMax); Q_UNUSED(depthOffset);
  return {}; // stub: subtask A ports correlationpanel.cpp::addCurveItems here
}
