// 层：测试壳
#include <QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextStream>

#include <cmath>
#include <limits>

#include "catalog/datacatalog.h"
#include "metadata/paleoprojectstore.h"
#include "services/paleotaskservice.h"
#include "services/petrophyscomputeservice.h"
#include "workflow/propertymodelworkflow.h"
#include "workflow/sectionworkbench.h"

using paleo::petrophys::PetroPhysTaskService;

namespace
{

bool writeLas(const QString &path, const QVector<QPair<QString, QVector<double>>> &curves)
{
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
    return false;
  QTextStream ts(&f);
  ts << "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nNULL. -999.25:\n";
  ts << "~Curve\n";
  for (int i = 0; i < curves.size(); ++i)
  {
    const QString unit = i == 0 ? QStringLiteral("M") : QStringLiteral("");
    ts << curves.at(i).first << "." << unit << " :\n";
  }
  ts << "~ASCII\n";
  const int n = curves.first().second.size();
  for (int row = 0; row < n; ++row)
  {
    for (int c = 0; c < curves.size(); ++c)
    {
      const double v = curves.at(c).second.at(row);
      if (c)
        ts << " ";
      ts << (std::isnan(v) ? QStringLiteral("-999.25") : QString::number(v, 'f', 4));
    }
    ts << "\n";
  }
  return true;
}

struct LogFile
{
  QString assetId;
  QString versionId;
  QString path;
  bool primary = false;
  int ordinal = 0;
};

bool addWellLogs(DataCatalog *cat, const QString &wellId, double x, double y,
                 const QVector<LogFile> &logs, QString *error)
{
  CatalogEntity ent;
  ent.id = wellId;
  ent.entityType = QStringLiteral("well");
  ent.name = wellId;
  ent.hasSurface = true;
  ent.surfaceX = x;
  ent.surfaceY = y;
  ent.coordinateStatus = QStringLiteral("untransformed");
  ent.td = 2000;
  if (!cat->addEntity(ent, error))
    return false;
  for (const LogFile &log : logs)
  {
    CatalogAsset asset;
    asset.id = log.assetId;
    asset.type = QStringLiteral("well_log");
    asset.format = QStringLiteral("las");
    asset.displayName = QFileInfo(log.path).fileName();
    if (!cat->addAsset(asset, error))
      return false;
    CatalogVersion ver;
    ver.id = log.versionId;
    ver.assetId = log.assetId;
    ver.stage = QStringLiteral("RAW");
    ver.versionNumber = 1;
    ver.managed = false;
    ver.path = QFileInfo(log.path).absoluteFilePath();
    ver.fileName = QFileInfo(log.path).fileName();
    if (!cat->addVersion(ver, error))
      return false;
    EntityAssetLink link;
    link.entityType = QStringLiteral("well");
    link.entityId = wellId;
    link.assetId = log.assetId;
    link.role = QStringLiteral("well_log");
    link.isPrimary = log.primary;
    link.ordinal = log.ordinal;
    if (!cat->addLink(link, error))
      return false;
  }
  return true;
}

// requestFromCatalog 只按 displayName/fileName 认层位，不打开栅格。
bool addHorizon(DataCatalog *cat, const QString &id, const QString &token, const QString &path,
                QString *error)
{
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write("x") != 1)
    return false;
  file.close();
  CatalogAsset asset;
  asset.id = id;
  asset.type = QStringLiteral("horizon");
  asset.format = QStringLiteral("tif");
  asset.displayName = token + QStringLiteral(".tif");
  if (!cat->addAsset(asset, error))
    return false;
  CatalogVersion ver;
  ver.id = id + QStringLiteral("-v");
  ver.assetId = id;
  ver.stage = QStringLiteral("RAW");
  ver.versionNumber = 1;
  ver.managed = false;
  ver.path = QFileInfo(path).absoluteFilePath();
  ver.fileName = token + QStringLiteral(".tif");
  return cat->addVersion(ver, error);
}

} // namespace

class TestWellLogConsumers : public QObject
{
  Q_OBJECT
private slots:
  void petrophys_readsRtFromNonPrimaryFile();
  void petrophys_phiDensityUsesAliasedRhob();
  void petrophys_expressionPrefersRealRhobOverDen();
  void petrophys_expressionDrivesOnAliasedRhob();
  void propertymodel_requestUsesUnion();
  void sectionworkbench_showsNonPrimaryCurve();
};

void TestWellLogConsumers::petrophys_readsRtFromNonPrimaryFile()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString fileA = dir.filePath(QStringLiteral("fileA.las"));
  const QString fileB = dir.filePath(QStringLiteral("fileB.las"));
  const QVector<double> depth{1000, 1001, 1002};
  QVERIFY(writeLas(fileA, {{QStringLiteral("DEPT"), depth},
                           {QStringLiteral("GR"), {40, 50, 60}}}));
  QVERIFY(writeLas(fileB, {{QStringLiteral("DEPT"), depth},
                           {QStringLiteral("NPHI"), {0.25, 0.30, 0.35}}}));

  DataCatalog catalog;
  QVERIFY(catalog.open(dir.path()));
  QString err;
  QVERIFY2(addWellLogs(&catalog, QStringLiteral("well-U"), 100, 200,
                       {{QStringLiteral("ast-a"), QStringLiteral("ver-a"), fileA, true, 0},
                        {QStringLiteral("ast-b"), QStringLiteral("ver-b"), fileB, false, 1}},
                       &err),
           qPrintable(err));

  PaleoProjectStore store;
  PaleoTaskService tasks(&store);
  PetroPhysTaskService svc(&tasks, &store);
  PetroPhysTaskService::BatchRequest req;
  PetroPhysTaskService::WellRef well;
  well.wellId = QStringLiteral("well-U");
  well.lasPath = QFileInfo(fileA).absoluteFilePath();
  well.sourceVersionId = QStringLiteral("ver-a");
  req.wells.append(well);
  req.formula = PetroPhysTaskService::Formula::PhiNeutron;
  req.outputMnemonic = QStringLiteral("PHIN");
  req.writeProduct = false;

  bool done = false;
  PetroPhysTaskService::BatchResult batch;
  PaleoTask *task = svc.startBatch(req, &catalog, QString(),
                                   [&](bool, const PetroPhysTaskService::BatchResult &res) {
                                     done = true;
                                     batch = res;
                                   });
  QVERIFY(task);
  QSignalSpy finishedSpy(task, &PaleoTask::finished);
  QVERIFY(finishedSpy.wait(30000));
  QVERIFY(done);
  QCOMPARE(batch.wells.size(), 1);
  QVERIFY2(batch.wells.at(0).ok, qPrintable(batch.wells.at(0).error));
  QVERIFY(!batch.wells.at(0).error.contains(QStringLiteral("找不到")));
  QVERIFY(batch.ok);
  int finite = 0;
  bool sawFileB = false;
  for (double v : batch.wells.at(0).values)
  {
    if (!std::isfinite(v))
      continue;
    ++finite;
    if (std::fabs(v - 0.25) < 1e-6 || std::fabs(v - 0.30) < 1e-6 || std::fabs(v - 0.35) < 1e-6)
      sawFileB = true;
  }
  QVERIFY(finite >= 1);
  QVERIFY(sawFileB);
}

// 主文件没有 RHOB，另外两个文件都有：显示名变成 RHOB@file。φD 必须用上其中一条。
void TestWellLogConsumers::petrophys_phiDensityUsesAliasedRhob()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString fileA = dir.filePath(QStringLiteral("fileA.las"));
  const QString fileB = dir.filePath(QStringLiteral("fileB.las"));
  const QString fileC = dir.filePath(QStringLiteral("fileC.las"));
  const QVector<double> depth{1000, 1001, 1002};
  QVERIFY(writeLas(fileA, {{QStringLiteral("DEPT"), depth},
                           {QStringLiteral("GR"), {40, 50, 60}}}));
  QVERIFY(writeLas(fileB, {{QStringLiteral("DEPT"), depth},
                           {QStringLiteral("RHOB"), {2.30, 2.40, 2.50}}}));
  QVERIFY(writeLas(fileC, {{QStringLiteral("DEPT"), depth},
                           {QStringLiteral("RHOB"), {1.50, 1.50, 1.50}}}));

  DataCatalog catalog;
  QVERIFY(catalog.open(dir.path()));
  QString err;
  QVERIFY2(addWellLogs(&catalog, QStringLiteral("well-D"), 100, 200,
                       {{QStringLiteral("ast-a"), QStringLiteral("ver-a"), fileA, true, 0},
                        {QStringLiteral("ast-b"), QStringLiteral("ver-b"), fileB, false, 1},
                        {QStringLiteral("ast-c"), QStringLiteral("ver-c"), fileC, false, 2}},
                       &err),
           qPrintable(err));

  PaleoProjectStore store;
  PaleoTaskService tasks(&store);
  PetroPhysTaskService svc(&tasks, &store);
  PetroPhysTaskService::BatchRequest req;
  PetroPhysTaskService::WellRef well;
  well.wellId = QStringLiteral("well-D");
  well.lasPath = QFileInfo(fileA).absoluteFilePath();
  well.sourceVersionId = QStringLiteral("ver-a");
  req.wells.append(well);
  req.formula = PetroPhysTaskService::Formula::PhiDensity;
  req.params.rhoMa = 2.65;
  req.params.rhoFluid = 1.0;
  req.outputMnemonic = QStringLiteral("PHID");
  req.writeProduct = false;

  bool done = false;
  PetroPhysTaskService::BatchResult batch;
  PaleoTask *task = svc.startBatch(req, &catalog, QString(),
                                   [&](bool, const PetroPhysTaskService::BatchResult &res) {
                                     done = true;
                                     batch = res;
                                   });
  QVERIFY(task);
  QSignalSpy finishedSpy(task, &PaleoTask::finished);
  QVERIFY(finishedSpy.wait(30000));
  QVERIFY(done);
  QCOMPARE(batch.wells.size(), 1);
  QVERIFY2(batch.wells.at(0).ok, qPrintable(batch.wells.at(0).error));
  const QVector<double> expect{(2.65 - 2.30) / 1.65, (2.65 - 2.40) / 1.65, (2.65 - 2.50) / 1.65};
  QCOMPARE(batch.wells.at(0).values.size(), expect.size());
  for (int i = 0; i < expect.size(); ++i)
    QVERIFY2(std::fabs(batch.wells.at(0).values.at(i) - expect.at(i)) < 1e-6,
             qPrintable(QString::number(batch.wells.at(0).values.at(i))));
}

// 主文件 DEN、另一文件真 RHOB。GR+RHOB 必须用真 RHOB，不能把 DEN 当成 RHOB。
void TestWellLogConsumers::petrophys_expressionPrefersRealRhobOverDen()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString fileA = dir.filePath(QStringLiteral("fileA.las"));
  const QString fileB = dir.filePath(QStringLiteral("fileB.las"));
  const QVector<double> depth{1000, 1001, 1002};
  QVERIFY(writeLas(fileA, {{QStringLiteral("DEPT"), depth},
                           {QStringLiteral("GR"), {10, 20, 30}},
                           {QStringLiteral("DEN"), {1.5, 1.5, 1.5}}}));
  QVERIFY(writeLas(fileB, {{QStringLiteral("DEPT"), depth},
                           {QStringLiteral("RHOB"), {2.3, 2.4, 2.5}}}));

  DataCatalog catalog;
  QVERIFY(catalog.open(dir.path()));
  QString err;
  QVERIFY2(addWellLogs(&catalog, QStringLiteral("well-E"), 100, 200,
                       {{QStringLiteral("ast-a"), QStringLiteral("ver-a"), fileA, true, 0},
                        {QStringLiteral("ast-b"), QStringLiteral("ver-b"), fileB, false, 1}},
                       &err),
           qPrintable(err));

  PaleoProjectStore store;
  PaleoTaskService tasks(&store);
  PetroPhysTaskService svc(&tasks, &store);
  PetroPhysTaskService::BatchRequest req;
  PetroPhysTaskService::WellRef well;
  well.wellId = QStringLiteral("well-E");
  well.lasPath = QFileInfo(fileA).absoluteFilePath();
  well.sourceVersionId = QStringLiteral("ver-a");
  req.wells.append(well);
  req.formula = PetroPhysTaskService::Formula::Expression;
  req.expression = QStringLiteral("GR+RHOB");
  req.outputMnemonic = QStringLiteral("SUM");
  req.writeProduct = false;

  bool done = false;
  PetroPhysTaskService::BatchResult batch;
  PaleoTask *task = svc.startBatch(req, &catalog, QString(),
                                   [&](bool, const PetroPhysTaskService::BatchResult &res) {
                                     done = true;
                                     batch = res;
                                   });
  QVERIFY(task);
  QSignalSpy finishedSpy(task, &PaleoTask::finished);
  QVERIFY(finishedSpy.wait(30000));
  QVERIFY(done);
  QCOMPARE(batch.wells.size(), 1);
  QVERIFY2(batch.wells.at(0).ok, qPrintable(batch.wells.at(0).error));
  const QVector<double> expect{12.3, 22.4, 32.5};
  QCOMPARE(batch.wells.at(0).values.size(), expect.size());
  for (int i = 0; i < expect.size(); ++i)
    QVERIFY2(std::fabs(batch.wells.at(0).values.at(i) - expect.at(i)) < 1e-6,
             qPrintable(QString::number(batch.wells.at(0).values.at(i))));
}

// 两条 RHOB 都被加成 RHOB@file。表达式 RHOB 用先出现的那条文件做深度网格。
void TestWellLogConsumers::petrophys_expressionDrivesOnAliasedRhob()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString fileA = dir.filePath(QStringLiteral("fileA.las"));
  const QString fileB = dir.filePath(QStringLiteral("fileB.las"));
  const QString fileC = dir.filePath(QStringLiteral("fileC.las"));
  QVERIFY(writeLas(fileA, {{QStringLiteral("DEPT"), {1000, 1100, 1200}},
                           {QStringLiteral("GR"), {10, 20, 30}}}));
  QVERIFY(writeLas(fileB, {{QStringLiteral("DEPT"), {1000, 1001, 1002}},
                           {QStringLiteral("RHOB"), {2, 4, 6}}}));
  QVERIFY(writeLas(fileC, {{QStringLiteral("DEPT"), {1000, 1001, 1002}},
                           {QStringLiteral("RHOB"), {9, 9, 9}}}));

  DataCatalog catalog;
  QVERIFY(catalog.open(dir.path()));
  QString err;
  QVERIFY2(addWellLogs(&catalog, QStringLiteral("well-F"), 100, 200,
                       {{QStringLiteral("ast-a"), QStringLiteral("ver-a"), fileA, true, 0},
                        {QStringLiteral("ast-b"), QStringLiteral("ver-b"), fileB, false, 1},
                        {QStringLiteral("ast-c"), QStringLiteral("ver-c"), fileC, false, 2}},
                       &err),
           qPrintable(err));

  PaleoProjectStore store;
  PaleoTaskService tasks(&store);
  PetroPhysTaskService svc(&tasks, &store);
  PetroPhysTaskService::BatchRequest req;
  PetroPhysTaskService::WellRef well;
  well.wellId = QStringLiteral("well-F");
  well.lasPath = QFileInfo(fileA).absoluteFilePath();
  well.sourceVersionId = QStringLiteral("ver-a");
  req.wells.append(well);
  req.formula = PetroPhysTaskService::Formula::Expression;
  req.expression = QStringLiteral("RHOB");
  req.outputMnemonic = QStringLiteral("OUT");
  req.writeProduct = false;

  bool done = false;
  PetroPhysTaskService::BatchResult batch;
  PaleoTask *task = svc.startBatch(req, &catalog, QString(),
                                   [&](bool, const PetroPhysTaskService::BatchResult &res) {
                                     done = true;
                                     batch = res;
                                   });
  QVERIFY(task);
  QSignalSpy finishedSpy(task, &PaleoTask::finished);
  QVERIFY(finishedSpy.wait(30000));
  QVERIFY(done);
  QCOMPARE(batch.wells.size(), 1);
  QVERIFY2(batch.wells.at(0).ok, qPrintable(batch.wells.at(0).error));
  const QVector<double> expect{2, 4, 6};
  QCOMPARE(batch.wells.at(0).values.size(), expect.size());
  for (int i = 0; i < expect.size(); ++i)
    QVERIFY2(std::fabs(batch.wells.at(0).values.at(i) - expect.at(i)) < 1e-6,
             qPrintable(QString::number(batch.wells.at(0).values.at(i))));
}

void TestWellLogConsumers::propertymodel_requestUsesUnion()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString fileA = dir.filePath(QStringLiteral("fileA.las"));
  const QString fileB = dir.filePath(QStringLiteral("fileB.las"));
  const QVector<double> depth{1000, 1001, 1002};
  QVERIFY(writeLas(fileA, {{QStringLiteral("DEPT"), depth},
                           {QStringLiteral("GR"), {10, 11, 12}}}));
  QVERIFY(writeLas(fileB, {{QStringLiteral("DEPT"), depth},
                           {QStringLiteral("RT"), {8.5, 9.5, 10.5}}}));

  DataCatalog catalog;
  QVERIFY(catalog.open(dir.path()));
  QString err;
  QVERIFY2(addHorizon(&catalog, QStringLiteral("hz-top"), QStringLiteral("TOP"),
                      dir.filePath(QStringLiteral("TOP.tif")), &err),
           qPrintable(err));
  QVERIFY2(addHorizon(&catalog, QStringLiteral("hz-bot"), QStringLiteral("BOT"),
                      dir.filePath(QStringLiteral("BOT.tif")), &err),
           qPrintable(err));
  QVERIFY2(addWellLogs(&catalog, QStringLiteral("well-U"), 15, 25,
                       {{QStringLiteral("ast-a"), QStringLiteral("ver-a"), fileA, true, 0},
                        {QStringLiteral("ast-b"), QStringLiteral("ver-b"), fileB, false, 1}},
                       &err),
           qPrintable(err));

  PropertyModelWorkflow wf(&catalog, dir.path());
  const PropertyModelRequest req = wf.requestFromCatalog(
      QStringLiteral("TOP"), QStringLiteral("BOT"), QStringLiteral("RT"), 1,
      paleo::stratgrid::Aggregator::ThicknessWeightedMean, 2.0, &err);
  QVERIFY2(err.isEmpty(), qPrintable(err));
  QCOMPARE(static_cast<int>(req.wells.size()), 1);
  QCOMPARE(req.wells[0].wellId, QStringLiteral("well-U"));
  QCOMPARE(static_cast<int>(req.wells[0].curve.size()), 3);
  QCOMPARE(req.wells[0].curve[0].md, 1000.0);
  QVERIFY(std::fabs(req.wells[0].curve[0].value - 8.5) < 1e-6);
  QVERIFY(std::fabs(req.wells[0].curve[1].value - 9.5) < 1e-6);
  QVERIFY(std::fabs(req.wells[0].curve[2].value - 10.5) < 1e-6);
}

void TestWellLogConsumers::sectionworkbench_showsNonPrimaryCurve()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString fileA = dir.filePath(QStringLiteral("fileA.las"));
  const QString fileB = dir.filePath(QStringLiteral("fileB.las"));
  const QVector<double> depth{1000, 1001, 1002};
  QVERIFY(writeLas(fileA, {{QStringLiteral("DEPT"), depth},
                           {QStringLiteral("GR"), {40, 50, 60}}}));
  QVERIFY(writeLas(fileB, {{QStringLiteral("DEPT"), depth},
                           {QStringLiteral("SP"), {1, 2, 3}}}));

  DataCatalog catalog;
  QVERIFY(catalog.open(dir.path()));
  QString err;
  QVERIFY2(addWellLogs(&catalog, QStringLiteral("well-U"), 100, 200,
                       {{QStringLiteral("ast-a"), QStringLiteral("ver-a"), fileA, true, 0},
                        {QStringLiteral("ast-b"), QStringLiteral("ver-b"), fileB, false, 1}},
                       &err),
           qPrintable(err));

  SectionWorkbench bench(&catalog);
  const std::vector<seismic::SectionWellInfo> wells = bench.sectionWells();
  QCOMPARE(static_cast<int>(wells.size()), 1);
  QVERIFY(!wells[0].alignmentStatus.contains(QStringLiteral("深度单位未知")));
  QStringList names;
  for (const seismic::WellCurveItem &curve : wells[0].curves)
    names.append(curve.curveName);
  QVERIFY2(names.contains(QStringLiteral("GR")), qPrintable(names.join(QLatin1Char(','))));
  bool sawSp = false;
  for (const QString &name : names)
  {
    if (name.compare(QStringLiteral("SP"), Qt::CaseInsensitive) == 0 ||
        name.startsWith(QStringLiteral("SP@"), Qt::CaseInsensitive))
      sawSp = true;
  }
  QVERIFY2(sawSp, qPrintable(names.join(QLatin1Char(','))));
  QCOMPARE(names.size(), 2);
}

QTEST_MAIN(TestWellLogConsumers)
#include "tst_welllog_consumers.moc"
