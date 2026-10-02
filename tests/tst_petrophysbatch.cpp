// 层：数据（测试壳位于 tests/，被测对象为 services 批处理任务面）
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
#include "io/lasparser.h"
#include "io/perffixtures.h"
#include "metadata/paleoprojectstore.h"
#include "services/paleotaskservice.h"
#include "services/petrophyscomputeservice.h"

using paleo::petrophys::PetroPhysTaskService;
using Formula = PetroPhysTaskService::Formula;
static constexpr double kNan = std::numeric_limits<double>::quiet_NaN();

namespace
{
// 手工 LAS（值已知，供解析断言）：curves[0] 为 DEPT；NaN 写 -999.25。
bool makeHandLas(const QString &path,
                 const QVector<QPair<QString, QVector<double>>> &curves)
{
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
    return false;
  QTextStream ts(&f);
  ts << "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nNULL. -999.25:\n";
  ts << "~Curve\n";
  for (const auto &c : curves)
    ts << c.first << ".M :\n";
  ts << "~ASCII\n";
  const qsizetype n = curves.first().second.size();
  for (qsizetype i = 0; i < n; ++i)
  {
    for (qsizetype c = 0; c < curves.size(); ++c)
    {
      const double v = curves.at(c).second.at(i);
      if (c)
        ts << " ";
      ts << (std::isnan(v) ? QStringLiteral("-999.25") : QString::number(v, 'f', 4));
    }
    ts << "\n";
  }
  return true;
}

// GR 井：i=0..n-1，GR = 20 + 8i（IGR 线性上升，含 100+ 越上界段），
// k=missingAt 处 NaN。
QVector<QPair<QString, QVector<double>>> grWellCurves(int n, int missingAt = -1)
{
  QVector<double> dept(n), gr(n);
  for (int i = 0; i < n; ++i)
  {
    dept[i] = 1000.0 + 0.125 * i;
    gr[i] = 20.0 + 8.0 * i;
  }
  if (missingAt >= 0 && missingAt < n)
    gr[missingAt] = kNan;
  return {{QStringLiteral("DEPT"), dept}, {QStringLiteral("GR"), gr}};
}

PetroPhysTaskService::FormulaParams vshParams()
{
  PetroPhysTaskService::FormulaParams p;
  p.grAutoBaseline = false;
  p.grMin = 20.0;
  p.grMax = 100.0;
  return p;
}

} // namespace

// 服务面断言口径：数值核正确性已在 tst_petrophys/tst_curveexpr 证明，本测
// 试聚焦编排契约——进度单调、取消协作、产物落盘走写队列、catalog DERIVED
// 可查、QC 统计/异常区间入结果。等待用 QSignalSpy::wait（事件循环驱动）。
class TestPetroPhysBatch : public QObject
{
  Q_OBJECT
private slots:
  void testValidationRejects();
  void testBatchComputeProductAndCatalog();
  void testProgressMonotonic();
  void testCancelCooperative();
  void testExpressionBatch();
  void testQcBandAndStats();
  void testArchieInlineDensity();
  void testResolveWellLas();
};

void TestPetroPhysBatch::testValidationRejects()
{
  PaleoProjectStore store;
  PaleoTaskService tasks(&store);
  PetroPhysTaskService svc(&tasks, &store);

  auto expectReject = [&](PetroPhysTaskService::BatchRequest r, const char *what)
  {
    bool called = false;
    PaleoTask *t = svc.startBatch(
        r, nullptr, QString(),
        [&](bool ok, const PetroPhysTaskService::BatchResult &) { called = ok; });
    QVERIFY2(t == nullptr && !called, what);
  };

  PetroPhysTaskService::BatchRequest r;
  r.wells.append({QStringLiteral("well-A"), QStringLiteral("/tmp/a.las"), QString()});
  r.outputMnemonic = QStringLiteral("VSH");
  r.formula = Formula::VshGrLinear;
  r.params = vshParams();

  PetroPhysTaskService::BatchRequest bad = r;
  bad.wells.clear();
  expectReject(bad, "empty wells must reject");

  bad = r;
  bad.outputMnemonic = QStringLiteral("V SH");
  expectReject(bad, "mnemonic with space must reject");

  bad = r;
  bad.params.grAutoBaseline = false;
  bad.params.grMin = kNan;
  expectReject(bad, "missing grMin must reject");

  bad = r;
  bad.formula = Formula::PhiDensity;
  expectReject(bad, "missing rho params must reject");

  bad = r;
  bad.formula = Formula::Expression;
  bad.expression = QString();
  expectReject(bad, "empty expression must reject");

  bad = r;
  bad.formula = Formula::SwArchie;
  bad.params.swPorosityMnemonic = QString();
  expectReject(bad, "archie without phi source must reject");
}

void TestPetroPhysBatch::testBatchComputeProductAndCatalog()
{
  QTemporaryDir dir;
  const QString projDir = dir.path();
  QDir(projDir).mkpath(QStringLiteral("artifacts/metadata"));
  QDir(projDir).mkpath(QStringLiteral("las"));
  QDir(projDir).mkpath(QStringLiteral("out"));

  // 三口手值井：GR = 20+8i（i<10），10 行
  QStringList wells = {QStringLiteral("well-A"), QStringLiteral("well-B"),
                       QStringLiteral("well-C")};
  for (const QString &w : wells)
    QVERIFY(makeHandLas(QStringLiteral("%1/las/%2.las").arg(projDir, w),
                        grWellCurves(10)));

  // catalog：井实体 + well_log RAW 资产/版本/主链接（import 先例的最小同构）
  DataCatalog catalog;
  QVERIFY(catalog.open(projDir));
  for (const QString &w : wells)
  {
    CatalogEntity e;
    e.id = w;
    e.entityType = QStringLiteral("well");
    e.name = w.mid(5);
    QString err;
    QVERIFY2(catalog.addEntity(e, &err), qPrintable(err));
    const QString assetId = catalog.nextAssetId();
    CatalogAsset a;
    a.id = assetId;
    a.type = QStringLiteral("well_log");
    a.format = QStringLiteral("las");
    a.displayName = w + QStringLiteral(".las");
    QVERIFY(catalog.addAsset(a, &err));
    CatalogVersion v;
    v.id = catalog.nextVersionId();
    v.assetId = assetId;
    v.stage = QStringLiteral("RAW");
    v.versionNumber = 1;
    v.managed = false;
    v.path = QStringLiteral("%1/las/%2.las").arg(projDir, w);
    v.fileName = w + QStringLiteral(".las");
    QVERIFY(catalog.addVersion(v, &err));
    EntityAssetLink l;
    l.entityType = QStringLiteral("well");
    l.entityId = w;
    l.assetId = assetId;
    l.role = QStringLiteral("well_log");
    l.isPrimary = true;
    QVERIFY(catalog.addLink(l, &err));
    (void)err;
  }

  PaleoProjectStore store;
  QSignalSpy writeSpy(&store, &PaleoProjectStore::writeCompleted);
  PaleoTaskService tasks(&store);
  PetroPhysTaskService svc(&tasks, &store);

  PetroPhysTaskService::BatchRequest r;
  for (const QString &w : wells)
    r.wells.append({w, QStringLiteral("%1/las/%2.las").arg(projDir, w), QString()});
  r.formula = Formula::VshGrLinear;
  r.params = vshParams();
  r.outputMnemonic = QStringLiteral("VSH");
  r.outputUnit = QStringLiteral("v/v");
  r.qcBandEnabled = true;
  r.qcLo = 0.0;
  r.qcHi = 1.0;

  bool done = false;
  PetroPhysTaskService::BatchResult batch;
  PaleoTask *task = svc.startBatch(
      r, &catalog, QStringLiteral("%1/out").arg(projDir),
      [&](bool ok, const PetroPhysTaskService::BatchResult &res)
      {
        done = true;
        batch = res;
        QVERIFY(ok);
      });
  QVERIFY(task != nullptr);
  QSignalSpy finishedSpy(task, &PaleoTask::finished);
  QVERIFY(finishedSpy.wait(30000));
  QVERIFY(done);

  // 逐井：结果 + 产物 + 登记
  QCOMPARE(batch.wells.size(), 3);
  QCOMPARE(batch.succeeded, 3);
  QCOMPARE(batch.failed, 0);
  for (const auto &w : batch.wells)
  {
    QVERIFY2(w.ok, qPrintable(w.error));
    QCOMPARE(w.values.size(), 10);
    // 解析断言：IGR = (20+8i−20)/80 钳 [0,1] = i/10（i≤9 全在内域）
    for (int i = 0; i < 10; ++i)
    {
      const double want = i / 10.0;
      QVERIFY2(std::fabs(w.values.at(i) - want) < 1e-9,
               qPrintable(QString("%1 row %2: %3 vs %4")
                              .arg(w.wellId)
                              .arg(i)
                              .arg(w.values.at(i))
                              .arg(want)));
    }
    QVERIFY(QFile::exists(w.productPath));

    // 产物读回闭环：DEPT + VSH，值一致（NaN round-trip 经 -999.25）
    const LasDoc doc = LasParser::parseDoc(w.productPath);
    QVERIFY2(doc.ok, qPrintable(doc.error));
    const QStringList wantNames{QStringLiteral("DEPT"), QStringLiteral("VSH")};
    QVERIFY2(doc.curveNames == wantNames, qPrintable(doc.curveNames.join(u',')));
    QCOMPARE(doc.curves.at(1).values.size(), 10);
    for (int i = 0; i < 10; ++i)
      QVERIFY2(std::fabs(doc.curves.at(1).values.at(i) - i / 10.0) < 1e-6,
               "product value round-trip");

    // catalog DERIVED 可查：资产/版本/父链/井链接
    QVERIFY2(!w.assetId.isEmpty(), qPrintable(w.error));
    QCOMPARE(w.assetId,
             QStringLiteral("petrophys_vsh_lin_%1").arg(w.wellId));
    const CatalogAsset asset = catalog.assetById(w.assetId);
    QCOMPARE(asset.type, QStringLiteral("well_log"));
    const auto versions = catalog.versionsForAsset(w.assetId);
    QCOMPARE(versions.size(), 1);
    QCOMPARE(versions.first().stage, QStringLiteral("DERIVED"));
    QVERIFY(versions.first().extra.value(QStringLiteral("origin")).toString()
            == QStringLiteral("petrophysics"));
    const auto links = catalog.linksForAsset(w.assetId);
    QCOMPARE(links.size(), 1);
    QCOMPARE(links.first().entityId, w.wellId);
    QCOMPARE(links.first().role, QStringLiteral("well_log"));
    QVERIFY(!links.first().isPrimary);
  }

  // 写路径走队列的证据：writeCompleted 信号（enqueueWrite 终态发射）×3
  QVERIFY2(writeSpy.size() >= 3,
           qPrintable(QString("writeCompleted fired %1 times, want >=3").arg(writeSpy.size())));
}

void TestPetroPhysBatch::testProgressMonotonic()
{
  QTemporaryDir dir;
  // 6 口合成井 × 2000 行（PerfFixtures 默认曲线集 DEPT/GR/DT/RHOB/NPHI）
  PetroPhysTaskService::BatchRequest r;
  for (int i = 0; i < 6; ++i)
  {
    const QString p = QStringLiteral("%1/w%2.las").arg(dir.path()).arg(i);
    QVERIFY(PerfFixtures::makeSyntheticLas(p, 2000));
    r.wells.append({QStringLiteral("well-%1").arg(i), p, QString()});
  }
  r.formula = Formula::VshGrLinear; // auto 基线（合成 GR 有分布）
  r.outputMnemonic = QStringLiteral("VSH");

  PaleoProjectStore store;
  PaleoTaskService tasks(&store);
  PetroPhysTaskService svc(&tasks, &store);

  QVector<int> percents;
  bool done = false;
  PaleoTask *task = svc.startBatch(r, nullptr, QString(),
                                   [&](bool, const PetroPhysTaskService::BatchResult &)
                                   { done = true; });
  QVERIFY(task);
  QObject::connect(task, &PaleoTask::changed, [&]()
  {
    percents.append(task->percent());
    QVERIFY2(task->bytesDone() >= 0, "bytes known");
  });
  QSignalSpy finishedSpy(task, &PaleoTask::finished);
  QVERIFY(finishedSpy.wait(60000));
  QVERIFY(done);
  QVERIFY(percents.size() >= 3); // 至少若干次变更
  for (int i = 1; i < percents.size(); ++i)
    QVERIFY2(percents.at(i) >= percents.at(i - 1),
             qPrintable(QString("percent regressed: %1 -> %2")
                            .arg(percents.at(i - 1))
                            .arg(percents.at(i))));
  QCOMPARE(task->state(), PaleoTask::State::Succeeded);
  QCOMPARE(task->percent(), 100);
}

void TestPetroPhysBatch::testCancelCooperative()
{
  QTemporaryDir dir;
  // 40 口 × 8000 行：足够长，井间检查点必然在循环内命中
  PetroPhysTaskService::BatchRequest r;
  for (int i = 0; i < 40; ++i)
  {
    const QString p = QStringLiteral("%1/c%2.las").arg(dir.path()).arg(i);
    QVERIFY(PerfFixtures::makeSyntheticLas(p, 8000));
    r.wells.append({QStringLiteral("well-%1").arg(i), p, QString()});
  }
  r.formula = Formula::VshGrLinear;
  r.outputMnemonic = QStringLiteral("VSH");
  r.writeProduct = false;

  PaleoProjectStore store;
  PaleoTaskService tasks(&store);
  PetroPhysTaskService svc(&tasks, &store);

  bool done = false;
  PetroPhysTaskService::BatchResult batch;
  PaleoTask *task = svc.startBatch(r, nullptr, QString(),
                                   [&](bool, const PetroPhysTaskService::BatchResult &res)
                                   {
                                     done = true;
                                     batch = res;
                                   });
  QVERIFY(task);
  // 等至少一口井完成（detail 有井名）再取消——验证「中途」语义
  QVERIFY(QTest::qWaitFor([&]() { return !task->detailText().isEmpty(); }, 30000));
  task->requestCancel();
  QSignalSpy finishedSpy(task, &PaleoTask::finished);
  QVERIFY(finishedSpy.wait(60000));
  QVERIFY(done);
  QCOMPARE(task->state(), PaleoTask::State::Cancelled);
  QVERIFY(!batch.ok);
  QCOMPARE(batch.error, QStringLiteral("cancelled"));
  QVERIFY2(batch.wells.size() < 40,
           qPrintable(QString("cancel too late: %1 wells done").arg(batch.wells.size())));
  QVERIFY(batch.wells.size() >= 1);
}

void TestPetroPhysBatch::testExpressionBatch()
{
  QTemporaryDir dir;
  // 手值井：RHOB = 2.0+0.1i，NPHI(%) = 10+i；expr = RHOB - 0.05*NPHI
  // → 2.0+0.1i − (10+i)/20 = 1.5 + 0.05i … 井 B 缺 NPHI → 逐井失败路径
  QVector<QPair<QString, QVector<double>>> a;
  {
    const int n = 10;
    QVector<double> dept(n), rb(n), np(n);
    for (int i = 0; i < n; ++i)
    {
      dept[i] = 2000.0 + i;
      rb[i] = 2.0 + 0.1 * i;
      np[i] = 10.0 + i;
    }
    a = {{QStringLiteral("DEPT"), dept}, {QStringLiteral("RHOB"), rb},
         {QStringLiteral("NPHI"), np}};
  }
  QVERIFY(makeHandLas(QStringLiteral("%1/A.las").arg(dir.path()), a));
  {
    QVector<double> dept(10), rb(10);
    for (int i = 0; i < 10; ++i)
    {
      dept[i] = 3000.0 + i;
      rb[i] = 2.5;
    }
    QVERIFY(makeHandLas(QStringLiteral("%1/B.las").arg(dir.path()),
                        {{QStringLiteral("DEPT"), dept}, {QStringLiteral("RHOB"), rb}}));
  }

  PaleoProjectStore store;
  PaleoTaskService tasks(&store);
  PetroPhysTaskService svc(&tasks, &store);

  PetroPhysTaskService::BatchRequest r;
  r.wells.append({QStringLiteral("well-A"),
                  QStringLiteral("%1/A.las").arg(dir.path()), QString()});
  r.wells.append({QStringLiteral("well-B"),
                  QStringLiteral("%1/B.las").arg(dir.path()), QString()});
  r.formula = Formula::Expression;
  r.expression = QStringLiteral("RHOB - 0.05*NPHI");
  r.outputMnemonic = QStringLiteral("EXR");
  r.writeProduct = false;

  bool done = false;
  PetroPhysTaskService::BatchResult batch;
  PaleoTask *task = svc.startBatch(r, nullptr, QString(),
                                   [&](bool, const PetroPhysTaskService::BatchResult &res)
                                   {
                                     done = true;
                                     batch = res;
                                   });
  QVERIFY(task);
  QSignalSpy finishedSpy(task, &PaleoTask::finished);
  QVERIFY(finishedSpy.wait(30000));
  QVERIFY(done);

  QCOMPARE(batch.wells.size(), 2);
  QVERIFY(batch.wells.at(0).ok);
  QVERIFY(!batch.wells.at(1).ok); // B 井缺 NPHI → 未知曲线报错
  QVERIFY2(batch.wells.at(1).error.contains(QStringLiteral("unknown curve")),
           qPrintable(batch.wells.at(1).error));
  for (int i = 0; i < 10; ++i)
  {
    const double want = 1.5 + 0.05 * i;
    QVERIFY2(std::fabs(batch.wells.at(0).values.at(i) - want) < 1e-9,
             qPrintable(QString("expr row %1: %2 vs %3")
                            .arg(i)
                            .arg(batch.wells.at(0).values.at(i))
                            .arg(want)));
  }
  QCOMPARE(batch.succeeded, 1);
  QCOMPARE(batch.failed, 1);
  QVERIFY(!batch.ok);
}

void TestPetroPhysBatch::testQcBandAndStats()
{
  QTemporaryDir dir;
  // DEN 曲线：[2.3, 2.4, 1.4, 1.3, NaN, 2.4, 3.5, 2.3]——qc [1.5,3.0]：
  // 两段越界（行 2-3 / 行 6），NaN 断段；stats 数值手算。
  const QVector<double> dept = {1000, 1001, 1002, 1003, 1004, 1005, 1006, 1007};
  const QVector<double> den = {2.3, 2.4, 1.4, 1.3, kNan, 2.4, 3.5, 2.3};
  QVERIFY(makeHandLas(QStringLiteral("%1/Q.las").arg(dir.path()),
                      {{QStringLiteral("DEPT"), dept}, {QStringLiteral("DEN"), den}}));

  PaleoProjectStore store;
  PaleoTaskService tasks(&store);
  PetroPhysTaskService svc(&tasks, &store);

  PetroPhysTaskService::BatchRequest r;
  r.wells.append({QStringLiteral("well-Q"),
                  QStringLiteral("%1/Q.las").arg(dir.path()), QString()});
  r.formula = Formula::Expression;
  r.expression = QStringLiteral("DEN");
  r.outputMnemonic = QStringLiteral("DENCOPY");
  r.writeProduct = false;
  r.qcBandEnabled = true;
  r.qcLo = 1.5;
  r.qcHi = 3.0;

  bool done = false;
  PetroPhysTaskService::BatchResult batch;
  PaleoTask *task = svc.startBatch(r, nullptr, QString(),
                                   [&](bool, const PetroPhysTaskService::BatchResult &res)
                                   {
                                     done = true;
                                     batch = res;
                                   });
  QVERIFY(task);
  QSignalSpy finishedSpy(task, &PaleoTask::finished);
  QVERIFY(finishedSpy.wait(30000));
  QVERIFY(done);
  QVERIFY(batch.wells.at(0).ok);

  const auto &w = batch.wells.at(0);
  QCOMPARE(w.stats.n, 8);
  QCOMPARE(w.stats.valid, 7);
  QCOMPARE(w.stats.nulls, 1);
  QVERIFY2(std::fabs(w.stats.nullRate - 0.125) < 1e-12, "1/8");
  QCOMPARE(w.stats.min, 1.3);
  QCOMPARE(w.stats.max, 3.5);
  QVERIFY(std::isnan(w.values.at(4))); // null 传播端到端
  QCOMPARE(w.anomalies.size(), 2);
  QCOMPARE(w.anomalies.at(0).fromIndex, 2);
  QCOMPARE(w.anomalies.at(0).toIndex, 3);
  QCOMPARE(w.anomalies.at(0).samples, 2);
  QCOMPARE(w.anomalies.at(0).from, 1002.0);
  QCOMPARE(w.anomalies.at(1).fromIndex, 6); // NaN 断段
  QCOMPARE(w.anomalies.at(1).samples, 1);
}

void TestPetroPhysBatch::testArchieInlineDensity()
{
  QTemporaryDir dir;
  // RHOB = 2.45（φD = (2.65−2.45)/1.65 = 0.2/1.65）、RT = 5：
  // Sw = sqrt(0.1/(φ²·5))；a=1/m=2/n=2/rw=0.1（tst_petrophys 同口径）
  const int n = 4;
  QVector<double> dept(n), rb(n), rt(n);
  for (int i = 0; i < n; ++i)
  {
    dept[i] = 1500.0 + i;
    rb[i] = 2.45;
    rt[i] = 5.0;
  }
  QVERIFY(makeHandLas(QStringLiteral("%1/S.las").arg(dir.path()),
                      {{QStringLiteral("DEPT"), dept}, {QStringLiteral("RHOB"), rb},
                       {QStringLiteral("RT"), rt}}));

  PaleoProjectStore store;
  PaleoTaskService tasks(&store);
  PetroPhysTaskService svc(&tasks, &store);

  PetroPhysTaskService::BatchRequest r;
  r.wells.append({QStringLiteral("well-S"),
                  QStringLiteral("%1/S.las").arg(dir.path()), QString()});
  r.formula = Formula::SwArchie;
  PetroPhysTaskService::FormulaParams p;
  p.rhoMa = 2.65;
  p.rhoFluid = 1.0;
  p.archieA = 1.0;
  p.archieM = 2.0;
  p.archieN = 2.0;
  p.rw = 0.1;
  r.params = p;
  r.outputMnemonic = QStringLiteral("SW");
  r.writeProduct = false;

  bool done = false;
  PetroPhysTaskService::BatchResult batch;
  PaleoTask *task = svc.startBatch(r, nullptr, QString(),
                                   [&](bool, const PetroPhysTaskService::BatchResult &res)
                                   {
                                     done = true;
                                     batch = res;
                                   });
  QVERIFY(task);
  QSignalSpy finishedSpy(task, &PaleoTask::finished);
  QVERIFY(finishedSpy.wait(30000));
  QVERIFY(done);
  QVERIFY2(batch.wells.at(0).ok, qPrintable(batch.wells.at(0).error));
  const double phi = 0.2 / 1.65;
  const double sw = std::sqrt(0.1 / (phi * phi * 5.0));
  for (int i = 0; i < n; ++i)
    QVERIFY2(std::fabs(batch.wells.at(0).values.at(i) - sw) < 1e-9,
             qPrintable(QString("sw %1 vs %2").arg(batch.wells.at(0).values.at(i)).arg(sw)));
}

void TestPetroPhysBatch::testResolveWellLas()
{
  QTemporaryDir dir;
  DataCatalog catalog;
  QVERIFY(catalog.open(dir.path()));
  CatalogEntity e;
  e.id = QStringLiteral("well-R1");
  e.entityType = QStringLiteral("well");
  e.name = QStringLiteral("R1");
  QString err;
  QVERIFY(catalog.addEntity(e, &err));
  const QString assetId = catalog.nextAssetId();
  CatalogAsset a;
  a.id = assetId;
  a.type = QStringLiteral("well_log");
  a.format = QStringLiteral("las");
  QVERIFY(catalog.addAsset(a, &err));
  CatalogVersion v;
  v.id = catalog.nextVersionId();
  v.assetId = assetId;
  v.stage = QStringLiteral("RAW");
  v.versionNumber = 1;
  v.managed = false;
  v.path = QStringLiteral("/data/r1.las");
  QVERIFY(catalog.addVersion(v, &err));
  EntityAssetLink l;
  l.entityType = QStringLiteral("well");
  l.entityId = QStringLiteral("well-R1");
  l.assetId = assetId;
  l.role = QStringLiteral("well_log");
  l.isPrimary = true;
  QVERIFY(catalog.addLink(l, &err));

  QStringList missing;
  const auto refs = PetroPhysTaskService::resolveWellLas(
      &catalog, dir.path(),
      {QStringLiteral("well-R1"), QStringLiteral("well-Ghost")}, &missing);
  QCOMPARE(refs.size(), 1);
  QCOMPARE(refs.first().wellId, QStringLiteral("well-R1"));
  QCOMPARE(refs.first().sourceVersionId, v.id);
  QVERIFY(refs.first().lasPath.contains(QStringLiteral("r1.las")));
  QCOMPARE(missing.size(), 1);
  QVERIFY(missing.first().contains(QStringLiteral("well-Ghost")));
}

QTEST_MAIN(TestPetroPhysBatch)
#include "tst_petrophysbatch.moc"
