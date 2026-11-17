// 层：测试壳
#include "fixtures/pythonnumerictablefixture.h"
#include "services/pythontableoutputspec.h"
#include "services/pythonsandboxlimits.h"
#include "workflow/pythontableoutputoperation.h"
#include "workflow/pythongeneratedoperation.h"
#include "services/pythongeneratedspec.h"
#include "ai/chat/domaintools.h"
#include <QtTest>
using namespace paleo;
namespace {
QJsonObject declaration() {
  return {{"contract","paleo.python-table-declaration.v1"},{"maxRows",4},
    {"columns",QJsonArray{QJsonObject{{"id","source_row"},{"label","Physical row"},{"unit","1"},{"dtype","<i8"},{"role","source_row"}},
      QJsonObject{{"id","value"},{"label","Value"},{"unit","mV"},{"dtype","<f8"},{"role","value"}}}}};
}
}
class TestPythonTableOutputOperations:public QObject {
  Q_OBJECT
private slots:
  void explicitOutputVersionKeepsReportOnlyContracts() {
    const auto spec=python::tableOutputsAlgorithmSpec();QCOMPARE(spec.id,QString("python:numeric_table_outputs"));
    QCOMPARE(python::generatedAlgorithmSpec().implementationVersion,QString("python-generated-numeric.v1"));
    QCOMPARE(python::numericTableAlgorithmSpec().implementationVersion,QString("python-numeric-table-report.v1"));
    QStringList required;for(const auto &port:spec.outputs)if(port.required)required.append(port.id);
    QCOMPARE(required,QStringList({"REPORT","TABLE"}));
    const auto tool=aiToolSpec(QLatin1String(python::TableOutputsTool));QVERIFY(!tool.name.isEmpty());
    QCOMPARE(tool.effectsForIntent("compute-only"),host::Read|host::PrivateStage|host::LocalExecute);
    QVERIFY(tool.effectsForIntent("publish")&host::CatalogPublish);
  }
  void outputDeclarationIsRequiredOnlyForFreshComputation() {
    numeric_table_fixture::Model f;QVERIFY2(f.open(),qPrintable(f.error));
    QJsonObject args{{"client_request_id","outputs"},{"binding_mode","pinned-value"},
      {"inputs",python::numericTableSelectionJson(f.plan.selection)},
      {"parameters",QJsonObject{{"code",QString::fromUtf8(numeric_table_fixture::sourceCode())},{"output_table",declaration()}}}};
    QVERIFY(python::validateTableOutputsToolArguments(args).isEmpty());
    QVERIFY(!python::validateNumericTableToolArguments(args).isEmpty()); // Old tool never silently upgrades.
    auto missing=args;auto params=args.value("parameters").toObject();params.remove("output_table");missing["parameters"]=params;
    QCOMPARE(python::validateTableOutputsToolArguments(missing),QString("python_output_declaration_invalid"));
    auto publish=args;publish["publish_intent"]="publish";publish["parameters"]=QJsonObject{};
    publish["inputs"]=QJsonObject{{"source_operation_id","original"}};QVERIFY(python::validateTableOutputsToolArguments(publish).isEmpty());
    publish["parameters"]=args.value("parameters");QVERIFY(!python::validateTableOutputsToolArguments(publish).isEmpty());
  }
  void declarationFailurePrecedesRuntimeOrSdkSelection() {
    bool selected=false;auto runtime=std::make_shared<python::RuntimeProvider>(python::RuntimeConfig{});
    auto adapter=python::tableOutputsScriptAdapter(runtime,{},[&](const auto &,auto &,auto &){selected=true;return QString();});
    capability::ExecuteRequest request;request.capabilityId=QLatin1String(python::TableOutputsCapability);request.contractVersion=1;QString code;
    QVERIFY(!adapter.capture(request,{},&code).worker);QCOMPARE(code,QString("python_output_declaration_invalid"));QVERIFY(!selected);
    request.parameters={{"output_table",declaration()}};
    QVERIFY(!adapter.capture(request,{},&code).worker);QVERIFY(!selected);QVERIFY(!code.isEmpty());
  }
  void reportOnlyAndTypedRestorersRejectEachOthersRecords() {
    auto runtime=std::make_shared<python::RuntimeProvider>(python::RuntimeConfig{});
    const auto typed=python::tableOutputsScriptAdapter(runtime,{},{}),old=python::numericTableScriptAdapter(runtime,{},{});
    capability::OperationLog::Record record;record.capabilityId=QLatin1String(python::NumericTableCapability);record.contractVersion=1;
    record.implementationVersion=python::numericTableAlgorithmSpec().implementationVersion;
    QCOMPARE(typed.validateFrozen(record),QString("invalid_python_table_outputs_plan"));
    record.capabilityId=QLatin1String(python::TableOutputsCapability);record.implementationVersion=python::tableOutputsAlgorithmSpec().implementationVersion;
    QCOMPARE(old.validateFrozen(record),QString("invalid_python_table_plan"));
  }
  void hostPipeCeilingOnlyNarrowsApprovedPolicy() {
    python::SandboxRequest original,effective;const auto policy=original.policy.json();
    QVERIFY(python::constrainSandboxOutput(original,effective).isEmpty());QCOMPARE(effective.policy.json(),policy);
    original.stdoutLimitBytes=12*1024*1024;QVERIFY(python::constrainSandboxOutput(original,effective).isEmpty());
    QCOMPARE(effective.policy.outputBytes,original.stdoutLimitBytes);QCOMPARE(original.policy.json(),policy);
    auto expected=original.policy;expected.outputBytes=original.stdoutLimitBytes;QCOMPARE(effective.policy.json(),expected.json());
    original.policy.outputBytes=128;QCOMPARE(python::constrainSandboxOutput(original,effective),QString("python_output_limit_invalid"));
    original.stdoutLimitBytes=128;QVERIFY(python::constrainSandboxOutput(original,effective).isEmpty());QCOMPARE(effective.policy.outputBytes,qint64(128));
    original.stdoutLimitBytes=-1;QCOMPARE(python::constrainSandboxOutput(original,effective),QString("python_output_limit_invalid"));
  }
};
QTEST_GUILESS_MAIN(TestPythonTableOutputOperations)
#include "tst_pythontableoutputoperations.moc"
