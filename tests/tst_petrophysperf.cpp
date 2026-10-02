// 层：数据（测试壳位于 tests/，被测对象为 services 批处理任务面；性能档案）
#include <QtTest>

#include <QDir>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "io/perffixtures.h"
#include "metadata/paleoprojectstore.h"
#include "services/paleotaskservice.h"
#include "services/petrophyscomputeservice.h"

#include <cmath>

using paleo::petrophys::PetroPhysTaskService;
using Formula = PetroPhysTaskService::Formula;

// 20 井实测口径（Oracle#6）：合成井 = 真工区 A1.Las 同形状（DEPT/GR/DT/
// RHOB/NPHI 五道、15,580 行 ≈ 252.57→2200.07m @0.125m）。绝对毫秒禁断言
// （README 纪律）——BASELINE qInfo 行供 docs/progress 誊表；机器无关护栏
// 只有比率门：Σcompute < Σparse（逐点公式核毫秒级，解析是主导成本；
// 真回归（表达式引擎退化/核内多余拷贝）给 O(rows) 级反转）。
class TestPetroPhysPerf : public QObject
{
  Q_OBJECT
private slots:
  void testTwentyWellBatch();
  void testRealA1Smoke();
};

void TestPetroPhysPerf::testTwentyWellBatch()
{
  QTemporaryDir dir;
  constexpr int kWells = 20;
  constexpr int kRows = 15580; // A1.Las 同形状（0.125m 步长全井段）

  PetroPhysTaskService::BatchRequest r;
  for (int i = 0; i < kWells; ++i)
  {
    const QString p = QStringLiteral("%1/W%2.las").arg(dir.path()).arg(i, 2, 10, QLatin1Char('0'));
    QVERIFY2(PerfFixtures::makeSyntheticLas(p, kRows),
             "synthetic well fixture");
    r.wells.append({QStringLiteral("well-W%1").arg(i, 2, 10, QLatin1Char('0')), p,
                    QString()});
  }
  r.formula = Formula::VshGrLinear; // auto 基线
  r.outputMnemonic = QStringLiteral("VSH");
  r.qcBandEnabled = true;
  r.writeProduct = true;

  PaleoProjectStore store;
  PaleoTaskService tasks(&store);
  PetroPhysTaskService svc(&tasks, &store);

  QElapsedTimer wall;
  bool done = false;
  PetroPhysTaskService::BatchResult batch;
  wall.start();
  PaleoTask *task = svc.startBatch(r, nullptr, QStringLiteral("%1/out").arg(dir.path()),
                                   [&](bool, const PetroPhysTaskService::BatchResult &res)
                                   {
                                     done = true;
                                     batch = res;
                                   });
  QVERIFY(task);
  QSignalSpy finishedSpy(task, &PaleoTask::finished);
  QVERIFY(finishedSpy.wait(120000));
  const double wallMs = double(wall.nsecsElapsed()) / 1.0e6;
  QVERIFY(done);

  QCOMPARE(batch.wells.size(), kWells);
  QCOMPARE(batch.succeeded, kWells);
  double sumParse = 0.0, sumCompute = 0.0;
  qint64 rows = 0;
  for (const auto &w : batch.wells)
  {
    QVERIFY2(w.ok, qPrintable(w.error));
    sumParse += w.parseMs;
    sumCompute += w.computeMs;
    rows += w.stats.n;
    QVERIFY(QFile::exists(w.productPath));
  }
  QCOMPARE(rows, qint64(kWells) * kRows);

  qInfo("BASELINE petrophys_20well_wall_ms = %.1f", wallMs);
  qInfo("BASELINE petrophys_20well_parse_ms_sum = %.1f", sumParse);
  qInfo("BASELINE petrophys_20well_compute_ms_sum = %.1f", sumCompute);
  qInfo("BASELINE petrophys_20well_rows_per_ms = %.1f",
        double(rows) / qMax(wallMs, 0.001));

  // 比率门：逐点核必须远低于解析成本（机器无关；真回归 O(rows) 反转）
  QVERIFY2(sumCompute < sumParse,
           qPrintable(QString("compute %1 ms should be < parse %2 ms")
                          .arg(sumCompute)
                          .arg(sumParse)));
}

void TestPetroPhysPerf::testRealA1Smoke()
{
#ifdef PROJECT_FIXTURE_DIR
  const QString las = QStringLiteral(PROJECT_FIXTURE_DIR) + QStringLiteral("/A1.Las");
  if (!QFile::exists(las))
    QSKIP("A1.Las not present");
  QTemporaryDir dir;
  PetroPhysTaskService::BatchRequest r;
  r.wells.append({QStringLiteral("well-A1"), las, QString()});
  r.formula = Formula::PhiDensity; // DEN→RHOB canonical；ρma 2.65/ρf 1.0
  r.params.rhoMa = 2.65;
  r.params.rhoFluid = 1.0;
  r.outputMnemonic = QStringLiteral("PHID");
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
  QSignalSpy finishedSpy(task, &PaleoTask::finished);
  QVERIFY(finishedSpy.wait(60000));
  QVERIFY(done);
  QVERIFY2(batch.wells.at(0).ok, qPrintable(batch.wells.at(0).error));
  // A1.Las 是脱敏桩：数据段全部列恒 -99999（NULL token）——真数据边缘：
  // DEN→RHOB canonical 命中真头 + 全空列诚实传播为全 NaN（valid=0）。
  qInfo("BASELINE petrophys_a1_real_rows = %d", batch.wells.at(0).stats.n);
  qInfo("BASELINE petrophys_a1_real_nulls = %d", batch.wells.at(0).stats.nulls);
  qInfo("BASELINE petrophys_a1_real_parse_ms = %.1f", batch.wells.at(0).parseMs);
  qInfo("BASELINE petrophys_a1_real_compute_ms = %.1f", batch.wells.at(0).computeMs);
  QVERIFY(batch.wells.at(0).stats.n > 0);
  QCOMPARE(batch.wells.at(0).stats.valid, 0);
  QCOMPARE(batch.wells.at(0).stats.nulls, batch.wells.at(0).stats.n);
  QVERIFY(std::isnan(batch.wells.at(0).values.constFirst()));
#else
  QSKIP("PROJECT_FIXTURE_DIR not defined");
#endif
}

QTEST_MAIN(TestPetroPhysPerf)
#include "tst_petrophysperf.moc"
