// 层：测试
// G06：多源地质证据冲突分析（纯域层聚合器）——分类、定级、表达与排序。
#include "../src/domain/evidenceconflict.h"
#include <QtTest>
using namespace paleo::evidenceconflict;

namespace {
const QVariantMap kTarget{{"feature_uid", "polygon-A"}, {"horizon", "H1"}, {"version_id", "ver-1"}};

Opinion makeOpinion(const QString &kind, const QString &label, const QString &version, int code,
                    const QVariantMap &extra = {}) {
  Opinion op;
  op.sourceKind = kind;
  op.label = label;
  op.versionId = version;
  op.faciesCode = code;
  op.known = code != 0;
  op.extra = extra;
  return op;
}
QVariantMap byCode(const QVariantList &findings, const QString &code) {
  for (const auto &value : findings)
    if (value.toMap().value("code").toString() == code) return value.toMap();
  return {};
}
QStringList conflictTypesOf(const QVariantList &findings) {
  QStringList out;
  for (const auto &value : findings) out << value.toMap().value("conflict_type").toString();
  return out;
}
} // namespace

class TestEvidenceConflict : public QObject {
  Q_OBJECT
private slots:
  void wellVersusPredictionFlagsMinority() {
    const QList<Opinion> opinions{makeOpinion("well", "井 A", "log-20261001-aa", 1),
                                  makeOpinion("prediction", "相预测", "pred-v7", 2)};
    const QVariantMap attribution{{"status", "ok"}};
    const auto findings = analyze(opinions, 2, kTarget, attribution);
    QCOMPARE(findings.size(), 1);
    const auto finding = findings.first().toMap();
    QCOMPARE(finding.value("code").toString(), QString("minority_opinion"));
    QCOMPARE(finding.value("severity").toString(), QString("Warning"));
    QCOMPARE(finding.value("conflict_type").toString(), QString("spatial_contradiction"));
    QCOMPARE(finding.value("evidence").toMap().value("label").toString(), QString("井 A"));
    QCOMPARE(finding.value("evidence").toMap().value("source").toString(), QString("well"));
    QCOMPARE(finding.value("evidence").toMap().value("version_id").toString(), QString("log-20261001-aa"));
    QVERIFY(finding.value("message").toString().contains("井 A"));
    QVERIFY(finding.value("message").toString().contains("相 2"));
    QVERIFY(finding.value("basis").toString().contains("图斑为相 2"));
    QVERIFY(finding.value("basis").toString().contains("相 1"));
    QVERIFY(!finding.value("suggestion").toString().isEmpty());
    QCOMPARE(finding.value("status").toString(), QString("open"));
    // 多数方（预测，与图斑一致）不产出 finding。
    QVERIFY(byCode(findings, "consistent_sources").isEmpty());
  }

  void singleFactorVersusTargetClassification() {
    const QList<Opinion> opinions{makeOpinion("single_factor", "砂地比", "sf-v3", 1)};
    const auto findings = analyze(opinions, 2, kTarget, {{"status", "ok"}});
    QCOMPARE(findings.size(), 1);
    const auto finding = findings.first().toMap();
    QCOMPARE(finding.value("code").toString(), QString("minority_opinion"));
    // 无空间落点的来源按解释矛盾表达。
    QCOMPARE(finding.value("conflict_type").toString(), QString("interpretation_contradiction"));
    QCOMPARE(finding.value("evidence").toMap().value("label").toString(), QString("砂地比"));
    QVERIFY(finding.value("basis").toString().contains("图斑为相 2"));
  }

  void manualTargetConflictIsInterpretationContradiction() {
    // 无自动归因：图斑编码视为人工解释，井意见与其不符 → 解释矛盾。
    const QList<Opinion> opinions{makeOpinion("well", "井 B", "log-v2", 1)};
    const auto findings = analyze(opinions, 2, kTarget, {});
    QCOMPARE(findings.size(), 1);
    const auto finding = findings.first().toMap();
    QCOMPARE(finding.value("code").toString(), QString("manual_target_conflict"));
    QCOMPARE(finding.value("conflict_type").toString(), QString("interpretation_contradiction"));
    QCOMPARE(finding.value("severity").toString(), QString("Warning"));
    QVERIFY(finding.value("message").toString().contains("人工"));
    QVERIFY(finding.value("message").toString().contains("井 B"));
    QVERIFY(finding.value("message").toString().contains("相 1"));
    // attribution.status=manual 同样按人工口径。
    const auto manual = analyze(opinions, 2, kTarget, {{"status", "manual"}});
    QCOMPARE(manual.first().toMap().value("code").toString(), QString("manual_target_conflict"));
  }

  void opinionHorizonMismatchIsDataQuality() {
    const QList<Opinion> opinions{makeOpinion("well", "井 C", "log-v3", 2, {{"horizon", "H2"}})};
    const auto findings = analyze(opinions, 2, kTarget, {{"status", "ok"}});
    const auto finding = byCode(findings, "opinion_horizon_mismatch");
    QCOMPARE(finding.value("severity").toString(), QString("Warning"));
    QCOMPARE(finding.value("conflict_type").toString(), QString("data_quality"));
    QVERIFY(finding.value("message").toString().contains("H2"));
    QVERIFY(finding.value("message").toString().contains("H1"));
    // 意见本身与图斑一致 → 同报告附带一致性说明，不因层位不符转为矛盾。
    QCOMPARE(byCode(findings, "consistent_sources").value("conflict_type").toString(), QString("consistency_note"));
  }

  void inactiveConstraintIsDataQuality() {
    const QList<Opinion> opinions{makeOpinion("constraint", "约束 C-44", "c-v2", 2, {{"constraint_inactive", true}})};
    const auto findings = analyze(opinions, 2, kTarget, {{"status", "ok"}});
    const auto finding = byCode(findings, "constraint_inactive");
    QCOMPARE(finding.value("severity").toString(), QString("Warning"));
    QCOMPARE(finding.value("conflict_type").toString(), QString("data_quality"));
    QVERIFY(finding.value("message").toString().contains("约束 C-44"));
    QVERIFY(finding.value("suggestion").toString().contains("约束"));
  }

  void missingSourceMergedAsBrokenReference() {
    const QVariantList existing{QVariantMap{{"severity", "Blocking"},
                                           {"code", "missing_source"},
                                           {"evidence_id", "ev-9"},
                                           {"source_version", "gone-v1"},
                                           {"message", "引用的来源版本已不存在"}}};
    const auto findings = analyze({}, 2, kTarget, {}, existing);
    QCOMPARE(findings.size(), 1); // 既有 findings 存在则不产空态说明
    const auto finding = findings.first().toMap();
    QCOMPARE(finding.value("severity").toString(), QString("Blocking"));
    QCOMPARE(finding.value("code").toString(), QString("missing_source"));
    QCOMPARE(finding.value("conflict_type").toString(), QString("broken_reference"));
    QCOMPARE(finding.value("message").toString(), QString("引用的来源版本已不存在"));
    QCOMPARE(finding.value("status").toString(), QString("open"));
    QCOMPARE(finding.value("evidence").toMap().value("label").toString(), QString("ev-9"));
    QCOMPARE(finding.value("evidence").toMap().value("version_id").toString(), QString("gone-v1"));
    QCOMPARE(finding.value("target").toMap().value("feature_uid").toString(), QString("polygon-A"));
    QVERIFY(byCode(findings, "no_known_opinions").isEmpty());
  }

  void contradictingEvidenceMappedToManualDispute() {
    const QVariantList existing{QVariantMap{{"severity", "Warning"},
                                           {"code", "contradicting_evidence"},
                                           {"evidence_id", "ev-3"},
                                           {"message", "保留反驳证据，请查看争议来源"}}};
    const auto findings = analyze({}, 2, kTarget, {}, existing);
    const auto finding = byCode(findings, "contradicting_evidence");
    QCOMPARE(finding.value("conflict_type").toString(), QString("manual_dispute"));
    QCOMPARE(finding.value("severity").toString(), QString("Warning"));
    // disputed 观察同属人工争议。
    const QVariantList disputed{QVariantMap{{"severity", "warn"}, {"code", "disputed_observation"}, {"message", "争议"}}};
    const auto mapped = analyze({}, 2, kTarget, {}, disputed);
    QCOMPARE(byCode(mapped, "disputed_observation").value("conflict_type").toString(), QString("manual_dispute"));
    QCOMPARE(byCode(mapped, "disputed_observation").value("severity").toString(), QString("Warning"));
  }

  void classificationAndStaleCodesMapped() {
    const QVariantList existing{QVariantMap{{"severity", "Warning"}, {"code", "classification_changed"}, {"message", "分类表已改"}},
                               QVariantMap{{"severity", "Warning"}, {"code", "stale_source"}, {"message", "来源已过期"}}};
    const auto mapped = analyze({}, 2, kTarget, {}, existing);
    QCOMPARE(byCode(mapped, "classification_changed").value("conflict_type").toString(), QString("data_quality"));
    QCOMPARE(byCode(mapped, "stale_source").value("conflict_type").toString(), QString("broken_reference"));
  }

  void allConsistentProducesSingleNote() {
    const QList<Opinion> opinions{makeOpinion("well", "井 A", "log-v1", 2),
                                  makeOpinion("prediction", "相预测", "pred-v7", 2),
                                  makeOpinion("single_factor", "砂地比", "sf-v3", 2)};
    const auto findings = analyze(opinions, 2, kTarget, {{"status", "ok"}});
    QCOMPARE(findings.size(), 1);
    const auto finding = findings.first().toMap();
    QCOMPARE(finding.value("code").toString(), QString("consistent_sources"));
    QCOMPARE(finding.value("severity").toString(), QString("Info"));
    QCOMPARE(finding.value("conflict_type").toString(), QString("consistency_note"));
    QVERIFY(finding.value("message").toString().contains("3"));
    QVERIFY(finding.value("basis").toString().contains("井 A"));
  }

  void noOpinionsYieldsHonestEmptyState() {
    const auto findings = analyze({}, 0, kTarget, {});
    QCOMPARE(findings.size(), 1);
    const auto finding = findings.first().toMap();
    QCOMPARE(finding.value("code").toString(), QString("no_known_opinions"));
    QCOMPARE(finding.value("severity").toString(), QString("Info"));
    QCOMPARE(finding.value("conflict_type").toString(), QString("model_uncertainty"));
    QVERIFY(finding.value("message").toString().contains("无已知相意见来源"));
    // unknown 意见不算一致也不算冲突：只报空态（注明来源数）。
    QList<Opinion> unknown{makeOpinion("well", "井 D", "log-v4", 0)};
    unknown[0].known = false;
    const auto withUnknown = analyze(unknown, 2, kTarget, {});
    QCOMPARE(withUnknown.size(), 1);
    QCOMPARE(byCode(withUnknown, "no_known_opinions").value("severity").toString(), QString("Info"));
    QVERIFY(byCode(withUnknown, "no_known_opinions").value("message").toString().contains("1 个来源"));
  }

  void unresolvedAttributionYieldsModelUncertainty() {
    const QVariantMap attribution{{"status", "unresolved"},
                                  {"calculation_version", "calc-77"},
                                  {"message", "部分输入缺失，重放结果仅覆盖可用的冻结输入"},
                                  {"unresolved", QVariantList{QVariantMap{{"layer_id", "dem-30"},
                                                                          {"version_id", "v4"},
                                                                          {"reason", "冻结输入无法按保存时状态打开"}}}}};
    const auto findings = analyze({}, 2, kTarget, attribution);
    QCOMPARE(findings.size(), 1); // 有归因缺口即非空态
    const auto finding = findings.first().toMap();
    QCOMPARE(finding.value("code").toString(), QString("attribution_unresolved"));
    QCOMPARE(finding.value("severity").toString(), QString("Warning"));
    QCOMPARE(finding.value("conflict_type").toString(), QString("model_uncertainty"));
    QVERIFY(finding.value("message").toString().contains("dem-30"));
    QVERIFY(finding.value("basis").toString().contains("1 个"));
    QCOMPARE(finding.value("evidence").toMap().value("version_id").toString(), QString("calc-77"));
  }

  void partialConsistencySeverityByMajority() {
    const QVariantList distribution{QVariantMap{{"code", 3}, {"cells", 60}, {"fraction", 0.6}},
                                   QVariantMap{{"code", 2}, {"cells", 40}, {"fraction", 0.4}}};
    const QVariantMap attribution{{"status", "ok"},
                                  {"consistent_fraction", 0.4},
                                  {"decided_cells", 100},
                                  {"code_distribution", distribution}};
    const auto minority = analyze({}, 2, kTarget, attribution);
    const auto finding = byCode(minority, "partial_consistency");
    QCOMPARE(finding.value("severity").toString(), QString("Warning"));
    QCOMPARE(finding.value("conflict_type").toString(), QString("model_uncertainty"));
    QVERIFY(finding.value("message").toString().contains("60% 相 3"));
    QVERIFY(finding.value("message").toString().contains("图斑为相 2"));
    // 多数像元仍与图斑一致 → 降为 Info 说明。
    auto mostly = attribution;
    mostly.insert("consistent_fraction", 0.8);
    QCOMPARE(byCode(analyze({}, 2, kTarget, mostly), "partial_consistency").value("severity").toString(),
             QString("Info"));
    // 完全一致不产出该 finding；字段不存在也不产出（空态说明另算）。
    mostly.insert("consistent_fraction", 1.0);
    QVERIFY(byCode(analyze({}, 2, kTarget, mostly), "partial_consistency").isEmpty());
    mostly.remove("consistent_fraction");
    QVERIFY(byCode(analyze({}, 2, kTarget, mostly), "partial_consistency").isEmpty());
  }

  void opinionsFromAttributionBuildsPerInputOpinions() {
    const QVariantMap attribution{{"status", "ok"},
                                  {"inputs", QVariantList{
                                                 QVariantMap{{"layer_id", "sf-sand"},
                                                             {"version_id", "v1"},
                                                             {"title", "砂地比"},
                                                             {"priority", 1},
                                                             {"decision", "threshold_classification"},
                                                             {"cells", 60},
                                                             {"fraction", 0.75},
                                                             {"code_distribution", QVariantList{QVariantMap{{"code", 2}, {"cells", 60}}}}},
                                                 QVariantMap{{"layer_id", "sf-geo"},
                                                             {"version_id", "v2"},
                                                             {"priority", 2},
                                                             {"decision", "priority_first_valid"},
                                                             {"cells", 20},
                                                             {"fraction", 0.25},
                                                             {"code_distribution", QVariantList{QVariantMap{{"code", 3}, {"cells", 20}}}}}}}};
    const auto opinions = opinionsFromAttribution(attribution);
    QCOMPARE(opinions.size(), 2);
    QCOMPARE(opinions[0].sourceKind, QString("single_factor"));
    QCOMPARE(opinions[0].label, QString("砂地比"));
    QCOMPARE(opinions[0].versionId, QString("v1"));
    QCOMPARE(opinions[0].faciesCode, 2);
    QVERIFY(opinions[0].known);
    QVERIFY(qFuzzyCompare(opinions[0].weight, 0.75));
    QCOMPARE(opinions[0].extra.value("layer_id").toString(), QString("sf-sand"));
    QCOMPARE(opinions[0].detail, QString("优先级 1 · threshold_classification"));
    QCOMPARE(opinions[1].faciesCode, 3);
    QCOMPARE(opinions[1].label, QString("sf-geo")); // 无 title 回退 layer_id
    QVERIFY(qFuzzyCompare(opinions[1].weight, 0.25));
    // inputs 空而汇总分布存在 → 一条汇总意见（众数 + 占比）。
    const QVariantMap aggregate{{"status", "ok"},
                                {"code_distribution", QVariantList{QVariantMap{{"code", 5}, {"cells", 90}, {"fraction", 0.9}},
                                                                   QVariantMap{{"code", 6}, {"cells", 10}, {"fraction", 0.1}}}}};
    const auto summary = opinionsFromAttribution(aggregate);
    QCOMPARE(summary.size(), 1);
    QCOMPARE(summary[0].faciesCode, 5);
    QVERIFY(summary[0].known);
    QVERIFY(qFuzzyCompare(summary[0].weight, 0.9));
    // 人工归因没有自动 per-input 意见。
    QVERIFY(opinionsFromAttribution({{"status", "manual"}}).isEmpty());
    // 聚合后的意见可直接进入 analyze：与图斑不符时点名该输入。
    const auto findings = analyze(summary, 6, kTarget, aggregate);
    QCOMPARE(byCode(findings, "minority_opinion").value("evidence").toMap().value("label").toString(),
             QString("归因重放（汇总分布）"));
  }

  void severityOrderingBlockingFirst() {
    const QList<Opinion> opinions{makeOpinion("well", "井 E", "log-v5", 2, {{"horizon", "H2"}})};
    const QVariantList existing{QVariantMap{{"severity", "Info"}, {"code", "legacy_role"}, {"message", "旧证据"}},
                                QVariantMap{{"severity", "Blocking"},
                                            {"code", "missing_source_feature"},
                                            {"evidence_id", "ev-1"},
                                            {"message", "来源图元不存在"}}};
    const auto findings = analyze(opinions, 2, kTarget, {{"status", "ok"}}, existing);
    QCOMPARE(findings.size(), 4);
    QCOMPARE(findings[0].toMap().value("severity").toString(), QString("Blocking"));
    QCOMPARE(findings[0].toMap().value("code").toString(), QString("missing_source_feature"));
    QCOMPARE(findings[1].toMap().value("severity").toString(), QString("Warning"));
    QCOMPARE(findings[1].toMap().value("code").toString(), QString("opinion_horizon_mismatch"));
    QCOMPARE(findings[2].toMap().value("severity").toString(), QString("Info"));
    QCOMPARE(findings[3].toMap().value("severity").toString(), QString("Info"));
    QVERIFY(conflictTypesOf(findings).contains(QString("broken_reference")));
  }

  void severityNormalizationAndVocabularies() {
    QCOMPARE(normalizeSeverity("Blocking"), QString("Blocking"));
    QCOMPARE(normalizeSeverity("BLOCKING"), QString("Blocking"));
    QCOMPARE(normalizeSeverity("critical"), QString("Blocking"));
    QCOMPARE(normalizeSeverity("Fatal"), QString("Blocking"));
    QCOMPARE(normalizeSeverity("Error"), QString("Error"));
    QCOMPARE(normalizeSeverity("warning"), QString("Warning"));
    QCOMPARE(normalizeSeverity("warn"), QString("Warning"));
    QCOMPARE(normalizeSeverity("Info"), QString("Info"));
    QCOMPARE(normalizeSeverity("note"), QString("Info"));
    QCOMPARE(normalizeSeverity("informational"), QString("Info"));
    QCOMPARE(normalizeSeverity(""), QString("Warning"));    // 未知宁可多显示
    QCOMPARE(normalizeSeverity("古怪值"), QString("Warning"));
    const QStringList expectedSeverities{"Blocking", "Error", "Warning", "Info"};
    QCOMPARE(severities(), expectedSeverities);
    QCOMPARE(conflictTypes().size(), 7);
    QVERIFY(conflictTypes().contains(QString("spatial_contradiction")));
    QVERIFY(conflictTypes().contains(QString("interpretation_contradiction")));
    QVERIFY(conflictTypes().contains(QString("model_uncertainty")));
    QVERIFY(conflictTypes().contains(QString("manual_dispute")));
    QVERIFY(conflictTypes().contains(QString("broken_reference")));
    QVERIFY(conflictTypes().contains(QString("data_quality")));
    QVERIFY(conflictTypes().contains(QString("consistency_note")));
  }
};

QTEST_GUILESS_MAIN(TestEvidenceConflict)
#include "tst_evidenceconflict.moc"
