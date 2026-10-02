// 层：测试壳
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
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

WellCurve wellNamed(const QString &id, const QString &curve, double x, double y, double z0, double z1,
                    double value)
{
  WellCurve w = wellAt(x, y, z0, z1, value);
  w.wellId = id;
  w.curveName = curve;
  return w;
}

bool addWellLog(DataCatalog *cat, const QString &wellId, const QString &assetId, const QString &path,
                bool hasSurface, double x, double y, QString *error)
{
  CatalogEntity ent;
  ent.id = wellId;
  ent.entityType = QStringLiteral("well");
  ent.name = wellId;
  ent.hasSurface = hasSurface;
  ent.surfaceX = x;
  ent.surfaceY = y;
  if (!cat->addEntity(ent, error))
    return false;
  CatalogAsset asset;
  asset.id = assetId;
  asset.type = QStringLiteral("well_log");
  asset.format = QStringLiteral("las");
  asset.displayName = QFileInfo(path).fileName();
  if (!cat->addAsset(asset, error))
    return false;
  CatalogVersion ver;
  ver.id = assetId + QStringLiteral("-v");
  ver.assetId = assetId;
  ver.stage = QStringLiteral("RAW");
  ver.versionNumber = 1;
  ver.managed = false;
  ver.path = path;
  ver.fileName = QFileInfo(path).fileName();
  if (!cat->addVersion(ver, error))
    return false;
  EntityAssetLink link;
  link.entityType = QStringLiteral("well");
  link.entityId = wellId;
  link.assetId = assetId;
  link.role = QStringLiteral("well_log");
  link.isPrimary = true;
  return cat->addLink(link, error);
}

bool writeLas(const QString &path, const QString &curve, const QString &rows)
{
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return false;
  const QByteArray body =
      QStringLiteral("~Version Information\n"
                     " VERS. 2.0 :\n"
                     " WRAP. NO :\n"
                     "~Well\n"
                     " STRT.M 0 :\n"
                     " STOP.M 40 :\n"
                     " STEP.M 10 :\n"
                     " NULL. -999.25 :\n"
                     " WELL. W :\n"
                     "~Curve\n"
                     " DEPT.M : Depth\n"
                     " %1. : Curve\n"
                     "~A\n"
                     "%2")
          .arg(curve, rows)
          .toUtf8();
  if (f.write(body) != body.size() || !f.flush())
    return false;
  f.close();
  return f.error() == QFileDevice::NoError;
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
  void reorderWellsKeepsHashAndBytes();
  void faultWallSeparatesWells();
  void embeddedSurfacesDoNotClaimParents();
  void relativeSurfacePathsResolveForParents();
  void rasterWithoutGeoreferenceIsRejected();
  void requestFromCatalogBuildsWellsAndHorizons();
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

  const auto hole = PropertyModelWorkflow::segmentsFromWkt(
      QStringLiteral("POLYGON ((0 0, 3 0, 3 3, 0 0), (1 1, 2 1, 2 2, 1 1))"));
  QCOMPARE(static_cast<int>(hole.size()), 3);
  QCOMPARE(hole[0].x0, 0.0);
  QCOMPARE(hole[0].x1, 3.0);
  QCOMPARE(hole[2].x1, 0.0);
  QCOMPARE(hole[2].y1, 0.0);
  for (const FaultSegment &seg : hole)
    QVERIFY(!(seg.x0 == 1.0 && seg.y0 == 1.0));

  const auto zline = PropertyModelWorkflow::segmentsFromWkt(
      QStringLiteral("LINESTRING Z (0 0 9, 4 0 9)"));
  QCOMPARE(static_cast<int>(zline.size()), 1);
  QCOMPARE(zline[0].x0, 0.0);
  QCOMPARE(zline[0].y0, 0.0);
  QCOMPARE(zline[0].x1, 4.0);
  QCOMPARE(zline[0].y1, 0.0);

  const auto zpoly = PropertyModelWorkflow::segmentsFromWkt(
      QStringLiteral("POLYGON Z ((0 0 9, 2 0 8, 2 2 7, 0 0 9))"));
  QCOMPARE(static_cast<int>(zpoly.size()), 3);
  QCOMPARE(zpoly[0].x1, 2.0);
  QCOMPARE(zpoly[0].y1, 0.0);
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

  // 1.0 是 commit 之后的完成通知：返回 false 也不撤销已登记版本。
  const PropertyModelOutput noticed = wf.run(req, [](double fraction, const QString &) {
    return fraction < 1.0;
  });
  QVERIFY2(noticed.ok, qPrintable(noticed.error));
  QVERIFY(!noticed.versionId.isEmpty());
  QVERIFY(QFile::exists(noticed.path));
  QCOMPARE(cat.versionById(noticed.versionId).stage, QStringLiteral("DERIVED"));
}

void TestPropWorkflow::reorderWellsKeepsHashAndBytes()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(tmp.path()));
  PropertyModelWorkflow wf(&cat, tmp.path());

  PropertyModelRequest base;
  base.useEmbeddedSurfaces = true;
  base.propertyName = QStringLiteral("PROP");
  base.topName = QStringLiteral("T");
  base.botName = QStringLiteral("B");
  base.nLayers = 1;
  base.top.cols = base.bot.cols = 4;
  base.top.rows = base.bot.rows = 4;
  base.top.dx = base.bot.dx = 10;
  base.top.dy = base.bot.dy = 10;
  base.top.z.assign(16, 0.0f);
  base.bot.z.assign(16, 40.0f);

  PropertyModelRequest first = base;
  first.wells.push_back(wellNamed(QStringLiteral("W-B"), QStringLiteral("PHIT"), 15.0, 5.0, 0.0, 40.0, 8.0));
  first.wells.push_back(wellNamed(QStringLiteral("W-A"), QStringLiteral("GR"), 5.0, 5.0, 0.0, 40.0, 2.0));
  PropertyModelRequest second = base;
  second.wells.push_back(wellNamed(QStringLiteral("W-A"), QStringLiteral("GR"), 5.0, 5.0, 0.0, 40.0, 2.0));
  second.wells.push_back(wellNamed(QStringLiteral("W-B"), QStringLiteral("PHIT"), 15.0, 5.0, 0.0, 40.0, 8.0));

  const QString hashA = PropertyModelWorkflow::paramHash(first, first.top, first.bot);
  const QString hashB = PropertyModelWorkflow::paramHash(second, second.top, second.bot);
  QCOMPARE(hashB, hashA);

  const PropertyModelOutput outA = wf.run(first);
  const PropertyModelOutput outB = wf.run(second);
  QVERIFY2(outA.ok, qPrintable(outA.error));
  QVERIFY2(outB.ok, qPrintable(outB.error));
  QCOMPARE(outA.paramHash, hashA);
  QCOMPARE(outB.paramHash, hashA);
  QCOMPARE(cat.versionById(outA.versionId).extra.value(QStringLiteral("curves")).toString(),
           QStringLiteral("GR,PHIT"));
  QCOMPARE(cat.versionById(outB.versionId).extra.value(QStringLiteral("curves")).toString(),
           QStringLiteral("GR,PHIT"));
  QVERIFY(cat.versionById(outA.versionId).parentVersionIds.isEmpty());
  QFile fileA(outA.path);
  QFile fileB(outB.path);
  QVERIFY(fileA.open(QIODevice::ReadOnly));
  QVERIFY(fileB.open(QIODevice::ReadOnly));
  const QByteArray bytesA = fileA.readAll();
  QCOMPARE(bytesA, fileB.readAll());
  PropertyVolume back;
  QJsonObject prov;
  QString err;
  QVERIFY2(readPropertyBlob(bytesA, &back, &prov, &err), qPrintable(err));
  QCOMPARE(prov.value(QStringLiteral("curve")).toString(), QStringLiteral("GR,PHIT"));
}

void TestPropWorkflow::faultWallSeparatesWells()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(tmp.path()));
  PropertyModelWorkflow wf(&cat, tmp.path());

  PropertyModelRequest req;
  req.useEmbeddedSurfaces = true;
  req.propertyName = QStringLiteral("GR");
  req.topName = QStringLiteral("T");
  req.botName = QStringLiteral("B");
  req.nLayers = 1;
  req.top.cols = req.bot.cols = 4;
  req.top.rows = req.bot.rows = 2;
  req.top.dx = req.bot.dx = 10;
  req.top.dy = req.bot.dy = 10;
  req.top.z.assign(8, 0.0f);
  req.bot.z.assign(8, 20.0f);
  // 柱心 x = (i+0.5)*10。x=20 的竖墙隔开 i<=1 与 i>=2。
  req.faults.push_back(FaultSegment{20.0, -1.0, 20.0, 30.0});
  req.wells.push_back(wellNamed(QStringLiteral("L"), QStringLiteral("GR"), 5.0, 5.0, 0.0, 20.0, 2.0));
  req.wells.push_back(wellNamed(QStringLiteral("R"), QStringLiteral("GR"), 35.0, 5.0, 0.0, 20.0, 8.0));

  const PropertyModelOutput out = wf.run(req);
  QVERIFY2(out.ok, qPrintable(out.error));
  QCOMPARE(out.volume.blockCount, 2);
  QCOMPARE(out.volume.grid.ni, 4);
  QCOMPARE(out.volume.grid.nj, 2);
  QCOMPARE(out.volume.grid.nk, 1);
  for (int j = 0; j < 2; ++j)
  {
    for (int i = 0; i < 4; ++i)
    {
      const float v = out.volume.values[static_cast<std::size_t>(out.volume.grid.cellIndex(i, j, 0))];
      QCOMPARE(v, i < 2 ? 2.0f : 8.0f);
    }
  }

  QString err;
  const PropertyGridSlice slice = PropertyModelWorkflow::gridSlice(out.volume, 2, 0, &err);
  QCOMPARE(slice.width, 4);
  QCOMPARE(slice.height, 2);
  QCOMPARE(slice.values[0], 2.0f);
  QCOMPARE(slice.values[3], 8.0f);
  QVERIFY(slice.valueMin == 2.0f);
  QVERIFY(slice.valueMax == 8.0f);
  const PropertyGridSlice bad = PropertyModelWorkflow::gridSlice(out.volume, 9, 0, &err);
  QCOMPARE(bad.width, 0);
  QVERIFY(bad.values.empty());
  QVERIFY(err.contains(QStringLiteral("切片轴")));
}

void TestPropWorkflow::embeddedSurfacesDoNotClaimParents()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString topPath = tmp.filePath(QStringLiteral("top.tif"));
  const QString botPath = tmp.filePath(QStringLiteral("bot.tif"));
  QVERIFY(writeFlat(topPath, 4, 0.0f));
  QVERIFY(writeFlat(botPath, 4, 40.0f));
  DataCatalog cat;
  QVERIFY(cat.open(tmp.path()));
  QString catErr;
  QVERIFY2(addExternal(&cat, QStringLiteral("ast-top"), topPath, &catErr), qPrintable(catErr));
  QVERIFY2(addExternal(&cat, QStringLiteral("ast-bot"), botPath, &catErr), qPrintable(catErr));

  PropertyModelWorkflow wf(&cat, tmp.path());
  PropertyModelRequest req;
  req.useEmbeddedSurfaces = true;
  req.topPath = topPath;
  req.botPath = botPath;
  req.propertyName = QStringLiteral("GR");
  req.topName = QStringLiteral("T");
  req.botName = QStringLiteral("B");
  req.nLayers = 1;
  req.top.cols = req.bot.cols = 4;
  req.top.rows = req.bot.rows = 4;
  req.top.dx = req.bot.dx = 10;
  req.top.dy = req.bot.dy = 10;
  req.top.z.assign(16, 0.0f);
  req.bot.z.assign(16, 40.0f);
  req.wells.push_back(wellAt(5.0, 5.0, 0.0, 40.0, 1.0));
  const PropertyModelOutput out = wf.run(req);
  QVERIFY2(out.ok, qPrintable(out.error));
  QVERIFY(cat.versionById(out.versionId).parentVersionIds.isEmpty());
}

void TestPropWorkflow::relativeSurfacePathsResolveForParents()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  QVERIFY(QDir(tmp.path()).mkpath(QStringLiteral("horizons")));
  const QString topPath = tmp.filePath(QStringLiteral("horizons/top.tif"));
  const QString botPath = tmp.filePath(QStringLiteral("horizons/bot.tif"));
  QVERIFY(writeFlat(topPath, 4, 0.0f));
  QVERIFY(writeFlat(botPath, 4, 40.0f));
  DataCatalog cat;
  QVERIFY(cat.open(tmp.path()));
  QString catErr;
  QVERIFY2(addExternal(&cat, QStringLiteral("ast-top"), topPath, &catErr), qPrintable(catErr));
  QVERIFY2(addExternal(&cat, QStringLiteral("ast-bot"), botPath, &catErr), qPrintable(catErr));

  PropertyModelRequest req;
  req.propertyName = QStringLiteral("GR");
  req.topName = QStringLiteral("T");
  req.botName = QStringLiteral("B");
  req.topPath = QStringLiteral("horizons/top.tif");
  req.botPath = QStringLiteral("horizons/bot.tif");
  req.nLayers = 1;
  req.wells.push_back(wellAt(15.0, 25.0, 0.0, 40.0, 4.0));
  PropertyModelWorkflow wf(&cat, tmp.path());
  const PropertyModelOutput out = wf.run(req);
  QVERIFY2(out.ok, qPrintable(out.error));
  const QStringList parents = cat.versionById(out.versionId).parentVersionIds;
  QVERIFY(parents.contains(QStringLiteral("ast-top-v")));
  QVERIFY(parents.contains(QStringLiteral("ast-bot-v")));
}

void TestPropWorkflow::rasterWithoutGeoreferenceIsRejected()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString path = tmp.filePath(QStringLiteral("bare.tif"));
  GDALDriverH drv = GDALGetDriverByName("GTiff");
  QVERIFY(drv);
  GDALDatasetH ds = GDALCreate(drv, path.toUtf8().constData(), 2, 2, 1, GDT_Float32, nullptr);
  QVERIFY(ds);
  float px[4] = {1.f, 1.f, 1.f, 1.f};
  QCOMPARE(GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Write, 0, 0, 2, 2, px, 2, 2, GDT_Float32, 0, 0),
           CE_None);
  GDALClose(ds);

  SurfaceGrid grid;
  QString err;
  QVERIFY(!PropertyModelWorkflow::loadSurface(path, &grid, &err));
  QVERIFY2(err.contains(QStringLiteral("no georeference")), qPrintable(err));
  QCOMPARE(grid.cols, 0);
  QCOMPARE(grid.rows, 0);
}

void TestPropWorkflow::requestFromCatalogBuildsWellsAndHorizons()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString c3Path = tmp.filePath(QStringLiteral("C3.tif"));
  const QString d72Path = tmp.filePath(QStringLiteral("D72.tif"));
  const QString decoyPath = tmp.filePath(QStringLiteral("C30.tif"));
  const QString lowerPath = tmp.filePath(QStringLiteral("c3.tif"));
  QVERIFY(writeFlat(c3Path, 4, 0.0f));
  QVERIFY(writeFlat(d72Path, 4, 40.0f));
  QVERIFY(writeFlat(decoyPath, 4, 5.0f));
  QVERIFY(writeFlat(lowerPath, 4, 9.0f));
  const QString lasA = tmp.filePath(QStringLiteral("A.las"));
  const QString lasB = tmp.filePath(QStringLiteral("B.las"));
  const QString lasC = tmp.filePath(QStringLiteral("C.las"));
  QVERIFY(writeLas(lasA, QStringLiteral("GR"),
                   QStringLiteral("0.0 6.0\n10.0 6.0\n20.0 -999.25\n40.0 6.0\n")));
  QVERIFY(writeLas(lasB, QStringLiteral("RHOB"), QStringLiteral("0.0 2.3\n40.0 2.3\n")));
  QVERIFY(writeLas(lasC, QStringLiteral("GR"), QStringLiteral("0.0 1.0\n40.0 1.0\n")));

  DataCatalog cat;
  QVERIFY(cat.open(tmp.path()));
  QString catErr;
  auto addHorizon = [&](const QString &id, const QString &display, const QString &fileName,
                        const QString &path, int version) {
    CatalogAsset asset;
    asset.id = id;
    asset.type = QStringLiteral("horizon");
    asset.format = QStringLiteral("tif");
    asset.displayName = display;
    if (cat.assetById(id).id.isEmpty())
    {
      if (!cat.addAsset(asset, &catErr))
        return false;
    }
    CatalogVersion ver;
    ver.id = id + QStringLiteral("-v") + QString::number(version);
    ver.assetId = id;
    ver.stage = QStringLiteral("RAW");
    ver.versionNumber = version;
    ver.managed = false;
    ver.path = path;
    ver.fileName = fileName;
    return cat.addVersion(ver, &catErr);
  };
  // 精确主名（C3.dat / C3.tif，最新版是栅格）压过 C30 包含匹配和 c3 大小写干扰。
  QVERIFY2(addHorizon(QStringLiteral("hz-exact"), QStringLiteral("C3.dat"), QStringLiteral("C3.dat"),
                      tmp.filePath(QStringLiteral("C3.dat")), 1),
           qPrintable(catErr));
  QVERIFY2(addHorizon(QStringLiteral("hz-exact"), QStringLiteral("C3.dat"), QStringLiteral("C3.tif"),
                      c3Path, 2),
           qPrintable(catErr));
  QVERIFY2(addHorizon(QStringLiteral("hz-c30"), QStringLiteral("C30.tif"), QStringLiteral("C30.tif"),
                      decoyPath, 8),
           qPrintable(catErr));
  QVERIFY2(addHorizon(QStringLiteral("hz-lower"), QStringLiteral("c3.tif"), QStringLiteral("c3.tif"),
                      lowerPath, 9),
           qPrintable(catErr));
  QVERIFY2(addHorizon(QStringLiteral("hz-d72"), QStringLiteral("D72.dat"), QStringLiteral("D72.tif"),
                      d72Path, 3),
           qPrintable(catErr));
  QVERIFY2(addWellLog(&cat, QStringLiteral("W-A"), QStringLiteral("las-a"), lasA, true, 15.0, 25.0, &catErr),
           qPrintable(catErr));
  QVERIFY2(addWellLog(&cat, QStringLiteral("W-B"), QStringLiteral("las-b"), lasB, true, 5.0, 5.0, &catErr),
           qPrintable(catErr));
  QVERIFY2(addWellLog(&cat, QStringLiteral("W-C"), QStringLiteral("las-c"), lasC, false, 5.0, 5.0, &catErr),
           qPrintable(catErr));

  PropertyModelWorkflow wf(&cat, tmp.path());
  QString err = QStringLiteral("stale");
  const PropertyModelRequest req = wf.requestFromCatalog(
      QStringLiteral("C3"), QStringLiteral("D72"), QStringLiteral("GR"), 1,
      Aggregator::ThicknessWeightedMean, 2.0, &err);
  QVERIFY2(err.isEmpty(), qPrintable(err));
  QCOMPARE(req.topPath, c3Path);
  QCOMPARE(req.botPath, d72Path);
  QCOMPARE(req.topName, QStringLiteral("C3"));
  QCOMPARE(req.botName, QStringLiteral("D72"));
  QCOMPARE(req.propertyName, QStringLiteral("GR"));
  QCOMPARE(req.nLayers, 1);
  QCOMPARE(req.idwPower, 2.0);
  QVERIFY(!req.useEmbeddedSurfaces);
  QVERIFY(req.faults.empty());
  QCOMPARE(static_cast<int>(req.wells.size()), 1);
  QCOMPARE(req.wells[0].wellId, QStringLiteral("W-A"));
  QCOMPARE(req.wells[0].curveName, QStringLiteral("GR"));
  QCOMPARE(req.wells[0].stations.size(), static_cast<std::size_t>(2));
  QCOMPARE(req.wells[0].stations[0].x, 15.0);
  QCOMPARE(req.wells[0].stations[0].y, 25.0);
  QCOMPARE(req.wells[0].stations[0].z, 0.0);
  QCOMPARE(req.wells[0].stations[0].md, 0.0);
  QCOMPARE(req.wells[0].stations[1].md, 40.0);
  QCOMPARE(req.wells[0].stations[1].z, 40.0);
  QCOMPARE(static_cast<int>(req.wells[0].curve.size()), 4);
  QCOMPARE(req.wells[0].curve[2].md, 20.0);
  QVERIFY(std::isnan(req.wells[0].curve[2].value));

  const PropertyModelOutput out = wf.run(req);
  QVERIFY2(out.ok, qPrintable(out.error));
  const int cell = out.volume.grid.cellIndex(1, 1, 0);
  QCOMPARE(out.volume.values[static_cast<std::size_t>(cell)], 6.0f);

  QString missingErr;
  const PropertyModelRequest missing = wf.requestFromCatalog(
      QStringLiteral("NOPE"), QStringLiteral("D72"), QStringLiteral("GR"), 4,
      Aggregator::Mean, 2.0, &missingErr);
  QVERIFY(missingErr.contains(QStringLiteral("NOPE")));
  QVERIFY(missing.topPath.isEmpty());
  QVERIFY(missing.wells.empty());

  const PropertyGridSlice slice = PropertyModelWorkflow::gridSlice(out.volume, 2, 0, &err);
  QCOMPARE(slice.width, out.volume.grid.ni);
  QCOMPARE(slice.height, out.volume.grid.nj);
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
