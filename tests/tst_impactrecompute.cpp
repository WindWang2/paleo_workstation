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
#include "../src/domain/faciescatalog.h"
#include "../src/workflow/mappingworkbench.h"
#include "../src/services/paleotaskservice.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
#include <atomic>
#include <gdal.h>
#include <qgsapplication.h>
#include <qgsfeatureiterator.h>
#include <qgsgeometry.h>
#include <qgsvectorlayer.h>
#include <qgsrasterlayer.h>
#include <memory>
#include <thread>

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
      QString faciesError;
      return work.saveFacies(QStringLiteral("D61"), FaciesCatalog::defaults().mid(0, 7), &faciesError);
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
        f.work.compose("D61", {factorV1}, {{"thresholds", QVariantList{20., 25., 30., 35., 40., 45.}}},
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
                                         {{"thresholds", QVariantList{20., 25., 30., 35., 40., 45.}}},
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

    // X6：输入尚无新版本时潜在影响条目如实 blocked_upstream，执行不产生
    // 任何新版本（不是把冻结输入伪装成重算结果）。
    const auto planBlocked = f.work.recomputePlan(impactAll, &error);
    const auto blockedEntries = planBlocked.value("entries").toList();
    QCOMPARE(blockedEntries.size(), 1);
    const auto blockedEntry = blockedEntries.first().toMap();
    QCOMPARE(blockedEntry.value("mode").toString(),
             QStringLiteral("blocked_upstream"));
    QVERIFY(!blockedEntry.value("reason").toString().isEmpty());
    QCOMPARE(blockedEntry.value("inputs_changed").toInt(), 0);
    const int composedBeforeBlocked = countKind(f.catalog, "composed_facies");
    const auto blockedRun = f.work.executeRecompute(planBlocked, &error);
    const auto blockedRows = blockedRun.value("results").toList();
    QCOMPARE(blockedRows.size(), 1);
    QVERIFY(!blockedRows.first().toMap().value("ok").toBool());
    QVERIFY(blockedRows.first().toMap().value("skipped").toBool());
    QCOMPARE(countKind(f.catalog, "composed_facies"), composedBeforeBlocked);

    // 上游重算出新版本后：同一影响报告生成的计划转 whole_rerun，执行用
    // 当前输入版本（而非 ordered_inputs 里冻结的旧版本）。
    QVERIFY2(f.work.generateFactor(
                  "D61", "sandthick",
                  {{"pointsLayerId", "wells.D61"}, {"field", "z"}, {"cellSize", 10}},
                  &error),
             qPrintable(error));
    const auto factorV2 = f.latest("single_factor_raster");
    QVERIFY(factorV2 != factor);
    const auto factorV2Version = f.work.versionForLayer(factorV2);
    QCOMPARE(factorV2Version.assetId, factorVersion.assetId);
    const auto plan = f.work.recomputePlan(impactAll, &error);
    const auto entries = plan.value("entries").toList();
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().toMap().value("mode").toString(),
             QStringLiteral("whole_rerun"));
    QCOMPARE(entries.first().toMap().value("inputs_changed").toInt(), 1);
    const auto execution = f.work.executeRecompute(plan, &error);
    const auto rows = execution.value("results").toList();
    QCOMPARE(rows.size(), 1);
    QVERIFY2(rows.first().toMap().value("ok").toBool(),
             qPrintable(rows.first().toMap().value("error").toString()));
    const auto polygonsV2 = f.latest("facies_polygons");
    QVERIFY(polygonsV2 != polygons);
    const auto composedV2 =
        f.work.versionForLayer(polygonsV2).parentVersionIds.first();
    const auto orderedInputs = f.catalog.versionById(composedV2)
                                   .extra.value("ordered_inputs")
                                   .toList();
    QCOMPARE(orderedInputs.size(), 1);
    QCOMPARE(orderedInputs.first().toMap().value("version_id").toString(),
             factorV2Version.id);
  }
  // R1：计划绑定分析时刻的输入版本；分析后输入再更新 → 整计划拒绝。
  void stalePlanBindingIsRejected() {
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
    const auto factorV1Version = f.work.versionForLayer(factorV1);
    QVERIFY(!f.work.compose("D61", {factorV1},
                            {{"thresholds", QVariantList{20., 25., 30., 35., 40., 45.}}},
                            &error)
                 .isEmpty());
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
    const auto factorV2Version =
        f.work.versionForLayer(f.latest("single_factor_raster"));
    const auto impact = f.work.analyzeImpact(
        {{"kind", "input_version"},
         {"before_version_id", factorV1Version.id},
         {"after_version_id", factorV2Version.id},
         {"horizon", "D61"}},
        &error);
    QVERIFY2(!impact.isEmpty(), qPrintable(error));
    const auto plan = f.work.recomputePlan(impact, &error);
    QVERIFY2(!plan.isEmpty(), qPrintable(error));
    QCOMPARE(plan.value("binding").toMap().value("source_after_version").toString(),
             factorV2Version.id);

    // 分析完成后输入第三次更新——报告绑定过期，执行整体拒绝。
    QVERIFY(f.declarePoints({QVariantMap{{"x", 0}, {"y", 0}, {"z", 30}},
                             QVariantMap{{"x", 100}, {"y", 0}, {"z", 30}},
                             QVariantMap{{"x", 0}, {"y", 100}, {"z", 30}},
                             QVariantMap{{"x", 100}, {"y", 100}, {"z", 30}}},
                            "samples-c.geojson", &error));
    QVERIFY2(f.work.generateFactor(
                  "D61", "sandthick",
                  {{"pointsLayerId", "wells.D61"}, {"field", "z"}, {"cellSize", 10}},
                  &error),
             qPrintable(error));
    const int composedCount = countKind(f.catalog, "composed_facies");
    const auto execution = f.work.executeRecompute(plan, &error);
    QVERIFY2(!error.isEmpty(), "过期计划必须拒绝执行");
    QVERIFY(error.contains(QStringLiteral("过期")));
    QVERIFY(execution.value("results").toList().isEmpty());
    QCOMPARE(countKind(f.catalog, "composed_facies"), composedCount);
  }
  // R4：像元级重放证明结果不变 → no_change，执行不产生新版本。
  void identicalInputYieldsNoChangePlan() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    const QVariantList points{QVariantMap{{"x", 0}, {"y", 0}, {"z", 0}},
                              QVariantMap{{"x", 100}, {"y", 0}, {"z", 20}},
                              QVariantMap{{"x", 0}, {"y", 100}, {"z", 40}},
                              QVariantMap{{"x", 100}, {"y", 100}, {"z", 60}}};
    QVERIFY(f.declarePoints(points, "samples-a.geojson", &error));
    QVERIFY2(f.work.generateFactor(
                  "D61", "sandthick",
                  {{"pointsLayerId", "wells.D61"}, {"field", "z"}, {"cellSize", 10}},
                  &error),
             qPrintable(error));
    const auto factorV1 = f.latest("single_factor_raster");
    const auto factorV1Version = f.work.versionForLayer(factorV1);
    QVERIFY(!f.work.compose("D61", {factorV1},
                            {{"thresholds", QVariantList{20., 25., 30., 35., 40., 45.}}},
                            &error)
                 .isEmpty());
    // 同数据再生成一个新版本：值不变 → 分相结果逐像元相同。
    QVERIFY(f.declarePoints(points, "samples-b.geojson", &error));
    QVERIFY2(f.work.generateFactor(
                  "D61", "sandthick",
                  {{"pointsLayerId", "wells.D61"}, {"field", "z"}, {"cellSize", 10}},
                  &error),
             qPrintable(error));
    const auto factorV2Version =
        f.work.versionForLayer(f.latest("single_factor_raster"));
    QCOMPARE(factorV2Version.assetId, factorV1Version.assetId);
    const auto impact = f.work.analyzeImpact(
        {{"kind", "input_version"},
         {"before_version_id", factorV1Version.id},
         {"after_version_id", factorV2Version.id},
         {"horizon", "D61"}},
        &error);
    QVERIFY2(!impact.isEmpty(), qPrintable(error));
    const auto determined = impact.value("determined").toList();
    QCOMPARE(determined.size(), 1);
    QCOMPARE(determined.first().toMap().value("changed_cells").toLongLong(), 0);
    const auto plan = f.work.recomputePlan(impact, &error);
    const auto entries = plan.value("entries").toList();
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().toMap().value("mode").toString(),
             QStringLiteral("no_change"));
    const int composedCount = countKind(f.catalog, "composed_facies");
    const auto execution = f.work.executeRecompute(plan, &error);
    const auto results = execution.value("results").toList();
    QCOMPARE(results.size(), 1);
    QVERIFY(results.first().toMap().value("skipped").toBool());
    QCOMPARE(countKind(f.catalog, "composed_facies"), composedCount);
  }
  // P1-B 0a：定网格栅格输入的新版本网格口径不同（范围/分辨率变化）→
  // 无法像元级证明，退化为 potential，不伪装成 determined。
  void changedInputGridDegradesToPotential() {
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
    const auto factorV1Version = f.work.versionForLayer(factorV1);
    QVERIFY(!f.work.compose("D61", {factorV1},
                            {{"thresholds", QVariantList{20., 25., 30., 35., 40., 45.}}},
                            &error)
                 .isEmpty());
    const auto composed = f.work.versionForLayer(f.latest("facies_polygons"))
                              .parentVersionIds.first();
    // 同资产新版本但范围更大（定网格栅格变了）→ 潜在影响。
    QVERIFY(f.declarePoints({QVariantMap{{"x", -50}, {"y", -50}, {"z", 100}},
                             QVariantMap{{"x", 150}, {"y", 0}, {"z", 100}},
                             QVariantMap{{"x", 0}, {"y", 150}, {"z", 100}},
                             QVariantMap{{"x", 150}, {"y", 150}, {"z", 100}}},
                            "samples-b.geojson", &error));
    QVERIFY2(f.work.generateFactor(
                  "D61", "sandthick",
                  {{"pointsLayerId", "wells.D61"}, {"field", "z"}, {"cellSize", 10}},
                  &error),
             qPrintable(error));
    const auto factorV2Version =
        f.work.versionForLayer(f.latest("single_factor_raster"));
    QCOMPARE(factorV2Version.assetId, factorV1Version.assetId);
    const auto impact = f.work.analyzeImpact(
        {{"kind", "input_version"},
         {"before_version_id", factorV1Version.id},
         {"after_version_id", factorV2Version.id},
         {"horizon", "D61"}},
        &error);
    QVERIFY2(!impact.isEmpty(), qPrintable(error));
    bool foundPotential = false;
    for (const auto &value : impact.value("potential_products").toList())
      foundPotential |= value.toMap().value("version_id").toString() == composed;
    QVERIFY(foundPotential);
    for (const auto &value : impact.value("determined").toList())
      QVERIFY(value.toMap().value("product_version").toString() != composed);
    // 对照：同口径新版本仍是 determined（网格未变才可像元级证明）。
    QVERIFY(f.declarePoints({QVariantMap{{"x", 0}, {"y", 0}, {"z", 0}},
                             QVariantMap{{"x", 100}, {"y", 0}, {"z", 20}},
                             QVariantMap{{"x", 0}, {"y", 100}, {"z", 40}},
                             QVariantMap{{"x", 100}, {"y", 100}, {"z", 60}}},
                            "samples-c.geojson", &error));
    QVERIFY2(f.work.generateFactor(
                  "D61", "sandthick",
                  {{"pointsLayerId", "wells.D61"}, {"field", "z"}, {"cellSize", 10}},
                  &error),
             qPrintable(error));
    const auto factorV3Version =
        f.work.versionForLayer(f.latest("single_factor_raster"));
    const auto sameGrid = f.work.analyzeImpact(
        {{"kind", "input_version"},
         {"before_version_id", factorV1Version.id},
         {"after_version_id", factorV3Version.id},
         {"horizon", "D61"}},
        &error);
    bool foundDetermined = false;
    for (const auto &value : sameGrid.value("determined").toList())
      foundDetermined |= value.toMap().value("product_version").toString() ==
                         composed;
    QVERIFY(foundDetermined);
  }
  // R3：人工复核全链——Confirmed 解释计入保护口径、重算不动历史版本、
  // 新成果不继承解释、幂等续跑、与旧版本可对比。
  void controlledRecomputePreservesHistoryAndIsIdempotent() {
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
    const auto factorV1Version =
        f.work.versionForLayer(f.latest("single_factor_raster"));
    const auto polygonsV1 =
        f.work.compose("D61", {f.latest("single_factor_raster")},
                       {{"thresholds", QVariantList{20., 25., 30., 35., 40., 45.}}}, &error);
    QVERIFY2(!polygonsV1.isEmpty(), qPrintable(error));
    const auto composedV1 =
        f.work.versionForLayer(polygonsV1).parentVersionIds.first();

    // 编辑副本 → 证据 + Confirmed 复核 → 保存 edited_facies 版本。
    const auto draft = f.work.copyForEditing(polygonsV1, {}, &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(draft));
    QVERIFY(layer);
    QgsFeature feature;
    auto it = layer->getFeatures();
    QVERIFY(it.nextFeature(feature));
    it.close();
    QVERIFY(f.work.addEvidence(
        draft, {feature.id()},
        {{"text", QStringLiteral("井资料显示边界需复核")},
         {"role", QStringLiteral("SUPPORTED_BY")},
         {"type", QStringLiteral("well")},
         {"reason", QStringLiteral("人工复核记录")}},
        &error));
    const auto snapshot = f.work.featureEvidence(draft, {feature.id()});
    const auto featureUid = snapshot.value("feature_uid").toString();
    QVERIFY(!featureUid.isEmpty());
    QVERIFY(f.work.changeEvidence(
        draft, {feature.id()}, "review",
        {{"reason", QStringLiteral("复核确认")},
         {"status", QStringLiteral("Confirmed")},
         {"expected_revision",
          snapshot.value("interpretation").toMap().value("revision")}},
        &error));
    QVERIFY(layer->commitChanges());
    QVERIFY2(f.work.saveEditingVersion(draft, &error), qPrintable(error));
    const auto edited = f.work.versionForLayer(draft);
    QCOMPARE(edited.extra.value("kind").toString(),
             QStringLiteral("edited_facies"));
    const QString interpretationBefore =
        interpretationText(f, edited.id, featureUid);
    QVERIFY(!interpretationBefore.isEmpty());

    // 输入整体变化 → 影响分析：受影响图斑含 edited_facies 上的 F1。
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
    const auto factorV2Version =
        f.work.versionForLayer(f.latest("single_factor_raster"));
    const auto impact = f.work.analyzeImpact(
        {{"kind", "input_version"},
         {"before_version_id", factorV1Version.id},
         {"after_version_id", factorV2Version.id},
         {"horizon", "D61"}},
        &error);
    QVERIFY2(!impact.isEmpty(), qPrintable(error));
    const auto determined = impact.value("determined").toList();
    QCOMPARE(determined.size(), 1);
    const auto affected =
        determined.first().toMap().value("affected_features").toList();
    // 同一图斑可能在草稿版本与已保存版本各有一行——钉到保存版本查。
    bool editedFeatureFound = false;
    for (const auto &value : affected) {
      const auto row = value.toMap();
      if (row.value("feature_uid").toString() == featureUid &&
          row.value("kind").toString() == QLatin1String("edited_facies") &&
          row.value("product_version").toString() == edited.id) {
        editedFeatureFound = true;
        QCOMPARE(row.value("review_status").toString(),
                 QStringLiteral("Confirmed"));
        QVERIFY(!row.value("interpretation_id").toString().isEmpty());
      }
    }
    QVERIFY2(editedFeatureFound, "受影响图斑未覆盖 edited_facies 后代");

    // 计划保护口径：Confirmed 计数 + review_required 点名 F1。
    const auto plan = f.work.recomputePlan(impact, &error);
    QVERIFY2(!plan.isEmpty(), qPrintable(error));
    const auto entries = plan.value("entries").toList();
    QCOMPARE(entries.size(), 1);
    const auto protection = entries.first().toMap().value("protection").toMap();
    QVERIFY(protection.value("confirmed_features").toInt() >= 1);
    bool requiredFound = false;
    for (const auto &value : protection.value("review_required").toList())
      requiredFound |= value.toMap().value("feature_uid").toString() ==
                       featureUid;
    QVERIFY(requiredFound);
    QCOMPARE(protection.value("scope").toString(), QStringLiteral("affected"));

    // 执行：新 composed 版本 + 重算血缘；历史版本的解释原样。
    const int composedCount = countKind(f.catalog, "composed_facies");
    const auto execution = f.work.executeRecompute(plan, &error);
    const auto results = execution.value("results").toList();
    QCOMPARE(results.size(), 1);
    QVERIFY2(results.first().toMap().value("ok").toBool(),
             qPrintable(results.first().toMap().value("error").toString()));
    QCOMPARE(countKind(f.catalog, "composed_facies"), composedCount + 1);
    const auto polygonsV2 = f.latest("facies_polygons");
    QVERIFY(polygonsV2 != polygonsV1);
    const auto composedV2 =
        f.work.versionForLayer(polygonsV2).parentVersionIds.first();
    const auto provenance = f.catalog.versionById(composedV2)
                                .extra.value("recompute_provenance")
                                .toMap();
    QCOMPARE(provenance.value("recompute_of").toString(), composedV1);
    // P1 的编辑版本未被改写：同要素解释原文一致。
    QCOMPARE(interpretationText(f, edited.id, featureUid),
             interpretationBefore);
    // 新成果的相面不继承任何人工解释。
    const auto polygonsV2Id = f.work.versionForLayer(polygonsV2).id;
    QVERIFY(!polygonsV2Id.isEmpty());
    auto *polygons2 =
        qobject_cast<QgsVectorLayer *>(f.layers.instantiate(polygonsV2));
    QVERIFY(polygons2);
    QgsFeature fresh;
    auto freshIt = polygons2->getFeatures();
    while (freshIt.nextFeature(fresh))
      QVERIFY(fresh.attribute("paleo_interpretation").toString().isEmpty());

    // 幂等续跑：再执行同计划 → already_done，不产新版本。
    const auto again = f.work.executeRecompute(plan, &error);
    const auto againRows = again.value("results").toList();
    QCOMPARE(againRows.size(), 1);
    QVERIFY(againRows.first().toMap().value("ok").toBool());
    QVERIFY(againRows.first().toMap().value("already_done").toBool());
    QCOMPARE(countKind(f.catalog, "composed_facies"), composedCount + 1);

    // X13：重算成果与旧版本像元对比（此前只在测试外不可达）。
    const auto comparison =
        f.work.compareWithRecomputeSource(polygonsV2, &error);
    QVERIFY2(!comparison.isEmpty(), qPrintable(error));
    QVERIFY(comparison.value("cells_changed").toLongLong() > 0);
    QVERIFY(QFile::exists(comparison.value("report_path").toString()));
    // 非重算产物如实拒绝。
    QVERIFY(f.work.compareWithRecomputeSource(polygonsV1, &error).isEmpty());
    QVERIFY(!error.isEmpty());
  }
  // —— A：影响分析/版本对比后台任务化 ——
  // 覆盖：同步/后台结果等价（小输入）、GUI 事件循环在排队与执行期间保持
  // 响应（受控同步点：占满池的 blocker 任务）、忙拒绝、排队期协作取消、
  // 目录修订前进（旧请求晚到）不发布、切工程重绑不发布、执行期取消、
  // 对象销毁安全、版本对比后台等价 + 取消。
  void backgroundImpactAnalysisLifecycle() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    // 链（cellSize 1 → 100×100 网格，worker 段有可观察时长）：
    QVERIFY(f.declarePoints({QVariantMap{{"x", 0}, {"y", 0}, {"z", 0}},
                             QVariantMap{{"x", 100}, {"y", 0}, {"z", 20}},
                             QVariantMap{{"x", 0}, {"y", 100}, {"z", 40}},
                             QVariantMap{{"x", 100}, {"y", 100}, {"z", 60}}},
                            "bg-samples-a.geojson", &error));
    QVERIFY2(f.work.generateFactor(
                  "D61", "sandthick",
                  {{"pointsLayerId", "wells.D61"}, {"field", "z"}, {"cellSize", 0.5}},
                  &error),
             qPrintable(error));
    const auto factorV1 = f.latest("single_factor_raster");
    QVERIFY(!factorV1.isEmpty());
    const auto factorV1Version = f.work.versionForLayer(factorV1);
    const auto polygonsV1 = f.work.compose(
        "D61", {factorV1}, {{"thresholds", QVariantList{20., 25., 30., 35., 40., 45.}}},
        &error);
    QVERIFY2(!polygonsV1.isEmpty(), qPrintable(error));
    QVERIFY(f.declarePoints({QVariantMap{{"x", 0}, {"y", 0}, {"z", 100}},
                             QVariantMap{{"x", 100}, {"y", 0}, {"z", 100}},
                             QVariantMap{{"x", 0}, {"y", 100}, {"z", 100}},
                             QVariantMap{{"x", 100}, {"y", 100}, {"z", 100}}},
                            "bg-samples-b.geojson", &error));
    QVERIFY2(f.work.generateFactor(
                  "D61", "sandthick",
                  {{"pointsLayerId", "wells.D61"}, {"field", "z"}, {"cellSize", 0.5}},
                  &error),
             qPrintable(error));
    const auto factorV2 = f.latest("single_factor_raster");
    const auto factorV2Version = f.work.versionForLayer(factorV2);
    QVERIFY(factorV2Version.assetId == factorV1Version.assetId);

    // 1. 同步路径契约保留（无任务服务时 analyzeLayerVersionImpact 同口径）。
    const auto syncReport = f.work.analyzeImpact(
        {{"kind", "input_version"},
         {"before_version_id", factorV1Version.id},
         {"after_version_id", factorV2Version.id},
         {"horizon", "D61"}},
        &error);
    QVERIFY2(!syncReport.isEmpty(), qPrintable(error));

    // 2. 后台执行 + 与同步结果等价（除时间戳/报告路径外逐字段一致）。
    PaleoTaskService tasks;
    f.work.setTaskService(&tasks);
    QSignalSpy ready(&f.work, &MappingWorkbench::impactAnalysisReady);
    QElapsedTimer launched;
    launched.start();
    QVERIFY2(f.work.startImpactAnalysis(factorV2, "D61", &error), qPrintable(error));
    QVERIFY(f.work.impactAnalysisBusy());
    QVERIFY2(launched.elapsed() < 500,
             qPrintable(QStringLiteral("start 阻塞 %1ms").arg(launched.elapsed())));
    QString busyError;
    QVERIFY(!f.work.startImpactAnalysis(factorV2, "D61", &busyError));
    QVERIFY2(busyError.contains(QStringLiteral("已有")),
             qPrintable(busyError)); // 忙拒绝
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 60000);
    {
      const auto args = ready.takeFirst();
      QCOMPARE(args.at(1).toString(), QString());
      const auto bg = args.at(0).toMap();
      QVariantMap expected = syncReport, actual = bg;
      expected.remove("analyzed_at");
      actual.remove("analyzed_at");
      expected.remove("report_path");
      actual.remove("report_path");
      QCOMPARE(actual, expected);
      QVERIFY(QFile::exists(bg.value("report_path").toString()));
      QVERIFY(!f.work.impactAnalysisBusy()); // 成功后恢复
    }

    // 3. GUI 响应性：排队期间与 worker 执行期间，GUI 事件循环都在处理事件。
    tasks.setMaxWorkerThreads(1);
    // 占满唯一 worker 线程 → 影响分析排队（确定性同步点）。返回的闸门由
    // 用例显式放开；启动失败返回空（QVERIFY 留在用例体——非 void lambda
    // 里 QVERIFY 的 return 不可用）。
    const auto startBlocked = [&f, &tasks, &factorV2](const char *tag) {
      auto release = std::make_shared<std::atomic_bool>(false);
      tasks.start(QStringLiteral("blocker-%1").arg(QLatin1String(tag)),
                  [release](PaleoTask *) {
                    while (!release->load())
                      QThread::msleep(5);
                    return QString();
                  });
      QString err;
      const bool ok = f.work.startImpactAnalysis(factorV2, "D61", &err);
      if (!ok)
        qWarning("startImpactAnalysis failed: %s", qPrintable(err));
      return ok ? release : nullptr;
    };
    {
      const auto release = startBlocked("tick");
      QVERIFY(release);
      QVERIFY(f.work.impactAnalysisBusy());
      int queuedTicks = 0;
      QElapsedTimer t;
      t.start();
      while (t.elapsed() < 150) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        ++queuedTicks;
      }
      QVERIFY2(queuedTicks >= 4,
               qPrintable(QStringLiteral("排队 %1 ticks——GUI 循环被阻塞")
                              .arg(queuedTicks)));
      release->store(true);
      bool busySeenWhileProcessing = false;
      t.restart();
      while (t.elapsed() < 60000 && ready.count() == 0) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        if (f.work.impactAnalysisBusy())
          busySeenWhileProcessing = true;
      }
      QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 60000);
      QVERIFY2(busySeenWhileProcessing,
               "worker 执行期间 GUI 事件循环未处理事件");
      QVERIFY(!ready.first().at(0).toMap().isEmpty());
      ready.clear();
      QTRY_VERIFY_WITH_TIMEOUT(!f.work.impactAnalysisBusy(), 5000);
    }

    // 4. 排队期协作取消（确定性同步点）：不发布任何报告。
    {
      const auto release = startBlocked("cancel");
      QVERIFY(release);
      f.work.cancelImpactAnalysis();
      release->store(true);
      QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 60000);
      const auto args = ready.takeFirst();
      QVERIFY(args.at(0).toMap().isEmpty());
      QVERIFY2(args.at(1).toString().contains(QStringLiteral("取消")),
               qPrintable(args.at(1).toString()));
      QTRY_VERIFY_WITH_TIMEOUT(!f.work.impactAnalysisBusy(), 5000);
    }

    // 5. 旧请求晚到：排队期间目录修订前进 → 过期结果不发布、不写报告。
    {
      const auto release = startBlocked("stale");
      QVERIFY(release);
      CatalogAsset bump;
      bump.id = QStringLiteral("bg-bump-asset");
      bump.type = QStringLiteral("document");
      bump.displayName = QStringLiteral("bg-bump");
      QString catError;
      QVERIFY2(f.catalog.addAsset(bump, &catError), qPrintable(catError));
      release->store(true);
      QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 60000);
      const auto args = ready.takeFirst();
      QVERIFY(args.at(0).toMap().isEmpty());
      QVERIFY2(args.at(1).toString().contains(QStringLiteral("过期")),
               qPrintable(args.at(1).toString()));
      QTRY_VERIFY_WITH_TIMEOUT(!f.work.impactAnalysisBusy(), 5000);
    }

    // 6. 切工程（重绑同一 catalog 也算世代切换）：排队即取消，不发布。
    {
      const auto release = startBlocked("rebind");
      QVERIFY(release);
      f.work.bindCatalog(&f.catalog, f.dir.path());
      release->store(true);
      QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 60000);
      const auto args = ready.takeFirst();
      QVERIFY(args.at(0).toMap().isEmpty());
      QTRY_VERIFY_WITH_TIMEOUT(!f.work.impactAnalysisBusy(), 5000);
    }

    // 7. 对象销毁：作业在飞时销毁 workbench（连带 runner），worker 经
    //    shared 状态安全收尾，不崩溃、不回调已亡对象。
    {
      auto *ephemeral = new Fixture;
      QVERIFY(ephemeral->init());
      PaleoTaskService doomed;
      ephemeral->work.setTaskService(&doomed);
      doomed.setMaxWorkerThreads(1);
      std::atomic_bool release{false};
      doomed.start(QStringLiteral("blocker"), [&release](PaleoTask *) {
        while (!release.load())
          QThread::msleep(5);
        return QString();
      });
      QString err;
      QVERIFY2(ephemeral->work.startImpactAnalysis(factorV2Of(*ephemeral, err),
                                                   QStringLiteral("D61"), &err),
               qPrintable(err));
      delete ephemeral; // 在飞作业 + runner + workbench 全部销毁
      release.store(true);
      for (int i = 0; i < 20; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
      QVERIFY(doomed.shutdown());
    }
  }
  // 销毁用夹具的第二个单因素版本层（与首个构成可分析的版本对；无版本对
  // 场景由 prepare 同步失败——该分支也要如实报错，不崩溃）。
  static QString factorV2Of(Fixture &f, QString &error) {
    QString err;
    for (const char *name : {"doom-samples-a.geojson", "doom-samples-b.geojson"}) {
      if (!f.declarePoints({QVariantMap{{"x", 0}, {"y", 0}, {"z", 0}},
                            QVariantMap{{"x", 100}, {"y", 0}, {"z", 20}}},
                           name, &err))
        return (error = err), QString();
      if (!f.work.generateFactor(
              "D61", "sandthick",
              {{"pointsLayerId", "wells.D61"}, {"field", "z"}, {"cellSize", 4}},
              &err))
        return (error = err), QString();
    }
    return f.latest("single_factor_raster");
  }

  // 版本对比后台化：受控重算产物 vs 旧版本的像元 diff 在 worker 执行，
  // 结果与同步 compareWithRecomputeSource 等价；取消不发布。
  void backgroundVersionCompareEquivalenceAndCancel() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    QVERIFY(f.declarePoints({QVariantMap{{"x", 0}, {"y", 0}, {"z", 0}},
                             QVariantMap{{"x", 100}, {"y", 0}, {"z", 20}},
                             QVariantMap{{"x", 0}, {"y", 100}, {"z", 40}},
                             QVariantMap{{"x", 100}, {"y", 100}, {"z", 60}}},
                            "bc-samples-a.geojson", &error));
    QVERIFY2(f.work.generateFactor(
                  "D61", "sandthick",
                  {{"pointsLayerId", "wells.D61"}, {"field", "z"}, {"cellSize", 0.5}},
                  &error),
             qPrintable(error));
    const auto factorV1 = f.latest("single_factor_raster");
    const auto polygonsV1 = f.work.compose(
        "D61", {factorV1}, {{"thresholds", QVariantList{20., 25., 30., 35., 40., 45.}}},
        &error);
    QVERIFY2(!polygonsV1.isEmpty(), qPrintable(error));
    QVERIFY(f.declarePoints({QVariantMap{{"x", 0}, {"y", 0}, {"z", 100}},
                             QVariantMap{{"x", 100}, {"y", 0}, {"z", 100}},
                             QVariantMap{{"x", 0}, {"y", 100}, {"z", 100}},
                             QVariantMap{{"x", 100}, {"y", 100}, {"z", 100}}},
                            "bc-samples-b.geojson", &error));
    QVERIFY2(f.work.generateFactor(
                  "D61", "sandthick",
                  {{"pointsLayerId", "wells.D61"}, {"field", "z"}, {"cellSize", 0.5}},
                  &error),
             qPrintable(error));
    const auto factorV2 = f.latest("single_factor_raster");
    const auto impact = f.work.analyzeImpact(
        {{"kind", "input_version"},
         {"before_version_id", f.work.versionForLayer(factorV1).id},
         {"after_version_id", f.work.versionForLayer(factorV2).id},
         {"horizon", "D61"}},
        &error);
    QVERIFY2(!impact.isEmpty(), qPrintable(error));
    const auto plan = f.work.recomputePlan(impact, &error);
    QVERIFY2(!plan.isEmpty(), qPrintable(error));
    // 无任务服务：executeRecompute 同步产出受控重算成果。
    const auto execution = f.work.executeRecompute(plan, &error);
    const auto results = execution.value("results").toList();
    QVERIFY2(!results.isEmpty() && results.first().toMap().value("ok").toBool(),
             qPrintable(error));
    const auto polygonsV2 = f.latest("facies_polygons");
    QVERIFY(!polygonsV2.isEmpty());

    // 同步基准。
    const auto syncReport = f.work.compareWithRecomputeSource(polygonsV2, &error);
    QVERIFY2(!syncReport.isEmpty(), qPrintable(error));

    // 后台等价。
    PaleoTaskService tasks;
    f.work.setTaskService(&tasks);
    QSignalSpy ready(&f.work, &MappingWorkbench::versionCompareReady);
    QVERIFY2(f.work.startVersionCompare(polygonsV2, &error), qPrintable(error));
    QVERIFY(f.work.versionCompareBusy());
    QString busyError;
    QVERIFY(!f.work.startVersionCompare(polygonsV2, &busyError));
    QVERIFY2(busyError.contains(QStringLiteral("已有")), qPrintable(busyError));
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 60000);
    {
      const auto args = ready.takeFirst();
      QCOMPARE(args.at(1).toString(), QString());
      const auto bg = args.at(0).toMap();
      QVariantMap expected = syncReport, actual = bg;
      expected.remove("report_path");
      actual.remove("report_path");
      QCOMPARE(actual, expected);
      QVERIFY(QFile::exists(bg.value("report_path").toString()));
      QVERIFY(!f.work.versionCompareBusy());
    }

    // 排队期取消：不发布。
    tasks.setMaxWorkerThreads(1);
    std::atomic_bool release{false};
    tasks.start(QStringLiteral("blocker"), [&release](PaleoTask *) {
      while (!release.load())
        QThread::msleep(5);
      return QString();
    });
    QVERIFY2(f.work.startVersionCompare(polygonsV2, &error), qPrintable(error));
    f.work.cancelVersionCompare();
    release.store(true);
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 60000);
    const auto args = ready.takeFirst();
    QVERIFY(args.at(0).toMap().isEmpty());
    QVERIFY2(args.at(1).toString().contains(QStringLiteral("取消")),
             qPrintable(args.at(1).toString()));
    QTRY_VERIFY_WITH_TIMEOUT(!f.work.versionCompareBusy(), 5000);
  }

  static int countKind(const DataCatalog &catalog, const QString &kind) {    int n = 0;
    for (const auto &v : catalog.versions())
      if (v.extra.value("kind").toString() == kind)
        ++n;
    return n;
  }
  // 读成果版本 GPKG 中某图斑的 paleo_interpretation 原文（历史未改写证据）。
  static QString interpretationText(const Fixture &f, const QString &versionId,
                                    const QString &uid) {
    const auto v = f.catalog.versionById(versionId);
    if (v.id.isEmpty())
      return {};
    QgsVectorLayer layer(
        DataCatalog::resolvedVersionPath(f.dir.path(), v) +
            v.extra.value("source_suffix").toString(),
        QStringLiteral("check"), QStringLiteral("ogr"));
    if (!layer.isValid())
      return {};
    QgsFeature feature;
    auto it = layer.getFeatures();
    const int uidIndex = layer.fields().indexOf("paleo_feature_uid");
    const int interpIndex = layer.fields().indexOf("paleo_interpretation");
    while (it.nextFeature(feature))
      if (uidIndex >= 0 && feature.attribute(uidIndex).toString() == uid)
        return interpIndex < 0 ? QString()
                               : feature.attribute(interpIndex).toString();
    return {};
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
