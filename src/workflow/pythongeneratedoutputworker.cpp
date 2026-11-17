// 层：功能
#include "pythongeneratedoutputoperation_internal.h"
#include "pythonnumerictableoperation_internal.h"
#include "pythonseismictraceoperation_internal.h"
#include "services/pythongeneratedoutputspec.h"
#include "services/pythongeneratedreportcontent.h"
#include "services/pythonseismictracespec.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <utility>
namespace paleo::python::generated_outputs_operation {
namespace {
constexpr auto Schema="paleo.python-generated-outputs-operation.v1";
bool table(const QJsonObject &plan){return plan.value("inputKind")=="numeric_table";}
GeneratedWorkerOutputs outputs(const QByteArray &bytes,const QJsonObject &plan,const ExecutionPolicy &policy,const std::function<bool()> &cancelled) {
  GeneratedWorkerOutputs out;const auto metadata=plan.value("inputMetadata").toObject(),profile=plan.value("outputProfile").toObject();
  const bool valid=table(plan)?validateNumericTableInputMetadata(metadata):validateSeismicTraceInputMetadata(metadata);
  if(!valid){out.code="python_generated_identity_invalid";return out;}
  const auto report=[&](const QByteArray &raw) {
    auto accepted=bindGeneratedReportContent(validateGeneratedReportContent(raw,policy),metadata,
      plan.value("codeSha256").toString(),plan.value("runtimeManifestSha256").toString(),policy);
    if(!accepted.ok())return accepted;auto body=QJsonDocument::fromJson(accepted.bytes).object();
    body["outputProvenance"]=profile;accepted.bytes=QJsonDocument(body).toJson(QJsonDocument::Compact);
    if(accepted.bytes.size()>256*1024){accepted.code="python_output_budget_exceeded";accepted.bytes.clear();}return accepted;
  };
  const auto accepted=validateImageOutput(bytes,profile.value("table").toObject(),profile.value("image").toObject(),
    profile.value("pngEncoder").toObject(),policy,report,cancelled);out.code=accepted.code;if(!accepted.ok())return out;
  const auto provenance=[&](QJsonObject metadata) {
    metadata["inputProvenance"]=plan.value("inputMetadata").toObject().value("provenance");
    metadata["codeSha256"]=plan.value("codeSha256");metadata["runtimeManifestSha256"]=plan.value("runtimeManifestSha256");return metadata.toVariantMap();
  };
  out.artifacts={{"REPORT","REPORT.json",accepted.report,{{"encoding","utf8"}}},
    {"TABLE","TABLE.csv",accepted.csv,provenance(accepted.tableMetadata)},
    {"IMAGE","IMAGE.png",accepted.png,provenance(accepted.imageMetadata)}};
  out.summary=accepted.summary;out.summary["outputProtocol"]="paleo.python-output-frame.v3";
  out.summary["outputProfileSha256"]=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(profile).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());return out;
}
}
QJsonObject inputPlan(QJsonObject plan) {
  const bool isTable=table(plan);plan.remove("inputKind");plan.remove("outputProfile");
  plan["schema"]=isTable?"paleo.python-numeric-table-operation.v1":"paleo.python-seismic-trace-operation.v1";
  plan["capability"]=isTable?QLatin1String(NumericTableCapability):QLatin1String(SeismicTraceCapability);return plan;
}
QString checkPlan(const QJsonObject &plan,const std::shared_ptr<RuntimeProvider> &runtime,const std::shared_ptr<SandboxProvider> &sandbox,bool backend) {
  if(plan.value("schema")!=QLatin1String(Schema)||plan.value("capability")!=QLatin1String(GeneratedOutputsCapability)||
      (plan.value("inputKind")!="numeric_table"&&plan.value("inputKind")!="seismic_trace")||!plan.value("outputProfile").isObject())return "invalid_python_outputs_plan";
  if(!runtime)return "python_execution_denied";
  auto code=validateGeneratedOutputsProfile(plan.value("outputProfile").toObject(),runtime->policy(),backend);if(!code.isEmpty())return code;
  return table(plan)?numeric_table_operation::checkPlan(inputPlan(plan),runtime,sandbox,backend):
    seismic_trace_operation::checkPlan(inputPlan(plan),runtime,sandbox,backend);
}
QString checkRecord(const capability::OperationLog::Record &record,const std::shared_ptr<RuntimeProvider> &runtime,const std::shared_ptr<SandboxProvider> &sandbox,bool backend) {
  if(record.capabilityId!=QLatin1String(GeneratedOutputsCapability)||record.contractVersion!=1||
      record.implementationVersion!=generatedOutputsAlgorithmSpec().implementationVersion)return "invalid_python_outputs_plan";
  const auto plan=record.frozenRequest.value("plan").toObject();
  const bool publication=record.frozenRequest.value("phase")=="publish"&&!record.frozenRequest.value("sourceOperationId").toString().isEmpty();
  auto code=checkPlan(plan,runtime,sandbox,backend&&!publication);if(!code.isEmpty())return code;
  auto original=record;const bool isTable=table(plan);original.capabilityId=isTable?QLatin1String(NumericTableCapability):QLatin1String(SeismicTraceCapability);
  original.implementationVersion=isTable?numericTableAlgorithmSpec().implementationVersion:seismicTraceAlgorithmSpec().implementationVersion;
  original.frozenRequest["plan"]=inputPlan(plan);
  return isTable?numeric_table_operation::checkRecord(original,runtime,sandbox,backend):seismic_trace_operation::checkRecord(original,runtime,sandbox,backend);
}
capability::OperationInputValidator inputValidator(const QJsonObject &plan,std::shared_ptr<RuntimeProvider> runtime,std::shared_ptr<SandboxProvider> sandbox) {
  const auto inner=table(plan)?numeric_table_operation::inputValidator(inputPlan(plan),runtime,sandbox):
    seismic_trace_operation::inputValidator(inputPlan(plan),runtime,sandbox);
  return [plan,runtime,sandbox,inner](const capability::OperationWorkContext &context) {
    auto code=checkPlan(plan,runtime,sandbox,false);if(!code.isEmpty())return code;
    if(context.frozenPlan!=plan)return QString("invalid_python_outputs_plan");auto view=context;view.frozenPlan=inputPlan(plan);return inner(view);
  };
}
capability::OperationWorker worker(const QJsonObject &plan,std::shared_ptr<RuntimeProvider> runtime,std::shared_ptr<SandboxProvider> sandbox) {
  GeneratedWorkerChecks checks;checks.plan=checkPlan;checks.invalidPlanCode="invalid_python_outputs_plan";checks.outputs=outputs;checks.stdoutLimitBytes=NumericOutputFrameBytes;
  checks.bundle=[](const InputBundle &bundle,const QJsonObject &plan){return table(plan)?numeric_table_operation::checkBundle(bundle,inputPlan(plan)):
    seismic_trace_operation::checkBundle(bundle,inputPlan(plan));};
  return generatedReportWorker(plan,std::move(runtime),std::move(sandbox),std::move(checks));
}
}
