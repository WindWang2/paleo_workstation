// 层：测试壳（真机数据门控）
#include <QtTest>

#include "catalog/datacatalog.h"
#include "services/projectdata.h"
#include "workflow/sectionworkbench.h"
#include "workflow/welltrajectorylayer.h"

// goal/well-trajectory 轮5：真工区回退面实测（Oracle 第 5 条）——
// 真工区（966MB，井位/LAS/分层/时深，无测斜数据）上：
//   1. 每口井 trajectoryFor → nullopt（无链接不是错误，lastError 空）；
//   2. 平面轨迹线层产出为空（不造数据）；
//   3. 剖面 sectionWells 每口井 trajectory 空（垂直简化保持）。
// 全程只读。PALEO_REAL_PROJECT_AREA 未设置时 QSKIP（CI 不红）。
class tst_deviation_realarea : public QObject
{
  Q_OBJECT

private slots:
  void realAreaAllWellsFallBackToVertical();
};

void tst_deviation_realarea::realAreaAllWellsFallBackToVertical()
{
  const QString area = qEnvironmentVariable("PALEO_REAL_PROJECT_AREA");
  if (area.isEmpty())
    QSKIP("PALEO_REAL_PROJECT_AREA not set — real-area deviation fallback skipped");

  DataCatalog catalog;
  QString err;
  QVERIFY2(catalog.open(area, &err), qPrintable(err));
  const int nWells = catalog.entities(QStringLiteral("well")).size();
  QVERIFY2(nWells > 0, "真工区应已有井实体（先跑导入 smoke）");
  qInfo() << "real-area wells:" << nWells;

  ProjectDataFacade facade;
  QVERIFY(facade.setProjectDir(area));

  int nUnlinked = 0;
  int nBroken = 0;
  for (const CatalogEntity &e : catalog.entities(QStringLiteral("well")))
  {
    const auto survey = facade.trajectoryFor(e.id);
    if (!survey)
    {
      ++nUnlinked;
      if (!facade.lastError().isEmpty())
        ++nBroken; // 链接在但坏——真工区不应出现（出现了要如实暴露）
    }
  }
  QCOMPARE(nUnlinked, nWells); // 真工区无测斜数据：全井 nullopt 回退
  QCOMPARE(nBroken, 0);

  // 平面轨迹线层：空产出（无 error——「没有测斜」不是坏数据）。
  QString geoErr;
  QVERIFY(paleo::wellTrajectoriesGeoJson(&facade, &catalog, &geoErr).isEmpty());
  QVERIFY(geoErr.isEmpty());

  // 剖面消费面：每口井 trajectory 空（视图保持垂直简化路径）。
  SectionWorkbench bench(&catalog);
  const std::vector<seismic::SectionWellInfo> wells = bench.sectionWells();
  QVERIFY(!wells.empty());
  for (const seismic::SectionWellInfo &w : wells)
    QVERIFY2(w.trajectory.empty(),
             qPrintable(QStringLiteral("井 %1 不应有轨迹").arg(w.wellName)));

  qInfo() << "real-area deviation fallback: all" << nWells
          << "wells vertical (no trajectory links)";
}

QTEST_MAIN(tst_deviation_realarea)
#include "tst_deviation_realarea.moc"
