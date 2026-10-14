// 层：测试
#include "../src/domain/interpretationevidence.h"
#include <QtTest>
using namespace paleo::evidence;
class TestInterpretationEvidence : public QObject {
  Q_OBJECT
private:
  const QVariantMap target{{"feature_uid", "polygon-A"}, {"horizon", "H1"}, {"version_id", "v1"}, {"geometry_hash", "shape1"}, {"facies_schema_hash", "schema1"}};
  const QVariantMap source{{"id", "well-evidence"}, {"text", "井 A 支持河道"}, {"type", "well"}, {"role", "SUPPORTED_BY"}, {"source_version", "log-v1"}};
private slots:
  void fullHistoryOrSupersededForksOnlyInAnotherSavedVersion() {
    Revision r; QString error;
    QVERIFY(revise({}, {}, target, "review", {{"status", "Superseded"}}, "local:user", "由新解释替代", 0, &r, &error));
    const auto parent = r.interpretation;
    QVERIFY(!revise({}, parent, target, "edit", {}, "local:user", "继续修订", 1, &r, &error));
    auto saved = target; saved.insert("version_id", "saved-v2");
    QVERIFY2(revise({}, parent, saved, "edit", {}, "local:user", "在保存的新版本继续修订", 1, &r, &error), qPrintable(error));
    QVERIFY(r.interpretation.value("id") != parent.value("id")); QCOMPARE(r.interpretation.value("revision").toInt(), 1);
    QCOMPARE(r.interpretation.value("inherited_interpretation").toMap(), parent);
    QCOMPARE(r.interpretation.value("parent_version_id").toString(), QString("saved-v2"));
    auto full = parent; full.insert("status", "NeedsReview"); full.insert("revision", 10000);
    QVariantList history;
    for (int n = 0; n < 10000; ++n) history << QVariantMap{{"event_id", QString::number(n)}, {"actor_id", "local:user"}, {"reason", "修订"},
      {"timestamp", "2026-10-14T00:00:00.000Z"}, {"before_revision", n}, {"after_revision", n + 1}, {"version_id", "v1"}};
    full.insert("history", history); QVERIFY(validateInterpretation(full, &error));
    QVERIFY(!revise({}, full, target, "edit", {}, "local:user", "满额旧解释只读", 10000, &r, &error));
    QVERIFY2(revise({}, full, saved, "edit", {}, "local:user", "新成果修订", 10000, &r, &error), qPrintable(error));
    QCOMPARE(r.interpretation.value("history").toList().size(), 1);
    QCOMPARE(r.interpretation.value("inherited_interpretation").toMap().value("history").toList().size(), 10000);
    QCOMPARE(full.value("history").toList().size(), 10000);
  }
  void repeatedSpatialEditsDoNotRecursivelyEmbedAuditHistory() {
    Revision r; QString error;
    for (int n = 0; n < 100; ++n) {
      const auto before = encode(r.interpretation);
      QVERIFY2(revise(r.records, r.interpretation, target, "edit",
          {{"before", QVariantMap{{"interpretation", before}, {"evidence", encode(r.records)}, {"geometry", "POLYGON((0 0,1 0,1 1,0 0))"}}}},
          "local:user", "调整几何", n, &r, &error), qPrintable(error));
    }
    QCOMPARE(r.interpretation.value("history").toList().size(), 100);
    const auto change = r.interpretation.value("history").toList().last().toMap().value("change").toMap();
    QVERIFY(!change.value("before").toMap().value("interpretation").toMap().contains("history"));
    QVERIFY(!change.value("before").toMap().contains("evidence"));
    QVERIFY(encode(r.interpretation).size() < 300000);
    QVariantMap restored; QVERIFY(decodeInterpretation(encode(r.interpretation), &restored, &error));
    QCOMPARE(restored.value("history").toList().size(), 100);
  }
  void legacyAndCorruptReads() {
    QVariantList rows;
    QString error;
    QVERIFY(decodeRecords("", &rows, &error)); QVERIFY(rows.isEmpty());
    QVERIFY(decodeRecords("[{\"id\":\"legacy\",\"text\":\"旧证据\"}]", &rows, &error));
    QCOMPARE(rows.front().toMap().value("text").toString(), QString("旧证据"));
    const auto prior = rows;
    QVERIFY(!decodeRecords("{bad", &rows, &error)); QCOMPARE(rows, prior);
    QVERIFY(!decodeRecords("{}", &rows, &error)); QCOMPARE(rows, prior);
    QVERIFY(!decodeRecords("[1]", &rows, &error));
    QVERIFY(!decodeRecords("[{\"id\":\"x\"},{\"id\":\"x\"}]", &rows, &error));
    QVariantMap interpretation;
    QVERIFY(decodeInterpretation("", &interpretation, &error));
    QVERIFY(!decodeInterpretation("[]", &interpretation, &error));
  }
  void reviewUnlinkHistoryAndRoundTrip() {
    QString error;
    Revision r;
    QVERIFY2(revise({}, {}, target, "link", source, "local:geologist", "观察井相", 0, &r, &error), qPrintable(error));
    QCOMPARE(r.interpretation.value("revision").toInt(), 1);
    QCOMPARE(r.interpretation.value("status").toString(), QString("NeedsReview"));
    const auto key = r.interpretation.value("id");
    QVERIFY(!key.toString().isEmpty());
    QVERIFY(!r.interpretation.value("assertion_id").toString().isEmpty());
    QVERIFY2(revise(r.records, r.interpretation, target, "review", {{"status", "Confirmed"}}, "local:geologist", "接受该解释", 1, &r, &error), qPrintable(error));
    QCOMPARE(r.interpretation.value("id"), key);
    QCOMPARE(r.interpretation.value("status").toString(), QString("Confirmed"));
    // Accepting the interpretation never verifies its underlying observation.
    QCOMPARE(r.records[0].toMap().value("verification").toString(), QString("Unverified"));
    QVERIFY(revise(r.records, r.interpretation, target, "verify_evidence", {{"evidence_id", "well-evidence"}, {"verification", "Verified"}}, "local:checker", "核对原始测井", 2, &r, &error));
    QCOMPARE(r.records[0].toMap().value("verification").toString(), QString("Verified"));
    QVERIFY(revise(r.records, r.interpretation, target, "unlink", {{"evidence_id", "well-evidence"}}, "local:geologist", "纠正空间误关联", 3, &r, &error));
    QCOMPARE(r.records.size(), 1); QVERIFY(r.records[0].toMap().value("unlinked").toBool());
    const auto events = r.interpretation.value("history").toList(); QCOMPARE(events.size(), 4);
    const auto e = events.back().toMap();
    QCOMPARE(e.value("before_record").toMap().value("source_version").toString(), QString("log-v1"));
    QCOMPARE(e.value("before_revision").toInt(), 3); QCOMPARE(e.value("after_revision").toInt(), 4);
    QVERIFY(!e.value("event_id").toString().isEmpty());
    QVariantList savedRecords; QVariantMap savedInterpretation;
    QVERIFY(decodeRecords(encode(r.records), &savedRecords, &error));
    QVERIFY(decodeInterpretation(encode(r.interpretation), &savedInterpretation, &error));
    QCOMPARE(savedRecords[0].toMap().value("source_version"), r.records[0].toMap().value("source_version"));
    QCOMPARE(savedInterpretation.value("history").toList().size(), 4);
  }
  void rejectsConflictMissingIdentityAndIllegalStateWithoutMutation() {
    Revision r; QString error;
    QVERIFY(revise({}, {}, target, "link", source, "local:user", "reason", 0, &r, &error));
    const auto records = r.records; const auto i = r.interpretation;
    QVERIFY(!revise(records, i, target, "unlink", {{"evidence_id", "well-evidence"}}, "local:user", "reason", 0, &r, &error));
    QCOMPARE(r.records, records); QCOMPARE(r.interpretation, i);
    QVERIFY(!revise(records, i, target, "review", {{"status", "Unknown"}}, "local:user", "reason", 1, &r, &error));
    QVERIFY(!revise(records, i, target, "review", {{"status", "Confirmed"}}, "", "reason", 1, &r, &error));
    QVERIFY(!revise(records, i, target, "review", {{"status", "Confirmed"}}, "local:user", "", 1, &r, &error));
    auto other = target; other.insert("horizon", "H2");
    QVERIFY(!revise(records, i, other, "review", {{"status", "Confirmed"}}, "local:user", "reason", 1, &r, &error));
    QVERIFY(!revise(records, i, target, "link", source, "local:user", "reason", 1, &r, &error));
    QVERIFY(revise(records, i, target, "review", {{"status", "Superseded"}}, "local:user", "new version", 1, &r, &error));
    QVERIFY(!revise(r.records, r.interpretation, target, "review", {{"status", "Confirmed"}}, "local:user", "reason", 2, &r, &error));
  }
  void conflictsGeometryAndMissingSourcesRemainExplicit() {
    Revision r; QString error;
    auto contradiction = source; contradiction.insert("role", "CONTRADICTED_BY");
    QVERIFY(revise({}, {}, target, "link", contradiction, "local:user", "conflicting input", 0, &r, &error));
    auto moved = target; moved.insert("geometry_hash", "shape2");
    const auto findings = check(r.records, r.interpretation, moved);
    QSet<QString> codes;
    for (const auto &finding : findings) codes.insert(finding.toMap().value("code").toString());
    QVERIFY(codes.contains("geometry_changed")); QVERIFY(codes.contains("contradicting_evidence"));
    // Warnings permit deliberate dispute/acceptance; broken source identity blocks confirmation.
    QVERIFY(revise(r.records, r.interpretation, target, "review", {{"status", "Disputed"}}, "local:user", "保留争议", 1, &r, &error));
    auto missing = r.records[0].toMap(); missing.insert("source_missing", true); r.records[0] = missing;
    const auto original = r.interpretation;
    QVERIFY(!revise(r.records, r.interpretation, target, "review", {{"status", "Confirmed"}}, "local:user", "review", 2, &r, &error));
    QCOMPARE(r.interpretation, original);
  }
};
QTEST_GUILESS_MAIN(TestInterpretationEvidence)
#include "tst_interpretationevidence.moc"
