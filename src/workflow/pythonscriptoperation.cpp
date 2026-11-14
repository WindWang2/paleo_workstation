// 层：功能
#include "pythonscriptoperation.h"
#include "services/pythoncapabilityspec.h"
#include "services/pythoninputbundle.h"
#include "services/pythonprocess.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <cmath>

namespace paleo::python {
capability::OperationWorker trustedScriptWorker(const QJsonObject &,std::shared_ptr<RuntimeProvider>);
namespace {
QString checkPlan(const QJsonObject &plan,const std::shared_ptr<RuntimeProvider> &provider) {
  if(plan.value("schema")!="paleo.python-operation.v1" || plan.value("capability")!=QLatin1String(SummaryCapability) ||
      plan.value("scriptSha256").toString()!=summaryScriptSha256() || !plan.value("storedInput").isObject() ||
      plan.value("documentId").toString().isEmpty() || plan.value("callerScope").toString().isEmpty())return "invalid_python_plan";
  if(!provider || !provider->policy().allowTrusted)return "python_execution_denied";
  if(plan.value("policy").toObject()!=provider->policy().json())return "python_policy_changed";
  const auto status=provider->snapshot();
  if(!status.ready())return status.code;
  if(plan.value("runtimeManifestSha256").toString()!=status.runtime->manifestSha256)return "python_runtime_changed";
  return {};
}
QString recordPlan(const capability::OperationLog::Record &record,const std::shared_ptr<RuntimeProvider> &provider) {
  const auto plan=record.frozenRequest.value("plan").toObject();
  if(record.capabilityId!=QLatin1String(SummaryCapability) || record.contractVersion!=1 ||
      plan.value("documentId").toString()!=record.documentId || plan.value("callerScope").toString()!=record.callerScope)
    return "invalid_python_plan";
  return checkPlan(plan,provider);
}
}
capability::FrozenOperation freezeTrustedScript(const capability::ExecuteRequest &request,const QJsonObject &intent,
    std::shared_ptr<RuntimeProvider> provider,const QString &templateRoot,QString *error) {
  const auto fail=[&](const QString &code){if(error)*error=code;return capability::FrozenOperation{};};
  if(request.capabilityId!=QLatin1String(SummaryCapability) || request.contractVersion!=1)return fail("execution_adapter_unavailable");
  if(request.documentId.isEmpty() || request.callerScope.isEmpty() || request.clientRequestId.isEmpty())return fail("python_scope_required");
  if(request.publishIntent=="publish")return fail("source_operation_required");
  if(!request.publishIntent.isEmpty() && request.publishIntent!="prepare" && request.publishIntent!="compute-only")return fail("unsupported_intent");
  if(!provider || !provider->policy().allowTrusted)return fail("python_execution_denied");
  const auto runtime=provider->snapshot();if(!runtime.ready())return fail(runtime.code);
  if(!request.parameters.isEmpty() || request.inputs.keys()!=QStringList{"values"})return fail("invalid_python_parameters");
  const auto values=QJsonValue::fromVariant(request.inputs.value("values"));
  if(!values.isArray() || values.toArray().isEmpty() || values.toArray().size()>8192)return fail("invalid_python_input");
  for(const auto &number:values.toArray())if(!number.isDouble() || !std::isfinite(number.toDouble()))return fail("invalid_python_input");
  QFile code(templateRoot+"/summary.py");
  if(!code.open(QIODevice::ReadOnly) || code.size()<1 || code.size()>provider->policy().codeBytes)return fail("python_template_unavailable");
  const auto source=code.read(provider->policy().codeBytes+1);
  if(QString::fromLatin1(QCryptographicHash::hash(source,QCryptographicHash::Sha256).toHex())!=summaryScriptSha256())return fail("python_template_changed");
  const auto input=QJsonDocument(QJsonObject{{"values",values}}).toJson(QJsonDocument::Compact);
  const QJsonObject metadata{{"documentId",request.documentId},{"callerScope",request.callerScope},
    {"capability",request.capabilityId},{"inputContract","paleo.python-summary.input.v1"},{"inputProtocol","paleo.python-json.v1"}};
  const auto frozen=freezeInputBundle(request.projectDir,{{"code.py",source},{"request.json",input}},metadata,provider->policy());
  if(!frozen.ok())return fail(frozen.code);
  capability::FrozenOperation plan;plan.capabilityId=request.capabilityId;plan.contractVersion=1;
  plan.callerScope=request.callerScope;plan.clientRequestId=request.clientRequestId;plan.intent=intent;
  plan.requiredEffects=host::LocalExecute;plan.phase=request.publishIntent.isEmpty()?"compute-only":request.publishIntent;
  plan.outputSpec=summaryAlgorithmSpec();plan.outputPorts={"REPORT"};plan.implementationVersion=plan.outputSpec.implementationVersion;
  plan.frozenPlan={{"schema","paleo.python-operation.v1"},{"capability",request.capabilityId},
    {"documentId",request.documentId},{"callerScope",request.callerScope},{"scriptSha256",summaryScriptSha256()},
    {"runtimeManifestSha256",runtime.runtime->manifestSha256},{"policy",provider->policy().json()},{"storedInput",frozen.reference}};
  plan.worker=trustedScriptWorker(plan.frozenPlan,provider);
  plan.inputValidator=[snapshot=plan.frozenPlan,provider](const capability::OperationWorkContext &context) {
    const auto code=checkPlan(snapshot,provider);if(!code.isEmpty())return code;
    return verifyInputBundle(context.projectDir,snapshot.value("storedInput").toObject(),provider->policy(),context.cancelled).ok()?
      QString():QStringLiteral("python_input_changed");
  };
  if(error)error->clear();return plan;
}
capability::CapabilityExecutionAdapter trustedScriptAdapter(std::shared_ptr<RuntimeProvider> provider,const QString &templateRoot) {
  capability::CapabilityExecutionAdapter adapter;
  adapter.freeze=[provider,templateRoot](const auto &request,const auto &intent,QString *error) {
    return freezeTrustedScript(request,intent,provider,templateRoot,error);
  };
  adapter.validateFrozen=[provider](const auto &record){return recordPlan(record,provider);};
  adapter.restoreWorker=[provider](const auto &record,QString *error) {
    const auto code=recordPlan(record,provider);if(error)*error=code;
    return code.isEmpty()?trustedScriptWorker(record.frozenRequest.value("plan").toObject(),provider):capability::OperationWorker{};
  };
  adapter.restoreInputValidator=[provider](const auto &record,QString *error)->capability::OperationInputValidator {
    const auto code=recordPlan(record,provider);if(error)*error=code;if(!code.isEmpty())return {};
    const auto plan=record.frozenRequest.value("plan").toObject();
    return [plan,provider](const capability::OperationWorkContext &context) {
      const auto input=verifyInputBundle(context.projectDir,plan.value("storedInput").toObject(),provider->policy(),context.cancelled);
      return input.ok()?QString():input.code;
    };
  };
  return adapter;
}
} // namespace paleo::python
