// 层：测试壳
#include <QtTest/QtTest>

#include "services/algorithmcontract.h"
#include "services/algoparamschema.h"
#include "services/capabilityspec.h"

class TestAlgorithmContract : public QObject {
  Q_OBJECT
private slots:
  void versionAndParams();
  void exposureIsNotComplete();
  void constraintDefaultsStayOnTheSpec();
  void aliasesDoNotCollide();
};

void TestAlgorithmContract::versionAndParams() {
  QVERIFY(AlgorithmSpecRegistry::knows(QStringLiteral("native:depth_batch_convert")));
  QVERIFY(AlgorithmSpecRegistry::specAtVersion(QStringLiteral("native:depth_batch_convert"), 2).id.isEmpty());
  QVERIFY(!AlgorithmSpecRegistry::specAtVersion(QStringLiteral("native:depth_batch_convert"), 1).id.isEmpty());
  QVariantMap missing;
  const auto rejected = validateAndFillParams(QStringLiteral("native:depth_batch_convert"), 1, missing);
  QVERIFY(!rejected.ok);
  QCOMPARE(rejected.code, QStringLiteral("missing_required"));
  QVariantMap params;
  params.insert(QStringLiteral("TIME_SEMANTIC"), QStringLiteral("TWT"));
  params.insert(QStringLiteral("DEPTH_SEMANTIC"), QStringLiteral("TVD"));
  params.insert(QStringLiteral("EXTRAPOLATION"), QStringLiteral("reject_outside"));
  params.insert(QStringLiteral("MODEL_REVISION"), QStringLiteral("m1"));
  params.insert(QStringLiteral("NOT_A_PARAM"), 1);
  const auto unknown = validateAndFillParams(QStringLiteral("native:depth_batch_convert"), 1, params);
  QVERIFY(!unknown.ok);
  QCOMPARE(unknown.code, QStringLiteral("unknown_param"));
  params.remove(QStringLiteral("NOT_A_PARAM"));
  const auto ok = validateAndFillParams(QStringLiteral("native:depth_batch_convert"), 1, params);
  QVERIFY(ok.ok);
  QCOMPARE(ok.effective.value(QStringLiteral("TIME_SEMANTIC")).toString(), QStringLiteral("TWT"));

  QVariantMap seismic;
  seismic.insert(QStringLiteral("ATTRIBUTES"), QStringList{QStringLiteral("rms"), QStringLiteral("nope")});
  seismic.insert(QStringLiteral("SAMPLE_INTERVAL_MS"), 4.0);
  const auto badList = validateAndFillParams(QStringLiteral("native:seismic_attribute_group"), 1, seismic);
  QVERIFY(!badList.ok);
  QCOMPARE(badList.issues.first().field, QStringLiteral("ATTRIBUTES"));
  seismic.insert(QStringLiteral("ATTRIBUTES"), QStringList{QStringLiteral("rms"), QStringLiteral("envelope")});
  const auto filled = validateAndFillParams(QStringLiteral("native:seismic_attribute_group"), 1, seismic);
  QVERIFY(filled.ok);
  QCOMPARE(filled.effective.value(QStringLiteral("WINDOW_HALF")).toInt(), 8);
  QCOMPARE(filled.effective.value(QStringLiteral("COHERENCE_WEIGHT")).toString(), QStringLiteral("equal"));
}

void TestAlgorithmContract::exposureIsNotComplete() {
  const AlgorithmSpec idw = AlgorithmSpecRegistry::spec(QStringLiteral("paleo:paleo_constraint_idw"));
  QVERIFY(idw.complete);
  QString reason;
  QCOMPARE(capabilityExposureName(CapabilityRegistry::exposureForAlgorithm(idw.id, &reason)),
           QStringLiteral("callable"));
  QString nativeReason;
  QCOMPARE(capabilityExposureName(
               CapabilityRegistry::exposureForAlgorithm(QStringLiteral("inversion:bandlimited"), &nativeReason)),
           QStringLiteral("internal"));
  QVERIFY(nativeReason.contains(QStringLiteral("typed native")));
  QString legacyReason;
  QCOMPARE(capabilityExposureName(
               CapabilityRegistry::exposureForAlgorithm(QStringLiteral("paleo:paleo_welldist"), &legacyReason)),
           QStringLiteral("legacy-adapter"));
  QVERIFY(!CapabilityRegistry::callableIds().contains(QStringLiteral("paleo:paleo_welldist")));
  QCOMPARE(AlgorithmParamSchema::registeredAlgorithms().size(), 5);
  QVERIFY(!AlgorithmParamSchema::knows(QStringLiteral("native:seismic_attribute_group")));
}

void TestAlgorithmContract::constraintDefaultsStayOnTheSpec() {
  QVariantMap user;
  user.insert(QStringLiteral("FIELD"), QStringLiteral("z"));
  user.insert(QStringLiteral("CELL_SIZE"), 12.5);
  const auto effective = validateAndFillParams(QStringLiteral("paleo:paleo_constraint_idw"), 1, user);
  QVERIFY(effective.ok);
  QCOMPARE(effective.effective.value(QStringLiteral("ANISO_RATIO")).toDouble(), 2.0);
  QVERIFY(!effective.effective.contains(QStringLiteral("FACIES_CODE")));
  const auto latest = validateAndFillParams(QStringLiteral("paleo:paleo_constraint_idw"), 9, user);
  QCOMPARE(latest.code, QStringLiteral("unsupported_version"));
}

void TestAlgorithmContract::aliasesDoNotCollide() {
  QVERIFY(CapabilityRegistry::aliasCollisions().isEmpty());
  QString code;
  QCOMPARE(CapabilityRegistry::resolveAlias(QStringLiteral("paleo.depth_convert_batch"), &code),
           QStringLiteral("depth.convert_batch"));
  QVERIFY(code.isEmpty());
  const CapabilitySpec facies = CapabilityRegistry::spec(QStringLiteral("facies.draft.publish"), 1);
  QCOMPARE(facies.algorithmId, QStringLiteral("paleo:facies_draft_mapping"));
  QCOMPARE(AlgorithmSpecRegistry::spec(facies.algorithmId).outputs.size(), 2);
  const CapabilitySpec missing = CapabilityRegistry::spec(QStringLiteral("wellfacies.predict"), 1);
  QCOMPARE(capabilityExposureName(missing.exposure), QStringLiteral("unavailable"));
}

QTEST_MAIN(TestAlgorithmContract)
#include "tst_algorithm_contract.moc"
