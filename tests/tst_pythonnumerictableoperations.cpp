// 层：测试壳
#include "services/pythonnumerictable.h"
#include "services/pythonnumerictablespec.h"
#include "services/pythongenerated.h"
#include "services/pythongeneratedspec.h"
#include "workflow/pythonnumerictableoperation.h"
#include "workflow/pythongeneratedoperation.h"
#include "ai/chat/domaintools.h"
#include "fixtures/pythonenvironmentfixture.h"
#include <QtTest>
using namespace paleo;
class TestPythonNumericTableOperations:public QObject {
  Q_OBJECT
private slots:
  void separateCapabilityNeverReplacesLegacyCurveV1() {
    const auto old=python::generatedAlgorithmSpec(),current=python::numericTableAlgorithmSpec();QVERIFY(old.id!=current.id);
    QCOMPARE(old.id,QString("python:generated_compute"));QCOMPARE(old.implementationVersion,QString("python-generated-numeric.v1"));
    QCOMPARE(current.implementationVersion,QString("python-numeric-table-report.v1"));QCOMPARE(current.inputs[0].id,QString("TABLE"));
    const auto tool=aiToolSpec(QLatin1String(python::NumericTableTool));QVERIFY(!tool.name.isEmpty());
    QCOMPARE(tool.effectsForIntent("compute-only"),host::Read|host::PrivateStage|host::LocalExecute);
  }
  void trustedPermissionDoesNotAdmitGeneratedTablePreparation() {
    python::ExecutionPolicy policy;policy.allowTrusted=true;
    auto runtime=std::make_shared<python::RuntimeProvider>(python::RuntimeConfig{},policy);bool selected=false;
    const auto adapter=python::numericTableScriptAdapter(runtime,{},[&](const auto &,auto &,auto &){selected=true;return QString();});
    capability::ExecuteRequest request;request.capabilityId=QLatin1String(python::NumericTableCapability);request.contractVersion=1;
    request.documentId="doc";request.callerScope="agent:test";request.projectDir="project";request.clientRequestId="denied";QString code;
    const auto prepared=adapter.capture(request,{},&code);QCOMPARE(code,QString("python_execution_denied"));QVERIFY(!prepared.worker);QVERIFY(!selected);
  }
  void missingRuntimeCannotStartSdkSelectionOrFallbackInterpreter() {
    python::ExecutionPolicy policy;policy.allowGenerated=true;
    auto runtime=std::make_shared<python::RuntimeProvider>(python::RuntimeConfig{},policy);bool selected=false;
    const auto adapter=python::numericTableScriptAdapter(runtime,{},[&](const auto &,auto &,auto &){selected=true;return QString();});
    capability::ExecuteRequest request;request.capabilityId=QLatin1String(python::NumericTableCapability);request.contractVersion=1;
    request.documentId="doc";request.callerScope="agent:test";request.projectDir="project";request.clientRequestId="missing";QString code;
    const auto prepared=adapter.capture(request,{},&code);QVERIFY(!code.isEmpty());QVERIFY(!prepared.worker);QVERIFY(!selected);
  }
  void restorersRejectTheOtherExplicitContract() {
    auto runtime=std::make_shared<python::RuntimeProvider>(python::RuntimeConfig{});
    const auto table=python::numericTableScriptAdapter(runtime,{},{}),curves=python::generatedScriptAdapter(runtime,{},{});
    capability::OperationLog::Record old;old.capabilityId=QLatin1String(python::GeneratedCapability);old.contractVersion=1;
    old.implementationVersion=python::generatedAlgorithmSpec().implementationVersion;
    QCOMPARE(table.validateFrozen(old),QString("invalid_python_table_plan"));
    capability::OperationLog::Record current;current.capabilityId=QLatin1String(python::NumericTableCapability);current.contractVersion=1;
    current.implementationVersion=python::numericTableAlgorithmSpec().implementationVersion;
    QCOMPARE(curves.validateFrozen(current),QString("invalid_python_generated_plan"));
  }
  void managedTableRequiresItsIdentityEnvelopeAndActualBackendProof() {
    python_environment_fixture::Fixture fixture(QFINDTESTDATA("../vendor/agent-python/templates"));QVERIFY2(fixture.error.isEmpty(),qPrintable(fixture.error));
    auto registry=std::make_shared<python::PythonEnvironmentRegistry>(fixture.configurationPath());
    const auto activated=registry->activate(fixture.candidate("runtime","fixture",false,true),0);QVERIFY2(activated.ok(),qPrintable(activated.code));
    const auto selected=registry->active(python::EnvironmentMode::Generated);QVERIFY(selected.ok());QVERIFY(!selected.runtime->policy().allowTrusted);
    QVERIFY(!selected.sandbox->snapshot().value("ready").toBool());bool called=false;
    const auto adapter=python::environmentNumericTableAdapter(registry,[&](const auto &,auto &,auto &){called=true;return QString();});
    capability::ExecuteRequest request;request.capabilityId=QLatin1String(python::NumericTableCapability);request.contractVersion=1;
    request.documentId="doc";request.callerScope="agent:test";request.projectDir="project";request.clientRequestId="unprobed";QString code;
    QVERIFY(!adapter.capture(request,{},&code).worker);QVERIFY(!code.isEmpty());QVERIFY(!called);
    capability::OperationLog::Record record;record.capabilityId=request.capabilityId;record.contractVersion=1;
    record.frozenRequest={{"plan",QJsonObject{{"schema","paleo.python-numeric-table-operation.v1"}}}};
    QCOMPARE(adapter.validateFrozen(record),QString("invalid_python_environment_plan"));
    const QJsonObject inner{{"runtimeManifestSha256",selected.identity.runtimeSha256},{"policy",selected.identity.policy.json()}};
    record.frozenRequest={{"plan",QJsonObject{{"schema","paleo.python-environment-operation.v1"},
      {"environmentIdentityId",selected.identity.id},{"plan",inner}}}};
    registry->denyInMemory(selected.identity.id);QVERIFY(registry->revoke(selected.identity.id,1).ok());
    QCOMPARE(adapter.validateFrozen(record),QString("python_environment_identity_revoked"));
  }
};
QTEST_GUILESS_MAIN(TestPythonNumericTableOperations)
#include "tst_pythonnumerictableoperations.moc"
