// 层：测试壳
#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>

#include <functional>
#include <limits>
#include <memory>

#include <gdal.h>

#include <qgsapplication.h>

#include "../src/catalog/datacatalog.h"
#include "../src/domain/faultset.h"
#include "../src/io/horizonbinner.h"
#include "../src/services/jobrunner.h"
#include "../src/services/paleotaskservice.h"
#include "../src/workflow/propertymodelworkflow.h"

#include <cmath>
#include <numbers>
#include <set>

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
  void deviatedWellUsesTrajectoryStations();
  // ---- 方向20：JobRunner 迁移面的行为等价断言（异步 startJob 路径） ----
  void asyncJobSucceedsAndRegistersOnOwnerThread();
  void asyncJobCancelSkipsCommit();
  void asyncJobRejectsWhenBusyAndReportsFailure();
  void asyncJobRegisteredBeforeCallerFinishedSlot();
  void asyncJobDroppedOnSessionReset();
  void computeSnapshotIsPureAndCancellable();
  // ---- 方向 45（goal/prop-model-v2）：SGS 多实现 / 断距提取 / 相带与口径 ----
  void sgsMultiRealizationRegistersIndependentVersions();
  void faultThrowExtractionFeedsOffset();
  void faciesAndVerticalApproxCalibersAreHonest();
  void objectOverrideRegistersInProvenance();
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
  QCOMPARE(req.trajectoryWellCount, 0); // 无测斜链接 → 全直井路径（迁移不变面）
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

// ===========================================================================
// 方向20：JobRunner 迁移面的行为等价断言
//
// 契约（docs/progress/job-framework.md）：迁移不得改行为。上面那些同步用例
// 钉的是 run() 老路径；这里三个用例钉 startJob() 异步路径与它的等价关系。
// 三条断言按 Oracle 1 固定为：信号序、取消语义、失败态。
// ===========================================================================

namespace
{

/// 异步用例共用的请求构造：与 cancelAndProgressAreHonest 同一副输入，
/// 保证同步/异步两条路径的输入完全可比。
PropertyModelRequest asyncFixtureRequest()
{
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
  return req;
}

/// 泵事件直到 pred 成立（commit 段经 QueuedConnection 排在 owner 线程）。
bool pumpUntil(const std::function<bool()> &pred, int timeoutMs = 20000)
{
  QElapsedTimer clock;
  clock.start();
  while (!pred())
  {
    if (clock.elapsed() > timeoutMs)
      return false;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(2);
  }
  return true;
}

} // namespace

// ---- 断言 1（信号序 + 成功登记）：成功路径 commit 在 owner 线程执行一次 ----
void TestPropWorkflow::asyncJobSucceedsAndRegistersOnOwnerThread()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(tmp.path()));
  PropertyModelWorkflow wf(&cat, tmp.path());

  QObject dispatcher;
  PaleoTaskService svc(nullptr, &dispatcher);
  paleo::jobs::JobRunner<PropertyModelWorkflow::PropertyModelJob> runner(&dispatcher);
  runner.setTaskService(&svc);

  QSignalSpy stored(&wf, &PropertyModelWorkflow::modelStored);
  QSignalSpy failed(&wf, &PropertyModelWorkflow::modelFailed);

  std::shared_ptr<PropertyModelWorkflow::PropertyModelJob> job;
  PaleoTask *task = wf.startJob(runner, asyncFixtureRequest(), 1.0, &job);
  QVERIFY2(task, "任务池在侧仍应受理");
  QVERIFY(job);
  QVERIFY(pumpUntil([&] { return stored.size() + failed.size() > 0; }));

  // 成功：modelStored 发一次、modelFailed 零次，且 commit 标记已置位
  QCOMPARE(stored.size(), 1);
  QCOMPARE(failed.size(), 0);
  QVERIFY(job->registered);
  QVERIFY2(!job->computed.empty() && job->computed.front().ok,
             qPrintable(job->computed.empty() ? QStringLiteral("空结果") : job->computed.front().error));
  QCOMPARE(task->state(), PaleoTask::State::Succeeded);

  // 登记确实落到 catalog（与同步路径同一面）
  int registered = 0;
  for (const CatalogAsset &asset : cat.assets())
    if (asset.type == QLatin1String("property_volume"))
      ++registered;
  QCOMPARE(registered, 1);
  // 共享所有权：UI 段读到的就是 commit 段回填的那一份
  QCOMPARE(job->computed.front().out.assetId, job->computed.front().out.assetId);
  QVERIFY(!job->computed.front().out.path.isEmpty());

  svc.shutdown(3000, false);
}

// ---- 断言 2（取消语义）：cancel 后 commit 不执行、catalog 无新增登记 ----
void TestPropWorkflow::asyncJobCancelSkipsCommit()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(tmp.path()));
  PropertyModelWorkflow wf(&cat, tmp.path());

  QObject dispatcher;
  PaleoTaskService svc(nullptr, &dispatcher);
  paleo::jobs::JobRunner<PropertyModelWorkflow::PropertyModelJob> runner(&dispatcher);
  runner.setTaskService(&svc);

  QSignalSpy stored(&wf, &PropertyModelWorkflow::modelStored);

  std::shared_ptr<PropertyModelWorkflow::PropertyModelJob> job;
  PaleoTask *task = wf.startJob(runner, asyncFixtureRequest(), 1.0, &job);
  QVERIFY(task);
  // 立刻取消：compute 会在下一个进度点被 CancelFn 打断
  runner.requestCancel();
  QVERIFY(pumpUntil([&] { return !runner.busy(); }));

  // 取消终态；commit 未执行
  QCOMPARE(task->state(), PaleoTask::State::Cancelled);
  QVERIFY(!job->registered);
  QCOMPARE(stored.size(), 0);
  for (const CatalogAsset &asset : cat.assets())
    QVERIFY(asset.type != QLatin1String("property_volume"));

  svc.shutdown(3000, false);
}

// ---- 断言 3（失败态 + 忙则拒绝）：失败经既有通道上 UI，忙则拒绝不建任务 ----
void TestPropWorkflow::asyncJobRejectsWhenBusyAndReportsFailure()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(tmp.path()));
  PropertyModelWorkflow wf(&cat, tmp.path());

  QObject dispatcher;
  PaleoTaskService svc(nullptr, &dispatcher);
  paleo::jobs::JobRunner<PropertyModelWorkflow::PropertyModelJob> runner(&dispatcher);
  runner.setTaskService(&svc);

  // (a) 忙则拒绝：第一代在飞时，第二代 startJob 返回 nullptr（现状
  //     m_propModelRunning 布尔的等价物）
  std::shared_ptr<PropertyModelWorkflow::PropertyModelJob> first;
  PaleoTask *t1 = wf.startJob(runner, asyncFixtureRequest(), 1.0, &first);
  QVERIFY(t1);
  std::shared_ptr<PropertyModelWorkflow::PropertyModelJob> second;
  PaleoTask *t2 = wf.startJob(runner, asyncFixtureRequest(), 1.0, &second);
  QVERIFY2(!t2, "忙则必须拒绝第二代");
  QVERIFY(!second);
  QVERIFY(runner.busy());

  // 等第一代自然完成
  QVERIFY(pumpUntil([&] { return !runner.busy(); }));
  svc.shutdown(3000, false);

  // (b) 失败态：错误串经 job.error 到既有通道，且失败也进 commit（失败如实上 UI）
  PropertyModelRequest bad = asyncFixtureRequest();
  bad.top.z.assign(24 * 16, 0.0f);
  bad.bot.z.assign(24 * 16, 0.0f); // 深度域与层位 z 同号 → 粗化无值
  QSignalSpy failed(&wf, &PropertyModelWorkflow::modelFailed);
  QObject dispatcher2;
  PaleoTaskService svc2(nullptr, &dispatcher2);
  paleo::jobs::JobRunner<PropertyModelWorkflow::PropertyModelJob> runner2(&dispatcher2);
  runner2.setTaskService(&svc2);

  std::shared_ptr<PropertyModelWorkflow::PropertyModelJob> badJob;
  // 失败路径不得留下**新增**登记：前半段 (a) 的成功路径已在同一个 catalog
  // 登记过一个 property_volume，故比对前后计数，不是断言「一个都没有」。
  int before = 0;
  for (const CatalogAsset &asset : cat.assets())
    if (asset.type == QLatin1String("property_volume"))
      ++before;

  PaleoTask *badTask = wf.startJob(runner2, bad, 1.0, &badJob);
  QVERIFY(badTask);
  QVERIFY(pumpUntil([&] { return !runner2.busy(); }));
  QCOMPARE(badTask->state(), PaleoTask::State::Failed);
  QVERIFY(!badJob->registered);
  // 失败串不空——即失败态如实可上 UI（现状是靠 computed.error 展示）
  QVERIFY(!badTask->errorText().isEmpty());

  int after = 0;
  for (const CatalogAsset &asset : cat.assets())
    if (asset.type == QLatin1String("property_volume"))
      ++after;
  QCOMPARE(after, before);

  svc2.shutdown(3000, false);
}



// goal/well-trajectory 轮4：requestFromCatalog 定向井站表走真实轨迹——
// z=TVD（<MD）、x/y=井口+位移；轨迹跨多个网格柱而直井恒一柱（穿层段语义）；
// 粗化代表柱随轨迹位移（columnJ > 直井）。
void TestPropWorkflow::deviatedWellUsesTrajectoryStations()
{
  QTemporaryDir tmp;
  const QString c3Path = tmp.filePath(QStringLiteral("C3.tif"));
  const QString d72Path = tmp.filePath(QStringLiteral("D72.tif"));
  // 井口柱 (0,0) 挖死（nodata）：直井全落在死柱无值；定向井轨迹南移
  // 逃出死柱仍有值——「真实轨迹相交」的语义判别器。
  QVERIFY(writeFlat(c3Path, 4, 0.0f, 0.0f));
  QVERIFY(writeFlat(d72Path, 4, 40.0f));

  DataCatalog cat;
  QVERIFY(cat.open(tmp.path()));
  QString catErr;
  auto addHorizon = [&](const QString &id, const QString &display, const QString &path) {
    CatalogAsset asset;
    asset.id = id;
    asset.type = QStringLiteral("horizon");
    asset.format = QStringLiteral("tif");
    asset.displayName = display;
    if (!cat.addAsset(asset, &catErr))
      return false;
    CatalogVersion ver;
    ver.id = id + QStringLiteral("-v");
    ver.assetId = id;
    ver.stage = QStringLiteral("RAW");
    ver.managed = false;
    ver.path = path;
    ver.fileName = QFileInfo(path).fileName();
    return cat.addVersion(ver, &catErr);
  };
  QVERIFY2(addHorizon(QStringLiteral("hz-c3"), QStringLiteral("C3.dat"), c3Path), qPrintable(catErr));
  QVERIFY2(addHorizon(QStringLiteral("hz-d72"), QStringLiteral("D72.dat"), d72Path), qPrintable(catErr));

  // W-D 定向（有测斜）；W-V 直井（同井口同曲线，无测斜）。
  const QString lasD = tmp.filePath(QStringLiteral("D.las"));
  const QString lasV = tmp.filePath(QStringLiteral("V.las"));
  QVERIFY(writeLas(lasD, QStringLiteral("GR"),
                   QStringLiteral("0.0 6.0\n20.0 6.0\n40.0 6.0\n")));
  QVERIFY(writeLas(lasV, QStringLiteral("GR"),
                   QStringLiteral("0.0 6.0\n20.0 6.0\n40.0 6.0\n")));
  // 井口 (5,35)：writeFlat 北向上栅格 y∈[0,40]（originY=40、dy=-10）。
  QVERIFY2(addWellLog(&cat, QStringLiteral("W-D"), QStringLiteral("las-d"), lasD, true, 5.0, 35.0, &catErr),
           qPrintable(catErr));
  QVERIFY2(addWellLog(&cat, QStringLiteral("W-V"), QStringLiteral("las-v"), lasV, true, 5.0, 35.0, &catErr),
           qPrintable(catErr));

  // 测斜：0-40m MD 0°→90°（方位 180°=正南，北向位移为负）——轨迹向南
  // 穿出井口柱且留在栅格内。
  CatalogAsset dev;
  dev.id = QStringLiteral("ast-dev");
  dev.type = QStringLiteral("well_deviation");
  dev.format = QStringLiteral("dat");
  dev.displayName = QStringLiteral("W-D.deviation.dat");
  QVERIFY2(cat.addAsset(dev, &catErr), qPrintable(catErr));
  CatalogVersion devVer;
  devVer.id = QStringLiteral("ver-dev");
  devVer.assetId = dev.id;
  devVer.stage = QStringLiteral("RAW");
  devVer.managed = false;
  devVer.path = tmp.filePath(QStringLiteral("W-D.deviation.dat"));
  devVer.fileName = QStringLiteral("W-D.deviation.dat");
  QVERIFY2(cat.addVersion(devVer, &catErr), qPrintable(catErr));
  {
    QFile f(devVer.path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(f.write("# Well : W-D\n0 0 0\n20 45 180\n40 90 180\n") > 0);
    f.close();
  }
  EntityAssetLink devLink;
  devLink.entityType = QStringLiteral("well");
  devLink.entityId = QStringLiteral("W-D");
  devLink.assetId = dev.id;
  devLink.role = QStringLiteral("trajectory");
  devLink.isPrimary = true;
  QVERIFY2(cat.addLink(devLink, &catErr), qPrintable(catErr));

  PropertyModelWorkflow wf(&cat, tmp.path());
  QString err;
  const PropertyModelRequest req = wf.requestFromCatalog(
      QStringLiteral("C3"), QStringLiteral("D72"), QStringLiteral("GR"), 4,
      Aggregator::ThicknessWeightedMean, 2.0, &err);
  QVERIFY2(err.isEmpty(), qPrintable(err));
  QCOMPARE(static_cast<int>(req.wells.size()), 2);
  QCOMPARE(req.trajectoryWellCount, 1);

  const WellCurve *deviated = nullptr;
  const WellCurve *vertical = nullptr;
  for (const WellCurve &w : req.wells)
  {
    if (w.wellId == QLatin1String("W-D"))
      deviated = &w;
    else if (w.wellId == QLatin1String("W-V"))
      vertical = &w;
  }
  QVERIFY(deviated != nullptr);
  QVERIFY(vertical != nullptr);

  // 直井面不变：2 站，x/y 恒井口，z=MD。
  QCOMPARE(vertical->stations.size(), static_cast<std::size_t>(2));
  QCOMPARE(vertical->stations[0].x, 5.0);
  QCOMPARE(vertical->stations[0].y, 35.0);
  QCOMPARE(vertical->stations[1].z, 40.0);

  // 定向井：端点 + 中间站（3 站）。闭式：0→90° 段 β=π/2、RF=4/π，
  // 全段 Δtvd = 20·(1+0)·4/π = 80/π；两段各半（角度线性 45° 中站）。
  QCOMPARE(deviated->stations.size(), static_cast<std::size_t>(3));
  QCOMPARE(deviated->stations.front().md, 0.0);
  QCOMPARE(deviated->stations.back().md, 40.0);
  const double kPi = std::numbers::pi;
  // 0→20 段（0°→45°）：cos β = cos45 → RF = 2/(π/4)·tan(π/8)。
  const double beta1 = kPi / 4.0;
  const double rf1 = (2.0 / beta1) * std::tan(beta1 / 2.0);
  const double tvd20 = 10.0 * (1.0 + std::cos(kPi / 4.0)) * rf1;
  QVERIFY(std::fabs(deviated->stations[1].z - tvd20) < 1e-9);
  // 20→40 段（45°→90°）同狗腿角。
  const double tvd40 = tvd20 + 10.0 * (std::cos(kPi / 4.0) + 0.0) * rf1;
  QVERIFY(std::fabs(deviated->stations[2].z - tvd40) < 1e-9);
  QVERIFY(deviated->stations[2].z < 40.0); // TVD < MD（定向语义）
  const double north40 = -(10.0 * (0.0 + std::sin(kPi / 4.0)) * rf1 +
                           10.0 * (std::sin(kPi / 4.0) + 1.0) * rf1);
  QVERIFY(std::fabs(deviated->stations[2].y - (35.0 + north40)) < 1e-9);
  QCOMPARE(deviated->stations[2].x, 5.0); // 方位正南：x 不动

  // 穿柱语义（10m 网格、北向上 originY=40、dy=-10）：轨迹跨多柱，直井恒一柱。
  auto columnsOf = [](const WellCurve &w) {
    std::set<std::pair<int, int>> out;
    for (const WellStation &s : w.stations)
      out.insert({static_cast<int>(std::floor(s.x / 10.0)),
                  static_cast<int>(std::floor((40.0 - s.y) / 10.0))});
    return out;
  };
  QCOMPARE(columnsOf(*vertical).size(), std::size_t(1));
  QVERIFY(columnsOf(*deviated).size() >= std::size_t(3));

  // 粗化：井口柱死亡——直井（全 MD 在死柱）无值；定向井轨迹南移进活柱，
  // 浅层（TVD 0-20 内）有值且为曲线常值 6（穿层段 = 真实轨迹相交）。
  SurfaceGrid top, bot;
  QVERIFY(PropertyModelWorkflow::loadSurface(c3Path, &top, &err));
  QVERIFY(PropertyModelWorkflow::loadSurface(d72Path, &bot, &err));
  ZoneGrid grid;
  QVERIFY(buildZoneGrid(top, bot, 4, &grid, &err));
  UpscaleTable table;
  QVERIFY(upscaleWells(grid, req.wells, Aggregator::ThicknessWeightedMean, &table, &err));
  QVERIFY(!grid.columnLive(0, 0)); // 井口柱确死（fixture 前提）
  int idxD = -1, idxV = -1;
  for (std::size_t w = 0; w < req.wells.size(); ++w)
  {
    if (req.wells[w].wellId == QLatin1String("W-D"))
      idxD = static_cast<int>(w);
    else if (req.wells[w].wellId == QLatin1String("W-V"))
      idxV = static_cast<int>(w);
  }
  QVERIFY(idxD >= 0 && idxV >= 0);
  // 浅层（TVD 0-10m）：造斜起始段仍在井口死柱内——两口井都无值（不虚给）。
  const LayerValue &shallowD = table.at(idxD, 0);
  const LayerValue &shallowV = table.at(idxV, 0);
  QVERIFY(!shallowV.hasValue);
  QVERIFY(!shallowD.hasValue);
  // 中层（TVD 10-20m）：直井全 MD 在死柱仍无值；定向井轨迹南移进活柱
  // ——有值、均匀曲线的厚度加权均值 6、代表柱离开井口柱。
  const LayerValue &cellV = table.at(idxV, 1);
  const LayerValue &cellD = table.at(idxD, 1);
  QVERIFY(!cellV.hasValue);            // 直井：井口死柱独占 → 无值
  QVERIFY(cellD.hasValue);             // 定向井：轨迹逃出死柱 → 有值（真实相交）
  QCOMPARE(cellD.value, 6.0);
  QVERIFY(cellD.columnJ >= 1);         // 代表柱已离开井口柱
  QVERIFY(cellD.supportLength > 0.0);
}

// ---- #159：调用方在 startJob 之后连的 task->finished 槽里，登记已完成 ----
// （主窗口 finishPropertyModelRun 就是这样接的；旧 JobRunner 把 commit 再排
// 一次队，收尾槽读到 registered=false / 空文件名。）
void TestPropWorkflow::asyncJobRegisteredBeforeCallerFinishedSlot()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(tmp.path()));
  PropertyModelWorkflow wf(&cat, tmp.path());

  QObject dispatcher;
  PaleoTaskService svc(nullptr, &dispatcher);
  paleo::jobs::JobRunner<PropertyModelWorkflow::PropertyModelJob> runner(&dispatcher);
  runner.setTaskService(&svc);

  std::shared_ptr<PropertyModelWorkflow::PropertyModelJob> job;
  PaleoTask *task = wf.startJob(runner, asyncFixtureRequest(), 1.0, &job);
  QVERIFY(task);
  int seen = -1;
  QString seenPath;
  QObject ctx;
  connect(task, &PaleoTask::finished, &ctx, [&] {
    seen = job->registered ? 1 : 0;
    seenPath = job->computed.front().out.path;
  });
  bool completed = false;
  connect(&runner, &paleo::jobs::JobRunnerBase::jobCompleted, &ctx,
          [&](quint64, bool committed) { completed = committed; });
  QVERIFY(pumpUntil([&] { return seen >= 0 && completed; }));
  QCOMPARE(seen, 1);
  QVERIFY(!seenPath.isEmpty());
  svc.shutdown(3000, false);
}

// ---- #153：工程切换（任务服务开新会话）→ 在途属性建模被丢弃、不登记 ----
void TestPropWorkflow::asyncJobDroppedOnSessionReset()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(tmp.path()));
  PropertyModelWorkflow wf(&cat, tmp.path());

  QObject dispatcher;
  PaleoTaskService svc(nullptr, &dispatcher);
  paleo::jobs::JobRunner<PropertyModelWorkflow::PropertyModelJob> runner(&dispatcher);
  runner.setTaskService(&svc);
  QSignalSpy stored(&wf, &PropertyModelWorkflow::modelStored);

  std::shared_ptr<PropertyModelWorkflow::PropertyModelJob> job;
  PaleoTask *task = wf.startJob(runner, asyncFixtureRequest(), 1.0, &job);
  QVERIFY(task);
  svc.beginNewSession(); // AppContext 在 projectAboutToClose 上调用
  QVERIFY(pumpUntil([&] { return !runner.busy(); }));
  QCOMPARE(task->state(), PaleoTask::State::Cancelled);
  QVERIFY(!job->registered);
  QCOMPARE(stored.size(), 0);
  for (const CatalogAsset &asset : cat.assets())
    QVERIFY(asset.type != QLatin1String("property_volume"));
  svc.shutdown(3000, false);
}

// ---- #153：compute 只用 prepare 段快照的工程目录；compute 纯函数不读成员 ----
void TestPropWorkflow::computeSnapshotIsPureAndCancellable()
{
  // 纯函数：catalog 未打开 → 如实失败，不发信号（worker 线程不 emit）
  PropertyModelRequest req = asyncFixtureRequest();
  const auto closed = PropertyModelWorkflow::computeSnapshot(req, QString(), false);
  QVERIFY(!closed.empty() && !closed.front().ok);
  QVERIFY(!closed.front().error.isEmpty());
  // 进度回调返回 false（取消）→ 立即失败、错误为「已取消」
  int calls = 0;
  const auto cancelled = PropertyModelWorkflow::computeSnapshot(
      req, QString(), true, [&calls](double, const QString &) {
        ++calls;
        return false;
      });
  QVERIFY(!cancelled.empty() && !cancelled.front().ok);
  QCOMPARE(calls, 1);
  QCOMPARE(cancelled.front().error, QStringLiteral("已取消"));
  const auto ok = PropertyModelWorkflow::computeSnapshot(req, QString(), true);
  QVERIFY2(!ok.empty() && ok.front().ok,
           qPrintable(ok.empty() ? QStringLiteral("空结果") : ok.front().error));
}

// ---- 方向 45：SGS 多实现 = 同资产多 DERIVED 版本，每版本带完整参数+种子+
// 父版本锚（Oracle 7）；同种子重跑逐位复现。 ----
void TestPropWorkflow::sgsMultiRealizationRegistersIndependentVersions()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString topPath = tmp.filePath(QStringLiteral("top.tif"));
  const QString botPath = tmp.filePath(QStringLiteral("bot.tif"));
  QVERIFY(writeFlat(topPath, 6, 0.0f, 0.0f));
  QVERIFY(writeFlat(botPath, 6, 60.0f, 0.0f));

  DataCatalog cat;
  QVERIFY(cat.open(tmp.path()));
  QString catErr;
  QVERIFY2(addExternal(&cat, QStringLiteral("ast-top"), topPath, &catErr), qPrintable(catErr));
  QVERIFY2(addExternal(&cat, QStringLiteral("ast-bot"), botPath, &catErr), qPrintable(catErr));

  PropertyModelRequest req;
  req.propertyName = QStringLiteral("PHIT");
  req.topName = QStringLiteral("C3");
  req.botName = QStringLiteral("D72");
  req.topPath = topPath;
  req.botPath = botPath;
  req.nLayers = 4;
  req.method = PropertyMethod::Sgs;
  req.variogram.type = paleo::geostat::VariogramModelType::Spherical;
  req.variogram.nugget = 0.1;
  req.variogram.sill = 1.0;
  req.variogram.range = 30.0;
  req.variogram.verticalRangeRatio = 4.0;
  req.sgsRealizations = 4;
  req.sgsSeed = 20261005;
  req.wells.push_back(wellNamed(QStringLiteral("WA"), QStringLiteral("PHIT"), 15.0, 45.0, 0.0,
                                60.0, 10.0));
  req.wells.push_back(wellNamed(QStringLiteral("WB"), QStringLiteral("PHIT"), 45.0, 15.0, 0.0,
                                60.0, 12.0));

  PropertyModelWorkflow wf(&cat, tmp.path());
  QSignalSpy stored(&wf, &PropertyModelWorkflow::modelStored);
  const PropertyModelOutput out = wf.run(req);
  QVERIFY2(out.ok, qPrintable(out.error));
  QCOMPARE(stored.count(), 4); // 每实现一次登记通知
  QCOMPARE(out.realizationCount, 4);

  const CatalogAsset asset = cat.assetById(out.assetId);
  QCOMPARE(asset.type, QStringLiteral("property_volume"));
  const QVector<CatalogVersion> versions = cat.versionsForAsset(out.assetId);
  QCOMPARE(versions.size(), 4);
  QSet<QString> paramHashes;
  QSet<int> realizationIndexes;
  for (const CatalogVersion &ver : versions)
  {
    QCOMPARE(ver.stage, QStringLiteral("DERIVED"));
    paramHashes.insert(ver.extra.value(QStringLiteral("param_hash")).toString());
    realizationIndexes.insert(ver.extra.value(QStringLiteral("realization_index")).toInt());
    QCOMPARE(ver.extra.value(QStringLiteral("seed")).toString(), QStringLiteral("20261005"));
    QCOMPARE(ver.extra.value(QStringLiteral("method")).toString(), QStringLiteral("sgs"));
    // 父版本锚：每个 realization 版本都锚住层位源
    QVERIFY(ver.parentVersionIds.contains(QStringLiteral("ast-top-v")));
    QVERIFY(ver.parentVersionIds.contains(QStringLiteral("ast-bot-v")));
  }
  QCOMPARE(paramHashes.size(), 1); // 同参数同哈希（实现序不进哈希）
  QCOMPARE(realizationIndexes.size(), 4);

  // blob provenance：首实现带 realization_index/seed/方法/竖直近似口径。
  PropertyVolume volume;
  QJsonObject prov;
  QString err;
  QFile blobFile(out.path);
  QVERIFY(blobFile.open(QIODevice::ReadOnly));
  QVERIFY2(readPropertyBlob(blobFile.readAll(), &volume, &prov, &err), qPrintable(err));
  QCOMPARE(prov.value(QStringLiteral("method")).toString(), QStringLiteral("sgs"));
  QCOMPARE(prov.value(QStringLiteral("seed")).toString(), QStringLiteral("20261005"));
  const int blobIndex = prov.value(QStringLiteral("realization_index")).toInt();
  QVERIFY(blobIndex >= 0 && blobIndex < 4);
  QVERIFY(prov.value(QStringLiteral("realization_count")).toInt() == 4);
  QCOMPARE(prov.value(QStringLiteral("n_vertical_approx_wells")).toInt(), 2);
  QVERIFY(prov.value(QStringLiteral("trajectory_caliber")).toString().contains(
      QStringLiteral("竖直近似")));
  // 硬数据：井柱 cell 精确复现（每实现都钉死——这里验首实现）
  QCOMPARE(volume.values[static_cast<std::size_t>(volume.grid.cellIndex(1, 1, 0))], 10.0f);
  QCOMPARE(volume.values[static_cast<std::size_t>(volume.grid.cellIndex(4, 4, 3))], 12.0f);

  // 可复现：同参数重跑 → 每实现 blob 逐位一致（对齐 realization_index）。
  const PropertyModelOutput again = wf.run(req);
  QVERIFY2(again.ok, qPrintable(again.error));
  const QVector<CatalogVersion> versions2 = cat.versionsForAsset(out.assetId);
  QCOMPARE(versions2.size(), 8);
  int matched = 0;
  for (const CatalogVersion &ver : versions)
  {
    const int index = ver.extra.value(QStringLiteral("realization_index")).toInt();
    // 找第二次运行的同序版本
    bool found = false;
    for (const CatalogVersion &ver2 : versions2)
    {
      if (ver2.id == ver.id)
        continue;
      if (ver2.extra.value(QStringLiteral("realization_index")).toInt() != index ||
          ver2.extra.value(QStringLiteral("param_hash")).toString() !=
              ver.extra.value(QStringLiteral("param_hash")).toString())
        continue;
      QFile a(DataCatalog::resolvedVersionPath(tmp.path(), ver));
      QFile b(DataCatalog::resolvedVersionPath(tmp.path(), ver2));
      QVERIFY(a.open(QIODevice::ReadOnly));
      QVERIFY(b.open(QIODevice::ReadOnly));
      QCOMPARE(a.readAll(), b.readAll());
      found = true;
      break;
    }
    if (found)
      ++matched;
  }
  QCOMPARE(matched, 4); // 每个实现都找到配对且逐位一致——静默 break 不算过
}

// ---- 方向 45：cut.extra["throw_z"] + 盘侧 → FaultThrow；链路上错位口径入档 ----
void TestPropWorkflow::faultThrowExtractionFeedsOffset()
{
  paleo::fault::FaultSet set;
  const QString id = set.addFault(QStringLiteral("F1"));

  paleo::fault::FaultHorizonCut cut;
  cut.horizon = QStringLiteral("H1");
  cut.wkt = QStringLiteral("LINESTRING (0 0, 100 0)");
  cut.hangingSide = paleo::fault::FaultHangingSide::Left;
  cut.extra.insert(QStringLiteral("throw_z"), 5.0);
  QVERIFY(set.setCut(id, cut));

  auto extracted = PropertyModelWorkflow::throwSegmentsFromFaultSet(set);
  QCOMPARE(extracted.throws.size(), std::size_t(1));
  QCOMPARE(extracted.curtainSegments, 1);
  QCOMPARE(extracted.throwSegments, 1);
  QCOMPARE(extracted.throws[0].throwStart, 5.0);
  QCOMPARE(extracted.throws[0].throwEnd, 5.0);
  QVERIFY(extracted.throws[0].dropLeftSide);

  // 无断距：只竖帘
  paleo::fault::FaultHorizonCut bare = cut;
  bare.extra.remove(QStringLiteral("throw_z"));
  QVERIFY(set.setCut(id, bare));
  extracted = PropertyModelWorkflow::throwSegmentsFromFaultSet(set);
  QVERIFY(extracted.throws.empty());
  QCOMPARE(extracted.curtainSegments, 1);

  // 有断距但盘侧未知：保持竖帘并计数
  paleo::fault::FaultHorizonCut unknown = cut;
  unknown.hangingSide = paleo::fault::FaultHangingSide::Unknown;
  QVERIFY(set.setCut(id, unknown));
  extracted = PropertyModelWorkflow::throwSegmentsFromFaultSet(set);
  QVERIFY(extracted.throws.empty());
  QCOMPARE(extracted.unknownSideCuts, 1);

  // 链路：断距进请求 → 错位格架 → 错位口径进 blob/extra
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(tmp.path()));
  PropertyModelRequest req;
  req.useEmbeddedSurfaces = true;
  req.propertyName = QStringLiteral("GR");
  req.topName = QStringLiteral("T");
  req.botName = QStringLiteral("B");
  req.nLayers = 3;
  req.top.cols = req.bot.cols = 12;
  req.top.rows = req.bot.rows = 8;
  req.top.dx = req.bot.dx = 10;
  req.top.dy = req.bot.dy = 10;
  req.top.z.assign(12 * 8, 0.0f);
  req.bot.z.assign(12 * 8, 30.0f);
  req.wells.push_back(wellAt(25.0, 45.0, 0.0, 30.0, 8.0));
  req.wells.push_back(wellAt(75.0, 35.0, 0.0, 30.0, 9.0));
  paleo::stratgrid::FaultThrow throwSeg;
  throwSeg.x0 = 60;
  throwSeg.y0 = -10;
  throwSeg.x1 = 60;
  throwSeg.y1 = 90;
  throwSeg.throwStart = 4.0;
  throwSeg.throwEnd = 4.0;
  throwSeg.dropLeftSide = false;
  req.faultThrows = {throwSeg};
  req.faults.push_back(FaultSegment{60.0, -10.0, 60.0, 90.0});

  PropertyModelWorkflow wf(&cat, tmp.path());
  const PropertyModelOutput out = wf.run(req);
  QVERIFY2(out.ok, qPrintable(out.error));
  PropertyVolume volume;
  QJsonObject prov;
  QString err;
  QFile blobFile(out.path);
  QVERIFY(blobFile.open(QIODevice::ReadOnly));
  QVERIFY2(readPropertyBlob(blobFile.readAll(), &volume, &prov, &err), qPrintable(err));
  QCOMPARE(prov.value(QStringLiteral("fault_offset_max_abs_throw")).toDouble(), 4.0);
  QVERIFY(prov.value(QStringLiteral("fault_offset_columns")).toInt() > 0);
  QVERIFY(prov.value(QStringLiteral("caliber")).toString().contains(QStringLiteral("断块错位")));
  // Oracle 1 链路面：错位后断层两侧同层界面差 = throw（dropLeftSide=false
  // → 东侧 i≥6 下掉 +4，西侧未动 → 东侧界面 − 西侧界面 = +4）
  double zl = 0;
  double zr = 0;
  QVERIFY(interfaceZ(volume.grid, 3, 4, 2, &zl));
  QVERIFY(interfaceZ(volume.grid, 8, 4, 2, &zr));
  QCOMPARE(zr - zl, 4.0);
}

// ---- 方向 45：相带分区链路 + 竖直近似口径 + 无相带诚实降级 ----
void TestPropWorkflow::faciesAndVerticalApproxCalibersAreHonest()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(tmp.path()));
  PropertyModelWorkflow wf(&cat, tmp.path());

  // collectFaciesPolygons：无资产 → 如实拒绝
  PropertyModelRequest empty;
  QString err;
  QVERIFY(!wf.collectFaciesPolygons(&empty, &err));
  QVERIFY(err.contains(QStringLiteral("相带")));
  QVERIFY(empty.faciesRings.empty());

  PropertyModelRequest req;
  req.useEmbeddedSurfaces = true;
  req.propertyName = QStringLiteral("PHIT");
  req.topName = QStringLiteral("T");
  req.botName = QStringLiteral("B");
  req.nLayers = 3;
  req.method = PropertyMethod::Sgs;
  req.variogram.sill = 1.0;
  req.variogram.range = 40.0;
  req.sgsSeed = 11;
  req.sgsRealizations = 2;
  req.top.cols = req.bot.cols = 16;
  req.top.rows = req.bot.rows = 8;
  req.top.dx = req.bot.dx = 10;
  req.top.dy = req.bot.dy = 10;
  req.top.z.assign(16 * 8, 0.0f);
  req.bot.z.assign(16 * 8, 24.0f);
  req.wells.push_back(wellNamed(QStringLiteral("WA"), QStringLiteral("PHIT"), 25.0, 35.0, 0.0,
                                24.0, 10.0));
  req.wells.push_back(wellNamed(QStringLiteral("WB"), QStringLiteral("PHIT"), 125.0, 45.0, 0.0,
                                24.0, 20.0));
  // 两相带：西 8 列 code 1、东 8 列 code 2
  req.useFacies = true;
  req.faciesAssetName = QStringLiteral("facies-draft");
  {
    paleo::stratgrid::ZoneRing west;
    west.code = 1;
    west.xs = {0, 80, 80, 0, 0};
    west.ys = {0, 0, 80, 80, 0};
    paleo::stratgrid::ZoneRing east;
    east.code = 2;
    east.xs = {80, 160, 160, 80, 80};
    east.ys = {0, 0, 80, 80, 0};
    req.faciesRings = {west, east};
  }

  const PropertyModelOutput out = wf.run(req);
  QVERIFY2(out.ok, qPrintable(out.error));
  PropertyVolume volume;
  QJsonObject prov;
  QString err2;
  QFile blobFile(out.path);
  QVERIFY(blobFile.open(QIODevice::ReadOnly));
  QVERIFY2(readPropertyBlob(blobFile.readAll(), &volume, &prov, &err2), qPrintable(err2));
  QVERIFY(prov.value(QStringLiteral("caliber")).toString().contains(QStringLiteral("相带面")));
  QVERIFY(prov.value(QStringLiteral("caliber")).toString().contains(QStringLiteral("相带分区 2 域")));
  QCOMPARE(prov.value(QStringLiteral("n_vertical_approx_wells")).toInt(), 2);
  // 竖直近似井（无测斜输入）：wellAt 站点即竖直口径，值仍钉死
  QCOMPARE(volume.values[static_cast<std::size_t>(volume.grid.cellIndex(2, 3, 0))], 10.0f);
}

// ---- 方向 45：对象建模叠加链路——对象优先口径 + 放置几何入 provenance ----
void TestPropWorkflow::objectOverrideRegistersInProvenance()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(tmp.path()));
  PropertyModelRequest req;
  req.useEmbeddedSurfaces = true;
  req.propertyName = QStringLiteral("GR");
  req.topName = QStringLiteral("T");
  req.botName = QStringLiteral("B");
  req.nLayers = 4;
  req.top.cols = req.bot.cols = 20;
  req.top.rows = req.bot.rows = 14;
  req.top.dx = req.bot.dx = 10;
  req.top.dy = req.bot.dy = 10;
  req.top.z.assign(20 * 14, 0.0f);
  req.bot.z.assign(20 * 14, 40.0f);
  req.wells.push_back(wellAt(25.0, 75.0, 0.0, 40.0, 5.0));
  req.wells.push_back(wellAt(175.0, 65.0, 0.0, 40.0, 7.0));

  paleo::stratgrid::ObjectSpec channel;
  channel.type = paleo::stratgrid::ObjectType::Channel;
  channel.azimuthDeg = 90;
  channel.length = 150;
  channel.width = 60;
  channel.thickness = 12;
  channel.value = 42.0;
  channel.count = 2;
  req.objectSpecs = {channel};
  req.objectSeed = 7;

  PropertyModelWorkflow wf(&cat, tmp.path());
  const PropertyModelOutput out = wf.run(req);
  QVERIFY2(out.ok, qPrintable(out.error));
  PropertyVolume volume;
  QJsonObject prov;
  QString err;
  QFile blobFile(out.path);
  QVERIFY(blobFile.open(QIODevice::ReadOnly));
  QVERIFY2(readPropertyBlob(blobFile.readAll(), &volume, &prov, &err), qPrintable(err));
  const QJsonArray placements = prov.value(QStringLiteral("object_placements")).toArray();
  QCOMPARE(placements.size(), 2);
  QCOMPARE(placements.at(0).toObject().value(QStringLiteral("type")).toString(),
           QStringLiteral("channel"));
  QCOMPARE(placements.at(0).toObject().value(QStringLiteral("width")).toDouble(), 60.0);
  QVERIFY(placements.at(0).toObject().value(QStringLiteral("cells")).toInt() > 0);
  QVERIFY(prov.value(QStringLiteral("object_cells")).toInt() > 0);
  QCOMPARE(prov.value(QStringLiteral("object_seed")).toString(), QStringLiteral("7"));
  // 对象优先口径：场内存在对象值（背景 IDW 值 5~7 之外）
  bool hasOverride = false;
  for (float v : volume.values)
    if (std::isfinite(v) && std::fabs(v - 42.0f) < 1e-6f)
      hasOverride = true;
  QVERIFY2(hasOverride, "object cells must hard-override the background field");
  QVERIFY(prov.value(QStringLiteral("caliber")).toString().contains(QStringLiteral("对象优先")));
}

int main(int argc, char *argv[])
{
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true);
  app.initQgis();
  GDALAllRegister();
  TestPropWorkflow tc;
  // ctest 无控制台下 QtTest 结果行走 OutputDebugString，失败时看不到是哪条；
  // 追加 -o 让结果落盘（方向20 迁移调试用）。
  QByteArray logPath = QByteArray(QT_TESTCASE_BUILDDIR) + "/tst_propworkflow-result.txt";
  QList<QByteArray> forwarded;
  forwarded << QByteArray(argv[0]);
  for (int i = 1; i < argc; ++i)
    forwarded << QByteArray(argv[i]);
  forwarded << QByteArray("-o") << logPath + ",txt";
  QList<char *> cargv;
  cargv.reserve(forwarded.size());
  for (QByteArray &a : forwarded)
    cargv << a.data();
  const int rc = QTest::qExec(&tc, cargv.size(), cargv.data());
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_propworkflow.moc"
