// 层：功能
// workflow/commanddispatcher.cpp — 见 commanddispatcher.h。
#include "commanddispatcher.h"

#include <QCoreApplication>
#include <QUuid>

namespace paleo::command {

namespace {
QString tr(const char *s) { return QCoreApplication::translate("PaleoCommands", s); }
} // namespace

bool surfaceAllows(const CommandSpec &spec, const CommandContext &ctx, QString *reason) {
  if (!ctx.isAgent())
    return true;
  if (!spec.hasSurface(SurfaceAgent)) {
    if (reason)
      *reason = tr("此命令未开放给 AI 助手");
    return false;
  }
  const CapabilitySpec cap = CapabilityRegistry::spec(spec.capabilityId, spec.contractVersion);
  if (cap.exposure != CapabilityExposure::Callable) {
    if (reason)
      *reason = cap.exposureReason.isEmpty() ? tr("能力未开放调用") : cap.exposureReason;
    return false;
  }
  return true;
}

CommandDispatcher::CommandDispatcher(QObject *parent)
  : QObject(parent), m_executor([](const capability::ExecuteRequest &r) { return capability::execute(r); }) {}

bool CommandDispatcher::bind(const QString &commandId, Handler handler, StateProvider state, QString *error) {
  if (!CommandRegistry::contains(commandId)) {
    if (error)
      *error = QStringLiteral("unregistered command '%1'").arg(commandId);
    return false;
  }
  if (!handler) {
    if (error)
      *error = QStringLiteral("empty handler for '%1'").arg(commandId);
    return false;
  }
  m_bindings.insert(commandId, Binding{std::move(handler), std::move(state)});
  return true;
}

void CommandDispatcher::unbind(const QString &commandId) { m_bindings.remove(commandId); }

bool CommandDispatcher::isBound(const QString &commandId) const { return m_bindings.contains(commandId); }

QStringList CommandDispatcher::boundIds() const {
  QStringList ids = m_bindings.keys();
  ids.sort();
  return ids;
}

CommandState CommandDispatcher::state(const QString &commandId, const CommandContext &ctx) const {
  CommandState st;
  const CommandSpec spec = CommandRegistry::spec(commandId);
  if (!spec.isValid()) {
    st.reason = tr("未登记的命令");
    return st;
  }
  if (!surfaceAllows(spec, ctx, &st.reason))
    return st;
  if (!spec.capabilityId.isEmpty()) {
    const CapabilitySpec cap = CapabilityRegistry::spec(spec.capabilityId, spec.contractVersion);
    if (cap.capabilityId.isEmpty()) {
      st.reason = tr("能力目录里没有这条能力");
      return st;
    }
  }
  const auto it = m_bindings.constFind(commandId);
  if (it == m_bindings.constEnd()) {
    // Compute 命令没有 UI 处理器时仍可经 prepare/execute 桥执行（Agent/批处理）。
    if (spec.kind == CommandKind::Compute && ctx.callerScope != QLatin1String("ui")) {
      st.enabled = true;
      return st;
    }
    st.reason = tr("命令尚未接入当前窗口");
    return st;
  }
  if (it->state) {
    st = it->state(ctx);
    if (!st.enabled && st.reason.isEmpty())
      st.reason = tr("当前不可用");
    if (st.enabled)
      st.reason.clear();
    return st;
  }
  st.enabled = true;
  return st;
}

bool CommandDispatcher::trigger(const QString &commandId, const CommandContext &ctx, const QVariantMap &params,
                                QString *reason) {
  const CommandState st = state(commandId, ctx);
  const auto it = m_bindings.constFind(commandId);
  if (!st.enabled || it == m_bindings.constEnd()) {
    const QString why = st.enabled ? tr("命令尚未接入当前窗口") : st.reason;
    if (reason)
      *reason = why;
    emit commandRejected(commandId, why);
    return false;
  }
  // 拷一份：处理器可能重绑自己（workbench 重建镜像）。
  const Handler handler = it->handler;
  handler(ctx, params);
  emit commandTriggered(commandId, ctx.callerScope);
  return true;
}

PreparedPlan CommandDispatcher::prepare(const QString &commandId, const CommandContext &ctx,
                                        const QVariantMap &params, const QVariantMap &inputs,
                                        const QString &publishIntent) const {
  PreparedPlan plan;
  plan.commandId = commandId;
  if (m_hostRequired && !m_host) {
    plan.code = QStringLiteral("host_unavailable");
    return plan;
  }
  const CommandSpec spec = CommandRegistry::spec(commandId);
  if (!spec.isValid()) {
    plan.code = QStringLiteral("unknown_command");
    return plan;
  }
  if (spec.kind != CommandKind::Compute || spec.capabilityId.isEmpty()) {
    plan.code = QStringLiteral("not_compute");
    return plan;
  }
  plan.capability = CapabilityRegistry::spec(spec.capabilityId, spec.contractVersion);
  if (plan.capability.capabilityId.isEmpty()) {
    plan.code = QStringLiteral("capability_missing");
    return plan;
  }
  QString why;
  if (!surfaceAllows(spec, ctx, &why)) {
    plan.code = spec.hasSurface(SurfaceAgent) ? QStringLiteral("capability_not_callable")
                                              : QStringLiteral("surface_denied");
    return plan;
  }
  if (plan.capability.exposure != CapabilityExposure::Callable) {
    plan.code = QStringLiteral("capability_not_callable");
    return plan;
  }
  if (!plan.capability.algorithmId.isEmpty()) {
    QString code;
    plan.effectiveParameters = capability::effectiveScalarParams(
        plan.capability.algorithmId, plan.capability.algorithmContractVersion, params, &code);
    if (code != QLatin1String("ok")) { // 网关约定：成功码为 "ok"
      plan.code = code.isEmpty() ? QStringLiteral("parameter_invalid") : code;
      return plan;
    }
  }
  capability::ExecuteRequest &r = plan.request;
  r.capabilityId = plan.capability.capabilityId;
  r.contractVersion = plan.capability.contractVersion;
  r.documentId = ctx.documentId;
  r.callerScope = ctx.callerScope;
  r.clientRequestId =
      ctx.clientRequestId.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : ctx.clientRequestId;
  if (ctx.isAgent() && ctx.clientRequestId.isEmpty()) {
    plan.code = QStringLiteral("client_request_id_required");
    return plan;
  }
  // 用户原样参数交网关（网关自己再校验/填默认并算指纹，有效参数只作展示）。
  r.parameters = params;
  r.inputs = inputs;
  r.publishIntent = publishIntent;
  if (m_host) {
    const auto it = m_bindings.constFind(commandId);
    if (it != m_bindings.cend() && it->state && !it->state(ctx).enabled) {
      plan.code = QStringLiteral("command_disabled");
      return plan;
    }
    const QPointer<const CommandDispatcher> self(this);
    const bool hadBinding = it != m_bindings.cend();
    auto guard = [self, commandId, hadBinding]() -> QString {
      if (!self) return QStringLiteral("host_unavailable");
      const auto current = self->m_bindings.constFind(commandId);
      if (hadBinding && current == self->m_bindings.cend()) return QStringLiteral("command_unbound");
      if (current != self->m_bindings.cend() && current->state && !current->state(self->currentContext()).enabled)
        return QStringLiteral("command_disabled");
      return {};
    };
    const auto prepared = m_host->prepare(r, m_hostCaller, spec.requiredContext, commandId, std::move(guard));
    plan.code = prepared.code;
    plan.hostHandle = prepared.handle;
    plan.ok = prepared.ok;
    return plan;
  }
  if (m_hostBinder)
    m_hostBinder(r);
  plan.ok = true;
  return plan;
}

capability::ExecuteResult CommandDispatcher::execute(const PreparedPlan &plan) {
  if (m_hostRequired && !m_host) {
    capability::ExecuteResult rejected;
    rejected.code = QStringLiteral("host_unavailable");
    return rejected;
  }
  if (!plan.ok) {
    capability::ExecuteResult rejected;
    rejected.code = plan.code.isEmpty() ? QStringLiteral("plan_not_prepared") : plan.code;
    rejected.execution = QStringLiteral("Rejected");
    return rejected;
  }
  if (m_host) {
    const auto boundCommand = m_host->preparedCommand(plan.hostHandle);
    if (!boundCommand.isNull() && boundCommand != plan.commandId) {
      capability::ExecuteResult rejected;
      rejected.code = QStringLiteral("prepared_tampered");
      return rejected;
    }
    const auto it = m_bindings.constFind(plan.commandId);
    if (it != m_bindings.cend() && it->state && !it->state(currentContext()).enabled) {
      capability::ExecuteResult rejected;
      rejected.code = QStringLiteral("command_disabled");
      rejected.execution = QStringLiteral("Rejected");
      return rejected;
    }
  }
  capability::ExecuteResult result = m_host ? m_host->execute(plan.hostHandle, plan.request) : m_executor(plan.request);
  if (!result.operationId.isEmpty())
    emit operationChanged(result.operationId, plan.commandId);
  if (result.ok)
    emit commandTriggered(plan.commandId, m_host ? m_host->callerScope(m_hostCaller) : plan.request.callerScope);
  return result;
}

void CommandDispatcher::setExecutor(Executor executor) {
  m_executor = executor ? std::move(executor)
                        : Executor([](const capability::ExecuteRequest &r) { return capability::execute(r); });
}

void CommandDispatcher::setHostBinder(HostBinder binder) { m_hostBinder = std::move(binder); }
void CommandDispatcher::setCapabilityHost(capability::CapabilityHost *value, host::Caller caller) {
  m_host = value;
  m_hostCaller = caller;
  m_hostRequired = true;
}

PaleoTask *CommandDispatcher::start(const PreparedPlan &plan, QObject *receiver, const QString &title,
                                  std::function<void(capability::ExecuteResult)> finished, QString *error) {
  QString code;
  if (!m_host) code = QStringLiteral("host_unavailable");
  else if (!plan.ok) code = plan.code.isEmpty() ? QStringLiteral("plan_not_prepared") : plan.code;
  if (code.isEmpty()) {
    const auto boundCommand = m_host->preparedCommand(plan.hostHandle);
    if (!boundCommand.isNull() && boundCommand != plan.commandId) code = QStringLiteral("prepared_tampered");
  }
  const auto it = m_bindings.constFind(plan.commandId);
  if (code.isEmpty() && it != m_bindings.cend() && it->state && !it->state(currentContext()).enabled)
    code = QStringLiteral("command_disabled");
  if (!code.isEmpty()) { if (error) *error = code; return nullptr; }
  const QPointer<CommandDispatcher> self(this);
  return m_host->start(plan.hostHandle, plan.request, receiver, title,
      [self, commandId = plan.commandId, scope = m_host->callerScope(m_hostCaller),
       finished = std::move(finished)](capability::ExecuteResult result) {
        if (self) {
          if (!result.operationId.isEmpty()) emit self->operationChanged(result.operationId, commandId);
          if (result.ok) emit self->commandTriggered(commandId, scope);
        }
        if (finished) finished(std::move(result));
      }, error);
}

void CommandDispatcher::setContextProvider(std::function<CommandContext()> provider) {
  m_context = std::move(provider);
}

CommandContext CommandDispatcher::currentContext() const { return m_context ? m_context() : CommandContext(); }

} // namespace paleo::command
