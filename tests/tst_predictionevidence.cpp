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
  // 预测专项 P0：成果来源三分（真实模型 / 模拟 Mock / 人工修订）必须显式
  // 落在证据链与界面摘要里；Mock 输入的派生与修订不得被「洗白」成真实模型
  // 结果；无独立验证数据时如实写「尚无独立验证」，置信度只是模型分数。
  void syntheticInputRemainsDistinctFromMock() {
    InputRef log; log.versionId = "v1"; log.syntheticInput = true;
    const auto evidence = build({log}, {{"method", "remote-well-facies-api-v2"}}, {});
    const auto provenance = evidence.value("provenance").toMap();
    QVERIFY(provenance.value("synthetic_input").toBool());
    QVERIFY(!provenance.value("based_on_mock").toBool());
    QCOMPARE(provenance.value("validation").toString(), QString("unvalidated"));
    QVERIFY(evidence.value("inputs").toList().first().toMap().value("synthetic_input").toBool());
    QVERIFY(summary(evidence).contains(QStringLiteral("含合成输入")));
  }
  void provenanceSeparatesModelMockAndRevision() {
    InputRef log{"v2", "a2", "A.las", "well_log", "", 1, "def", "RAW", false};
    const auto real = build({log}, {{"method", "remote-well-facies-api-v2"},
                                    {"model_names", QStringList{"m1"}}}, {});
    const auto rp = real.value("provenance").toMap();
    QCOMPARE(rp.value("kind").toString(), QString("model_prediction"));
    QCOMPARE(rp.value("based_on_mock").toBool(), false);
    QCOMPARE(rp.value("validation").toString(), QString("unvalidated"));
    QCOMPARE(rp.value("confidence_semantics").toString(), QString("model_score"));
    QVERIFY2(summary(real).contains(QStringLiteral("模型预测")), qPrintable(summary(real)));
    QVERIFY2(summary(real).contains(QStringLiteral("尚无独立验证")), qPrintable(summary(real)));
    QVERIFY(!summary(real).contains(QStringLiteral("准确率")));

    const auto mock = build({log}, {{"method", "mock-horizon-window-kmeans-well-majority-v2"},
                                    {"mock", true}}, {});
    QCOMPARE(mock.value("provenance").toMap().value("kind").toString(), QString("mock"));
    QVERIFY2(summary(mock).contains(QStringLiteral("模拟")), qPrintable(summary(mock)));

    // 派生自 Mock 输入但本版本 extra 未打 mock 标记：仍须识别为模拟来源。
    InputRef mockInput{"v3", "a3", "mock map", "facies", "seismic_mock_prediction", 1, "x", "DERIVED", true};
    const auto derived = build({mockInput}, {{"method", "paleo:paleo_facies_fusion"}}, {});
    QCOMPARE(derived.value("provenance").toMap().value("based_on_mock").toBool(), true);
    QCOMPARE(derived.value("provenance").toMap().value("kind").toString(), QString("mock"));

    // 人工修订：保持「修订」身份，同时保留其底图是否来自 Mock。
    const auto revised = build({mockInput}, {{"method", "manual-edit-copy"}, {"mock", true}}, {});
    const auto vp = revised.value("provenance").toMap();
    QCOMPARE(vp.value("kind").toString(), QString("manual_revision"));
    QCOMPARE(vp.value("based_on_mock").toBool(), true);
    QVERIFY2(summary(revised).contains(QStringLiteral("人工修订")), qPrintable(summary(revised)));
    QVERIFY2(summary(revised).contains(QStringLiteral("模拟")), qPrintable(summary(revised)));

    // 只有真的带了独立验证指标才可声称已验证。
    const auto validated = build({log}, {{"method", "remote-well-facies-api-v2"},
                                         {"model_names", QStringList{"m1"}},
                                         {"validation_metrics", QVariantMap{{"holdout_wells", 4}}}}, {});
    QCOMPARE(validated.value("provenance").toMap().value("validation").toString(), QString("validated"));
    // 空值指标不得抬成已验证（review L3）。
    const auto emptyMetrics = build({log}, {{"method", "remote-well-facies-api-v2"},
                                            {"model_names", QStringList{"m1"}},
                                            {"validation_metrics", QVariantMap{{"note", ""}, {"", 1}}}}, {});
    QCOMPARE(emptyMetrics.value("provenance").toMap().value("validation").toString(), QString("unvalidated"));
    // 来源块不进参数指纹（同参数同指纹）。
    QCOMPARE(real.value("parameter_hash"), build({log}, {{"method", "remote-well-facies-api-v2"},
                                                         {"model_names", QStringList{"m1"}}}, {}).value("parameter_hash"));
  }

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
