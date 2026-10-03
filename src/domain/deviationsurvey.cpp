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
// #168/#126：一段最小曲率圆弧的几何框架。切向 T(φ) = cosφ·t1 + sinφ·n，
// φ∈[0,β]，弧长 s = φ/β·ΔMD；位置 = R·(sinφ·t1 + (1−cosφ)·n)，R = ΔMD/β。
// 分量顺序 (north, east, down)。degenerate：β≈0（直线）或 β≈π（掉头，n 无定义）。
struct ArcFrame
{
  double t1[3] = {0, 0, 0};
  double n[3] = {0, 0, 0};
  double beta = 0.0;
  bool straight = false;
  bool reversal = false;
};

void unitTangent(double incDeg, double aziDeg, double out[3])
{
  const double i = incDeg * kDeg2Rad;
  const double a = aziDeg * kDeg2Rad;
  out[0] = std::sin(i) * std::cos(a);
  out[1] = std::sin(i) * std::sin(a);
  out[2] = std::cos(i);
}

ArcFrame arcFrame(const DeviationStation &a, const DeviationStation &b)
{
  ArcFrame f;
  double t2[3];
  unitTangent(a.inclinationDeg, a.azimuthDeg, f.t1);
  unitTangent(b.inclinationDeg, b.azimuthDeg, t2);
  double c = f.t1[0] * t2[0] + f.t1[1] * t2[1] + f.t1[2] * t2[2];
  c = std::min(1.0, std::max(-1.0, c));
  f.beta = std::acos(c);
  const double sb = std::sin(f.beta);
  if (f.beta < 1e-9)
  {
    f.straight = true;
    return f;
  }
  if (sb < 1e-9)
  {
    f.reversal = true;
    return f;
  }
  for (int k = 0; k < 3; ++k)
    f.n[k] = (t2[k] - c * f.t1[k]) / sb;
  return f;
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
      const double span = b.md - a.md;
      const double t = (md - a.md) / span;
      const TrajectoryPoint &base = m_points.at(i - 1);
      const ArcFrame f = arcFrame(a, b);
      if (!f.reversal)
      {
        // #168：沿站间最小曲率圆弧精确取点（切向 slerp），而非把井斜/方位线性
        // 内插后另算子段——后者偏离圆弧，起点近直井（方位无意义）时水平误差达米级。
        double dN, dE, dV;
        if (f.straight)
        {
          const double s = t * span;
          dN = s * f.t1[0];
          dE = s * f.t1[1];
          dV = s * f.t1[2];
        }
        else
        {
          const double r = span / f.beta;
          const double phi = t * f.beta;
          const double sp = std::sin(phi), cp = 1.0 - std::cos(phi);
          dN = r * (sp * f.t1[0] + cp * f.n[0]);
          dE = r * (sp * f.t1[1] + cp * f.n[1]);
          dV = r * (sp * f.t1[2] + cp * f.n[2]);
        }
        p.tvd = base.tvd + dV;
        p.north = base.north + dN;
        p.east = base.east + dE;
        return p;
      }
      // β≈π（原地掉头，圆弧平面不定）：退回角度内插子段。
      const double iEnd = a.inclinationDeg +
                          t * (b.inclinationDeg - a.inclinationDeg);
      const double aEnd =
          a.azimuthDeg + t * wrappedAzimuthDelta(a.azimuthDeg, b.azimuthDeg);
      double dTvd = 0, dNorth = 0, dEast = 0;
      segmentIncrement(t * span, a.inclinationDeg, a.azimuthDeg, iEnd,
                       aEnd, &dTvd, &dNorth, &dEast);
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

  // #126 契约：返回「首次到达」该垂深的 MD（最小的 md 使 tvdAt(md)≈tvd）。
  // 垂深可非单调（井斜>90° 上翘段），故不能用「tvd ≥ 末站垂深 → 表后外延」
  // 这类隐含单调的判据；按 MD 顺序扫描单调片段，命中即在片段内二分。
  const DeviationStation &first = m_stations.constFirst();
  const TrajectoryPoint &fp = m_points.constFirst();
  const double cFirst = std::cos(first.inclinationDeg * kDeg2Rad);
  if (cFirst > 1e-9 && tvd <= fp.tvd)
    return tvd / cFirst; // 表前：井口锚直线（垂深自 0 单调增至首站）

  // 单调片段内二分：tvdAt 在 [m0,m1] 单调，目标在两端值之间。
  const auto bisect = [this, tvd](double m0, double m1, double t0, double t1) {
    if (std::fabs(t1 - t0) <= 1e-12)
      return m0; // 水平片段：垂深不变，取片段首
    const bool increasing = t1 > t0;
    double lo = m0, hi = m1;
    for (int iter = 0; iter < 100; ++iter)
    {
      const double mid = 0.5 * (lo + hi);
      const double v = pointAt(mid).tvd;
      if ((v < tvd) == increasing)
        lo = mid;
      else
        hi = mid;
    }
    return 0.5 * (lo + hi);
  };
  const auto within = [tvd](double t0, double t1) {
    const double lo = std::min(t0, t1), hi = std::max(t0, t1);
    const double eps = 1e-9 * std::max(1.0, std::fabs(tvd));
    return tvd >= lo - eps && tvd <= hi + eps;
  };

  double deepestMd = fp.md;
  double deepestTvd = fp.tvd;
  for (int i = 1; i < m_points.size(); ++i)
  {
    const DeviationStation &sa = m_stations.at(i - 1);
    const DeviationStation &sb = m_stations.at(i);
    const TrajectoryPoint &a = m_points.at(i - 1);
    const TrajectoryPoint &b = m_points.at(i);
    // 圆弧上垂向切分量 cosφ·t1v + sinφ·nv 至多变号一次（β<π）→ 至多一个垂深极值。
    double split = std::numeric_limits<double>::quiet_NaN();
    const ArcFrame f = arcFrame(sa, sb);
    if (!f.straight && !f.reversal)
    {
      double phi = std::atan2(-f.t1[2], f.n[2]);
      if (phi < 0.0)
        phi += std::numbers::pi;
      if (phi > 1e-12 && phi < f.beta - 1e-12)
        split = a.md + phi / f.beta * (b.md - a.md);
    }
    else if (f.reversal)
    {
      // 掉头段：退回粗扫描找极值（罕见；保守取 64 等分中垂深最极端处）。
      double best = a.tvd;
      for (int k = 1; k < 64; ++k)
      {
        const double m = a.md + (b.md - a.md) * k / 64.0;
        const double v = pointAt(m).tvd;
        if ((b.tvd >= a.tvd) ? v < best : v > best)
        {
          best = v;
          split = m;
        }
      }
    }
    double m0 = a.md, t0 = a.tvd;
    if (std::isfinite(split))
    {
      const double ts = pointAt(split).tvd;
      if (ts > deepestTvd)
      {
        deepestTvd = ts;
        deepestMd = split;
      }
      if (within(t0, ts))
        return bisect(m0, split, t0, ts);
      m0 = split;
      t0 = ts;
    }
    if (b.tvd > deepestTvd)
    {
      deepestTvd = b.tvd;
      deepestMd = b.md;
    }
    if (within(t0, b.tvd))
      return bisect(m0, b.md, t0, b.tvd);
  }

  // 表后：沿末站姿态直线外延（垂深随 MD 单调，方向由 cos(i) 符号定）。
  const DeviationStation &last = m_stations.constLast();
  const TrajectoryPoint &lp = m_points.constLast();
  const double c = std::cos(last.inclinationDeg * kDeg2Rad);
  if ((c > 1e-9 && tvd >= lp.tvd) || (c < -1e-9 && tvd <= lp.tvd))
    return lp.md + (tvd - lp.tvd) / c;
  // 不可达（比轨迹最深处还深、且末段不再加深）：返回最接近处——最深点 MD
  // （水平末段即末站 MD，与旧契约一致）。调用方可用 tvdAt(结果) 判断是否命中。
  return deepestMd;
}

} // namespace paleo
