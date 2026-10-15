// 层：测试
// G10b：跨算法统一计算回执归一化（receiptsemantics 纯函数）。
// upstream_receipts 聚合走 evidencerecord 集成测试，这里只测 normalize /
// compareWithPlan 本身。纯域层：QCoreApplication 模式，无 QgsApplication。
#include "../src/domain/receiptsemantics.h"
#include <QJsonDocument>
#include <QtTest>

using namespace paleo::receipts;

class TestReceiptSemantics : public QObject {
  Q_OBJECT
private slots:
  void singleFactorRasterNormalizesBothDimensions() {
    const QVariantMap extra{{"kind", "single_factor_raster"},
                            {"title", "砂岩厚度·H2"},
                            {"algorithm_id", "constraint/local_direction_idw"},
                            {"engine_id", "local_direction_kriging"},
                            {"method_actual", "local_direction_idw"},
                            {"fallback_reason", "variogram_fit_failed"},
                            {"cell_size", 250.0},
                            {"finite_cells", 1000},
                            {"constraints_consumed", QVariantList{
                                QVariantMap{{"id", "b1"}, {"role", "hard_barrier"}},
                                QVariantMap{{"id", "d1"}, {"role", "direction_guide"}}}},
                            {"constraints_unused", QVariantList{
                                QVariantMap{{"id", "s1"}, {"reason", "disabled"}}}},
                            {"engine_samples_used", QVariantList{
                                QVariantMap{{"source_record_id", "1"}, {"well_id", "W1"}, {"participating", true}},
                                QVariantMap{{"source_record_id", "2"}, {"well_id", "W2"}, {"participating", true}}}},
                            {"engine_samples_skipped", QVariantList{
                                QVariantMap{{"source_record_id", "3"}, {"well_id", "W3"},
                                            {"participating", false}, {"reason", "field_missing_or_non_finite"}}}}};
    const auto n = normalize(extra);
    QCOMPARE(n.value("known").toBool(), true);
    QCOMPARE(n.value("legacy").toBool(), false);
    QCOMPARE(n.value("kind").toString(), QString("single_factor_raster"));
    // consumed 只有约束维度（计算事实），带角色。
    const auto consumed = n.value("consumed_inputs").toList();
    QCOMPARE(consumed.size(), 2);
    QCOMPARE(consumed.at(0).toMap().value("id").toString(), QString("b1"));
    QCOMPARE(consumed.at(0).toMap().value("role").toString(), QString("hard_barrier"));
    QCOMPARE(consumed.at(0).toMap().value("dimension").toString(), QString("constraints"));
    // skipped 双维度都保留并标注 dimension。
    const auto skipped = n.value("skipped_inputs").toList();
    QCOMPARE(skipped.size(), 2);
    QCOMPARE(skipped.at(0).toMap().value("id").toString(), QString("s1"));
    QCOMPARE(skipped.at(0).toMap().value("dimension").toString(), QString("constraints"));
    QCOMPARE(skipped.at(1).toMap().value("id").toString(), QString("W3"));
    QCOMPARE(skipped.at(1).toMap().value("dimension").toString(), QString("samples"));
    QCOMPARE(skipped.at(1).toMap().value("reason").toString(), QString("field_missing_or_non_finite"));
    // accepted = 引擎样点 participating=true。
    const auto accepted = n.value("accepted_inputs").toList();
    QCOMPARE(accepted.size(), 2);
    QCOMPARE(accepted.at(1).toMap().value("id").toString(), QString("W2"));
    QCOMPARE(accepted.at(1).toMap().value("dimension").toString(), QString("samples"));
    // 单因素无计划口径：declared 为空（N/A），不编造。
    QCOMPARE(n.value("declared_inputs").toList().size(), 0);
    // 参数透传关键键；回落时请求引擎进 ignored_parameters。
    const auto effective = n.value("effective_parameters").toMap();
    QCOMPARE(effective.value("algorithm_id").toString(), QString("constraint/local_direction_idw"));
    QCOMPARE(effective.value("cell_size").toDouble(), 250.0);
    QVERIFY(!effective.contains("title"));
    QCOMPARE(n.value("ignored_parameters").toMap().value("engine_id").toString(),
             QString("local_direction_kriging"));
    QCOMPARE(n.value("result_version").toMap().value("finite_cells").toInt(), 1000);
  }
  void singleFactorRasterLegacyWithoutReceipts() {
    const auto n = normalize(QVariantMap{{"kind", "single_factor_raster"},
                                         {"algorithm_id", "legacy_idw"}});
    QCOMPARE(n.value("known").toBool(), true);
    QCOMPARE(n.value("legacy").toBool(), true);
    QCOMPARE(n.value("consumed_inputs").toList().size(), 0);
    QVERIFY(!n.value("quality_findings").toList().isEmpty());
  }
  void composedFaciesJoinsOrderedWithReceipts() {
    const QVariantList ordered{
        QVariantMap{{"layer_id", "l1"}, {"version_id", "v1"}, {"priority", 1}},
        QVariantMap{{"layer_id", "l2"}, {"version_id", "v2"}, {"priority", 2}},
        QVariantMap{{"layer_id", "l3"}, {"version_id", "v3"}, {"priority", 3}}};
    const QVariantMap composeReceipts{
        {"inputs", QVariantList{
             QVariantMap{{"layer_id", "l1"}, {"version_id", "v1"}, {"priority", 1},
                         {"won_cells", QVariant::fromValue<qlonglong>(100)}},
             QVariantMap{{"layer_id", "l2"}, {"version_id", "v2"}, {"priority", 2},
                         {"won_cells", QVariant::fromValue<qlonglong>(0)}},
             QVariantMap{{"layer_id", "l3"}, {"version_id", "v3"}, {"priority", 3},
                         {"won_cells", QVariant::fromValue<qlonglong>(50)}}}},
        {"decided_cells", QVariant::fromValue<qlonglong>(150)},
        {"total_cells", QVariant::fromValue<qlonglong>(200)}};
    const QVariantMap extra{{"kind", "composed_facies"},
                            {"ordered_inputs", ordered},
                            {"compose_receipts", composeReceipts},
                            {"algorithm_parameters", QVariantMap{{"ordered_inputs", ordered},
                                                                 {"horizon", "H2"},
                                                                 {"crs", "wkt"},
                                                                 {"thresholds", QVariantList{0.3, 0.6}}}},
                            {"thresholds", QVariantList{0.3, 0.6}}};
    const auto n = normalize(extra);
    QCOMPARE(n.value("known").toBool(), true);
    QCOMPARE(n.value("legacy").toBool(), false);
    const auto declared = n.value("declared_inputs").toList();
    QCOMPARE(declared.size(), 3);
    QCOMPARE(declared.at(2).toMap().value("version_id").toString(), QString("v3"));
    QCOMPARE(declared.at(2).toMap().value("priority").toInt(), 3);
    // accepted：回执覆盖的全部输入（won_cells>=0 即接受过）。
    QCOMPARE(n.value("accepted_inputs").toList().size(), 3);
    // consumed：won_cells>0，带计数与 scope。
    const auto consumed = n.value("consumed_inputs").toList();
    QCOMPARE(consumed.size(), 2);
    QCOMPARE(consumed.at(0).toMap().value("version_id").toString(), QString("v1"));
    QCOMPARE(consumed.at(0).toMap().value("consumed_count").toLongLong(), qlonglong(100));
    QCOMPARE(consumed.at(0).toMap().value("participation_scope").toString(), QString("won_cells"));
    QCOMPARE(consumed.at(1).toMap().value("version_id").toString(), QString("v3"));
    // skipped：won_cells==0 → 被更高优先级覆盖。
    const auto skipped = n.value("skipped_inputs").toList();
    QCOMPARE(skipped.size(), 1);
    QCOMPARE(skipped.at(0).toMap().value("version_id").toString(), QString("v2"));
    QVERIFY(!skipped.at(0).toMap().value("reason").toString().isEmpty());
    const auto resultVersion = n.value("result_version").toMap();
    QCOMPARE(resultVersion.value("decided_cells").toLongLong(), qlonglong(150));
    QCOMPARE(resultVersion.value("total_cells").toLongLong(), qlonglong(200));
    // 输入清单不是参数：effective 里不重复出现 ordered_inputs。
    const auto effective = n.value("effective_parameters").toMap();
    QCOMPARE(effective.value("horizon").toString(), QString("H2"));
    QVERIFY(effective.contains("thresholds"));
    QVERIFY(!effective.contains("ordered_inputs"));
  }
  void composedFaciesFlagsIncompleteReceiptCoverage() {
    const QVariantList ordered{
        QVariantMap{{"layer_id", "l1"}, {"version_id", "v1"}, {"priority", 1}},
        QVariantMap{{"layer_id", "l9"}, {"version_id", "v9"}, {"priority", 9}}};
    const QVariantMap composeReceipts{
        {"inputs", QVariantList{QVariantMap{{"layer_id", "l1"}, {"version_id", "v1"},
                                            {"priority", 1}, {"won_cells", QVariant::fromValue<qlonglong>(10)}}}},
        {"decided_cells", QVariant::fromValue<qlonglong>(10)},
        {"total_cells", QVariant::fromValue<qlonglong>(10)}};
    const auto n = normalize(QVariantMap{{"kind", "composed_facies"},
                                         {"ordered_inputs", ordered},
                                         {"compose_receipts", composeReceipts}});
    QCOMPARE(n.value("legacy").toBool(), false);
    // 计划有、回执没有的输入：进 skipped 并记 finding，不静默消失。
    const auto skipped = n.value("skipped_inputs").toList();
    QCOMPARE(skipped.size(), 1);
    QCOMPARE(skipped.at(0).toMap().value("version_id").toString(), QString("v9"));
    QVERIFY(n.value("quality_findings").toList().size() > 0);
  }
  void faciesPolygonsParsesProvenanceJson() {
    const QVariantMap prov{{"algorithm", "paleo:paleo_facies_polygonize"},
                           {"smooth_passes", 1},
                           {"smoothed_cells", 42},
                           {"min_cells", 4},
                           {"min_fraction", 0.0025},
                           {"min_area", 0.0},
                           {"simplify", 0.0},
                           {"faces", 7},
                           {"arcs", 12},
                           {"slivers_merged", 3},
                           {"steps", QVariantList{"recode", "sliver", "polygonize", "dissolve", "boundary-graph"}},
                           {"grid", QVariantMap{{"cols", 10}, {"rows", 20}}}};
    const QString json = QString::fromUtf8(
        QJsonDocument::fromVariant(prov).toJson(QJsonDocument::Compact));
    const QVariantMap extra{{"kind", "facies_polygons"},
                            {"source_layer", "product.vx"},
                            {"computational_parent_versions", QStringList{"vp1"}},
                            {"polygonize_provenance", json}};
    const auto n = normalize(extra);
    QCOMPARE(n.value("known").toBool(), true);
    QCOMPARE(n.value("legacy").toBool(), false);
    const auto accepted = n.value("accepted_inputs").toList();
    QCOMPARE(accepted.size(), 1);
    QCOMPARE(accepted.at(0).toMap().value("version_id").toString(), QString("vp1"));
    QCOMPARE(accepted.at(0).toMap().value("layer_id").toString(), QString("product.vx"));
    const auto consumed = n.value("consumed_inputs").toList();
    QCOMPARE(consumed.size(), 1);
    QCOMPARE(consumed.at(0).toMap().value("participation_scope").toString(),
             QString("entire_parent_grid"));
    const auto effective = n.value("effective_parameters").toMap();
    QCOMPARE(effective.value("smooth_passes").toInt(), 1);
    QCOMPARE(effective.value("min_cells").toInt(), 4);
    const auto resultVersion = n.value("result_version").toMap();
    QCOMPARE(resultVersion.value("faces").toInt(), 7);
    QCOMPARE(resultVersion.value("smoothed_cells").toInt(), 42);
    QCOMPARE(resultVersion.value("arcs").toInt(), 12);
    QVERIFY(resultVersion.value("steps").toList().contains(QString("dissolve")));
    QCOMPARE(resultVersion.value("grid").toMap().value("cols").toInt(), 10);
  }
  void faciesPolygonsLegacyOrMalformedProvenance() {
    const auto missing = normalize(QVariantMap{{"kind", "facies_polygons"},
                                               {"computational_parent_versions", QStringList{"vp1"}}});
    QCOMPARE(missing.value("known").toBool(), true);
    QCOMPARE(missing.value("legacy").toBool(), true);
    const auto broken = normalize(QVariantMap{{"kind", "facies_polygons"},
                                              {"polygonize_provenance", "{\"faces\": oops"}});
    QCOMPARE(broken.value("legacy").toBool(), true);
    QVERIFY(!broken.value("quality_findings").toList().isEmpty());
  }
  void predictionKindUsesEvidenceWells() {
    const QVariantMap evidence{{"schema", "paleo_evidence_v1"},
                               {"inputs", QVariantList{
                                   QVariantMap{{"version_id", "vin1"}, {"name", "A.las"}},
                                   QVariantMap{{"version_id", "vin2"}, {"name", "seis.sgy"}}}},
                               {"parameters", QVariantMap{{"window_half_ms", 12.0}}},
                               {"support", QVariantMap{{"wells", QVariantMap{
                                   {"supporting_wells", QVariantList{
                                       QVariantMap{{"well_id", "W1"}, {"well_name", "wells/W1"}},
                                       QVariantMap{{"well_id", "W2"}}}},
                                   {"skipped_wells", QVariantList{
                                       QVariantMap{{"well_id", "W3"}, {"reason", "missing_logs"}}}}}}}}};
    const auto n = normalize(QVariantMap{{"kind", "wells_prediction"}, {"evidence", evidence}});
    QCOMPARE(n.value("known").toBool(), true);
    QCOMPARE(n.value("legacy").toBool(), false);
    QCOMPARE(n.value("kind").toString(), QString("wells_prediction"));
    QCOMPARE(n.value("declared_inputs").toList().size(), 2);
    QCOMPARE(n.value("accepted_inputs").toList().size(), 2);
    QCOMPARE(n.value("accepted_inputs").toList().at(0).toMap().value("title").toString(),
             QString("A.las"));
    const auto consumed = n.value("consumed_inputs").toList();
    QCOMPARE(consumed.size(), 2);
    QCOMPARE(consumed.at(0).toMap().value("id").toString(), QString("W1"));
    QCOMPARE(consumed.at(0).toMap().value("dimension").toString(), QString("wells"));
    const auto skipped = n.value("skipped_inputs").toList();
    QCOMPARE(skipped.size(), 1);
    QCOMPARE(skipped.at(0).toMap().value("id").toString(), QString("W3"));
    QCOMPARE(skipped.at(0).toMap().value("reason").toString(), QString("missing_logs"));
    QCOMPARE(skipped.at(0).toMap().value("dimension").toString(), QString("wells"));
    QCOMPARE(n.value("effective_parameters").toMap().value("window_half_ms").toDouble(), 12.0);
  }
  void unknownKindAndEmptyExtraAreLegacy() {
    const auto unknown = normalize(QVariantMap{{"kind", "mystery_kind"}, {"title", "t"}});
    QCOMPARE(unknown.value("known").toBool(), false);
    QCOMPARE(unknown.value("legacy").toBool(), true);
    QCOMPARE(unknown.value("kind").toString(), QString("mystery_kind"));
    QVERIFY(!unknown.value("quality_findings").toList().isEmpty());
    const auto empty = normalize(QVariantMap{});
    QCOMPARE(empty.value("known").toBool(), false);
    QCOMPARE(empty.value("legacy").toBool(), true);
    QVERIFY(empty.value("kind").toString().isEmpty());
    QCOMPARE(empty.value("consumed_inputs").toList().size(), 0);
    QCOMPARE(empty.value("effective_parameters").toMap().size(), 0);
    QCOMPARE(empty.value("quality_findings").toList().size(), 1);
  }
  void compareWithPlanConsistentWhenFactsMatch() {
    const auto n = normalize(QVariantMap{{"kind", "single_factor_raster"},
                                         {"constraints_consumed", QVariantList{
                                             QVariantMap{{"id", "b1"}, {"role", "hard_barrier"}},
                                             QVariantMap{{"id", "d1"}, {"role", "direction_guide"}}}},
                                         {"constraints_unused", QVariantList{
                                             QVariantMap{{"id", "s1"}, {"reason", "disabled"}}}}});
    const QVariantMap plan{{"decisions", QVariantList{
                              QVariantMap{{"id", "b1"}, {"planned", "consumed"}},
                              QVariantMap{{"id", "d1"}, {"planned", "consumed"}},
                              QVariantMap{{"id", "s1"}, {"planned", "available"}}}}};
    const auto cmp = compareWithPlan(n, plan);
    QCOMPARE(cmp.value("comparable").toBool(), true);
    QCOMPARE(cmp.value("consistent").toBool(), true);
    QCOMPARE(cmp.value("differences").toList().size(), 0);
    QVERIFY(!cmp.value("note").toString().isEmpty());
  }
  void compareWithPlanDetectsDrift() {
    // 计划消费 d1，实际回执显示未消费（带原因）；x9 实际消费但计划未登记。
    const auto n = normalize(QVariantMap{{"kind", "single_factor_raster"},
                                         {"constraints_consumed", QVariantList{
                                             QVariantMap{{"id", "b1"}, {"role", "hard_barrier"}},
                                             QVariantMap{{"id", "x9"}, {"role", "hard_barrier"}}}},
                                         {"constraints_unused", QVariantList{
                                             QVariantMap{{"id", "d1"}, {"reason", "out_of_domain"}},
                                             QVariantMap{{"id", "s1"}, {"reason", "disabled"}}}}});
    const QVariantMap plan{{"consumed", QVariantList{"b1", "d1"}},
                           {"unused", QVariantList{QVariantMap{{"id", "s1"}, {"reason", "disabled"}}}}};
    const auto cmp = compareWithPlan(n, plan);
    QCOMPARE(cmp.value("comparable").toBool(), true);
    QCOMPARE(cmp.value("consistent").toBool(), false);
    const auto differences = cmp.value("differences").toList();
    QCOMPARE(differences.size(), 2);
    const auto drift = differences.at(0).toMap();
    QCOMPARE(drift.value("id").toString(), QString("d1"));
    QCOMPARE(drift.value("planned").toString(), QString("consumed"));
    QCOMPARE(drift.value("actual").toString(), QString("unused"));
    QVERIFY(drift.value("reason").toString().contains("out_of_domain"));
    const auto unplanned = differences.at(1).toMap();
    QCOMPARE(unplanned.value("id").toString(), QString("x9"));
    QCOMPARE(unplanned.value("actual").toString(), QString("consumed"));
  }
  void compareWithPlanCannotCompareWithoutReceipts() {
    const auto legacy = normalize(QVariantMap{{"kind", "single_factor_raster"},
                                              {"algorithm_id", "legacy_idw"}});
    const QVariantMap plan{{"consumed", QVariantList{"b1"}}};
    const auto cmp = compareWithPlan(legacy, plan);
    QCOMPARE(cmp.value("comparable").toBool(), false);
    QCOMPARE(cmp.value("consistent").toBool(), false);
    QCOMPARE(cmp.value("differences").toList().size(), 0);
    QVERIFY(cmp.value("note").toString().contains("尚未运行"));
    // 计划快照自身无法解析时同样不可比较。
    const auto emptyPlan = compareWithPlan(
        normalize(QVariantMap{{"kind", "single_factor_raster"},
                              {"constraints_consumed", QVariantList{
                                  QVariantMap{{"id", "b1"}, {"role", "hard_barrier"}}}}}),
        QVariantMap{});
    QCOMPARE(emptyPlan.value("comparable").toBool(), false);
  }
};

QTEST_GUILESS_MAIN(TestReceiptSemantics)
#include "tst_receiptsemantics.moc"
