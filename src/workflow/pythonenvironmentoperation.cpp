// 层：功能
#include "pythonenvironmentoperation.h"
#include "pythonenvironmentoperation_internal.h"
#include "pythonanalysisoperation_internal.h"
#include "pythongeneratedoperation_internal.h"
#include "services/pythongenerated.h"

namespace paleo::python {
namespace {
constexpr auto Envelope = "paleo.python-environment-operation.v1";
EnvironmentMode mode(AnalysisKind kind) { return kind == AnalysisKind::Generated ? EnvironmentMode::Generated : EnvironmentMode::Trusted; }
QString interrupted(const EnvironmentSelection &selected, const capability::OperationWorkContext *context = nullptr) {
  if (selected.interrupted && !selected.interrupted->load() && selected.globalInterruption && !selected.globalInterruption->load()) return {};
  if (context && context->requestInterruption) context->requestInterruption();
  return "python_environment_identity_revoked";
}
std::function<bool()> stopped(const EnvironmentSelection &selected, const std::function<bool()> &cancelled,
                             std::function<void()> interrupt = {}) {
  return [selected, cancelled, interrupt] {
    if (!selected.interrupted || selected.interrupted->load() || !selected.globalInterruption || selected.globalInterruption->load()) {
      if (interrupt) interrupt(); return true;
    }
    return cancelled && cancelled();
  };
}
capability::OperationInputValidator validator(capability::OperationInputValidator inner,
    const EnvironmentSelection &selected, const QJsonObject &plan) {
  return [inner, selected, plan](const capability::OperationWorkContext &context) {
    const auto code = interrupted(selected, &context); if (!code.isEmpty()) return code;
    auto view = context; view.frozenPlan = plan; view.cancelled = stopped(selected, context.cancelled, context.requestInterruption);
    const auto result = inner ? inner(view) : QStringLiteral("input_validator_unavailable");
    const auto revoked = interrupted(selected, &context); return revoked.isEmpty() ? result : revoked;
  };
}
capability::OperationWorker worker(capability::OperationWorker inner, const EnvironmentSelection &selected,
    const QJsonObject &plan, EnvironmentMode executionMode, const std::shared_ptr<PythonEnvironmentRegistry> &registry,
    bool restoring) {
  return [inner, selected, plan, executionMode, registry, restoring](const capability::OperationWorkContext &context) {
    capability::OperationWorkResult result;
    auto code = interrupted(selected, &context); if (!code.isEmpty()) { result.code = code; return result; }
    auto view = context; view.frozenPlan = plan; view.cancelled = stopped(selected, context.cancelled, context.requestInterruption);
    if (restoring) {
      const auto proof = registry->probe(selected.identity.id, executionMode, view.cancelled);
      if (!proof.value("ready").toBool()) {
        result.code = proof.value("code").toString("python_environment_execution_unprobed");
        interrupted(selected, &context); return result;
      }
    }
    result = inner ? inner(view) : capability::OperationWorkResult{};
    code = interrupted(selected, &context);
    if (!code.isEmpty()) { result.ok = false; result.code = code; result.outputs = {}; result.prepared = {}; result.summary = {}; }
    return result;
  };
}
struct Resolved {
  EnvironmentSelection selected;
  capability::OperationLog::Record view;
  QJsonObject plan;
  QString code;
};
Resolved resolve(const capability::OperationLog::Record &record, EnvironmentMode executionMode,
                 const std::shared_ptr<PythonEnvironmentRegistry> &registry,const EnvironmentOperationContract &contract) {
  Resolved out; out.view = record; out.plan = record.frozenRequest.value("plan").toObject(); QString id;
  if (out.plan.value("schema") == QLatin1String(Envelope)) {
    if (out.plan.keys() != QStringList{"environmentIdentityId", "plan", "schema"} ||
        !out.plan.value("environmentIdentityId").isString() || out.plan.value("environmentIdentityId").toString().size() != 64 ||
        !out.plan.value("plan").isObject()) { out.code = "invalid_python_environment_plan"; return out; }
    id = out.plan.value("environmentIdentityId").toString(); out.plan = out.plan.value("plan").toObject();
    if (QByteArray::fromHex(id.toLatin1()).toHex() != id.toLatin1()) { out.code = "invalid_python_environment_plan"; return out; }
    out.view.frozenRequest.insert("plan", out.plan);
  } else if (!contract.allowLegacyPlans) { out.code = "invalid_python_environment_plan"; return out; }
  if (!registry) { out.code = "python_environment_disabled"; return out; }
  out.selected = registry->resolve(id, out.plan.value("runtimeManifestSha256").toString(), out.plan.value("policy").toObject(), executionMode);
  if (!out.selected.ok()) { out.code = out.selected.code; return out; }
  out.code = contract.checkRecord ? contract.checkRecord(out.view,out.selected) : QStringLiteral("python_contract_unavailable");
  return out;
}
capability::CapabilityPreparation capture(EnvironmentMode executionMode, const std::shared_ptr<PythonEnvironmentRegistry> &registry,
    const EnvironmentOperationContract &contract, const capability::ExecuteRequest &request, const QJsonObject &intent, QString *error) {
  const auto fail = [error](const QString &code) { if (error) *error = code; return capability::CapabilityPreparation{}; };
  if (!registry) return fail("python_environment_disabled");
  const auto selected = registry->active(executionMode); if (!selected.ok()) return fail(selected.code);
  if (executionMode != EnvironmentMode::Generated && !selected.trusted->snapshot().value("executionReady").toBool())
    return fail(selected.trusted->snapshot().value("code").toString("python_trusted_execution_unprobed"));
  if (!contract.captureAdapter) return fail("python_contract_unavailable");
  auto adapter = contract.captureAdapter(selected); if (!adapter.capture) return fail("execution_adapter_unavailable");
  auto preparation = adapter.capture(request, intent, error); if (!preparation.worker) return preparation;
  const auto validate = preparation.validate;
  preparation.validate = [validate, selected] {
    const auto code = interrupted(selected); if (!code.isEmpty()) return code;
    const auto result = validate ? validate() : QStringLiteral("python_analysis_validation_unavailable");
    const auto revoked = interrupted(selected); return revoked.isEmpty() ? result : revoked;
  };
  const auto materialize = preparation.worker;
  preparation.worker = [materialize, selected, executionMode, registry](const auto &cancelled, QString *error) {
    auto code = interrupted(selected); if (!code.isEmpty()) { if (error) *error = code; return capability::FrozenOperation{}; }
    auto frozen = materialize(stopped(selected, cancelled), error);
    code = interrupted(selected); if (!code.isEmpty()) { if (error) *error = code; return capability::FrozenOperation{}; }
    if (frozen.capabilityId.isEmpty()) return frozen;
    const auto plan = frozen.frozenPlan;
    frozen.frozenPlan = {{"schema", QLatin1String(Envelope)}, {"environmentIdentityId", selected.identity.id}, {"plan", plan}};
    frozen.worker = worker(frozen.worker, selected, plan, executionMode, registry, false);
    frozen.inputValidator = validator(frozen.inputValidator, selected, plan);
    return frozen;
  };
  return preparation;
}
} // namespace

capability::CapabilityExecutionAdapter environmentOperationAdapter(EnvironmentMode executionMode,
    std::shared_ptr<PythonEnvironmentRegistry> registry,EnvironmentOperationContract contract) {
  capability::CapabilityExecutionAdapter adapter;
  adapter.capture = [executionMode, registry, contract](const auto &request, const auto &intent, QString *error) {
    return capture(executionMode, registry, contract, request, intent, error);
  };
  adapter.validateFrozen = [executionMode, registry, contract](const auto &record) { return resolve(record, executionMode, registry, contract).code; };
  adapter.restoreInputValidator = [executionMode, registry, contract](const auto &record, QString *error) {
    const auto resolved = resolve(record, executionMode, registry, contract); if (error) *error = resolved.code;
    if (!resolved.code.isEmpty()) return capability::OperationInputValidator{};
    auto inner = contract.inputValidator ? contract.inputValidator(resolved.plan,resolved.selected) : capability::OperationInputValidator{};
    return validator(inner, resolved.selected, resolved.plan);
  };
  adapter.restoreWorker = [executionMode, registry, contract](const auto &record, QString *error) {
    const auto resolved = resolve(record, executionMode, registry, contract); if (error) *error = resolved.code;
    if (!resolved.code.isEmpty()) return capability::OperationWorker{};
    auto inner = contract.worker ? contract.worker(resolved.plan,resolved.selected) : capability::OperationWorker{};
    return worker(inner, resolved.selected, resolved.plan, executionMode, registry, true);
  };
  return adapter;
}
capability::CapabilityExecutionAdapter environmentAnalysisAdapter(AnalysisKind kind,
    std::shared_ptr<PythonEnvironmentRegistry> registry, AnalysisPlanProvider selector) {
  EnvironmentOperationContract contract;contract.allowLegacyPlans=true;
  contract.captureAdapter=[kind,selector](const EnvironmentSelection &selected) {
    return kind==AnalysisKind::Generated ? generatedScriptAdapter(selected.runtime,selected.sandbox,selector) :
      analysisScriptAdapter(kind,selected.runtime,selected.identity.scriptsRoot,selector);
  };
  contract.checkRecord=[kind](const auto &record,const EnvironmentSelection &selected) {
    return kind==AnalysisKind::Generated ? generated_operation::checkRecord(record,selected.runtime,selected.sandbox,false) :
      analysis_operation::checkRecord(record,kind,selected.runtime);
  };
  contract.inputValidator=[kind](const auto &plan,const EnvironmentSelection &selected) {
    return kind==AnalysisKind::Generated ? generated_operation::inputValidator(plan,selected.runtime,selected.sandbox) :
      analysis_operation::inputValidator(plan,kind,selected.runtime);
  };
  contract.worker=[kind](const auto &plan,const EnvironmentSelection &selected) {
    return kind==AnalysisKind::Generated ? generated_operation::worker(plan,selected.runtime,selected.sandbox) :
      analysis_operation::worker(plan,kind,selected.runtime);
  };
  return environmentOperationAdapter(mode(kind),std::move(registry),std::move(contract));
}
} // namespace paleo::python
