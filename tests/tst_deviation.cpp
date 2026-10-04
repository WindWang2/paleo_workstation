// 层：测试壳
#include <QtTest>

#include "domain/deviationsurvey.h"

#include <cmath>
#include <numbers>

using paleo::DeviationStation;
using paleo::TrajectoryPoint;
using paleo::WellDeviationSurvey;

constexpr double kPi = std::numbers::pi;

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

QTEST_MAIN(tst_deviation)
#include "tst_deviation.moc"
