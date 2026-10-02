// 层：数据（测试壳位于 tests/，被测对象为数据层纯数值核；真机数据门控）
#include <QtTest/QtTest>

#include "algorithms/gridsolver.h"
#include "io/horizonbinner.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QRegularExpression>

#include <cmath>
#include <vector>

using namespace paleo::gridsolver;

namespace
{
// QTest 槽内用的辅助：HorizonScatter → 核散点（QVector → std::vector）。
std::vector<ScatterPoint> scatterPoints(const HorizonScatter &sc)
{
  std::vector<ScatterPoint> pts;
  pts.reserve(sc.points.size());
  for (const HorizonScatterPoint &p : sc.points)
    pts.push_back({p.x, p.y, p.z});
  return pts;
}
} // namespace

// 真机 8 层位网格化实测（Oracle 6）：PALEO_REAL_PROJECT_AREA 指向真目录时
// 执行；未设置时 QSKIP（CI 不红）。输出行钉死 BASELINE <metric> = <value>
// 格式，人工誊入 docs/progress/gridding-surface-ops.md（全程只读，不写源
// 目录）。像元尺寸取层位头 Grid_size 反推的 dx——与导入装箱栅格同网格，
// 耗时口径可直接对比。
class TestGriddingRealArea : public QObject
{
  Q_OBJECT

private slots:
  void gridAllHorizons();
};

void TestGriddingRealArea::gridAllHorizons()
{
  const QString area = qEnvironmentVariable("PALEO_REAL_PROJECT_AREA");
  if (area.isEmpty())
    QSKIP("PALEO_REAL_PROJECT_AREA not set — real-data gridding skipped");

  const QDir horizonDir(QDir(area).absoluteFilePath(QStringLiteral("层位")));
  QStringList files = horizonDir.entryList(
      QStringList() << QStringLiteral("*.dat"), QDir::Files, QDir::Name);
  QVERIFY2(files.size() >= 8,
           qPrintable(QStringLiteral("expected >=8 horizon files, got %1 in %2")
                          .arg(files.size())
                          .arg(horizonDir.absolutePath())));

  qint64 totalMs = 0;
  for (const QString &name : files)
  {
    QFile f(horizonDir.absoluteFilePath(name));
    QVERIFY2(f.open(QIODevice::ReadOnly),
             qPrintable(QStringLiteral("cannot open %1").arg(name)));
    const QByteArray text = f.readAll();
    const HorizonScatter scatter = parseHorizonScatter(text);
    QVERIFY2(!scatter.points.isEmpty(),
             qPrintable(QStringLiteral("no scatter in %1").arg(name)));
    HorizonHeader hh;
    QVERIFY2(parseHorizonHeader(text, &hh),
             qPrintable(QStringLiteral("no header in %1").arg(name)));

    double minX = 1e300, maxX = -1e300, minY = 1e300, maxY = -1e300;
    for (const HorizonScatterPoint &p : scatter.points)
    {
      minX = qMin(minX, p.x);
      maxX = qMax(maxX, p.x);
      minY = qMin(minY, p.y);
      maxY = qMax(maxY, p.y);
    }
    const double cell = (hh.p2x - hh.p1x) / (hh.gridCols - 1);
    QVERIFY2(cell > 0 && std::isfinite(cell),
             qPrintable(QStringLiteral("bad header cell in %1").arg(name)));

    QElapsedTimer timer;
    timer.start();
    std::vector<float> z;
    GriddingStats stats;
    QString err;
    const GridGeometry geom = geometryForExtent(minX, maxX, minY, maxY, cell, &err);
    QVERIFY2(geom.isValid(), qPrintable(err));
    // 张力 0.25 = Processing/UI 的实用默认：纯最小曲率（T=0）在陡梯度边
    // 缘有已知的过冲振荡（Smith & Wessel 引入张力的动机），真机口径不拿
    // T=0 当默认。
    GriddingParams gp;
    gp.tension = 0.25;
    QVERIFY2(solveMinimumCurvature(scatterPoints(scatter), geom, gp, nullptr, &z, &stats,
                                   &err),
             qPrintable(QStringLiteral("%1: %2").arg(name, err)));
    const qint64 ms = timer.elapsed();
    totalMs += ms;

    // 基本健全性：解在散点极差邻域内（无发散/NaN 海）。
    int nonNull = 0;
    float zMin = 1e30f, zMax = -1e30f;
    for (float v : z)
      if (!std::isnan(v))
      {
        ++nonNull;
        zMin = qMin(zMin, v);
        zMax = qMax(zMax, v);
      }
    QVERIFY2(nonNull > int(z.size()) / 2, "majority of cells must be filled");
    const double sRange = stats.zMax - stats.zMin;
    // 样条外推的合法过冲界（±1 极差）：张力 0.25 下实测远小于此；超界 =
    // 发散/数值事故，不是外推。
    QVERIFY2(zMin >= stats.zMin - sRange && zMax <= stats.zMax + sRange,
             qPrintable(QStringLiteral("%1 solution out of data range: [%2,%3] vs [%4,%5]")
                            .arg(name)
                            .arg(zMin)
                            .arg(zMax)
                            .arg(stats.zMin)
                            .arg(stats.zMax)));

    const QString base = name.section(QLatin1Char('.'), 0, 0);
    const auto baseline = [&base](const char *metric, const QString &value)
    { qWarning("%s", qPrintable(QStringLiteral("BASELINE gridding_%1_%2 = %3")
                                    .arg(base, QString::fromUtf8(metric), value))); };
    baseline("overshoot_below", QString::number(double(stats.zMin - zMin), 'f', 3));
    baseline("overshoot_above", QString::number(double(zMax - stats.zMax), 'f', 3));
    baseline("ms", QString::number(ms));
    baseline("points", QString::number(qint64(scatter.points.size())));
    baseline("grid", QStringLiteral("%1x%2").arg(geom.cols).arg(geom.rows));
    baseline("sweeps", QString::number(stats.sweeps));
    baseline("finalDelta", QString::number(stats.finalDelta, 'g', 6));
    baseline("converged", QString::number(stats.converged ? 1 : 0));
  }
  qWarning("%s", qPrintable(
                     QStringLiteral("BASELINE gridding_all_horizons_ms = %1").arg(totalMs)));
  qWarning("%s", qPrintable(QStringLiteral("BASELINE gridding_horizon_count = %1")
                                .arg(files.size())));
}

QTEST_MAIN(TestGriddingRealArea)
#include "tst_gridding_realarea.moc"
