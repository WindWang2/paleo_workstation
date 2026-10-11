// 层：测试
// facies-ux 4：地震相 × 测井相一致性统计（逐井、混淆矩阵、一致率、κ）。
#include "../src/domain/faciesconsistency.h"
#include <QtTest>

using paleo::facies::ConsistencySample;
using paleo::facies::computeConsistency;

namespace {
ConsistencySample sample(const QString &id, std::optional<int> w, std::optional<int> s) {
  ConsistencySample out;
  out.wellId = id;
  out.wellName = id;
  out.wellCode = w;
  out.seismicCode = s;
  return out;
}
QVariantList schema() {
  return {QVariantMap{{"code", 1}, {"name", "A"}, {"color", "#ff0000"}},
          QVariantMap{{"code", 2}, {"name", "B"}, {"color", "#00ff00"}}};
}
} // namespace

class TestFaciesConsistency : public QObject {
  Q_OBJECT
private slots:
  void confusionAgreementAndKappa() {
    // 3 井一致（1-1, 1-1, 2-2），1 井不一致（2-1），1 井未预测，1 井图外。
    const auto r = computeConsistency(
        {sample("W1", 1, 1), sample("W2", 1, 1), sample("W3", 2, 2),
         sample("W4", 2, 1), sample("W5", std::nullopt, 2),
         sample("W6", 1, std::nullopt)},
        schema());
    QCOMPARE(r.compared, 4);
    QCOMPARE(r.agreed, 3);
    QCOMPARE(r.codes, (QVector<int>{1, 2}));
    QCOMPARE(r.confusion, (QVector<QVector<int>>{{2, 0}, {1, 1}}));
    QVERIFY(qFuzzyCompare(r.agreement, 0.75));
    // po=.75；pe=(2/4*3/4)+(2/4*1/4)=.5；κ=(.75-.5)/.5=.5
    QVERIFY(qAbs(r.kappa - 0.5) < 1e-9);
    QVERIFY(!r.rows[4].compared);
    QVERIFY(!r.rows[4].reason.isEmpty());
    QVERIFY(!r.rows[5].compared);
    QVERIFY(!r.rows[5].reason.isEmpty());
    const auto v = r.toVariant(schema());
    QCOMPARE(v.value("schema").toString(), QString("paleo_facies_consistency_v1"));
    QCOMPARE(v.value("uncompared").toInt(), 2);
    QCOMPARE(v.value("rows").toList().at(3).toMap().value("well_facies").toString(),
             QString("B"));
    QCOMPARE(v.value("rows").toList().at(3).toMap().value("seismic_facies").toString(),
             QString("A"));
    QCOMPARE(v.value("confusion").toList().at(1).toList().at(0).toInt(), 1);
  }
  void codesOutsideSchemaStillCounted() {
    const auto r = computeConsistency({sample("W1", 7, 7), sample("W2", 1, 7)}, schema());
    QVERIFY(r.codes.contains(7));
    QCOMPARE(r.compared, 2);
    QCOMPARE(r.agreed, 1);
  }
  void emptyComparisonIsSafe() {
    const auto r = computeConsistency({sample("W1", std::nullopt, std::nullopt)}, schema());
    QCOMPARE(r.compared, 0);
    QCOMPARE(r.agreement, 0.0);
    QCOMPARE(r.rows.size(), 1);
  }
};

QTEST_GUILESS_MAIN(TestFaciesConsistency)
#include "tst_faciesconsistency.moc"
