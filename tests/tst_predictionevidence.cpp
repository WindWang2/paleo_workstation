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
