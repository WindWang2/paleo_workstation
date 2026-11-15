// 层：测试壳
#include "fixtures/pythonseismictracehostfixture.h"
#include <QtTest>
using namespace paleo;
using namespace seismic_trace_host_fixture;
class TestPythonSeismicTraceNative:public QObject {
  Q_OBJECT
private slots:
  void actualSdkAndAuthenticatedTraceComputePagesRestartPublishOnce() {
    Fixture f;QVERIFY2(f.openTrace(),qPrintable(f.error));QVERIFY2(f.activateRuntime(),qPrintable(f.error));f.configureTrace();
    const auto identity=f.original.identity;const auto parent=f.source.versionId;const auto computed=f.call(QLatin1String(python::SeismicTraceTool),f.traceArguments("trace-compute"));
    QVERIFY2(computed.result.ok,qPrintable(computed.result.code));QVERIFY(f.derived().isEmpty());const auto saved=f.record("trace-compute");QCOMPARE(saved.computeCount,1);
    const auto envelope=saved.frozenRequest.value("plan").toObject();QCOMPARE(envelope.value("environmentIdentityId").toString(),identity.id);
    const auto plan=envelope.value("plan").toObject();QCOMPARE(plan.value("schema").toString(),QString("paleo.python-seismic-trace-operation.v1"));
    QCOMPARE(saved.stagePlan.value("parents").toArray(),QJsonArray{parent});const auto metadata=plan.value("inputMetadata").toObject();
    const auto provenance=metadata.value("provenance").toObject();QCOMPARE(provenance.value("readCut"),QJsonValue(python::seismicTraceReadCutJson(f.descriptor.cut)));
    const auto io=provenance.value("readMetrics").toObject();QCOMPARE(io.value("sdkRequestedSourceBytes").toString(),QString("652"));
    QCOMPARE(io.value("authenticatedFullHashBytes").toString(),QString::number(f.raw.size()));
    const auto report=f.report(saved.operationId);QVERIFY2(!report.isEmpty(),qPrintable(f.error));QCOMPARE(report.value("provenance"),metadata.value("provenance"));
    QCOMPARE(report.value("metrics").toArray().first().toObject().value("value").toDouble(),10.0);
    QCOMPARE(report.value("metrics").toArray().first().toObject().value("unit").toString(),QString("relative_amplitude"));
    // A second client consumes a fresh window under the same document-bound
    // authenticated index; this is real range I/O, not a repeated full-file SHA.
    const auto second=f.call(QLatin1String(python::SeismicTraceTool),f.traceArguments("trace-next"));QVERIFY2(second.result.ok,qPrintable(second.result.code));
    const auto next=f.record("trace-next").frozenRequest.value("plan").toObject().value("plan").toObject().value("inputMetadata").toObject();
    QCOMPARE(next.value("provenance").toObject().value("readMetrics").toObject().value("authenticatedFullHashBytes").toString(),QString("0"));
    QVERIFY(QFile::remove(f.source.path)); // Frozen publication does not reopen the source file.
    QVERIFY2(f.restartWithAnotherActivePolicy(),qPrintable(f.error));f.configureTrace();
    const auto historical=f.registry->resolve(identity.id,identity.runtimeSha256,identity.policy.json(),python::EnvironmentMode::Generated);
    QVERIFY2(historical.ok(),qPrintable(historical.code));QVERIFY(!historical.sandbox->snapshot().value("ready").toBool());
    const auto publication=f.publication("trace-publish",saved.operationId);const auto published=f.call(QLatin1String(python::SeismicTraceTool),publication);
    QVERIFY2(published.result.ok,qPrintable(published.result.code));QCOMPARE(f.derived().size(),1);QCOMPARE(f.derived().first().parentVersionIds,QStringList{parent});
    QCOMPARE(f.record("trace-publish").computeCount,0);QCOMPARE(f.record("trace-compute").computeCount,1);QCOMPARE(f.report(saved.operationId),report);
    QVERIFY(!historical.sandbox->snapshot().value("ready").toBool());QVERIFY(f.call(QLatin1String(python::SeismicTraceTool),publication).result.ok);QCOMPARE(f.derived().size(),1);
  }
};
QTEST_GUILESS_MAIN(TestPythonSeismicTraceNative)
#include "tst_pythonseismictrace_native.moc"
