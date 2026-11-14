// 层：功能
#include "pythongeneratedoperation_internal.h"
#include "services/pythongenerated.h"
#include "services/pythongeneratedspec.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

namespace paleo::python {
namespace {
QString hash(QByteArrayView bytes) {return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
QString backendStatus(const std::shared_ptr<SandboxProvider> &sandbox,const QString &expected={}) {
  if(!sandbox)return "python_sandbox_unavailable";const auto status=sandbox->snapshot();
  if(!status.value("ready").toBool())return status.value("code").toString("python_sandbox_unavailable");
  if(status.value("backend").toString().isEmpty() || (!expected.isEmpty() && status.value("backend")!=expected))return "python_sandbox_changed";
  return {};
}
capability::FrozenOperation materialize(const capability::ExecuteRequest &request,const QJsonObject &intent,
    const AnalysisReadPlan &readPlan,const QByteArray &source,const QJsonObject &identity,
    const std::shared_ptr<RuntimeProvider> &provider,const std::shared_ptr<SandboxProvider> &sandbox,
    const std::function<bool()> &cancelled,QString *error) {
  const auto fail=[&](const QString &code){if(error)*error=code;return capability::FrozenOperation{};};
  auto code=backendStatus(sandbox,identity.value("backend").toString());if(!code.isEmpty())return fail(code);
  const auto permit=claimExecutionPermit(cancelled);if(!permit)return fail("cancelled");
  const auto runtime=provider->inspectIntegrity(cancelled);if(!runtime.ready())return fail(runtime.code);
  if(runtime.runtime->manifestSha256!=identity.value("runtimeManifestSha256").toString())return fail("python_runtime_changed");
  if(provider->policy().json()!=identity.value("policy").toObject())return fail("python_policy_changed");
  const auto input=materializeAnalysis(readPlan,source,provider->policy(),permit,cancelled);if(!input.ok())return fail(input.code);
  capability::FrozenOperation out;out.capabilityId=QLatin1String(GeneratedCapability);out.contractVersion=1;
  out.callerScope=request.callerScope;out.clientRequestId=request.clientRequestId;out.intent=intent;
  out.requiredEffects=host::LocalExecute;out.phase=request.publishIntent.isEmpty()?QStringLiteral("compute-only"):request.publishIntent;
  out.outputSpec=generatedAlgorithmSpec();out.outputPorts={"REPORT"};out.implementationVersion=out.outputSpec.implementationVersion;
  out.parentVersionIds=analysisSourceVersionIds(input.metadata);if(out.parentVersionIds.isEmpty())return fail("python_analysis_source_mismatch");
  out.frozenPlan=identity;out.frozenPlan["inputMetadata"]=input.metadata;out.frozenPlan["storedInput"]=input.bundle.reference;
  out.worker=generated_operation::worker(out.frozenPlan,provider,sandbox);
  out.inputValidator=generated_operation::inputValidator(out.frozenPlan,provider,sandbox);
  if(error)error->clear();return out;
}
capability::CapabilityPreparation capture(const std::shared_ptr<RuntimeProvider> &provider,
    const std::shared_ptr<SandboxProvider> &sandbox,const AnalysisPlanProvider &selector,
    const capability::ExecuteRequest &request,const QJsonObject &intent,QString *error) {
  const auto fail=[&](const QString &code){if(error)*error=code;return capability::CapabilityPreparation{};};
  if(request.capabilityId!=QLatin1String(GeneratedCapability) || request.contractVersion!=1)return fail("execution_adapter_unavailable");
  if(request.publishIntent=="publish")return fail("source_operation_required");
  if(!request.publishIntent.isEmpty() && request.publishIntent!="prepare" && request.publishIntent!="compute-only")return fail("unsupported_intent");
  if(request.documentId.isEmpty() || request.callerScope.isEmpty() || request.clientRequestId.isEmpty() || request.projectDir.isEmpty())return fail("python_scope_required");
  if(!provider || !provider->policy().allowGenerated)return fail("python_execution_denied");
  const auto runtime=provider->snapshot();if(!runtime.ready())return fail(runtime.code);
  auto code=backendStatus(sandbox);if(!code.isEmpty())return fail(code);
  if(!selector)return fail("python_analysis_source_provider_unavailable");
  if(request.parameters.value("code").metaType().id()!=QMetaType::QString)return fail("python_code_required");
  QByteArray source;code=generatedSourceFromArguments({{"parameters",QJsonObject::fromVariantMap(request.parameters)}},source);
  if(code.isEmpty())code=validateGeneratedCode(source,provider->policy());if(!code.isEmpty())return fail(code);
  auto selected=request;selected.parameters.clear();AnalysisReadPlan readPlan;std::function<QString()> validate;
  code=selector(selected,readPlan,validate);if(!code.isEmpty())return fail(code);if(!validate)return fail("python_analysis_validation_unavailable");
  if(readPlan.kind!=AnalysisKind::Generated || readPlan.projectDir!=request.projectDir || readPlan.documentId!=request.documentId ||
      readPlan.callerScope!=request.callerScope)return fail("python_input_scope_mismatch");
  const QJsonObject identity{{"schema","paleo.python-generated-operation.v1"},{"executionMode","generated"},
    {"capability",request.capabilityId},{"documentId",request.documentId},{"callerScope",request.callerScope},
    {"codeSha256",hash(source)},{"runtimeManifestSha256",runtime.runtime->manifestSha256},
    {"backend",sandbox->snapshot().value("backend")},{"policy",provider->policy().json()}};
  capability::ExecuteRequest frozen;frozen.capabilityId=request.capabilityId;frozen.contractVersion=1;
  frozen.documentId=request.documentId;frozen.callerScope=request.callerScope;frozen.clientRequestId=request.clientRequestId;frozen.publishIntent=request.publishIntent;
  capability::CapabilityPreparation result;result.validate=std::move(validate);
  result.worker=[frozen,intent,readPlan,source,identity,provider,sandbox](const auto &cancelled,QString *error) {
    return materialize(frozen,intent,readPlan,source,identity,provider,sandbox,cancelled,error);
  };
  if(error)error->clear();return result;
}
QString checkRecord(const capability::OperationLog::Record &record,const std::shared_ptr<RuntimeProvider> &runtime,
                    const std::shared_ptr<SandboxProvider> &sandbox) {
  const auto plan=record.frozenRequest.value("plan").toObject();
  if(record.capabilityId!=QLatin1String(GeneratedCapability) || record.contractVersion!=1 ||
      record.implementationVersion!=generatedAlgorithmSpec().implementationVersion || plan.value("documentId").toString()!=record.documentId ||
      plan.value("callerScope").toString()!=record.callerScope || QJsonArray::fromStringList(analysisSourceVersionIds(plan.value("inputMetadata").toObject()))!=record.stagePlan.value("parents").toArray())
    return "invalid_python_generated_plan";
  const bool publication=record.frozenRequest.value("phase")=="publish" && !record.frozenRequest.value("sourceOperationId").toString().isEmpty();
  return generated_operation::checkPlan(plan,runtime,sandbox,!publication);
}
}
namespace generated_operation {
QString checkPlan(const QJsonObject &plan,const std::shared_ptr<RuntimeProvider> &provider,
                  const std::shared_ptr<SandboxProvider> &sandbox,bool requireBackend) {
  QStringList fields{"backend","callerScope","capability","codeSha256","documentId","executionMode","inputMetadata","policy","runtimeManifestSha256","schema","storedInput"};fields.sort();
  const auto metadata=plan.value("inputMetadata").toObject();
  if(plan.keys()!=fields || plan.value("schema")!="paleo.python-generated-operation.v1" || plan.value("executionMode")!="generated" ||
      plan.value("capability")!=QLatin1String(GeneratedCapability) || !validateAnalysisInputMetadata(metadata,AnalysisKind::Generated) ||
      metadata.value("provenance").toObject().value("documentId")!=plan.value("documentId") ||
      metadata.value("provenance").toObject().value("callerScope")!=plan.value("callerScope") ||
      plan.value("codeSha256").toString().size()!=64 || plan.value("backend").toString().isEmpty() ||
      !plan.value("storedInput").isObject() || plan.value("storedInput").toObject().isEmpty() ||
      QJsonDocument(metadata).toJson(QJsonDocument::Compact).size()>64*1024)return "invalid_python_generated_plan";
  if(!provider || !provider->policy().allowGenerated)return "python_execution_denied";
  if(provider->policy().json()!=plan.value("policy").toObject())return "python_policy_changed";
  const auto runtime=provider->snapshot();if(!runtime.ready())return runtime.code;
  if(runtime.runtime->manifestSha256!=plan.value("runtimeManifestSha256").toString())return "python_runtime_changed";
  return requireBackend?backendStatus(sandbox,plan.value("backend").toString()):QString();
}
QString checkBundle(const InputBundle &bundle,const QJsonObject &plan) {
  const auto metadata=plan.value("inputMetadata").toObject();const QJsonObject expected{
    {"documentId",plan.value("documentId")},{"callerScope",plan.value("callerScope")},{"inputProtocol","paleo.python-analysis-frame.v1"},
    {"inputContract","paleo.python-analysis-input.v1"},{"analysis","generated_numeric"},{"provenance",metadata.value("provenance")}};
  if(bundle.manifest.value("metadata")!=QJsonValue(expected))return "python_input_scope_mismatch";
  const auto control=QJsonDocument(metadata).toJson(QJsonDocument::Compact);QSet<QString> names;
  for(const auto &entry:bundle.manifest.value("files").toArray()) {
    const auto file=entry.toObject();const auto name=file.value("path").toString();names.insert(name);
    if(name=="code.py" && (file.value("sha256")!=plan.value("codeSha256") || file.value("size").toDouble(-1)<1 ||
        file.value("size").toDouble()>GeneratedCodeBytes))return "python_code_changed";
    if(name=="request.json" && (file.value("sha256").toString()!=hash(control) || file.value("size").toDouble(-1)!=control.size()))
      return "python_analysis_input_metadata_changed";
  }
  return names==QSet<QString>{"code.py","request.json","data.bin"}?QString():QStringLiteral("python_input_manifest_invalid");
}
capability::OperationInputValidator inputValidator(const QJsonObject &plan,std::shared_ptr<RuntimeProvider> provider,std::shared_ptr<SandboxProvider> sandbox) {
  return [plan,provider,sandbox](const capability::OperationWorkContext &context) {
    auto code=checkPlan(plan,provider,sandbox,false);if(!code.isEmpty())return code;
    if(context.frozenPlan!=plan)return QString("invalid_python_generated_plan");
    const auto bundle=verifyInputBundle(context.projectDir,plan.value("storedInput").toObject(),provider->policy(),context.cancelled);
    return bundle.ok()?checkBundle(bundle,plan):bundle.code;
  };
}
}
capability::CapabilityExecutionAdapter generatedScriptAdapter(std::shared_ptr<RuntimeProvider> runtime,
    std::shared_ptr<SandboxProvider> sandbox,AnalysisPlanProvider selector) {
  capability::CapabilityExecutionAdapter out;
  out.capture=[runtime,sandbox,selector](const auto &request,const auto &intent,QString *error){return capture(runtime,sandbox,selector,request,intent,error);};
  out.validateFrozen=[runtime,sandbox](const auto &record){return checkRecord(record,runtime,sandbox);};
  out.restoreWorker=[runtime,sandbox](const auto &record,QString *error) {
    const auto code=checkRecord(record,runtime,sandbox);if(error)*error=code;
    return code.isEmpty()?generated_operation::worker(record.frozenRequest.value("plan").toObject(),runtime,sandbox):capability::OperationWorker{};
  };
  out.restoreInputValidator=[runtime,sandbox](const auto &record,QString *error) {
    const auto code=checkRecord(record,runtime,sandbox);if(error)*error=code;
    return code.isEmpty()?generated_operation::inputValidator(record.frozenRequest.value("plan").toObject(),runtime,sandbox):capability::OperationInputValidator{};
  };
  return out;
}
}
