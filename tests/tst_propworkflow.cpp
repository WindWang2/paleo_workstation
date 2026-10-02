// 层：测试壳
#include <QtTest>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <limits>

#include <gdal.h>

#include <qgsapplication.h>

#include "../src/catalog/datacatalog.h"
#include "../src/domain/faultset.h"
#include "../src/io/horizonbinner.h"
#include "../src/workflow/propertymodelworkflow.h"

#include <cmath>

using namespace paleo::stratgrid;

namespace
{

bool writeFlat(const QString &path, int n, float z, float hole = std::numeric_limits<float>::quiet_NaN())
{
  BinnedHorizon b;
  b.rows = n;
  b.cols = n;
  b.dx = 10;
  b.dy = 10;
  b.originX = 0;
  b.originY = n * 10.0;
  b.z = QVector<float>(n * n, z);
  b.filledCells = n * n;
  if (std::isfinite(hole))
  {
    b.z[0] = -9999.0f;
    b.filledCells -= 1;
  }
  QString err;
  return writeHorizonGeoTiff(b, path, &err);
}

WellCurve wellAt(double x, double y, double z0, double z1, double value)
{
  WellCurve w;
  w.wellId = QStringLiteral("W1");
  w.curveName = QStringLiteral("GR");
  w.stations.push_back(WellStation{z0, x, y, z0});
  w.stations.push_back(WellStation{z1, x, y, z1});
  w.curve.push_back(CurvePoint{z0, value});
  w.curve.push_back(CurvePoint{z1, value});
  return w;
}

bool addExternal(DataCatalog *cat, const QString &id, const QString &path, QString *error)
{
  CatalogAsset asset;
  asset.id = id;
  asset.type = QStringLiteral("horizon");
  asset.format = QStringLiteral("tif");
  asset.displayName = id;
  if (!cat->addAsset(asset, error))
    return false;
  CatalogVersion ver;
  ver.id = id + QStringLiteral("-v");
  ver.assetId = id;
  ver.stage = QStringLiteral("RAW");
  ver.versionNumber = 1;
  ver.managed = false;
  ver.path = path;
  ver.fileName = QFileInfo(path).fileName();
  return cat->addVersion(ver, error);
}

} // namespace

class TestPropWorkflow : public QObject
{
  Q_OBJECT
private slots:
  void wktSegmentsAndFaultSet();
  void singularSurfaceDoesNotRegister();
  void chainRegistersDerivedAndIsReproducible();
  void cancelAndProgressAreHonest();
};

void TestPropWorkflow::wktSegmentsAndFaultSet()
{
  const auto line = PropertyModelWorkflow::segmentsFromWkt(
      QStringLiteral("LINESTRING (0 0, 5 0, 5 4)"));
  QCOMPARE(static_cast<int>(line.size()), 2);
  QCOMPARE(line[0].x1, 5.0);
  QCOMPARE(line[1].y1, 4.0);

  const auto ring = PropertyModelWorkflow::segmentsFromWkt(
      QStringLiteral("POLYGON ((0 0, 2 0, 2 2, 0 0))"));
  QCOMPARE(static_cast<int>(ring.size()), 3);

  paleo::fault::FaultSet set;
  const QString id = set.addFault(QStringLiteral("F1"));
  paleo::fault::FaultHorizonCut cut;
  cut.horizon = QStringLiteral("H1");
  cut.wkt = QStringLiteral("LINESTRING (0 0, 10 0)");
  QVERIFY(set.setCut(id, cut));
  const auto fromSet = PropertyModelWorkflow::segmentsFromFaultSet(set);
  QCOMPARE(static_cast<int>(fromSet.size()), 1);
  QCOMPARE(fromSet[0].x1, 10.0);

  QVERIFY(PropertyModelWorkflow::segmentsFromWkt(QStringLiteral("not a geometry")).empty());
}

void TestPropWorkflow::singularSurfaceDoesNotRegister()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(tmp.path()));
  PropertyModelWorkflow wf(&cat, tmp.path());
  PropertyModelRequest req;
  req.useEmbeddedSurfaces = true;
  req.top.cols = req.bot.cols = 3;
  req.top.rows = req.bot.rows = 3;
  req.top.dx = req.bot.dx = 10;
  req.top.dy = req.bot.dy = 10;
  req.top.z.assign(9, 5.0f);
  req.bot.z.assign(9, 5.0f);
  req.nLayers = 4;
  req.propertyName = QStringLiteral("PHIT");
  const PropertyModelOutput out = wf.run(req);
  QVERIFY(!out.ok);
  QVERIFY(out.error.contains(QStringLiteral("奇异面")));
  QVERIFY(out.path.isEmpty());
  for (const CatalogAsset &asset : cat.assets())
    QVERIFY(asset.type != QLatin1String("property_volume"));
}

void TestPropWorkflow::chainRegistersDerivedAndIsReproducible()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString topPath = tmp.filePath(QStringLiteral("top.tif"));
  const QString botPath = tmp.filePath(QStringLiteral("bot.tif"));
  QVERIFY(writeFlat(topPath, 4, 0.0f, 0.0f));
  QVERIFY(writeFlat(botPath, 4, 40.0f, 0.0f));

  DataCatalog cat;
  QVERIFY(cat.open(tmp.path()));
  QString catErr;
  QVERIFY2(addExternal(&cat, QStringLiteral("ast-top"), topPath, &catErr), qPrintable(catErr));
  QVERIFY2(addExternal(&cat, QStringLiteral("ast-bot"), botPath, &catErr), qPrintable(catErr));

  PropertyModelRequest req;
  req.propertyName = QStringLiteral("GR");
  req.topName = QStringLiteral("C3");
  req.botName = QStringLiteral("D72");
  req.topPath = topPath;
  req.botPath = botPath;
  req.nLayers = 4;
  req.aggregator = Aggregator::ThicknessWeightedMean;
  req.idwPower = 2.0;
  // 北向上：originY = 40，dy = -10，柱 (1,1) 中心 x=15，y=40-15=25。
  req.wells.push_back(wellAt(15.0, 25.0, 0.0, 40.0, 6.0));
  req.faults.push_back(FaultSegment{1000.0, 1000.0, 1001.0, 1000.0}); // 不切网格

  PropertyModelWorkflow wf(&cat, tmp.path());
  QSignalSpy stored(&wf, &PropertyModelWorkflow::modelStored);
  const PropertyModelOutput first = wf.run(req);
  QVERIFY2(first.ok, qPrintable(first.error));
  QCOMPARE(stored.count(), 1);
  QVERIFY(QFile::exists(first.path));
  QCOMPARE(first.liveColumns, 15); // 4*4 - 1 个 nodata 柱
  QCOMPARE(first.filledCells, 15 * 4);
  QCOMPARE(first.unfilledLiveCells, 0);

  const CatalogVersion ver = cat.versionById(first.versionId);
  QCOMPARE(ver.stage, QStringLiteral("DERIVED"));
  QCOMPARE(ver.extra.value(QStringLiteral("param_hash")).toString(), first.paramHash);
  QCOMPARE(ver.extra.value(QStringLiteral("aggregator")).toString(),
           QStringLiteral("thickness-weighted-mean"));
  QVERIFY(ver.parentVersionIds.contains(QStringLiteral("ast-top-v")));
  QVERIFY(ver.parentVersionIds.contains(QStringLiteral("ast-bot-v")));

  PropertyVolume back;
  QJsonObject prov;
  QString err;
  QFile blobFile(first.path);
  QVERIFY(blobFile.open(QIODevice::ReadOnly));
  QVERIFY2(readPropertyBlob(blobFile.readAll(), &back, &prov, &err), qPrintable(err));
  QCOMPARE(prov.value(QStringLiteral("param_hash")).toString(), first.paramHash);
  const int cell = back.grid.cellIndex(1, 1, 0);
  QCOMPARE(back.values[static_cast<std::size_t>(cell)], 6.0f);
  QVERIFY(std::isnan(back.values[static_cast<std::size_t>(back.grid.cellIndex(0, 0, 0))]));

  const PropertyModelOutput second = wf.run(req);
  QVERIFY2(second.ok, qPrintable(second.error));
  QCOMPARE(second.paramHash, first.paramHash);
  QCOMPARE(second.assetId, first.assetId);
  QVERIFY(second.versionId != first.versionId);
  QFile a(first.path);
  QFile b(second.path);
  QVERIFY(a.open(QIODevice::ReadOnly));
  QVERIFY(b.open(QIODevice::ReadOnly));
  QCOMPARE(a.readAll(), b.readAll());

  SurfaceGrid loaded;
  QVERIFY(PropertyModelWorkflow::loadSurface(topPath, &loaded, &err));
  QCOMPARE(loaded.cols, 4);
  QVERIFY(std::isnan(loaded.z[0]));
  QCOMPARE(loaded.dy, -10.0);
}

void TestPropWorkflow::cancelAndProgressAreHonest()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(tmp.path()));
  PropertyModelWorkflow wf(&cat, tmp.path());

  PropertyModelRequest req;
  req.useEmbeddedSurfaces = true;
  req.propertyName = QStringLiteral("SW");
  req.topName = QStringLiteral("T");
  req.botName = QStringLiteral("B");
  req.nLayers = 4;
  req.top.cols = req.bot.cols = 24;
  req.top.rows = req.bot.rows = 16;
  req.top.dx = req.bot.dx = 10;
  req.top.dy = req.bot.dy = 10;
  req.top.z.assign(24 * 16, 0.0f);
  req.bot.z.assign(24 * 16, 20.0f);
  req.wells.push_back(wellAt(5.0, 5.0, 0.0, 20.0, 1.0));

  std::vector<double> seen;
  QStringList stages;
  bool monotonic = true;
  const PropertyModelOutput cancelled = wf.run(req, [&](double fraction, const QString &stage) {
    if (!seen.empty() && fraction + 1e-12 < seen.back())
      monotonic = false;
    seen.push_back(fraction);
    stages.append(stage);
    return fraction < 0.45;
  });
  QVERIFY(monotonic);
  QVERIFY(!cancelled.ok);
  QVERIFY(cancelled.error.contains(QStringLiteral("已取消")));
  QVERIFY(cancelled.path.isEmpty());
  for (const CatalogAsset &asset : cat.assets())
    QVERIFY(asset.type != QLatin1String("property_volume"));
  QVERIFY(!seen.empty());
  QVERIFY(seen.back() + 1e-9 >= 0.45);

  seen.clear();
  stages.clear();
  monotonic = true;
  const PropertyModelOutput ok = wf.run(req, [&](double fraction, const QString &stage) {
    if (!seen.empty() && fraction + 1e-9 < seen.back())
      monotonic = false;
    seen.push_back(fraction);
    stages.append(stage);
    return true;
  });
  QVERIFY(monotonic);
  QVERIFY2(ok.ok, qPrintable(ok.error));
  QCOMPARE(seen.back(), 1.0);
  QVERIFY(stages.contains(QStringLiteral("建格架")));
  QVERIFY(stages.contains(QStringLiteral("充填")));
  QVERIFY(stages.contains(QStringLiteral("完成")));
  for (std::size_t i = 1; i < seen.size(); ++i)
    QVERIFY(seen[i] + 1e-12 >= seen[i - 1]);
}

int main(int argc, char *argv[])
{
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true);
  app.initQgis();
  GDALAllRegister();
  TestPropWorkflow tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_propworkflow.moc"
