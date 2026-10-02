// 层：测试壳
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "../src/algorithms/stratgrid/stratgrid.h"
#include "../src/algorithms/stratgrid/upscale.h"
#include "../src/io/lasparser.h"

#include <cmath>
#include <limits>

using namespace paleo::stratgrid;

namespace
{

ZoneGrid zone(float top, float bot, int nk, int cols = 3, int rows = 2)
{
  SurfaceGrid a;
  SurfaceGrid b;
  a.cols = b.cols = cols;
  a.rows = b.rows = rows;
  a.dx = b.dx = 10;
  a.dy = b.dy = 10;
  a.z.assign(static_cast<std::size_t>(cols * rows), top);
  b.z.assign(static_cast<std::size_t>(cols * rows), bot);
  ZoneGrid grid;
  QString err;
  const bool ok = buildZoneGrid(a, b, nk, &grid, &err);
  Q_ASSERT(ok);
  Q_UNUSED(err);
  return grid;
}

WellCurve vertical(const QString &id, double x, double y, double z0, double z1,
                   std::initializer_list<std::pair<double, double>> samples,
                   const QString &curve = QStringLiteral("GR"))
{
  WellCurve w;
  w.wellId = id;
  w.curveName = curve;
  w.stations.push_back(WellStation{z0, x, y, z0});
  w.stations.push_back(WellStation{z1, x, y, z1});
  for (const auto &s : samples)
    w.curve.push_back(CurvePoint{s.first, s.second});
  return w;
}

} // namespace

class TestUpscale : public QObject
{
  Q_OBJECT
private slots:
  void constantCurveStaysConstant();
  void interfaceBelongsToDeeperLayer();
  void wellMissingZoneIsAbsent();
  void gapIsNotBridged();
  void medianAndMode();
  void realLasFixture();
};

void TestUpscale::constantCurveStaysConstant()
{
  const ZoneGrid grid = zone(0.0f, 30.0f, 3);
  const WellCurve well = vertical(QStringLiteral("W1"), 5.0, 5.0, 0.0, 30.0,
                                  {{0.0, 7.0}, {10.0, 7.0}, {20.0, 7.0}, {30.0, 7.0}});
  const Aggregator aggs[] = {Aggregator::Mean, Aggregator::ThicknessWeightedMean,
                             Aggregator::Median, Aggregator::Mode};
  for (Aggregator agg : aggs)
  {
    UpscaleTable table;
    QString err;
    QVERIFY2(upscaleWells(grid, {well}, agg, &table, &err), qPrintable(err));
    QCOMPARE(table.nLayers, 3);
    for (int k = 0; k < 3; ++k)
    {
      QVERIFY(table.at(0, k).hasValue);
      QCOMPARE(table.at(0, k).value, 7.0);
      QCOMPARE(table.at(0, k).columnI, 0);
      QCOMPARE(table.at(0, k).columnJ, 0);
    }
  }
}

void TestUpscale::interfaceBelongsToDeeperLayer()
{
  const ZoneGrid grid = zone(0.0f, 30.0f, 3);
  // 界面 z=10 只进第 1 层，z=20 只进第 2 层，底 z=30 留在末层。
  const WellCurve well = vertical(QStringLiteral("W1"), 5.0, 5.0, 0.0, 30.0,
                                  {{0.0, 1.0}, {10.0, 2.0}, {20.0, 3.0}, {30.0, 3.0}});
  UpscaleTable table;
  QVERIFY(upscaleWells(grid, {well}, Aggregator::Mean, &table, nullptr));
  QVERIFY(table.at(0, 0).hasValue);
  QCOMPARE(table.at(0, 0).value, 1.0);
  QCOMPARE(table.at(0, 1).value, 2.0);
  QCOMPARE(table.at(0, 2).value, 3.0);

  UpscaleTable weighted;
  QVERIFY(upscaleWells(grid, {well}, Aggregator::ThicknessWeightedMean, &weighted, nullptr));
  // 段 [0,10] 线性 1→2，均值 1.5；[10,20] 2→3 均值 2.5；[20,30] 恒 3。
  QVERIFY(std::fabs(weighted.at(0, 0).value - 1.5) < 1e-9);
  QVERIFY(std::fabs(weighted.at(0, 1).value - 2.5) < 1e-9);
  QVERIFY(std::fabs(weighted.at(0, 2).value - 3.0) < 1e-9);
}

void TestUpscale::wellMissingZoneIsAbsent()
{
  const ZoneGrid grid = zone(0.0f, 30.0f, 3);
  const WellCurve above = vertical(QStringLiteral("W1"), 5.0, 5.0, 100.0, 110.0,
                                   {{100.0, 4.0}, {110.0, 4.0}});
  const WellCurve outside = vertical(QStringLiteral("W2"), -50.0, 5.0, 0.0, 30.0,
                                     {{0.0, 4.0}, {30.0, 4.0}});
  UpscaleTable table;
  QVERIFY(upscaleWells(grid, {above, outside}, Aggregator::ThicknessWeightedMean, &table, nullptr));
  QCOMPARE(table.nWells, 2);
  for (int w = 0; w < 2; ++w)
  {
    for (int k = 0; k < 3; ++k)
    {
      QVERIFY(!table.at(w, k).hasValue);
      QVERIFY(std::isnan(table.at(w, k).value));
      QVERIFY(!(table.at(w, k).value == 0.0));
    }
  }
}

void TestUpscale::gapIsNotBridged()
{
  const ZoneGrid grid = zone(0.0f, 10.0f, 1);
  const WellCurve well = vertical(QStringLiteral("W1"), 5.0, 5.0, 0.0, 10.0,
                                  {{1.0, 0.0}, {5.0, std::numeric_limits<double>::quiet_NaN()},
                                   {9.0, 10.0}});
  UpscaleTable weighted;
  QVERIFY(upscaleWells(grid, {well}, Aggregator::ThicknessWeightedMean, &weighted, nullptr));
  QVERIFY(!weighted.at(0, 0).hasValue);
  QVERIFY(std::isnan(weighted.at(0, 0).value));

  UpscaleTable mean;
  QVERIFY(upscaleWells(grid, {well}, Aggregator::Mean, &mean, nullptr));
  QVERIFY(mean.at(0, 0).hasValue);
  QCOMPARE(mean.at(0, 0).value, 5.0);
}

void TestUpscale::medianAndMode()
{
  const ZoneGrid grid = zone(0.0f, 10.0f, 1);
  const WellCurve codes = vertical(QStringLiteral("W1"), 5.0, 5.0, 0.0, 10.0,
                                   {{1.0, 1.0}, {2.0, 1.0}, {3.0, 2.0}, {4.0, 2.0}});
  UpscaleTable mode;
  QVERIFY(upscaleWells(grid, {codes}, Aggregator::Mode, &mode, nullptr));
  QCOMPARE(mode.at(0, 0).value, 1.0); // 1 与 2 各两次，并列取较小

  const WellCurve odds = vertical(QStringLiteral("W1"), 5.0, 5.0, 0.0, 10.0,
                                  {{1.0, 1.0}, {3.0, 1.0}, {5.0, 9.0}});
  UpscaleTable mode2;
  QVERIFY(upscaleWells(grid, {odds}, Aggregator::Mode, &mode2, nullptr));
  QCOMPARE(mode2.at(0, 0).value, 1.0);

  const WellCurve spread = vertical(QStringLiteral("W1"), 5.0, 5.0, 0.0, 10.0,
                                    {{1.0, 20.0}, {3.0, 30.0}, {8.0, 40.0}});
  UpscaleTable med;
  QVERIFY(upscaleWells(grid, {spread}, Aggregator::Median, &med, nullptr));
  QCOMPARE(med.at(0, 0).value, 30.0);

  QString err;
  WellCurve bad = codes;
  bad.stations.clear();
  UpscaleTable discarded;
  QVERIFY(!upscaleWells(grid, {bad}, Aggregator::Mean, &discarded, &err));
  QVERIFY(err.contains(QStringLiteral("轨迹")));
}

void TestUpscale::realLasFixture()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString lasPath = tmp.filePath(QStringLiteral("const.las"));
  QFile out(lasPath);
  QVERIFY(out.open(QIODevice::WriteOnly | QIODevice::Text));
  out.write(
      "~Version Information\n"
      "VERS.   2.0:\n"
      "WRAP.   NO:\n"
      "~Well\n"
      "STRT.M 100.0 :\n"
      "STOP.M 120.0 :\n"
      "STEP.M 1.0 :\n"
      "NULL.   -999.25 :\n"
      "WELL.   W-CONST :\n"
      "~Curve\n"
      "DEPT.M : DEPTH\n"
      "GR.GAPI : GAMMA\n"
      "~Ascii\n");
  for (int i = 0; i <= 20; ++i)
  {
    const double md = 100.0 + i;
    out.write(QStringLiteral("%1 42.0\n").arg(md, 0, 'f', 3).toUtf8());
  }
  out.close();

  QStringList names;
  QList<LasCurve> curves;
  QString err;
  QVERIFY2(LasParser::parse(lasPath, names, curves, &err), qPrintable(err));
  QCOMPARE(names.first(), QStringLiteral("DEPT"));
  int gr = -1;
  for (int i = 0; i < names.size(); ++i)
    if (names.at(i) == QLatin1String("GR"))
      gr = i;
  QVERIFY(gr > 0);

  WellCurve well;
  well.wellId = QStringLiteral("W-CONST");
  well.curveName = QStringLiteral("GR");
  const double md0 = curves.at(0).values.first();
  const double md1 = curves.at(0).values.last();
  well.stations.push_back(WellStation{md0, 5.0, 5.0, md0});
  well.stations.push_back(WellStation{md1, 5.0, 5.0, md1});
  for (int i = 0; i < curves.at(0).values.size(); ++i)
    well.curve.push_back(CurvePoint{curves.at(0).values.at(i), curves.at(gr).values.at(i)});

  const ZoneGrid grid = zone(100.0f, 120.0f, 4);
  UpscaleTable table;
  QVERIFY(upscaleWells(grid, {well}, Aggregator::ThicknessWeightedMean, &table, &err));
  for (int k = 0; k < 4; ++k)
  {
    QVERIFY(table.at(0, k).hasValue);
    QVERIFY(std::fabs(table.at(0, k).value - 42.0) < 1e-6);
  }

#ifdef PROJECT_FIXTURE_DIR
  const QString fixture = QStringLiteral(PROJECT_FIXTURE_DIR) + QStringLiteral("/A1.Las");
  if (!QFile::exists(fixture))
    QSKIP("A1.Las fixture not present");
  QStringList fnames;
  QList<LasCurve> fcurves;
  QVERIFY2(LasParser::parseDepthRange(fixture, 200.0, 400.0, fnames, fcurves, &err),
           qPrintable(err));
  QVERIFY(!fcurves.isEmpty());
  int fgr = -1;
  for (int i = 0; i < fnames.size(); ++i)
    if (fnames.at(i) == QLatin1String("GR"))
      fgr = i;
  QVERIFY(fgr > 0);
  WellCurve nullWell;
  nullWell.wellId = QStringLiteral("A1");
  nullWell.curveName = QStringLiteral("GR");
  const double a = fcurves.at(0).values.first();
  const double b = fcurves.at(0).values.last();
  nullWell.stations.push_back(WellStation{a, 5.0, 5.0, a});
  nullWell.stations.push_back(WellStation{b, 5.0, 5.0, b});
  bool anyFinite = false;
  double sum = 0;
  int finiteCount = 0;
  for (int i = 0; i < fcurves.at(0).values.size(); ++i)
  {
    const double v = fcurves.at(fgr).values.at(i);
    if (std::isfinite(v))
    {
      anyFinite = true;
      sum += v;
      ++finiteCount;
    }
    nullWell.curve.push_back(CurvePoint{fcurves.at(0).values.at(i), v});
  }
  if (!anyFinite)
  {
    // 脱敏桩：整段 NULL。粗化必须无值，不能写成 0。
    const ZoneGrid shallow = zone(static_cast<float>(a), static_cast<float>(b), 2);
    UpscaleTable nullTable;
    QVERIFY(upscaleWells(shallow, {nullWell}, Aggregator::Mean, &nullTable, nullptr));
    QVERIFY(!nullTable.at(0, 0).hasValue);
    QVERIFY(!nullTable.at(0, 1).hasValue);
    QVERIFY(std::isnan(nullTable.at(0, 0).value));
  }
  else
  {
    // 夹具这段有有限样点：单层均值必须等于样点算术平均（测试侧手算，不调用核）。
    const ZoneGrid shallow = zone(static_cast<float>(a), static_cast<float>(b), 1);
    UpscaleTable got;
    QVERIFY(upscaleWells(shallow, {nullWell}, Aggregator::Mean, &got, nullptr));
    QVERIFY(got.at(0, 0).hasValue);
    const double expect = sum / static_cast<double>(finiteCount);
    QVERIFY2(std::fabs(got.at(0, 0).value - expect) < 1e-6,
             qPrintable(QStringLiteral("upscale %1 vs mean %2 (n=%3)")
                            .arg(got.at(0, 0).value)
                            .arg(expect)
                            .arg(finiteCount)));
  }
#else
  QSKIP("PROJECT_FIXTURE_DIR not defined");
#endif
}

QTEST_GUILESS_MAIN(TestUpscale)
#include "tst_upscale.moc"
