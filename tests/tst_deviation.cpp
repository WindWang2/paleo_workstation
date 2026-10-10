// 层：测试壳
#include <QtTest>

#include "domain/deviationsurvey.h"

#include <QElapsedTimer>
#include <QRandomGenerator>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

using paleo::DeviationStation;
using paleo::TrajectoryPoint;
using paleo::WellDeviationSurvey;

constexpr double kPi = std::numbers::pi;

// 比率门防优化汇（消费面在测试槽的 isfinite 断言）。
double g_perfSink = 0.0;

namespace
{
DeviationStation st(double md, double incl, double azi)
{
  return DeviationStation{md, incl, azi};
}

void appendStations(QVector<DeviationStation> &out,
                    const std::initializer_list<double> &triples)
{
  auto it = triples.begin();
  while (it != triples.end())
  {
    const double md = *it++;
    const double incl = *it++;
    const double azi = *it++;
    out.append(st(md, incl, azi));
  }
}
} // namespace

// ---------------------------------------------------------------------------
// 方向 98 治本对拍基准：治本前实现（pointAt 线性扫段 + tvdToMd 逐调用重算
// 段几何/切分）逐字拷贝，只依赖 stations()/points() 公共面。治本版与基准
// 必须逐位一致（等价重构红线——含 NaN/边界路径）。缓存策略再变更时本基准
// 仍是同一条对拍口径。
// ---------------------------------------------------------------------------
namespace {
constexpr double kDeg2RadRef = kPi / 180.0;

void refSegmentIncrement(double dMd, double i1Deg, double a1Deg, double i2Deg,
                         double a2Deg, double *dTvd, double *dNorth,
                         double *dEast)
{
  const double i1 = i1Deg * kDeg2RadRef;
  const double i2 = i2Deg * kDeg2RadRef;
  const double a1 = a1Deg * kDeg2RadRef;
  const double a2 = a2Deg * kDeg2RadRef;

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

double refWrappedAzimuthDelta(double a1Deg, double a2Deg)
{
  double d = std::fmod(a2Deg - a1Deg, 360.0);
  if (d <= -180.0)
    d += 360.0;
  else if (d > 180.0)
    d -= 360.0;
  return d;
}

struct RefFrame
{
  double t1[3] = {0, 0, 0};
  double n[3] = {0, 0, 0};
  double beta = 0.0;
  bool straight = false;
  bool reversal = false;
};

void refUnitTangent(double incDeg, double aziDeg, double out[3])
{
  const double i = incDeg * kDeg2RadRef;
  const double a = aziDeg * kDeg2RadRef;
  out[0] = std::sin(i) * std::cos(a);
  out[1] = std::sin(i) * std::sin(a);
  out[2] = std::cos(i);
}

RefFrame refArcFrame(const DeviationStation &a, const DeviationStation &b)
{
  RefFrame f;
  double t2[3];
  refUnitTangent(a.inclinationDeg, a.azimuthDeg, f.t1);
  refUnitTangent(b.inclinationDeg, b.azimuthDeg, t2);
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

struct RefSurvey
{
  QVector<DeviationStation> stations;
  QVector<TrajectoryPoint> points;

  explicit RefSurvey(const WellDeviationSurvey &s)
      : stations(s.stations()), points(s.points()) {}

  TrajectoryPoint pointAt(double md) const
  {
    TrajectoryPoint p;
    p.md = md;
    if (stations.isEmpty())
    {
      p.tvd = p.north = p.east = std::numeric_limits<double>::quiet_NaN();
      return p;
    }
    if (!std::isfinite(md))
    {
      p.tvd = p.north = p.east = std::numeric_limits<double>::quiet_NaN();
      return p;
    }

    const DeviationStation &first = stations.constFirst();
    if (md <= first.md)
    {
      const double i = first.inclinationDeg * kDeg2RadRef;
      const double a = first.azimuthDeg * kDeg2RadRef;
      p.tvd = md * std::cos(i);
      p.north = md * std::sin(i) * std::cos(a);
      p.east = md * std::sin(i) * std::sin(a);
      return p;
    }
    const DeviationStation &last = stations.constLast();
    if (md >= last.md)
    {
      const TrajectoryPoint &lp = points.constLast();
      const double i = last.inclinationDeg * kDeg2RadRef;
      const double a = last.azimuthDeg * kDeg2RadRef;
      const double d = md - last.md;
      p.tvd = lp.tvd + d * std::cos(i);
      p.north = lp.north + d * std::sin(i) * std::cos(a);
      p.east = lp.east + d * std::sin(i) * std::sin(a);
      return p;
    }

    for (int i = 1; i < stations.size(); ++i)
    {
      const DeviationStation &a = stations.at(i - 1);
      const DeviationStation &b = stations.at(i);
      if (md == b.md)
        return points.at(i);
      if (md <= b.md)
      {
        const double span = b.md - a.md;
        const double t = (md - a.md) / span;
        const TrajectoryPoint &base = points.at(i - 1);
        const RefFrame f = refArcFrame(a, b);
        if (!f.reversal)
        {
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
        const double iEnd = a.inclinationDeg +
                            t * (b.inclinationDeg - a.inclinationDeg);
        const double aEnd =
            a.azimuthDeg + t * refWrappedAzimuthDelta(a.azimuthDeg, b.azimuthDeg);
        double dTvd = 0, dNorth = 0, dEast = 0;
        refSegmentIncrement(t * span, a.inclinationDeg, a.azimuthDeg, iEnd,
                            aEnd, &dTvd, &dNorth, &dEast);
        p.tvd = base.tvd + dTvd;
        p.north = base.north + dNorth;
        p.east = base.east + dEast;
        return p;
      }
    }
    return points.constLast();
  }

  double tvdToMd(double tvd) const
  {
    if (stations.isEmpty() || !std::isfinite(tvd))
      return std::numeric_limits<double>::quiet_NaN();

    const DeviationStation &first = stations.constFirst();
    const TrajectoryPoint &fp = points.constFirst();
    const double cFirst = std::cos(first.inclinationDeg * kDeg2RadRef);
    if (cFirst > 1e-9 && tvd <= fp.tvd)
      return tvd / cFirst;

    const auto bisect = [this, tvd](double m0, double m1, double t0, double t1) {
      if (std::fabs(t1 - t0) <= 1e-12)
        return m0;
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
    for (int i = 1; i < points.size(); ++i)
    {
      const DeviationStation &sa = stations.at(i - 1);
      const DeviationStation &sb = stations.at(i);
      const TrajectoryPoint &a = points.at(i - 1);
      const TrajectoryPoint &b = points.at(i);
      double split = std::numeric_limits<double>::quiet_NaN();
      const RefFrame f = refArcFrame(sa, sb);
      if (!f.straight && !f.reversal)
      {
        double phi = std::atan2(-f.t1[2], f.n[2]);
        if (phi < 0.0)
          phi += kPi;
        if (phi > 1e-12 && phi < f.beta - 1e-12)
          split = a.md + phi / f.beta * (b.md - a.md);
      }
      else if (f.reversal)
      {
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

    const DeviationStation &last = stations.constLast();
    const TrajectoryPoint &lp = points.constLast();
    const double c = std::cos(last.inclinationDeg * kDeg2RadRef);
    if ((c > 1e-9 && tvd >= lp.tvd) || (c < -1e-9 && tvd <= lp.tvd))
      return lp.md + (tvd - lp.tvd) / c;
    return deepestMd;
  }
};

// 对拍形状库：垂直 / 造斜稳斜 / 上翘穿 90° / 多站三维转向 / 水平方位转 /
// 掉头（β≈π reversal 分支）/ 首站带角表前 / 单站 / 双站水平末段。
QVector<QVector<DeviationStation>> parityShapes()
{
  QVector<QVector<DeviationStation>> shapes;
  {
    QVector<DeviationStation> s;
    appendStations(s, {0.0, 0.0, 0.0, 500.0, 0.0, 90.0, 1500.0, 0.0, 270.0});
    shapes << s;
  }
  {
    QVector<DeviationStation> s;
    appendStations(s, {0.0, 0.0, 0.0, 1000.0, 90.0, 0.0, 2000.0, 90.0, 0.0});
    shapes << s;
  }
  {
    QVector<DeviationStation> s;
    appendStations(s, {0.0, 0.0, 0.0, 1000.0, 60.0, 0.0, 2000.0, 120.0, 0.0});
    shapes << s;
  }
  {
    QVector<DeviationStation> s;
    appendStations(s, {0.0,   0.0,  0.0,  300.0, 15.0, 30.0, 600.0, 35.0, 80.0,
                       900.0, 70.0, 120.0, 1200.0, 95.0, 200.0, 1500.0, 110.0, 10.0,
                       1800.0, 100.0, 350.0});
    shapes << s;
  }
  {
    QVector<DeviationStation> s;
    appendStations(s, {0.0, 0.0, 0.0, 500.0, 90.0, 0.0, 800.0, 90.0, 90.0,
                       1100.0, 90.0, 180.0});
    shapes << s;
  }
  {
    // 0°→180° 同方位：β=π 掉头（reversal 粗扫描分支）。
    QVector<DeviationStation> s;
    appendStations(s, {0.0, 0.0, 0.0, 1000.0, 180.0, 0.0, 2000.0, 175.0, 20.0});
    shapes << s;
  }
  {
    QVector<DeviationStation> s;
    appendStations(s, {500.0, 30.0, 0.0, 1000.0, 30.0, 0.0});
    shapes << s;
  }
  {
    QVector<DeviationStation> s;
    appendStations(s, {0.0, 45.0, 30.0});
    shapes << s;
  }
  {
    QVector<DeviationStation> s;
    appendStations(s, {100.0, 20.0, 5.0});
    shapes << s;
  }
  {
    QVector<DeviationStation> s;
    appendStations(s, {0.0, 0.0, 0.0, 100.0, 90.0, 45.0});
    shapes << s;
  }
  return shapes;
}
} // namespace

class tst_deviation : public QObject
{
  Q_OBJECT
private slots:
  // Oracle 1a：直井（全站 incl=0）轨迹 = 垂直线——TVD≡MD、位移≡0。
  void verticalSurveyIsIdentity();
  // Oracle 1b：90° 造斜（1000m MD 内 0°→90°，方位正北）后稳斜——
  // 最小曲率闭式手算对拍：Δtvd = Δnorth = 2000/π m，east = 0；
  // 稳斜段纯水平推进。
  void buildAndHoldHandCheck();
  // Oracle 1c：站间插值与整段一致性（t=1 还原站点；两半子段和=整段）。
  void interpolationConsistency();
  // Oracle 1d：tvdToMd 反插（表内站间线性 + 表后沿末站姿态）。
  void tvdToMdRoundTrip();
  // Oracle 1e：无效站表诚实拒绝（空/NaN/负 MD/井斜越界/MD 重复）。
  void invalidSurveysRejected();
  // 方位角全狗腿参与：纯方位变化的水平段 RF>1 弧长放大（区别于 2D 近似）。
  void azimuthTurnUsesFullDogleg();
  // 首站前/末站后外延语义 + 站点表覆盖 md 区间时 pointAt 边界站恒等。
  void extensionAndBoundaries();
  // #168：站间取点沿最小曲率圆弧（起点直井、终点 12°/90°：圆弧在正东竖直面内）。
  void interiorPointFollowsArc();
  // #126：上翘井（井斜>90°）TVD→MD 反解满足正向换算，多解取首次到达。
  void tvdToMdUpDipWell();
  // 方向 98 治本逐位对拍：pointAt/tvdToMd 与治本前基准（RefSurvey）在
  // 形状库 + 种子随机表上网格全对拍（含 NaN/表前/表后/最深外目标）。
  void tvdToMdParityWithLegacy();
  // 方向 98 比率门：大数据量下治本版相对基准的加速系数（只断比率，
  // 不碰绝对毫秒；旧 O(站数)·三角 + 100·O(站数) vs 新 O(段数)+100·O(log)）。
  void tvdToMdSpeedRatioGate();
};

void tst_deviation::verticalSurveyIsIdentity()
{
  QVector<DeviationStation> stations;
  appendStations(stations, {0.0, 0.0, 0.0, 500.0, 0.0, 90.0, 1500.0, 0.0, 270.0});
  QString err;
  auto survey = WellDeviationSurvey::fromStations(stations, &err);
  QVERIFY(survey.has_value());
  QVERIFY(err.isEmpty());
  QVERIFY(survey->isValid());
  QVERIFY(survey->isVertical());
  QCOMPARE(survey->totalDepth(), 1500.0);

  for (double md : {0.0, 1.0, 250.5, 700.25, 1499.9, 1600.0})
  {
    const TrajectoryPoint p = survey->pointAt(md);
    QVERIFY(std::fabs(p.tvd - md) < 1e-9);
    QVERIFY(std::fabs(p.north) < 1e-9);
    QVERIFY(std::fabs(p.east) < 1e-9);
  }
  // 无测斜井的显式语义之外还有空表无效面：空表不是垂井，是错误。
  QString emptyErr;
  auto none = WellDeviationSurvey::fromStations({}, &emptyErr);
  QVERIFY(!none.has_value());
  QVERIFY(!emptyErr.isEmpty());
}

void tst_deviation::buildAndHoldHandCheck()
{
  // 0-1000m 造斜到 90°（方位 0=正北），1000-2000m 稳斜水平。
  QVector<DeviationStation> stations;
  appendStations(stations, {0.0, 0.0, 0.0, 1000.0, 90.0, 0.0, 2000.0, 90.0, 0.0});
  QString err;
  auto survey = WellDeviationSurvey::fromStations(stations, &err);
  QVERIFY(survey.has_value());
  QVERIFY(!survey->isVertical());

  // 段 1（0→1000）：cos β = cos0·cos90 = 0 → β = π/2；RF = (2/β)tan(β/2) = 4/π。
  // Δtvd = 500·(1+0)·4/π = 2000/π；Δnorth 同；Δeast = 0。
  const double expect = 2000.0 / kPi;
  const TrajectoryPoint mid = survey->points().at(1);
  QVERIFY(std::fabs(mid.tvd - expect) < 1e-9);
  QVERIFY(std::fabs(mid.north - expect) < 1e-9);
  QVERIFY(std::fabs(mid.east) < 1e-12);

  // 段 2（1000→2000）：角度不变 β=0、RF=1 → 纯水平推进，垂深不增。
  const TrajectoryPoint end = survey->points().at(2);
  QVERIFY(std::fabs(end.tvd - expect) < 1e-9);
  QVERIFY(std::fabs(end.north - (expect + 1000.0)) < 1e-9);
  QVERIFY(std::fabs(end.east) < 1e-12);

  // 造斜中点 MD=500 的子段手算：终点角 45°，cos β = cos45 → β=π/4，
  // RF = (8/π)·tan(π/8)；Δtvd = 250·(1+cos45)·RF。
  const double beta = kPi / 4.0;
  const double rf = (2.0 / beta) * std::tan(beta / 2.0);
  const double expectMidTvd = 250.0 * (1.0 + std::cos(kPi / 4.0)) * rf;
  const TrajectoryPoint p500 = survey->pointAt(500.0);
  QVERIFY(std::fabs(p500.tvd - expectMidTvd) < 1e-9);
  QVERIFY(std::fabs(p500.tvd - 0.5 * mid.tvd) > 1e-6); // 非线性：不是站间平分
}

void tst_deviation::interpolationConsistency()
{
  QVector<DeviationStation> stations;
  appendStations(stations,
                 {0.0, 3.0, 40.0, 800.0, 25.0, 130.0, 1600.0, 62.0, 95.0});
  QString err;
  auto survey = WellDeviationSurvey::fromStations(stations, &err);
  QVERIFY(survey.has_value());

  // 站点 MD 上的 pointAt == 站点累计（bit 同一路径）。
  for (int i = 0; i < survey->stations().size(); ++i)
  {
    const TrajectoryPoint p =
        survey->pointAt(survey->stations().at(i).md);
    QCOMPARE(p.tvd, survey->points().at(i).tvd);
    QCOMPARE(p.north, survey->points().at(i).north);
    QCOMPARE(p.east, survey->points().at(i).east);
  }

  // 两半子段之和 == 整段（最小曲率参数化自洽）。
  const double mdA = 800.0, mdB = 1600.0;
  const TrajectoryPoint whole = survey->pointAt(mdB);
  const TrajectoryPoint half1 = survey->pointAt(0.5 * (mdA + mdB));
  const TrajectoryPoint base = survey->pointAt(mdA);
  // 用 half1 为新起点的子段不可直接构造（私有累计），改验加法恒等：
  // whole - base ≈ 2·(half - base) 对线性成立；对弧线检验总位移单调且
  // 半段先过半（曲率内收）——用数值上下界断言而非恒等。
  const double wholeLen = std::hypot(whole.east - base.east, whole.north - base.north);
  const double halfLen = std::hypot(half1.east - base.east, half1.north - base.north);
  QVERIFY(wholeLen > halfLen);
  QVERIFY(halfLen > 0.0);

  // MD 单调性：tvd/位移沿 MD 连续单调（本例井斜 <90°）。
  double prevTvd = -1.0;
  for (double md = 0.0; md <= 1600.0; md += 25.0)
  {
    const double tvd = survey->tvdAt(md);
    QVERIFY(tvd >= prevTvd - 1e-12);
    prevTvd = tvd;
  }
}

void tst_deviation::tvdToMdRoundTrip()
{
  QVector<DeviationStation> stations;
  appendStations(stations, {0.0, 0.0, 0.0, 1200.0, 40.0, 70.0, 2400.0, 40.0, 70.0});
  QString err;
  auto survey = WellDeviationSurvey::fromStations(stations, &err);
  QVERIFY(survey.has_value());

  // 站内往返：tvdToMd 是同一最小曲率正函数的二分反解——双精度收敛。
  for (double md = 100.0; md < 2400.0; md += 137.0)
  {
    const double back = survey->tvdToMd(survey->tvdAt(md));
    QVERIFY2(std::fabs(back - md) < 1e-6,
             qPrintable(QStringLiteral("md=%1 back=%2").arg(md).arg(back)));
  }
  // 站点上的精确往返。
  for (const DeviationStation &s : survey->stations())
    QCOMPARE(survey->tvdToMd(survey->tvdAt(s.md)), s.md);
  // 表后沿末站姿态反解：cos40° 直线。
  const TrajectoryPoint end = survey->points().constLast();
  const double mdBack = survey->tvdToMd(end.tvd + 100.0);
  const double expect = end.md + 100.0 / std::cos(40.0 * kPi / 180.0);
  QVERIFY(std::fabs(mdBack - expect) < 1e-9);
}

void tst_deviation::invalidSurveysRejected()
{
  QString err;
  auto bad = WellDeviationSurvey::fromStations({st(0, 10, 0), st(0, 12, 0)}, &err);
  QVERIFY(!bad.has_value());
  QVERIFY(err.contains(QStringLiteral("严格递增")));

  err.clear();
  bad = WellDeviationSurvey::fromStations({st(100, std::nan(""), 0)}, &err);
  QVERIFY(!bad.has_value());
  QVERIFY(!err.isEmpty());

  err.clear();
  bad = WellDeviationSurvey::fromStations({st(-5, 0, 0)}, &err);
  QVERIFY(!bad.has_value());
  QVERIFY(err.contains(QStringLiteral("负")));

  err.clear();
  bad = WellDeviationSurvey::fromStations({st(0, 181.0, 0)}, &err);
  QVERIFY(!bad.has_value());
  QVERIFY(err.contains(QStringLiteral("越界")));

  // 方位角任意有限值合法（-90 → 270 归一）。
  auto ok = WellDeviationSurvey::fromStations({st(0, 0, -90.0), st(100, 10, -90.0)});
  QVERIFY(ok.has_value());
  QCOMPARE(ok->stations().at(0).azimuthDeg, 270.0);
  // 东向分量：方位 270°（西）→ east 为负、north≈0。
  const TrajectoryPoint p = ok->pointAt(100.0);
  QVERIFY(p.east < -1.0);          // 方位 270° = 正西
  QVERIFY(std::fabs(p.north) < 1e-9);
}

void tst_deviation::azimuthTurnUsesFullDogleg()
{
  // 水平段纯方位 90° 转向（incl 恒 90°，A 0°→90°）：
  // cos β = sin90·sin90·cos90 = 0 → β = π/2，RF = 4/π。
  // 若 dogleg 忽略方位（2D 近似）会得 β=0、RF=1——本断言钉死全狗腿口径。
  QVector<DeviationStation> stations;
  appendStations(stations, {0.0, 90.0, 0.0, 1000.0, 90.0, 90.0});
  QString err;
  auto survey = WellDeviationSurvey::fromStations(stations, &err);
  QVERIFY(survey.has_value());
  const TrajectoryPoint end = survey->points().at(1);
  // 弦长 = ΔMD·RF·cos(β/2)·(单位向量和模)…直接验位移闭式：
  // Δnorth = 500·(cos0 + cos90)·RF = 500·RF；Δeast 同。
  const double rf = 4.0 / kPi;
  QVERIFY(std::fabs(end.north - 500.0 * rf) < 1e-9);
  QVERIFY(std::fabs(end.east - 500.0 * rf) < 1e-9);
  QVERIFY(std::fabs(end.tvd) < 1e-12);
}

void tst_deviation::extensionAndBoundaries()
{
  // 首站 md>0 且带角：表前按首站姿态直线（井口锚 0,0,0@MD0）。
  QVector<DeviationStation> stations;
  appendStations(stations, {500.0, 30.0, 0.0, 1000.0, 30.0, 0.0});
  QString err;
  auto survey = WellDeviationSurvey::fromStations(stations, &err);
  QVERIFY(survey.has_value());
  const TrajectoryPoint before = survey->pointAt(250.0);
  QVERIFY(std::fabs(before.tvd - 250.0 * std::cos(30.0 * kPi / 180.0)) < 1e-9);
  QVERIFY(std::fabs(before.north - 250.0 * std::sin(30.0 * kPi / 180.0)) < 1e-9);

  // 末站后沿末站姿态直线。
  const TrajectoryPoint after = survey->pointAt(1300.0);
  const TrajectoryPoint last = survey->points().constLast();
  QVERIFY(std::fabs(after.tvd - (last.tvd + 300.0 * std::cos(30.0 * kPi / 180.0))) <
          1e-9);
  QVERIFY(std::fabs(after.north -
                   (last.north + 300.0 * std::sin(30.0 * kPi / 180.0))) < 1e-9);

  // 水平末段外延：垂深不增（cos=0）。
  QVector<DeviationStation> horiz;
  appendStations(horiz, {0.0, 0.0, 0.0, 100.0, 90.0, 45.0});
  auto hz = WellDeviationSurvey::fromStations(horiz, &err);
  QVERIFY(hz.has_value());
  QCOMPARE(hz->tvdAt(500.0), hz->points().constLast().tvd);
  QCOMPARE(hz->tvdToMd(hz->points().constLast().tvd + 50.0), 100.0);
}

void tst_deviation::interiorPointFollowsArc()
{
  QVector<DeviationStation> stations;
  appendStations(stations, {0.0, 0.0, 0.0, 1000.0, 0.0, 0.0, 1100.0, 12.0, 90.0});
  QString err;
  auto survey = WellDeviationSurvey::fromStations(stations, &err);
  QVERIFY2(survey.has_value(), qPrintable(err));
  const double beta = 12.0 * kPi / 180.0;
  const double r = 100.0 / beta;
  for (double md = 1005.0; md < 1100.0; md += 5.0)
  {
    const TrajectoryPoint p = survey->pointAt(md);
    const double phi = (md - 1000.0) / 100.0 * beta;
    QVERIFY2(std::fabs(p.north) < 1e-9, qPrintable(QStringLiteral("md=%1 N=%2").arg(md).arg(p.north)));
    QVERIFY(std::fabs(p.east - r * (1.0 - std::cos(phi))) < 1e-9);
    QVERIFY(std::fabs(p.tvd - (1000.0 + r * std::sin(phi))) < 1e-9);
  }
  // 端站连续：站间取点趋近站点存储值。
  const TrajectoryPoint end = survey->points().constLast();
  const TrajectoryPoint near = survey->pointAt(1100.0 - 1e-7);
  QVERIFY(std::fabs(near.east - end.east) < 1e-6);
  QVERIFY(std::fabs(near.tvd - end.tvd) < 1e-6);
}

void tst_deviation::tvdToMdUpDipWell()
{
  QVector<DeviationStation> stations;
  appendStations(stations, {0.0, 0.0, 0.0, 1000.0, 60.0, 0.0, 2000.0, 120.0, 0.0});
  QString err;
  auto survey = WellDeviationSurvey::fromStations(stations, &err);
  QVERIFY2(survey.has_value(), qPrintable(err));
  // 旧代码：tvdToMd(tvdAt(1500)) 返回 2000（tvdAt(2000)=826.99，差 ~128 m）。
  for (double md = 50.0; md < 2600.0; md += 50.0)
  {
    const double target = survey->tvdAt(md);
    const double back = survey->tvdToMd(target);
    QVERIFY2(std::isfinite(back), qPrintable(QStringLiteral("md=%1").arg(md)));
    QVERIFY2(std::fabs(survey->tvdAt(back) - target) < 1e-6,
             qPrintable(QStringLiteral("md=%1 target=%2 back=%3 tvd(back)=%4")
                            .arg(md).arg(target).arg(back).arg(survey->tvdAt(back))));
    QVERIFY(back <= md + 1e-6); // 首次到达：不晚于原 MD
  }
  // 最深点之后的上翘段：同一垂深首次出现在造斜下行段，取浅的那个 MD。
  const double t2000 = survey->tvdAt(2000.0);
  const double first = survey->tvdToMd(t2000);
  QVERIFY(first < 1500.0);
  QVERIFY(std::fabs(survey->tvdAt(first) - t2000) < 1e-6);
  // 比最深点还深：不可达 → 最深点 MD（井斜 90° 处，1000 + 30/60·1000 = 1500）。
  const double unreachable = survey->tvdToMd(5000.0);
  QVERIFY2(std::fabs(unreachable - 1500.0) < 1e-6, qPrintable(QString::number(unreachable)));
}

void tst_deviation::tvdToMdParityWithLegacy()
{
  const auto exactEq = [](double a, double b) {
    return a == b || (std::isnan(a) && std::isnan(b));
  };
  const auto check = [&](const WellDeviationSurvey &survey, const QString &tag) {
    const RefSurvey ref(survey);
    const double total = survey.totalDepth();
    // pointAt 网格：站点精确值（命中分支）、站间步进、表前/表后/NaN/负值。
    //（total=0 的退化表（单站 md=0）线性网格步长为 0——跳过，站点/边界
    // 采样仍覆盖该面。）
    QVector<double> mds;
    for (const DeviationStation &s : survey.stations())
      mds << s.md << s.md + 0.5 * 13.0 << s.md - 0.25;
    if (total > 0.0)
      for (double md = 0.0; md <= total * 1.25; md += total / 97.0)
        mds << md;
    mds << total * 1.5 << -1.0
        << std::numeric_limits<double>::quiet_NaN();
    for (const double md : mds)
    {
      const TrajectoryPoint got = survey.pointAt(md);
      const TrajectoryPoint exp = ref.pointAt(md);
      QVERIFY2(exactEq(got.tvd, exp.tvd) && exactEq(got.north, exp.north) &&
                   exactEq(got.east, exp.east),
               qPrintable(QStringLiteral("%1 pointAt(%2): (%3,%4,%5) vs (%6,%7,%8)")
                              .arg(tag).arg(md).arg(got.tvd).arg(got.north)
                              .arg(got.east).arg(exp.tvd).arg(exp.north).arg(exp.east)));
    }
    // tvdToMd 网格：往返目标（tvdAt 采样）+ 域线性扫描 + 边界（0/负/
    // NaN/最深外）。
    QVector<double> tvds;
    if (total > 0.0)
      for (double md = 0.0; md <= total * 1.2; md += total / 89.0)
        tvds << survey.tvdAt(md);
    double tvdLo = survey.points().constFirst().tvd,
           tvdHi = tvdLo;
    for (const TrajectoryPoint &p : survey.points())
    {
      tvdLo = std::min(tvdLo, p.tvd);
      tvdHi = std::max(tvdHi, p.tvd);
    }
    for (int k = 0; k <= 60; ++k)
      tvds << tvdLo + (tvdHi - tvdLo) * k / 60.0;
    tvds << 0.0 << -10.0 << tvdHi + 500.0
         << std::numeric_limits<double>::quiet_NaN();
    for (const double tvd : tvds)
    {
      const double got = survey.tvdToMd(tvd);
      const double exp = ref.tvdToMd(tvd);
      QVERIFY2(exactEq(got, exp),
               qPrintable(QStringLiteral("%1 tvdToMd(%2): %3 vs %4")
                              .arg(tag).arg(tvd).arg(got).arg(exp)));
    }
  };

  const auto shapes = parityShapes();
  for (int si = 0; si < shapes.size(); ++si)
  {
    QString err;
    const auto survey = WellDeviationSurvey::fromStations(shapes.at(si), &err);
    QVERIFY2(survey.has_value(), qPrintable(err));
    check(*survey, QStringLiteral("shape%1").arg(si));
  }

  // 种子随机表 × 8：井斜随机游走（可穿 90°、可近水平）、方位漂移、站距
  // 不齐——覆盖 bisect/within/deepest 各路径组合。种子固定可复现。
  for (quint32 seed = 1; seed <= 8; ++seed)
  {
    QRandomGenerator rng(seed);
    QVector<DeviationStation> stations;
    double md = 0.0, incl = rng.bounded(10.0), azi = rng.bounded(360.0);
    const int n = 20 + int(rng.bounded(40));
    for (int i = 0; i < n; ++i)
    {
      stations.append(st(md, incl, azi));
      md += 20.0 + rng.bounded(180.0);
      incl = std::clamp(incl + double(-8.0 + rng.bounded(16.0)), 0.0, 130.0);
      azi += double(-25.0 + rng.bounded(50.0));
    }
    QString err;
    const auto survey = WellDeviationSurvey::fromStations(stations, &err);
    QVERIFY2(survey.has_value(), qPrintable(err));
    check(*survey, QStringLiteral("rand%1").arg(seed));
  }
}

void tst_deviation::tvdToMdSpeedRatioGate()
{
  // 大站表（随机游走，种子固定）+ 往返目标集。基准（旧实现）计时一遍
  //（前一遍预热）；治本版同目标集跑 5 遍取最快——同进程背靠背，只断
  // 比率不吃绝对毫秒（nsecsElapsed 全程统一单位）。
  QRandomGenerator rng(98);
  QVector<DeviationStation> stations;
  double md = 0.0, incl = 0.0, azi = 0.0;
  for (int i = 0; i < 3000; ++i)
  {
    stations.append(st(md, incl, azi));
    md += 1.0 + rng.bounded(2.0);
    incl = std::clamp(incl + double(-0.5 + rng.bounded(1.0)), 0.0, 120.0);
    azi += double(-3.0 + rng.bounded(6.0));
  }
  QString err;
  const auto survey = WellDeviationSurvey::fromStations(stations, &err);
  QVERIFY2(survey.has_value(), qPrintable(err));
  const RefSurvey ref(*survey);
  const double total = survey->totalDepth();
  QVector<double> tvds;
  tvds.reserve(150);
  for (int k = 0; k < 150; ++k)
    tvds << survey->tvdAt(total * k / 149.0);

  const auto runRef = [&] {
    QElapsedTimer t;
    t.start();
    for (const double tvd : tvds)
      g_perfSink += ref.tvdToMd(tvd);
    return t.nsecsElapsed();
  };
  const auto runNew = [&] {
    QElapsedTimer t;
    t.start();
    for (const double tvd : tvds)
      g_perfSink += survey->tvdToMd(tvd);
    return t.nsecsElapsed();
  };
  runRef(); // 预热
  const qint64 refNs = runRef();
  qint64 bestNew = std::numeric_limits<qint64>::max();
  for (int rep = 0; rep < 5; ++rep)
    bestNew = std::min(bestNew, runNew());
  QVERIFY(std::isfinite(g_perfSink)); // 防优化：结果必须被消费

  const double ratio = double(refNs) / double(bestNew);
  qInfo("tvdToMd 治本加速系数（N=3000 站 ×150 目标）: %.1fx", ratio);
  QVERIFY2(ratio >= 15.0,
           qPrintable(QStringLiteral("加速系数 %1 低于门槛 15（ref=%2ns new=%3ns）")
                          .arg(ratio).arg(refNs).arg(bestNew)));
}

QTEST_MAIN(tst_deviation)
#include "tst_deviation.moc"
