// 层：数据
#include "deviationsurvey.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace paleo
{
namespace
{
constexpr double kDeg2Rad = std::numbers::pi / 180.0;

void fail(QString *error, const QString &text)
{
  if (error)
    *error = text;
}

// 段增量（最小曲率）：自 (md, i1, A1) 到角度 (i2, A2)、弧长 dMd 的位置增量。
// dogleg 只用角度差——弧长由调用方给（整段=ΔMD，站间查询=子段弧长）。
void segmentIncrement(double dMd, double i1Deg, double a1Deg, double i2Deg,
                      double a2Deg, double *dTvd, double *dNorth, double *dEast)
{
  const double i1 = i1Deg * kDeg2Rad;
  const double i2 = i2Deg * kDeg2Rad;
  const double a1 = a1Deg * kDeg2Rad;
  const double a2 = a2Deg * kDeg2Rad;

  double cosBeta = std::cos(i1) * std::cos(i2) +
                   std::sin(i1) * std::sin(i2) * std::cos(a2 - a1);
  cosBeta = std::min(1.0, std::max(-1.0, cosBeta));
  const double beta = std::acos(cosBeta);

  double rf = 1.0;
  if (beta > 1e-9)
    rf = (2.0 / beta) * std::tan(beta * 0.5);

  *dTvd = 0.5 * dMd * (std::cos(i1) + std::cos(i2)) * rf;
  *dNorth = 0.5 * dMd * (std::sin(i1) * std::cos(a1) + std::sin(i2) * std::cos(a2)) * rf;
  *dEast = 0.5 * dMd * (std::sin(i1) * std::sin(a1) + std::sin(i2) * std::sin(a2)) * rf;
}

// 方位差归到 (-180,180]：插值走短弧。
double wrappedAzimuthDelta(double a1Deg, double a2Deg)
{
  double d = std::fmod(a2Deg - a1Deg, 360.0);
  if (d <= -180.0)
    d += 360.0;
  else if (d > 180.0)
    d -= 360.0;
  return d;
}
} // namespace

std::optional<WellDeviationSurvey> WellDeviationSurvey::fromStations(
    QVector<DeviationStation> stations, QString *error)
{
  if (error)
    error->clear();
  WellDeviationSurvey survey;
  if (stations.isEmpty())
  {
    fail(error, QStringLiteral("测斜站表为空"));
    return std::nullopt;
  }
  for (const DeviationStation &s : stations)
  {
    if (!std::isfinite(s.md) || !std::isfinite(s.inclinationDeg) ||
        !std::isfinite(s.azimuthDeg))
    {
      fail(error, QStringLiteral("测斜站含非有限值（MD=%1）")
                       .arg(QString::number(s.md)));
      return std::nullopt;
    }
    if (s.md < 0.0)
    {
      fail(error, QStringLiteral("测斜站 MD 为负：%1").arg(QString::number(s.md)));
      return std::nullopt;
    }
    if (s.inclinationDeg < 0.0 || s.inclinationDeg > 180.0)
    {
      fail(error, QStringLiteral("井斜角越界 [0,180]：%1°")
                       .arg(QString::number(s.inclinationDeg)));
      return std::nullopt;
    }
  }

  std::sort(stations.begin(), stations.end(),
            [](const DeviationStation &a, const DeviationStation &b) {
              return a.md < b.md;
            });
  for (int i = 1; i < stations.size(); ++i)
  {
    if (stations.at(i).md - stations.at(i - 1).md <= 1e-9)
    {
      fail(error, QStringLiteral("测斜站 MD 未严格递增（%1 附近）")
                       .arg(QString::number(stations.at(i).md)));
      return std::nullopt;
    }
  }
  for (DeviationStation &s : stations)
  {
    double a = std::fmod(s.azimuthDeg, 360.0);
    if (a < 0.0)
      a += 360.0;
    s.azimuthDeg = a;
  }

  // 站点累计：首站前按首站姿态自井口 (0,0,0)@MD0 直线锚定。
  survey.m_stations = stations;
  survey.m_points.reserve(stations.size());
  {
    const DeviationStation &first = stations.constFirst();
    TrajectoryPoint p;
    p.md = first.md;
    p.tvd = first.md * std::cos(first.inclinationDeg * kDeg2Rad);
    p.north = first.md * std::sin(first.inclinationDeg * kDeg2Rad) *
              std::cos(first.azimuthDeg * kDeg2Rad);
    p.east = first.md * std::sin(first.inclinationDeg * kDeg2Rad) *
             std::sin(first.azimuthDeg * kDeg2Rad);
    survey.m_points.append(p);
  }
  for (int i = 1; i < stations.size(); ++i)
  {
    const DeviationStation &a = stations.at(i - 1);
    const DeviationStation &b = stations.at(i);
    double dTvd = 0, dNorth = 0, dEast = 0;
    segmentIncrement(b.md - a.md, a.inclinationDeg, a.azimuthDeg, b.inclinationDeg,
                     b.azimuthDeg, &dTvd, &dNorth, &dEast);
    const TrajectoryPoint &prev = survey.m_points.constLast();
    TrajectoryPoint p;
    p.md = b.md;
    p.tvd = prev.tvd + dTvd;
    p.north = prev.north + dNorth;
    p.east = prev.east + dEast;
    survey.m_points.append(p);
  }
  return survey;
}

bool WellDeviationSurvey::isVertical() const
{
  for (const DeviationStation &s : m_stations)
    if (s.inclinationDeg > 1e-6)
      return false;
  return !m_stations.isEmpty();
}

double WellDeviationSurvey::totalDepth() const
{
  return m_stations.isEmpty() ? std::numeric_limits<double>::quiet_NaN()
                              : m_stations.constLast().md;
}

TrajectoryPoint WellDeviationSurvey::pointAt(double md) const
{
  TrajectoryPoint p;
  p.md = md;
  if (m_stations.isEmpty())
  {
    p.tvd = p.north = p.east = std::numeric_limits<double>::quiet_NaN();
    return p;
  }
  if (!std::isfinite(md))
  {
    p.tvd = p.north = p.east = std::numeric_limits<double>::quiet_NaN();
    return p;
  }

  const DeviationStation &first = m_stations.constFirst();
  if (md <= first.md)
  {
    // 井口锚 (0,0,0)@MD0，按首站姿态直线。
    const double i = first.inclinationDeg * kDeg2Rad;
    const double a = first.azimuthDeg * kDeg2Rad;
    p.tvd = md * std::cos(i);
    p.north = md * std::sin(i) * std::cos(a);
    p.east = md * std::sin(i) * std::sin(a);
    return p;
  }
  const DeviationStation &last = m_stations.constLast();
  if (md >= last.md)
  {
    // 末站姿态直线外延。
    const TrajectoryPoint &lp = m_points.constLast();
    const double i = last.inclinationDeg * kDeg2Rad;
    const double a = last.azimuthDeg * kDeg2Rad;
    const double d = md - last.md;
    p.tvd = lp.tvd + d * std::cos(i);
    p.north = lp.north + d * std::sin(i) * std::cos(a);
    p.east = lp.east + d * std::sin(i) * std::sin(a);
    return p;
  }

  for (int i = 1; i < m_stations.size(); ++i)
  {
    const DeviationStation &a = m_stations.at(i - 1);
    const DeviationStation &b = m_stations.at(i);
    if (md == b.md)
      return m_points.at(i); // 站点 MD 精确命中（查询用存储值时免 ULP 差）
    if (md <= b.md)
    {
      const double t = (md - a.md) / (b.md - a.md);
      // 子段终点角度按狗腿弧长线性内插（方位走短弧）。
      const double iEnd = a.inclinationDeg +
                          t * (b.inclinationDeg - a.inclinationDeg);
      const double aEnd =
          a.azimuthDeg + t * wrappedAzimuthDelta(a.azimuthDeg, b.azimuthDeg);
      double dTvd = 0, dNorth = 0, dEast = 0;
      segmentIncrement(t * (b.md - a.md), a.inclinationDeg, a.azimuthDeg, iEnd,
                       aEnd, &dTvd, &dNorth, &dEast);
      const TrajectoryPoint &base = m_points.at(i - 1);
      p.tvd = base.tvd + dTvd;
      p.north = base.north + dNorth;
      p.east = base.east + dEast;
      return p;
    }
  }
  // 排序后严格递增保证上面必命中；此处不可达，保底末站值。
  return m_points.constLast();
}

double WellDeviationSurvey::tvdToMd(double tvd) const
{
  if (m_stations.isEmpty() || !std::isfinite(tvd))
    return std::numeric_limits<double>::quiet_NaN();

  const DeviationStation &first = m_stations.constFirst();
  const TrajectoryPoint &fp = m_points.constFirst();
  if (tvd <= fp.tvd)
  {
    // 表前：按首站姿态反解（cos i→0 的水平姿态无垂深增量，无解回 MD0）。
    const double c = std::cos(first.inclinationDeg * kDeg2Rad);
    return c > 1e-9 ? tvd / c : 0.0;
  }
  const DeviationStation &last = m_stations.constLast();
  const TrajectoryPoint &lp = m_points.constLast();
  if (tvd >= lp.tvd)
  {
    const double c = std::cos(last.inclinationDeg * kDeg2Rad);
    return c > 1e-9 ? lp.md + (tvd - lp.tvd) / c : lp.md;
  }
  for (int i = 1; i < m_points.size(); ++i)
  {
    const TrajectoryPoint &a = m_points.at(i - 1);
    const TrajectoryPoint &b = m_points.at(i);
    if (tvd <= b.tvd)
    {
      const double span = b.md - a.md;
      if (b.tvd - a.tvd <= 1e-9 || !(span > 0.0))
        return a.md; // 水平段：垂深不增，区间内 MD 不可分，取段首
      // 站间子段最小曲率的数值反解（二分）：与 pointAt 同一正函数，
      // 往返 md→tvd→md 误差收敛到双精度（井斜<90° 时段内单调唯一）。
      double lo = a.md, hi = b.md;
      for (int iter = 0; iter < 80; ++iter)
      {
        const double mid = 0.5 * (lo + hi);
        if (pointAt(mid).tvd < tvd)
          lo = mid;
        else
          hi = mid;
      }
      return 0.5 * (lo + hi);
    }
  }
  return lp.md;
}

} // namespace paleo
