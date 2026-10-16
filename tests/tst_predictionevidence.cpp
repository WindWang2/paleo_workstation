// 层：测试
// facies-ux 5：证据链结构、参数指纹稳定性（簿记字段不进指纹）。
#include "../src/domain/predictionevidence.h"
#include <QtTest>

using namespace paleo::evidence;

class TestPredictionEvidence : public QObject {
  Q_OBJECT
private slots:
  void buildCarriesInputsModelSupport() {
    InputRef seismic{"v1", "a1", "survey", "seismic", "", 3, "abc", "RAW", false};
    InputRef log{"v2", "a2", "A.las", "well_log", "", 1, "def", "RAW", true};
    const QVariantMap extra{{"method", "remote-well-facies-api-v2"},
                            {"model_names", QStringList{"m1"}},
                            {"wells", QStringList{"A", "B"}},
                            {"supporting_wells", QVariantList{QVariantMap{{"id", "A"}}}},
                            {"skipped_well_count", 1},
                            {"window_half_ms", 12.0},
                            {"facies", QVariantList{QVariantMap{{"code", 1}}}},
                            {"created_at", "2026-10-11T00:00:00Z"},
                            {"title", "t"}};
    const auto e = build({seismic, log}, extra, {{"qgis", "4.0"}});
    QCOMPARE(e.value("schema").toString(), QString("paleo_evidence_v1"));
    QCOMPARE(e.value("inputs").toList().size(), 2);
    QCOMPARE(e.value("inputs").toList().at(1).toMap().value("sha256").toString(), QString("def"));
    QCOMPARE(e.value("model").toMap().value("method").toString(), QString("remote-well-facies-api-v2"));
    QCOMPARE(e.value("model").toMap().value("model_names").toStringList(), QStringList{"m1"});
    const auto support = e.value("support").toMap();
    QCOMPARE(support.value("wells").toMap().value("supporting_wells").toList().size(), 1);
    QCOMPARE(support.value("seismic_attributes").toMap().value("inputs").toList().size(), 1);
    QCOMPARE(support.value("seismic_attributes").toMap().value("parameters").toMap().value("window_half_ms").toDouble(), 12.0);
    QVERIFY(!e.value("parameters").toMap().contains("created_at"));
    QVERIFY(!e.value("parameters").toMap().contains("facies"));
    QCOMPARE(e.value("parameter_hash").toString().size(), 64);
    QCOMPARE(e.value("facies_schema_hash").toString().size(), 64);
    QVERIFY(summary(e).contains("m1"));
    QVERIFY(summary({}).size() > 0);
  }
  // opt-04：validate 此前对"缺证据"与"证据健康"都返回空列表，四个调用点
  // 只消费 Blocking，于是早期版本可静默通过发布校验。缺证据必须显式告警。
  void validateWarnsOnMissingEvidence() {
    const auto empty = validate({});
    QCOMPARE(empty.size(), 1);
    const auto row = empty.at(0).toMap();
    QCOMPARE(row.value("severity").toString(), QString("Warning"));
    QCOMPARE(row.value("code").toString(), QString("no_evidence"));
    QVERIFY(!row.value("message").toString().isEmpty());
    // 必须是 Warning —— 既有 derivedassets / mapversioncontroller 只判 Blocking，
    // 若误升为 Blocking 会让所有早期版本无法登记。
    QVERIFY(row.value("severity").toString() != QString("Blocking"));
  }
  // 未知/损坏 schema 必须与"完全没有记录"可区分：前者 Blocking unknown_schema，
  // 后者 Warning no_evidence，两者不得互相污染。
  void validateDistinguishesUnknownSchemaFromAbsent() {
    const auto unknown = validate({{"schema", "bogus"}});
    QCOMPARE(unknown.size(), 1);
    QCOMPARE(unknown.at(0).toMap().value("severity").toString(), QString("Blocking"));
    QCOMPARE(unknown.at(0).toMap().value("code").toString(), QString("unknown_schema"));
    QVERIFY(unknown.at(0).toMap().value("code").toString() != QString("no_evidence"));

    const auto healthy = validate({{"schema", "paleo_evidence_v1"},
                                   {"inputs", QVariantList{}},
                                   {"parameters", QVariantMap{}},
                                   {"model", QVariantMap{}}});
    for (const auto &f : healthy)
      QVERIFY(f.toMap().value("code").toString() != QString("no_evidence"));
  }
  // 来源缺失是 Blocking（登记事务整体 abort），文案不得宣称"保留原引用"。
  void validateMissingSourceMessageMatchesBehavior() {
    InputRef gone{"v9", "a9", "gone", "seismic", "", 1, "", "RAW", false};
    gone.missing = true;
    const auto e = build({gone}, {{"method", "m"}}, {{"qgis", "4.0"}});
    const auto findings = validate(e);
    bool found = false;
    for (const auto &f : findings) {
      const auto row = f.toMap();
      if (row.value("code").toString() != QString("missing_source")) continue;
      found = true;
      QCOMPARE(row.value("severity").toString(), QString("Blocking"));
      QVERIFY(!row.value("message").toString().contains(QString("保留原引用")));
    }
    QVERIFY(found);
  }
  void fingerprintIgnoresBookkeeping() {
    QVariantMap a{{"method", "x"}, {"k", 1}, {"created_at", "t1"}, {"title", "A"}};
    QVariantMap b{{"k", 1}, {"method", "x"}, {"created_at", "t2"}, {"title", "B"}};
    QCOMPARE(fingerprint(parameters(a)), fingerprint(parameters(b)));
    b.insert("k", 2);
    QVERIFY(fingerprint(parameters(a)) != fingerprint(parameters(b)));
  }
};

QTEST_GUILESS_MAIN(TestPredictionEvidence)
#include "tst_predictionevidence.moc"
