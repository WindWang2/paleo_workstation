// 层：功能
#include "pythontableoutputoperation.h"
#include "pythonnumerictableoperation_internal.h"
#include "pythongeneratedworker.h"
#include "pythonenvironmentoperation_internal.h"
#include "services/pythontableoutputspec.h"
#include "services/pythonnumericoutput.h"
#include "services/pythonnumerictablereport.h"
#include "services/pythongeneratedreportcontent.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <utility>

namespace paleo::python {
namespace {
constexpr auto Schema="paleo.python-table-outputs-operation.v1";
QJsonObject outputContract() {
  return {{"protocol","paleo.python-output-frame.v2"},{"encoder","host-csv.numeric.v1"},
    {"frameBytes",NumericOutputFrameBytes},{"csvBytes",NumericCsvBytes}};
}
QJsonObject inputPlan(QJsonObject plan) {
  plan.remove("outputTable");plan.remove("outputContract");
  plan["schema"]="paleo.python-numeric-table-operation.v1";plan["capability"]=QLatin1String(NumericTableCapability);return plan;
}
QString checkPlan(const QJsonObject &plan,const std::shared_ptr<RuntimeProvider> &runtime,
    const std::shared_ptr<SandboxProvider> &sandbox,bool backend) {
  if(plan.value("schema")!=QLatin1String(Schema)||plan.value("capability")!=QLatin1String(TableOutputsCapability)||
      plan.value("outputContract")!=QJsonValue(outputContract())||!plan.value("outputTable").isObject())return "invalid_python_table_outputs_plan";
  auto code=validateNumericOutputDeclaration(plan.value("outputTable").toObject());if(!code.isEmpty())return code;
  return numeric_table_operation::checkPlan(inputPlan(plan),runtime,sandbox,backend);
}
GeneratedWorkerOutputs validateOutputs(const QByteArray &bytes,const QJsonObject &plan,const ExecutionPolicy &policy,
    const std::function<bool()> &cancelled) {
  GeneratedWorkerOutputs out;const auto metadata=plan.value("inputMetadata").toObject();
  if(!validateNumericTableInputMetadata(metadata)){out.code="python_generated_identity_invalid";return out;}
  const auto declaration=plan.value("outputTable").toObject();
  const auto validateReport=[&](const QByteArray &raw) {
    auto accepted=bindGeneratedReportContent(validateGeneratedReportContent(raw,policy),metadata,
      plan.value("codeSha256").toString(),plan.value("runtimeManifestSha256").toString(),policy);
    if(!accepted.ok())return accepted;
    auto report=QJsonDocument::fromJson(accepted.bytes).object();
    report["outputProvenance"]=QJsonObject{{"contract",outputContract()},{"tableDeclaration",declaration}};
    accepted.bytes=QJsonDocument(report).toJson(QJsonDocument::Compact);
    if(accepted.bytes.size()>256*1024){accepted.code="python_output_budget_exceeded";accepted.bytes.clear();}return accepted;
  };
  const auto accepted=validateNumericOutput(bytes,declaration,policy,validateReport,cancelled);out.code=accepted.code;
  if(!accepted.ok())return out;
  auto table=accepted.tableMetadata;table["provenance"]=metadata.value("provenance");
  table["codeSha256"]=plan.value("codeSha256");table["runtimeManifestSha256"]=plan.value("runtimeManifestSha256");
  out.artifacts={{"REPORT","REPORT.json",accepted.report,{{"encoding","utf8"}}},
    {"TABLE","TABLE.csv",accepted.csv,table.toVariantMap()}};
  out.summary=accepted.summary;out.summary["outputProtocol"]="paleo.python-output-frame.v2";
  out.summary["outputDeclarationSha256"]=QString::fromLatin1(QCryptographicHash::hash(
    QJsonDocument(declaration).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());return out;
}
capability::OperationWorker worker(const QJsonObject &plan,std::shared_ptr<RuntimeProvider> runtime,std::shared_ptr<SandboxProvider> sandbox) {
  GeneratedWorkerChecks checks;checks.plan=checkPlan;
  checks.bundle=[](const InputBundle &bundle,const QJsonObject &value){return numeric_table_operation::checkBundle(bundle,inputPlan(value));};
  checks.invalidPlanCode="invalid_python_table_outputs_plan";checks.outputs=validateOutputs;checks.stdoutLimitBytes=NumericOutputFrameBytes;
  return generatedReportWorker(plan,std::move(runtime),std::move(sandbox),std::move(checks));
}
capability::OperationInputValidator inputValidator(const QJsonObject &plan,std::shared_ptr<RuntimeProvider> runtime,std::shared_ptr<SandboxProvider> sandbox) {
  const auto inner=numeric_table_operation::inputValidator(inputPlan(plan),runtime,sandbox);
  return [plan,runtime,sandbox,inner](const capability::OperationWorkContext &context) {
    auto code=checkPlan(plan,runtime,sandbox,false);if(!code.isEmpty())return code;
    if(context.frozenPlan!=plan)return QString("invalid_python_table_outputs_plan");
    auto view=context;view.frozenPlan=inputPlan(plan);return inner(view);
  };
}
QString checkRecord(const capability::OperationLog::Record &record,const std::shared_ptr<RuntimeProvider> &runtime,
    const std::shared_ptr<SandboxProvider> &sandbox,bool backend) {
  if(record.capabilityId!=QLatin1String(TableOutputsCapability)||record.contractVersion!=1||
      record.implementationVersion!=tableOutputsAlgorithmSpec().implementationVersion)return "invalid_python_table_outputs_plan";
  const auto plan=record.frozenRequest.value("plan").toObject();auto code=checkPlan(plan,runtime,sandbox,false);if(!code.isEmpty())return code;
  auto original=record;original.capabilityId=QLatin1String(NumericTableCapability);
  original.implementationVersion=numericTableAlgorithmSpec().implementationVersion;
  original.frozenRequest["plan"]=inputPlan(plan);return numeric_table_operation::checkRecord(original,runtime,sandbox,backend);
}
capability::CapabilityPreparation capture(const capability::ExecuteRequest &request,const QJsonObject &intent,
    std::shared_ptr<RuntimeProvider> runtime,std::shared_ptr<SandboxProvider> sandbox,const NumericTablePlanProvider &selector,QString *error) {
  const auto fail=[&](const QString &code){if(error)*error=code;return capability::CapabilityPreparation{};};
  if(request.capabilityId!=QLatin1String(TableOutputsCapability)||request.contractVersion!=1)return fail("execution_adapter_unavailable");
  const auto declaration=QJsonObject::fromVariantMap(request.parameters).value("output_table").toObject();
  auto code=validateNumericOutputDeclaration(declaration);if(!code.isEmpty())return fail(code);
  auto original=request;original.capabilityId=QLatin1String(NumericTableCapability);original.parameters.remove("output_table");
  auto adapter=numericTableScriptAdapter(runtime,sandbox,selector);auto preparation=adapter.capture(original,intent,error);
  if(!preparation.worker)return preparation;const auto inner=preparation.worker;
  preparation.worker=[inner,declaration,runtime,sandbox](const auto &cancelled,QString *error) {
    auto operation=inner(cancelled,error);if(operation.capabilityId.isEmpty())return operation;
    operation.capabilityId=QLatin1String(TableOutputsCapability);operation.outputSpec=tableOutputsAlgorithmSpec();
    operation.outputPorts={"REPORT","TABLE"};operation.implementationVersion=operation.outputSpec.implementationVersion;
    operation.frozenPlan["schema"]=QLatin1String(Schema);operation.frozenPlan["capability"]=operation.capabilityId;
    operation.frozenPlan["outputContract"]=outputContract();operation.frozenPlan["outputTable"]=declaration;
    operation.worker=worker(operation.frozenPlan,runtime,sandbox);operation.inputValidator=inputValidator(operation.frozenPlan,runtime,sandbox);return operation;
  };
  return preparation;
}
}
capability::CapabilityExecutionAdapter tableOutputsScriptAdapter(std::shared_ptr<RuntimeProvider> runtime,
    std::shared_ptr<SandboxProvider> sandbox,NumericTablePlanProvider selector) {
  capability::CapabilityExecutionAdapter out;
  out.capture=[runtime,sandbox,selector](const auto &request,const auto &intent,QString *error){return capture(request,intent,runtime,sandbox,selector,error);};
  out.validateFrozen=[runtime,sandbox](const auto &record){return checkRecord(record,runtime,sandbox,true);};
  out.restoreWorker=[runtime,sandbox](const auto &record,QString *error) {
    const auto code=checkRecord(record,runtime,sandbox,true);if(error)*error=code;
    return code.isEmpty()?worker(record.frozenRequest.value("plan").toObject(),runtime,sandbox):capability::OperationWorker{};
  };
  out.restoreInputValidator=[runtime,sandbox](const auto &record,QString *error) {
    const auto code=checkRecord(record,runtime,sandbox,true);if(error)*error=code;
    return code.isEmpty()?inputValidator(record.frozenRequest.value("plan").toObject(),runtime,sandbox):capability::OperationInputValidator{};
  };
  return out;
}
capability::CapabilityExecutionAdapter environmentTableOutputsAdapter(std::shared_ptr<PythonEnvironmentRegistry> registry,NumericTablePlanProvider selector) {
  EnvironmentOperationContract contract;
  contract.captureAdapter=[selector](const EnvironmentSelection &selected){return tableOutputsScriptAdapter(selected.runtime,selected.sandbox,selector);};
  contract.checkRecord=[](const auto &record,const EnvironmentSelection &selected){return checkRecord(record,selected.runtime,selected.sandbox,false);};
  contract.inputValidator=[](const auto &plan,const EnvironmentSelection &selected){return inputValidator(plan,selected.runtime,selected.sandbox);};
  contract.worker=[](const auto &plan,const EnvironmentSelection &selected){return worker(plan,selected.runtime,selected.sandbox);};
  return environmentOperationAdapter(EnvironmentMode::Generated,std::move(registry),std::move(contract));
}
}
