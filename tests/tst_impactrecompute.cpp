// G05 空间影响分析 + G09 受控重算闭环：输入版本变化的确定性像元 diff、
// 约束编辑的潜在影响、重算计划/执行/新旧对比，以及历史版本不被改写。
#include "../src/catalog/datacatalog.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/metadata/layermanifest.h"
#include "../src/qgis/mappingartifactwriter.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/workflow/workflows.h"
#include "../src/workflow/mappingworkbench.h"
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
#include <gdal.h>
#include <qgsapplication.h>
#include <qgsfeatureiterator.h>
#include <qgsgeometry.h>
#include <qgsvectorlayer.h>
#include <qgsrasterlayer.h>
#include <memory>

class TestImpactRecompute : public QObject {
  Q_OBJECT
  struct Fixture {
    QTemporaryDir dir;
    DataCatalog catalog;
    QgisProjectService project;
    PaleoProjectStore store;
    LayerManifest manifest{dir.filePath("project.sqlite")};
    QgisLayerService layers{&project, &manifest};
    QgisProcessingService processing{&store};
    ConstraintWorkflow constraints{&processing, &layers};
    MappingWorkbench work{&layers, &processing, &project, &constraints};
    bool init() {
      if (!catalog.open(dir.path()) ||
          !project.createProject(dir.filePath("project.qgz")) ||
          !manifest.open())
        return false;
      store.setProjectPaths(project.projectPath(), dir.filePath("project.gpkg"),
                            dir.filePath("project.sqlite"));
      constraints.setStore(&store);
      constraints.setCatalog(&catalog, dir.path());
      project.setDeclarationProvider(
          [this](QVector<LayerDeclaration> *out, QString *error) {
            return layers.tryDeclared(out, error);
          });
      processing.setProject(project.project());
      work.bindCatalog(&catalog, dir.path());
      return true;
    }
    QString latest(const QString &kind) {
      QString found;
      int ver = 0;
      for (const auto &row : work.products("D61")) {
        auto r = row.toMap();
        if (r.value("kind") == kind && r.value("version").toInt() > ver) {
          found = r.value("id").toString();
          ver = r.value("version").toInt();
        }
      }
      return found;
    }
    bool declarePoints(const QVariantList &points, const QString &fileName,
                       QString *error) {
      const auto source = dir.filePath(fileName);
      if (!MappingArtifactWriter::points(source, points, error))
        return false;
      LayerDeclaration d;
      d.layerId = "wells.D61";
      d.horizon = "D61";
      d.type = "vector";
      d.source = source;
      d.title = QStringLiteral("样点");
      return layers.declare(d, error);
    }
  };
private slots:
  void deterministicImpactPlanAndControlledRecompute() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    QVERIFY(f.declarePoints({QVariantMap{{"x", 0}, {"y", 0}, {"z", 0}},
                             QVariantMap{{"x", 100}, {"y", 0}, {"z", 20}},
                             QVariantMap{{"x", 0}, {"y", 100}, {"z", 40}},
                             QVariantMap{{"x", 100}, {"y", 100}, {"z", 60}}},
                            "samples-a.geojson", &error));
    QVERIFY2(f.work.generateFactor(
                  "D61", "sandthick",
                  {{"pointsLayerId", "wells.D61"}, {"field", "z"}, {"cellSize", 10}},
                  &error),
             qPrintable(error));
    const auto factorV1 = f.latest("single_factor_raster");
    QVERIFY(!factorV1.isEmpty());
    const auto factorV1Version = f.work.versionForLayer(factorV1);
    const auto polygonsV1 =
        f.work.compose("D61", {factorV1}, {{"thresholds", QVariantList{20., 40.}}},
                       &error);
    QVERIFY2(!polygonsV1.isEmpty(), qPrintable(error));
    const auto composedV1 = f.work.versionForLayer(polygonsV1).parentVersionIds.first();
    const auto composedBefore = f.catalog.versionById(composedV1);

    // 输入版本更新：z 整体抬高 → 同阈值下相分布改变。
    QVERIFY(f.declarePoints({QVariantMap{{"x", 0}, {"y", 0}, {"z", 100}},
                             QVariantMap{{"x", 100}, {"y", 0}, {"z", 100}},
                             QVariantMap{{"x", 0}, {"y", 100}, {"z", 100}},
                             QVariantMap{{"x", 100}, {"y", 100}, {"z", 100}}},
                            "samples-b.geojson", &error));
    QVERIFY2(f.work.generateFactor(
                  "D61", "sandthick",
                  {{"pointsLayerId", "wells.D61"}, {"field", "z"}, {"cellSize", 10}},
                  &error),
             qPrintable(error));
    const auto factorV2 = f.latest("single_factor_raster");
    QVERIFY(factorV2 != factorV1);
    const auto factorV2Version = f.work.versionForLayer(factorV2);
    QVERIFY(factorV2Version.assetId == factorV1Version.assetId);

    // —— 影响分析：确定性像元 diff ——
    const auto impact = f.work.analyzeImpact(
        {{"kind", "input_version"},
         {"before_version_id", factorV1Version.id},
         {"after_version_id", factorV2Version.id},
         {"horizon", "D61"}},
        &error);
    QVERIFY2(!impact.isEmpty(), qPrintable(error));
    QCOMPARE(impact.value("status").toString(), QStringLiteral("ok"));
    QCOMPARE(impact.value("analysis_method").toString(),
             QStringLiteral("deterministic_cell_replay"));
    // 直接依赖包含融合成果（计算血缘事实）。
    bool directHasComposed = false;
    for (const auto &value : impact.value("direct_products").toList())
      if (value.toMap().value("version_id").toString() == composedV1)
        directHasComposed = true;
    QVERIFY(directHasComposed);
    // 确定性结果：变化像元非零且小于总像元；受影响图斑非空；范围在网格内。
    const auto determined = impact.value("determined").toList();
    QCOMPARE(determined.size(), 1);
    const auto entry = determined.first().toMap();
    QCOMPARE(entry.value("product_version").toString(), composedV1);
    QVERIFY(entry.value("changed_cells").toLongLong() > 0);
    QVERIFY(entry.value("changed_cells").toLongLong() <
            entry.value("total_cells").toLongLong());
    const auto extent = entry.value("changed_extent").toList();
    QCOMPARE(extent.size(), 4);
    QVERIFY(extent[0].toDouble() <= extent[2].toDouble());
    QVERIFY(!entry.value("affected_features").toList().isEmpty());
    // 报告快照落盘且可解析。
    QVERIFY(QFile::exists(impact.value("report_path").toString()));
    QFile report(impact.value("report_path").toString());
    QVERIFY(report.open(QIODevice::ReadOnly));
    QVERIFY(!QJsonDocument::fromJson(report.readAll()).isNull());

    // —— 历史版本不可改写：分析后旧版本 extra/参数原样 ——
    const auto composedAfterAnalysis = f.catalog.versionById(composedV1);
    QCOMPARE(composedAfterAnalysis.extra.value("thresholds").toList(),
             composedBefore.extra.value("thresholds").toList());
    QCOMPARE(composedAfterAnalysis.sha256, composedBefore.sha256);

    // —— 重算计划：local_rerun + 冻结/当前输入对照 + 保护口径 ——
    const auto plan = f.work.recomputePlan(impact, &error);
    QVERIFY2(!plan.isEmpty(), qPrintable(error));
    QCOMPARE(plan.value("status").toString(), QStringLiteral("ok"));
    const auto entries = plan.value("entries").toList();
    QCOMPARE(entries.size(), 1);
    const auto planEntry = entries.first().toMap();
    QCOMPARE(planEntry.value("mode").toString(), QStringLiteral("local_rerun"));
    QCOMPARE(planEntry.value("target_version").toString(), composedV1);
    const auto planInputs = planEntry.value("inputs").toList();
    QCOMPARE(planInputs.size(), 1);
    QCOMPARE(planInputs.first().toMap().value("frozen_version").toString(),
             factorV1Version.id);
    QCOMPARE(planInputs.first().toMap().value("current_version").toString(),
             factorV2Version.id);
    QVERIFY(planEntry.value("parameters").toMap().contains("thresholds"));
    QVERIFY(planEntry.value("protection").toMap().contains("policy"));

    // —— 受控重算：生成新版本，历史不动 ——
    const auto execution = f.work.executeRecompute(plan, &error);
    const auto results = execution.value("results").toList();
    QCOMPARE(results.size(), 1);
    QVERIFY2(results.first().toMap().value("ok").toBool(),
             qPrintable(results.first().toMap().value("error").toString()));
    const auto polygonsV2 = f.latest("facies_polygons");
    QVERIFY(!polygonsV2.isEmpty());
    QVERIFY(f.work.versionForLayer(polygonsV2).parentVersionIds.first() != composedV1);
    // 旧版本仍在目录且未变。
    QVERIFY(!f.catalog.versionById(composedV1).id.isEmpty());
    QCOMPARE(f.catalog.versionById(composedV1).sha256, composedBefore.sha256);

    // —— 新旧对比：像元级转移矩阵 ——
    const auto composedV2 = f.work.versionForLayer(polygonsV2).parentVersionIds.first();
    const auto comparison = f.work.compareVersions(composedV1, composedV2, &error);
    QVERIFY2(!comparison.isEmpty(), qPrintable(error));
    QCOMPARE(comparison.value("method").toString(), QStringLiteral("cell_diff"));
    QVERIFY(comparison.value("cells_changed").toLongLong() > 0);
    QVERIFY(comparison.value("total_cells").toLongLong() > 0);
    QVERIFY(!comparison.value("code_matrix").toList().isEmpty());
    const auto polygonCounts = comparison.value("polygon_counts").toMap();
    QVERIFY(polygonCounts.value("before").toInt() > 0);
    QVERIFY(polygonCounts.value("after").toInt() > 0);
  }
  void constraintChangeYieldsPotentialImpactAndUnusedIsHonest() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    QVERIFY(f.declarePoints({QVariantMap{{"x", 0}, {"y", 0}, {"z", 10}},
                             QVariantMap{{"x", 100}, {"y", 100}, {"z", 60}}},
                            "samples.geojson", &error));
    QVERIFY2(f.constraints.addConstraint("D61", "LINESTRING(50 0,50 100)",
                                         "line", -1, &error),
             qPrintable(error));
    QVERIFY2(f.work.generateFactor(
                  "D61", "sandthick",
                  {{"pointsLayerId", "wells.D61"}, {"field", "z"}, {"cellSize", 10}},
                  &error),
             qPrintable(error));
    const auto factor = f.latest("single_factor_raster");
    QVERIFY(!factor.isEmpty());
    const auto factorVersion = f.work.versionForLayer(factor);
    QVERIFY(factorVersion.extra.contains("constraints_consumed"));
    QVERIFY(!factorVersion.extra.value("constraints_consumed").toList().isEmpty());
    const auto polygons = f.work.compose("D61", {factor},
                                         {{"thresholds", QVariantList{20., 40.}}},
                                         &error);
    QVERIFY2(!polygons.isEmpty(), qPrintable(error));
    const auto composed = f.work.versionForLayer(polygons).parentVersionIds.first();

    // 消费过约束的单因素 + 其下游融合成果：均为潜在影响（需重算因素后才能
    // 确定空间范围——不伪造局部化）。
    const auto impact = f.work.analyzeImpact(
        {{"kind", "constraint_edited"}, {"horizon", "D61"},
         {"constraint_ids", QStringList{"nonexistent-constraint"}}},
        &error);
    QVERIFY2(!impact.isEmpty(), qPrintable(error));
    QCOMPARE(impact.value("status").toString(), QStringLiteral("no_impact"));
    QVERIFY(!impact.value("message").toString().isEmpty());

    const auto impactAll = f.work.analyzeImpact(
        {{"kind", "constraint_edited"}, {"horizon", "D61"}}, &error);
    QCOMPARE(impactAll.value("analysis_method").toString(),
             QStringLiteral("lineage_potential"));
    QVERIFY(!impactAll.value("direct_products").toList().isEmpty());
    bool composedPotential = false;
    for (const auto &value : impactAll.value("potential_products").toList())
      if (value.toMap().value("version_id").toString() == composed)
        composedPotential = true;
    QVERIFY(composedPotential);
    QVERIFY(impactAll.value("determined").toList().isEmpty());

    // 潜在影响路径的重算计划：whole_rerun，不伪装成局部重算。
    const auto plan = f.work.recomputePlan(impactAll, &error);
    const auto entries = plan.value("entries").toList();
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().toMap().value("mode").toString(),
             QStringLiteral("whole_rerun"));
  }
};
int main(int argc, char **argv) {
  QgsApplication app(argc, argv, true);
  app.setPrefixPath("/usr", true);
  app.initQgis();
  GDALAllRegister();
  TestImpactRecompute test;
  const int result = QTest::qExec(&test, argc, argv);
  QgsApplication::exitQgis();
  return result;
}
#include "tst_impactrecompute.moc"
