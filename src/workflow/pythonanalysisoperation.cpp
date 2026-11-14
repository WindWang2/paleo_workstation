// 层：功能
#include "pythonanalysisoperation_internal.h"
#include "services/pythonanalysiscapabilityspec.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

namespace paleo::python {
namespace {
QStringList parents(const QJsonObject &metadata) {
  QStringList result;
  for(const auto &source:metadata.value("provenance").toObject().value("sources").toArray()) {
    const auto id=source.toObject().value("sourceVersionId").toString();
    if(!id.isEmpty() && !result.contains(id))result.append(id);
  }
  return result;
}
QString digest(QByteArrayView bytes) {
  return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
}
QString checkBundle(const InputBundle &bundle,const QJsonObject &plan) {
  const auto scope=bundle.manifest.value("metadata").toObject();
  const auto expected=plan.value("inputMetadata").toObject();
  if(scope.value("documentId")!=plan.value("documentId") || scope.value("callerScope")!=plan.value("callerScope") ||
      scope.value("analysis")!=plan.value("analysis") || scope.value("inputProtocol")!="paleo.python-analysis-frame.v1" ||
      scope.value("provenance")!=expected.value("provenance"))return "python_input_scope_mismatch";
  const auto control=QJsonDocument(expected).toJson(QJsonDocument::Compact);
  bool matched=false;
  for(const auto &entry:bundle.manifest.value("files").toArray())if(entry.toObject().value("path")=="request.json") {
    const auto file=entry.toObject();matched=file.value("sha256").toString()==digest(control) && file.value("size").toDouble(-1)==control.size();
  }
  return matched?QString():QStringLiteral("python_analysis_input_metadata_changed");
}
capability::FrozenOperation materialize(const capability::ExecuteRequest &request,const QJsonObject &intent,
    const AnalysisReadPlan &readPlan,const QByteArray &source,const QJsonObject &identity,
    const std::shared_ptr<RuntimeProvider> &provider,const std::function<bool()> &cancelled,QString *error) {
  const auto fail=[&](const QString &code){if(error)*error=code;return capability::FrozenOperation{};};
  const auto permit=claimExecutionPermit(cancelled);if(!permit)return fail("cancelled");
  const auto runtime=provider->inspect(cancelled);if(!runtime.ready())return fail(runtime.code);
  if(runtime.runtime->manifestSha256!=identity.value("runtimeManifestSha256").toString())return fail("python_runtime_changed");
  if(provider->policy().json()!=identity.value("policy").toObject())return fail("python_policy_changed");
  const auto input=materializeAnalysis(readPlan,source,provider->policy(),permit,cancelled);if(!input.ok())return fail(input.code);
  capability::FrozenOperation out;out.capabilityId=request.capabilityId;out.contractVersion=1;
  out.callerScope=request.callerScope;out.clientRequestId=request.clientRequestId;out.intent=intent;
  out.requiredEffects=host::LocalExecute;out.phase=request.publishIntent.isEmpty()?QStringLiteral("compute-only"):request.publishIntent;
  out.outputSpec=analysisAlgorithmSpec(readPlan.kind);out.outputPorts={"REPORT"};out.implementationVersion=out.outputSpec.implementationVersion;
  out.parentVersionIds=parents(input.metadata);
  if(!out.outputSpec.complete || out.parentVersionIds.isEmpty())return fail("python_analysis_output_spec_unavailable");
  out.frozenPlan=identity;out.frozenPlan.insert("inputMetadata",input.metadata);out.frozenPlan.insert("storedInput",input.bundle.reference);
  out.worker=analysis_operation::worker(out.frozenPlan,readPlan.kind,provider);
  out.inputValidator=analysis_operation::inputValidator(out.frozenPlan,readPlan.kind,provider);
  if(error)error->clear();return out;
}
capability::CapabilityPreparation capture(AnalysisKind kind,const std::shared_ptr<RuntimeProvider> &provider,
    const QString &templateRoot,const AnalysisPlanProvider &selector,const capability::ExecuteRequest &request,
    const QJsonObject &intent,QString *error) {
  const auto fail=[&](const QString &code){if(error)*error=code;return capability::CapabilityPreparation{};};
  if(analysisKindId(kind).isEmpty() || request.capabilityId!=analysisCapabilityId(kind) || request.contractVersion!=1)
    return fail("execution_adapter_unavailable");
  if(request.documentId.isEmpty() || request.callerScope.isEmpty() || request.clientRequestId.isEmpty() || request.projectDir.isEmpty())
    return fail("python_scope_required");
  if(request.publishIntent=="publish")return fail("source_operation_required");
  if(!request.publishIntent.isEmpty() && request.publishIntent!="prepare" && request.publishIntent!="compute-only")return fail("unsupported_intent");
  if(!provider || !provider->policy().allowTrusted)return fail("python_execution_denied");
  const auto runtime=provider->snapshot();if(!runtime.ready())return fail(runtime.code);
  if(!selector)return fail("python_analysis_source_provider_unavailable");
  AnalysisReadPlan readPlan;std::function<QString()> validate;auto code=selector(request,readPlan,validate);
  if(!code.isEmpty())return fail(code);if(!validate)return fail("python_analysis_validation_unavailable");
  if(readPlan.kind!=kind || readPlan.projectDir!=request.projectDir || readPlan.documentId!=request.documentId ||
      readPlan.callerScope!=request.callerScope)return fail("python_input_scope_mismatch");
  if(readPlan.curves.isEmpty() || readPlan.curves.size()>AnalysisMaxChannels)return fail("python_analysis_channel_budget");
  QFile file(templateRoot+'/'+analysisKindId(kind)+".py");const QFileInfo info(file);
  if(!info.isFile() || info.isSymLink() || !file.open(QIODevice::ReadOnly) || file.size()<1 || file.size()>provider->policy().codeBytes)
    return fail("python_template_unavailable");
  const auto source=file.read(provider->policy().codeBytes+1);
  if(source.size()!=file.size() || digest(source)!=analysisTemplateSha256(kind))return fail("python_template_changed");
  const QJsonObject identity{{"schema","paleo.python-analysis-operation.v1"},{"capability",request.capabilityId},
    {"analysis",analysisKindId(kind)},{"documentId",request.documentId},{"callerScope",request.callerScope},
    {"scriptSha256",analysisTemplateSha256(kind)},{"runtimeManifestSha256",runtime.runtime->manifestSha256},{"policy",provider->policy().json()}};
  capability::ExecuteRequest frozenRequest;frozenRequest.capabilityId=request.capabilityId;frozenRequest.contractVersion=1;
  frozenRequest.documentId=request.documentId;frozenRequest.callerScope=request.callerScope;
  frozenRequest.clientRequestId=request.clientRequestId;frozenRequest.publishIntent=request.publishIntent;
  capability::CapabilityPreparation result;result.validate=std::move(validate);
  result.worker=[frozenRequest,intent,readPlan,source,identity,provider](const auto &cancelled,QString *error) {
    return materialize(frozenRequest,intent,readPlan,source,identity,provider,cancelled,error);
  };
  if(error)error->clear();return result;
}
} // namespace
namespace analysis_operation {
QString checkPlan(const QJsonObject &plan,AnalysisKind kind,const std::shared_ptr<RuntimeProvider> &provider) {
  QStringList expected{"analysis","callerScope","capability","documentId","inputMetadata","policy","runtimeManifestSha256","schema","scriptSha256","storedInput"};
  expected.sort();const auto metadata=plan.value("inputMetadata").toObject();const auto provenance=metadata.value("provenance").toObject();
  if(plan.keys()!=expected || plan.value("schema")!="paleo.python-analysis-operation.v1" ||
      plan.value("capability").toString()!=analysisCapabilityId(kind) || plan.value("analysis").toString()!=analysisKindId(kind) ||
      analysisTemplateSha256(kind).isEmpty() || plan.value("scriptSha256").toString()!=analysisTemplateSha256(kind) ||
      !plan.value("storedInput").isObject() || plan.value("storedInput").toObject().isEmpty() ||
      plan.value("documentId").toString().isEmpty() || plan.value("callerScope").toString().isEmpty() ||
      metadata.value("analysis")!=plan.value("analysis") || metadata.value("inputProtocol")!="paleo.python-analysis-frame.v1" ||
      provenance.value("documentId")!=plan.value("documentId") || provenance.value("callerScope")!=plan.value("callerScope") ||
      QJsonDocument(metadata).toJson(QJsonDocument::Compact).size()>64*1024 || parents(metadata).isEmpty())return "invalid_python_analysis_plan";
  if(!provider || !provider->policy().allowTrusted)return "python_execution_denied";
  if(plan.value("policy").toObject()!=provider->policy().json())return "python_policy_changed";
  const auto status=provider->snapshot();if(!status.ready())return status.code;
  return plan.value("runtimeManifestSha256").toString()==status.runtime->manifestSha256?QString():QStringLiteral("python_runtime_changed");
}
QString checkRecord(const capability::OperationLog::Record &record,AnalysisKind kind,const std::shared_ptr<RuntimeProvider> &provider) {
  const auto plan=record.frozenRequest.value("plan").toObject();
  if(record.capabilityId!=analysisCapabilityId(kind) || record.contractVersion!=1 ||
      record.implementationVersion!=analysisAlgorithmSpec(kind).implementationVersion ||
      plan.value("documentId").toString()!=record.documentId || plan.value("callerScope").toString()!=record.callerScope ||
      QJsonArray::fromStringList(parents(plan.value("inputMetadata").toObject()))!=record.stagePlan.value("parents").toArray())
    return "invalid_python_analysis_plan";
  return checkPlan(plan,kind,provider);
}
capability::OperationInputValidator inputValidator(const QJsonObject &plan,AnalysisKind kind,std::shared_ptr<RuntimeProvider> provider) {
  return [plan,kind,provider](const capability::OperationWorkContext &context) {
    auto code=checkPlan(plan,kind,provider);if(!code.isEmpty())return code;
    if(context.frozenPlan!=plan)return QString("invalid_python_analysis_plan");
    const auto bundle=verifyInputBundle(context.projectDir,plan.value("storedInput").toObject(),provider->policy(),context.cancelled);
    return bundle.ok()?checkBundle(bundle,plan):bundle.code;
  };
}
} // namespace analysis_operation
capability::CapabilityExecutionAdapter analysisScriptAdapter(AnalysisKind kind,std::shared_ptr<RuntimeProvider> provider,
    const QString &templateRoot,AnalysisPlanProvider selector) {
  capability::CapabilityExecutionAdapter adapter;
  adapter.capture=[kind,provider,templateRoot,selector](const auto &request,const auto &intent,QString *error) {
    return capture(kind,provider,templateRoot,selector,request,intent,error);
  };
  adapter.validateFrozen=[kind,provider](const auto &record){return analysis_operation::checkRecord(record,kind,provider);};
  adapter.restoreWorker=[kind,provider](const auto &record,QString *error) {
    const auto code=analysis_operation::checkRecord(record,kind,provider);if(error)*error=code;
    return code.isEmpty()?analysis_operation::worker(record.frozenRequest.value("plan").toObject(),kind,provider):capability::OperationWorker{};
  };
  adapter.restoreInputValidator=[kind,provider](const auto &record,QString *error) {
    const auto code=analysis_operation::checkRecord(record,kind,provider);if(error)*error=code;
    return code.isEmpty()?analysis_operation::inputValidator(record.frozenRequest.value("plan").toObject(),kind,provider):capability::OperationInputValidator{};
  };
  return adapter;
}
} // namespace paleo::python
