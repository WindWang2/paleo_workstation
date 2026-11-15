// 层：功能
#include "pythonenvironmentoperation.h"
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
    const QJsonObject &plan, AnalysisKind kind, const std::shared_ptr<PythonEnvironmentRegistry> &registry,
    bool restoring) {
  return [inner, selected, plan, kind, registry, restoring](const capability::OperationWorkContext &context) {
    capability::OperationWorkResult result;
    auto code = interrupted(selected, &context); if (!code.isEmpty()) { result.code = code; return result; }
    auto view = context; view.frozenPlan = plan; view.cancelled = stopped(selected, context.cancelled, context.requestInterruption);
    if (restoring) {
      const auto proof = registry->probe(selected.identity.id, mode(kind), view.cancelled);
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
Resolved resolve(const capability::OperationLog::Record &record, AnalysisKind kind,
                 const std::shared_ptr<PythonEnvironmentRegistry> &registry) {
  Resolved out; out.view = record; out.plan = record.frozenRequest.value("plan").toObject(); QString id;
  if (out.plan.value("schema") == QLatin1String(Envelope)) {
    if (out.plan.keys() != QStringList{"environmentIdentityId", "plan", "schema"} ||
        !out.plan.value("environmentIdentityId").isString() || out.plan.value("environmentIdentityId").toString().size() != 64 ||
        !out.plan.value("plan").isObject()) { out.code = "invalid_python_environment_plan"; return out; }
    id = out.plan.value("environmentIdentityId").toString(); out.plan = out.plan.value("plan").toObject();
    if (QByteArray::fromHex(id.toLatin1()).toHex() != id.toLatin1()) { out.code = "invalid_python_environment_plan"; return out; }
    out.view.frozenRequest.insert("plan", out.plan);
  }
  if (!registry) { out.code = "python_environment_disabled"; return out; }
  out.selected = registry->resolve(id, out.plan.value("runtimeManifestSha256").toString(), out.plan.value("policy").toObject(), mode(kind));
  if (!out.selected.ok()) { out.code = out.selected.code; return out; }
  out.code = kind == AnalysisKind::Generated ? generated_operation::checkRecord(out.view, out.selected.runtime, out.selected.sandbox, false) :
      analysis_operation::checkRecord(out.view, kind, out.selected.runtime);
  return out;
}
capability::CapabilityPreparation capture(AnalysisKind kind, const std::shared_ptr<PythonEnvironmentRegistry> &registry,
    const AnalysisPlanProvider &selector, const capability::ExecuteRequest &request, const QJsonObject &intent, QString *error) {
  const auto fail = [error](const QString &code) { if (error) *error = code; return capability::CapabilityPreparation{}; };
  if (!registry) return fail("python_environment_disabled");
  const auto selected = registry->active(mode(kind)); if (!selected.ok()) return fail(selected.code);
  if (kind != AnalysisKind::Generated && !selected.trusted->snapshot().value("executionReady").toBool())
    return fail(selected.trusted->snapshot().value("code").toString("python_trusted_execution_unprobed"));
  auto adapter = kind == AnalysisKind::Generated ? generatedScriptAdapter(selected.runtime, selected.sandbox, selector) :
      analysisScriptAdapter(kind, selected.runtime, selected.identity.scriptsRoot, selector);
  auto preparation = adapter.capture(request, intent, error); if (!preparation.worker) return preparation;
  const auto validate = preparation.validate;
  preparation.validate = [validate, selected] {
    const auto code = interrupted(selected); if (!code.isEmpty()) return code;
    const auto result = validate ? validate() : QStringLiteral("python_analysis_validation_unavailable");
    const auto revoked = interrupted(selected); return revoked.isEmpty() ? result : revoked;
  };
  const auto materialize = preparation.worker;
  preparation.worker = [materialize, selected, kind, registry](const auto &cancelled, QString *error) {
    auto code = interrupted(selected); if (!code.isEmpty()) { if (error) *error = code; return capability::FrozenOperation{}; }
    auto frozen = materialize(stopped(selected, cancelled), error);
    code = interrupted(selected); if (!code.isEmpty()) { if (error) *error = code; return capability::FrozenOperation{}; }
    if (frozen.capabilityId.isEmpty()) return frozen;
    const auto plan = frozen.frozenPlan;
    frozen.frozenPlan = {{"schema", QLatin1String(Envelope)}, {"environmentIdentityId", selected.identity.id}, {"plan", plan}};
    frozen.worker = worker(frozen.worker, selected, plan, kind, registry, false);
    frozen.inputValidator = validator(frozen.inputValidator, selected, plan);
    return frozen;
  };
  return preparation;
}
} // namespace

capability::CapabilityExecutionAdapter environmentAnalysisAdapter(AnalysisKind kind,
    std::shared_ptr<PythonEnvironmentRegistry> registry, AnalysisPlanProvider selector) {
  capability::CapabilityExecutionAdapter adapter;
  adapter.capture = [kind, registry, selector](const auto &request, const auto &intent, QString *error) {
    return capture(kind, registry, selector, request, intent, error);
  };
  adapter.validateFrozen = [kind, registry](const auto &record) { return resolve(record, kind, registry).code; };
  adapter.restoreInputValidator = [kind, registry](const auto &record, QString *error) {
    const auto resolved = resolve(record, kind, registry); if (error) *error = resolved.code;
    if (!resolved.code.isEmpty()) return capability::OperationInputValidator{};
    auto inner = kind == AnalysisKind::Generated ? generated_operation::inputValidator(resolved.plan, resolved.selected.runtime, resolved.selected.sandbox) :
        analysis_operation::inputValidator(resolved.plan, kind, resolved.selected.runtime);
    return validator(inner, resolved.selected, resolved.plan);
  };
  adapter.restoreWorker = [kind, registry](const auto &record, QString *error) {
    const auto resolved = resolve(record, kind, registry); if (error) *error = resolved.code;
    if (!resolved.code.isEmpty()) return capability::OperationWorker{};
    auto inner = kind == AnalysisKind::Generated ? generated_operation::worker(resolved.plan, resolved.selected.runtime, resolved.selected.sandbox) :
        analysis_operation::worker(resolved.plan, kind, resolved.selected.runtime);
    return worker(inner, resolved.selected, resolved.plan, kind, registry, true);
  };
  return adapter;
}
} // namespace paleo::python
