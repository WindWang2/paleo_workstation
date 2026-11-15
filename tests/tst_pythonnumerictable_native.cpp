// 层：测试壳
#include "fixtures/pythonnumerictablenativefixture.h"
#include <QtTest>
using namespace paleo;
using namespace numeric_table_native_fixture;
class TestPythonNumericTableNative:public QObject {
  Q_OBJECT
private slots:
  void managedSdkTableComputePagesRestartAndPublishOnce() {
    Fixture f;QVERIFY2(f.open(),qPrintable(f.error));QVERIFY2(f.activateRuntime(),qPrintable(f.error));f.configureManaged();
    const auto identity=f.original.identity;const auto parent=f.table.plan.source.versionId;
    const auto response=f.call(QLatin1String(python::NumericTableTool),f.arguments("table-compute"));
    QVERIFY2(response.result.ok,qPrintable(response.result.code));QVERIFY(f.derived().isEmpty());
    const auto saved=f.record("table-compute");QCOMPARE(saved.computeCount,1);
    const auto envelope=saved.frozenRequest.value("plan").toObject();
    QCOMPARE(envelope.value("schema").toString(),QString("paleo.python-environment-operation.v1"));
    QCOMPARE(envelope.value("environmentIdentityId").toString(),identity.id);
    const auto plan=envelope.value("plan").toObject();QCOMPARE(plan.value("schema").toString(),QString("paleo.python-numeric-table-operation.v1"));
    QCOMPARE(saved.stagePlan.value("parents").toArray(),QJsonArray{parent});
    const auto metadata=plan.value("inputMetadata").toObject();
    QCOMPARE(metadata.value("provenance").toObject().value("readCut"),QJsonValue(python::numericTableReadCutJson(f.table.descriptor.cut)));
    const auto report=f.report(saved.operationId);QVERIFY2(!report.isEmpty(),qPrintable(f.error));
    QCOMPARE(report.value("provenance"),metadata.value("provenance"));
    QCOMPARE(report.value("sourceKind").toString(),QString("source_table_numeric_projection"));
    const auto metrics=report.value("metrics").toArray();QCOMPARE(metrics.size(),2);
    QVERIFY(qAbs(metrics[0].toObject().value("value").toDouble()-2.0/3)<1e-12);
    QVERIFY(qAbs(metrics[1].toObject().value("value").toDouble()-2.0/3)<1e-12);
    QCOMPARE(metrics[0].toObject().value("unit").toString(),QString("mV"));
    const auto replay=f.call(QLatin1String(python::NumericTableTool),f.arguments("table-compute"));
    QVERIFY(replay.result.ok);QCOMPARE(replay.result.operationId,saved.operationId);QCOMPARE(f.record("table-compute").computeCount,1);
    const auto conflict=f.call(QLatin1String(python::NumericTableTool),f.arguments("table-compute",numeric_table_fixture::sourceCode()+"\n# changed\n"));
    QCOMPARE(conflict.result.code,QString("idempotency_conflict"));
    // Change the live declaration legitimately. Publication must use the frozen
    // ReadCut/bytes/report, without claiming this old cut is a SQL history lease.
    auto changed=f.table.declaration;changed.columns[0].unit="uV";std::string detail,revision;data::ModelChangeHead head;
    QVERIFY(f.table.sdk.readModelChangeHead(head,detail));QVERIFY(f.table.sdk.confirmNumericTableProjection(changed,head,f.table.descriptor.revision,revision,detail));
    QVERIFY(revision!=f.table.descriptor.revision);QVERIFY2(f.restartWithAnotherActivePolicy(),qPrintable(f.error));
    const auto historical=f.registry->resolve(identity.id,identity.runtimeSha256,identity.policy.json(),python::EnvironmentMode::Generated);
    QVERIFY2(historical.ok(),qPrintable(historical.code));QVERIFY(!historical.sandbox->snapshot().value("ready").toBool());
    const auto publish=f.publication("table-publish",saved.operationId);const auto published=f.call(QLatin1String(python::NumericTableTool),publish);
    QVERIFY2(published.result.ok,qPrintable(published.result.code));QCOMPARE(published.result.commitState,QString("committed"));
    QCOMPARE(f.record("table-publish").computeCount,0);QCOMPARE(f.record("table-compute").computeCount,1);
    QCOMPARE(f.derived().size(),1);QCOMPARE(f.derived().first().parentVersionIds,QStringList{parent});
    QVERIFY(!historical.sandbox->snapshot().value("ready").toBool());QCOMPARE(f.report(saved.operationId),report);
    QVERIFY(f.call(QLatin1String(python::NumericTableTool),publish).result.ok);QCOMPARE(f.derived().size(),1);
    f.registry->denyInMemory(identity.id);QVERIFY(f.registry->revoke(identity.id,2).ok());
    const auto denied=f.call(QLatin1String(python::NumericTableTool),f.publication("table-revoked",saved.operationId));
    QVERIFY(!denied.result.ok);QCOMPARE(denied.result.code,QString("python_environment_identity_revoked"));QCOMPARE(f.derived().size(),1);
  }
  void legacyGeneratedV1RestoresThroughManagedHostWithoutRecomputation() {
    Fixture f;QVERIFY2(f.open(true),qPrintable(f.error));QVERIFY2(f.activateRuntime(),qPrintable(f.error));
    QVERIFY2(f.selectLegacyCurve(),qPrintable(f.error));f.configureLegacyBare();const auto identity=f.original.identity;
    const auto computed=f.call(QLatin1String(python::GeneratedTool),f.arguments("legacy-compute",legacyCode(),true));
    QVERIFY2(computed.result.ok,qPrintable(computed.result.code));const auto saved=f.record("legacy-compute");QCOMPARE(saved.computeCount,1);
    const auto plan=saved.frozenRequest.value("plan").toObject();QCOMPARE(plan.value("schema").toString(),QString("paleo.python-generated-operation.v1"));
    QVERIFY(!plan.contains("environmentIdentityId"));const auto report=f.report(saved.operationId);QVERIFY2(!report.isEmpty(),qPrintable(f.error));
    QCOMPARE(report.value("metrics").toArray().first().toObject().value("value").toDouble(),2.0);
    QCOMPARE(report.value("metrics").toArray().first().toObject().value("unit").toString(),QString("API"));QVERIFY(f.derived().isEmpty());
    QVERIFY2(f.restartWithAnotherActivePolicy(),qPrintable(f.error));
    const auto historical=f.registry->resolve({},identity.runtimeSha256,identity.policy.json(),python::EnvironmentMode::Generated);
    QVERIFY2(historical.ok(),qPrintable(historical.code));QVERIFY(!historical.sandbox->snapshot().value("ready").toBool());
    const auto publish=f.publication("legacy-publish",saved.operationId);const auto published=f.call(QLatin1String(python::GeneratedTool),publish);
    QVERIFY2(published.result.ok,qPrintable(published.result.code));QCOMPARE(f.record("legacy-publish").computeCount,0);
    QCOMPARE(f.record("legacy-compute").computeCount,1);QCOMPARE(f.derived().size(),1);
    QCOMPARE(f.derived().first().parentVersionIds,QStringList{f.table.plan.source.versionId});QCOMPARE(f.report(saved.operationId),report);
    QVERIFY(!historical.sandbox->snapshot().value("ready").toBool());QVERIFY(f.call(QLatin1String(python::GeneratedTool),publish).result.ok);
    QCOMPARE(f.derived().size(),1);
  }
};
QTEST_GUILESS_MAIN(TestPythonNumericTableNative)
#include "tst_pythonnumerictable_native.moc"
