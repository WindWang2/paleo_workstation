// 层：视图
#include "intervalstatistics.h"

#include <QMap>
#include <QTextStream>

#include <algorithm>
#include <cmath>

namespace WellComposite
{

namespace {

CurveIntervalStats statsForCurve(const CurveData &c, double top, double bottom)
{
  CurveIntervalStats s;
  s.name = c.name;
  s.unit = c.unit;

  double sum = 0.0, sumSq = 0.0;
  double minV = std::numeric_limits<double>::max();
  double maxV = std::numeric_limits<double>::lowest();
  int n = 0;

  for (int i = 0; i < c.depths.size(); ++i)
  {
    const float d = c.depths.at(i);
    const float v = c.values.at(i);
    if (d < top || d >= bottom || !std::isfinite(v))
      continue;
    ++n;
    sum += v;
    sumSq += static_cast<double>(v) * v;
    minV = std::min<double>(minV, v);
    maxV = std::max<double>(maxV, v);
  }

  s.sampleCount = n;
  if (n > 0)
  {
    s.min = minV;
    s.max = maxV;
    s.mean = sum / n;
    if (n >= 2)
    {
      const double var = (sumSq - n * s.mean * s.mean) / (n - 1);
      s.stdDev = var > 0.0 ? std::sqrt(var) : 0.0;
    }
  }
  return s;
}

} // namespace

IntervalStatsReport computeIntervalStats(double top, double bottom,
                                         const QVector<CurveData> &continuousCurves,
                                         const QVector<CurveData> &discreteCurves,
                                         const QVector<LithologyInterval> &lithology,
                                         const QVector<QPair<double, QString>> &markers)
{
  IntervalStatsReport rep;
  rep.top = top;
  rep.bottom = bottom;
  if (bottom <= top)
    return rep;

  for (const auto &c : continuousCurves)
    rep.curves << statsForCurve(c, top, bottom);
  for (const auto &c : discreteCurves)
    rep.curves << statsForCurve(c, top, bottom);

  // 岩性净厚度（跨界区间按截断）与占比
  double totalLitho = 0.0;
  QMap<QString, double> thicknessByName;
  for (const auto &li : lithology)
  {
    const double lo = std::max<double>(li.topDepth, top);
    const double hi = std::min<double>(li.bottomDepth, bottom);
    if (hi <= lo)
      continue;
    thicknessByName[li.lithoName] += (hi - lo);
    totalLitho += (hi - lo);
  }
  // 按厚度降序
  QStringList names = thicknessByName.keys();
  std::sort(names.begin(), names.end(), [&](const QString &a, const QString &b) {
    return thicknessByName.value(a) > thicknessByName.value(b);
  });
  for (const QString &n : names)
  {
    LithoProportion p;
    p.name = n;
    p.thickness = thicknessByName.value(n);
    p.fraction = totalLitho > 0.0 ? p.thickness / totalLitho : 0.0;
    rep.lithology << p;
  }

  // 区间内标志层计数（线深落在 [top, bottom]）
  for (const auto &m : markers)
  {
    if (m.first >= top && m.first <= bottom)
      ++rep.markerCount;
  }

  return rep;
}

IntervalStatsReport computeIntervalStats(double top, double bottom, const ComprehensiveWellData &data)
{
  return computeIntervalStats(top, bottom, data.continuousCurves, data.discreteCurves,
                              data.lithologyIntervals, data.standardHorizons);
}

QString IntervalStatsReport::toTsv() const
{
  QString out;
  QTextStream ts(&out);
  const auto f = [](double v) { return QString::number(v, 'f', 3); };

  ts << QStringLiteral("区间\t%1\t%2\t跨度 %3 m\n")
            .arg(QString::number(top, 'f', 1), QString::number(bottom, 'f', 1),
                 QString::number(bottom - top, 'f', 1));
  ts << QStringLiteral("曲线\t单位\tn\tmin\tmax\tmean\tstd\n");
  for (const auto &c : curves)
    ts << c.name << QLatin1Char('\t') << (c.unit.isEmpty() ? QStringLiteral("-") : c.unit)
       << QLatin1Char('\t') << c.sampleCount << QLatin1Char('\t')
       << (c.sampleCount ? f(c.min) : QStringLiteral("-")) << QLatin1Char('\t')
       << (c.sampleCount ? f(c.max) : QStringLiteral("-")) << QLatin1Char('\t')
       << (c.sampleCount ? f(c.mean) : QStringLiteral("-")) << QLatin1Char('\t')
       << (c.sampleCount ? f(c.stdDev) : QStringLiteral("-")) << QLatin1Char('\n');

  ts << QStringLiteral("岩性\t厚度(m)\t占比\n");
  for (const auto &l : lithology)
    ts << l.name << QLatin1Char('\t') << QString::number(l.thickness, 'f', 1)
       << QLatin1Char('\t') << QString::number(l.fraction * 100.0, 'f', 1) << QLatin1Char('%')
       << QLatin1Char('\n');

  ts << QStringLiteral("标志层计数\t%1\n").arg(markerCount);
  return out;
}

} // namespace WellComposite
