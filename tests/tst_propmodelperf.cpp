// 层：测试壳
// 属性建模性能：比率门始终跑；绝对墙钟与真工区链只在
// PALEO_REAL_PROJECT_AREA 设置时断言（<30s、RSS 增量 <2GB）。
#include <QtTest>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include <gdal.h>
#include <qgsapplication.h>

#include "../src/catalog/datacatalog.h"
#include "../src/io/lasparser.h"
#include "../src/io/wellfileparsers.h"
#include "../src/workflow/propertymodelworkflow.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>

using namespace paleo::stratgrid;

namespace
{

SurfaceGrid constantGrid(int cols, int rows, float z)
{
  SurfaceGrid g;
  g.cols = cols;
  g.rows = rows;
  g.dx = 25;
  g.dy = -25;
  g.originX = 0;
  g.originY = rows * 25.0;
  g.z.assign(static_cast<std::size_t>(cols * rows), z);
  return g;
}

WellCurve vertical(const QString &id, double x, double y, double z0, double z1, double value)
{
  WellCurve w;
  w.wellId = id;
  w.curveName = QStringLiteral("GR");
  w.stations.push_back(WellStation{z0, x, y, z0});
  w.stations.push_back(WellStation{z1, x, y, z1});
  const int n = 11;
  for (int i = 0; i <= n; ++i)
  {
    const double md = z0 + (z1 - z0) * (static_cast<double>(i) / n);
    w.curve.push_back(CurvePoint{md, value});
  }
  return w;
}

PropertyModelRequest syntheticRequest(int cols, int rows, int layers, int wells)
{
  PropertyModelRequest req;
  req.useEmbeddedSurfaces = true;
  req.propertyName = QStringLiteral("GR");
  req.topName = QStringLiteral("TOP");
  req.botName = QStringLiteral("BOT");
  req.nLayers = layers;
  req.top = constantGrid(cols, rows, 1000.0f);
  req.bot = constantGrid(cols, rows, 1400.0f);
  for (int i = 0; i < wells; ++i)
  {
    const double x = req.top.dx * (0.5 + (i % cols));
    const double y = req.top.originY + req.top.dy * 0.5;
    req.wells.push_back(vertical(QStringLiteral("W%1").arg(i), x, y, 1000.0, 1400.0,
                                 20.0 + i));
  }
  return req;
}

// 纳秒。毫秒计时会把亚毫秒的大网格记成 0，比率门就被跳过。
qint64 runOnceNs(PropertyModelWorkflow &wf, const PropertyModelRequest &req, bool *monotonic)
{
  std::vector<double> seen;
  QElapsedTimer timer;
  timer.start();
  const PropertyModelOutput out = wf.run(req, [&](double fraction, const QString &) {
    if (!seen.empty() && fraction + 1e-9 < seen.back())
      *monotonic = false;
    seen.push_back(fraction);
    return true;
  });
  const qint64 ns = timer.nsecsElapsed();
  if (!out.ok || seen.empty() || seen.back() != 1.0)
    *monotonic = false;
  return out.ok ? ns : -1;
}

long rssKiB()
{
  std::ifstream in("/proc/self/status");
  std::string line;
  while (std::getline(in, line))
  {
    if (line.rfind("VmRSS:", 0) != 0)
      continue;
    long kb = -1;
    if (std::sscanf(line.c_str(), "VmRSS: %ld", &kb) == 1)
      return kb;
  }
  return -1;
}

SurfaceGrid subsample(const SurfaceGrid &src, int cols, int rows)
{
  SurfaceGrid g;
  g.cols = cols;
  g.rows = rows;
  g.originX = src.originX;
  g.originY = src.originY;
  g.dx = src.dx * static_cast<double>(src.cols) / cols;
  g.dy = src.dy * static_cast<double>(src.rows) / rows;
  g.z.resize(static_cast<std::size_t>(cols * rows));
  for (int j = 0; j < rows; ++j)
  {
    const int sj = std::min(src.rows - 1, j * src.rows / rows);
    for (int i = 0; i < cols; ++i)
    {
      const int si = std::min(src.cols - 1, i * src.cols / cols);
      g.z[static_cast<std::size_t>(j * cols + i)] =
          src.z[static_cast<std::size_t>(sj * src.cols + si)];
    }
  }
  return g;
}

QString findNamed(const QString &root, const QString &name)
{
  QString best;
  qint64 bestSize = -1;
  QDirIterator it(root, QStringList{name}, QDir::Files, QDirIterator::Subdirectories);
  while (it.hasNext())
  {
    const QString path = it.next();
    const qint64 sz = QFileInfo(path).size();
    if (sz > bestSize)
    {
      bestSize = sz;
      best = path;
    }
  }
  return best;
}

} // namespace

class TestPropModelPerf : public QObject
{
  Q_OBJECT
private slots:
  void cellCountRatio();
  void realAreaChain();
};

void TestPropModelPerf::cellCountRatio()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(tmp.path()));
  PropertyModelWorkflow wf(&cat, tmp.path());
  bool monotonic = true;
  const PropertyModelRequest small = syntheticRequest(40, 40, 8, 8);
  const PropertyModelRequest large = syntheticRequest(80, 80, 8, 8);
  constexpr qint64 kMinLargeNs = 50'000'000LL;
  constexpr int kMinIters = 3;
  constexpr int kMaxIters = 400;
  qint64 totalSmallNs = 0;
  qint64 totalLargeNs = 0;
  int iters = 0;
  while (iters < kMaxIters && (iters < kMinIters || totalLargeNs < kMinLargeNs))
  {
    const qint64 smallNs = runOnceNs(wf, small, &monotonic);
    const qint64 largeNs = runOnceNs(wf, large, &monotonic);
    QVERIFY2(smallNs >= 0 && largeNs >= 0, "synthetic chain failed");
    totalSmallNs += smallNs;
    totalLargeNs += largeNs;
    ++iters;
  }
  QVERIFY(monotonic);
  QVERIFY2(totalLargeNs >= kMinLargeNs,
           qPrintable(QStringLiteral("large cumulative %1 ns after %2 iters")
                          .arg(totalLargeNs)
                          .arg(iters)));
  // 4× 柱数（层数相同）。IDW 对单元线性；给调度噪声留到 8×。
  QVERIFY2(totalSmallNs > 0 && totalLargeNs < totalSmallNs * 8,
           qPrintable(QStringLiteral("ratio %1/%2 iters=%3")
                          .arg(totalLargeNs)
                          .arg(totalSmallNs)
                          .arg(iters)));
  qInfo().noquote() << QStringLiteral("BASELINE property-model ratio small=%1ms large=%2ms iters=%3")
                           .arg(totalSmallNs / 1.0e6, 0, 'f', 2)
                           .arg(totalLargeNs / 1.0e6, 0, 'f', 2)
                           .arg(iters);
}

void TestPropModelPerf::realAreaChain()
{
  const QString area = qEnvironmentVariable("PALEO_REAL_PROJECT_AREA");
  if (area.isEmpty())
    QSKIP("PALEO_REAL_PROJECT_AREA not set — real-area property model skipped");
  const QString topPath = findNamed(area, QStringLiteral("D63.tif"));
  const QString botPath = findNamed(area, QStringLiteral("D72.tif"));
  if (topPath.isEmpty() || botPath.isEmpty())
    QSKIP("D63/D72 horizon rasters not found under the real area");

  QString err;
  SurfaceGrid topFull;
  SurfaceGrid botFull;
  QVERIFY2(PropertyModelWorkflow::loadSurface(topPath, &topFull, &err), qPrintable(err));
  QVERIFY2(PropertyModelWorkflow::loadSurface(botPath, &botFull, &err), qPrintable(err));
  constexpr int kCols = 200;
  constexpr int kRows = 200;
  constexpr int kLayers = 20;
  PropertyModelRequest req;
  req.useEmbeddedSurfaces = true;
  req.topPath = topPath;
  req.botPath = botPath;
  req.propertyName = QStringLiteral("GR");
  req.topName = QStringLiteral("D63");
  req.botName = QStringLiteral("D72");
  req.nLayers = kLayers;
  req.aggregator = Aggregator::ThicknessWeightedMean;
  req.top = subsample(topFull, kCols, kRows);
  req.bot = subsample(botFull, kCols, kRows);

  double zLo = std::numeric_limits<double>::infinity();
  double zHi = -zLo;
  for (std::size_t i = 0; i < req.top.z.size(); ++i)
  {
    if (!std::isfinite(req.top.z[i]) || !std::isfinite(req.bot.z[i]))
      continue;
    zLo = std::min(zLo, static_cast<double>(req.top.z[i]));
    zHi = std::max(zHi, static_cast<double>(req.bot.z[i]));
  }
  QVERIFY(zHi > zLo);

  const QString headPath = QDir(area).filePath(QStringLiteral("井位/ExportWellHead.dat"));
  QFile headFile(headPath);
  QVERIFY2(headFile.open(QIODevice::ReadOnly), qPrintable(headPath));
  const QVector<WellHeadRecord> heads = parseWellHeadText(headFile.readAll());
  QVERIFY(!heads.isEmpty());
  const QDir lasDir(QDir(area).filePath(QStringLiteral("井曲线")));
  const QStringList lasNames = lasDir.entryList({QStringLiteral("*.Las"), QStringLiteral("*.las")},
                                                 QDir::Files);
  QVERIFY(!lasNames.isEmpty());
  int usedWells = 0;
  for (const QString &name : lasNames)
  {
    const QString path = lasDir.filePath(name);
    QString wellName;
    if (!LasParser::readWellInfo(path, wellName, &err) || wellName.isEmpty())
      wellName = QFileInfo(path).completeBaseName();
    const WellHeadRecord *head = nullptr;
    for (const WellHeadRecord &h : heads)
      if (h.name.compare(wellName, Qt::CaseInsensitive) == 0)
        head = &h;
    if (!head)
      continue;
    QStringList curveNames;
    QList<LasCurve> curves;
    if (!LasParser::parseDepthRange(path, zLo - 20.0, zHi + 20.0, curveNames, curves, &err))
      continue;
    int gr = -1;
    for (int i = 0; i < curveNames.size(); ++i)
      if (curveNames.at(i) == QLatin1String("GR"))
        gr = i;
    if (gr < 0 || curves.isEmpty())
      continue;
    WellCurve well;
    well.wellId = wellName;
    well.curveName = QStringLiteral("GR");
    const double md0 = curves.at(0).values.first();
    const double md1 = curves.at(0).values.last();
    if (!(md1 > md0))
      continue;
    well.stations.push_back(WellStation{md0, head->x, head->y, md0});
    well.stations.push_back(WellStation{md1, head->x, head->y, md1});
    for (int i = 0; i < curves.at(0).values.size(); ++i)
      well.curve.push_back(CurvePoint{curves.at(0).values.at(i), curves.at(gr).values.at(i)});
    req.wells.push_back(std::move(well));
    ++usedWells;
  }
  QVERIFY2(usedWells > 0, "no well overlapped the zone depth window");

  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(tmp.path()));
  PropertyModelWorkflow wf(&cat, tmp.path());
  const long rss0 = rssKiB();
  long peak = rss0;
  bool monotonic = true;
  QElapsedTimer timer;
  timer.start();
  const PropertyModelOutput out = wf.run(req, [&](double fraction, const QString &) {
    static double prev = -1;
    if (fraction + 1e-9 < prev)
      monotonic = false;
    prev = fraction;
    const long now = rssKiB();
    if (now > peak)
      peak = now;
    return true;
  });
  const qint64 ms = timer.elapsed();
  const long rss1 = rssKiB();
  QVERIFY2(out.ok, qPrintable(out.error));
  QVERIFY(monotonic);
  QVERIFY(out.filledCells > 0);
  if (out.liveColumns > 0)
    QVERIFY2(static_cast<long long>(out.filledCells) * 2 > out.liveColumns,
             qPrintable(QStringLiteral("filled=%1 live=%2")
                            .arg(out.filledCells)
                            .arg(out.liveColumns)));
  QVERIFY2(ms < 30000, qPrintable(QStringLiteral("chain %1 ms").arg(ms)));
  QVERIFY2(rss0 >= 0 && rss1 >= 0, "VmRSS unreadable");
  if (peak < rss0)
    peak = rss0;
  const long delta = std::max(rss1, peak) - rss0;
  QVERIFY2(delta >= 0, qPrintable(QStringLiteral("rss delta %1 KiB").arg(delta)));
  QVERIFY2(delta < 2L * 1024 * 1024,
           qPrintable(QStringLiteral("rss delta %1 KiB").arg(delta)));
  qInfo().noquote() << QStringLiteral(
                           "BASELINE property-model real cells=%1 wells=%2 ms=%3 rss0=%4 rss1=%5 "
                           "filled=%6 live=%7 peak=%8")
                           .arg(kCols * kRows * kLayers)
                           .arg(usedWells)
                           .arg(ms)
                           .arg(rss0)
                           .arg(rss1)
                           .arg(out.filledCells)
                           .arg(out.liveColumns)
                           .arg(peak);
}

int main(int argc, char *argv[])
{
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true);
  app.initQgis();
  GDALAllRegister();
  TestPropModelPerf tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_propmodelperf.moc"
