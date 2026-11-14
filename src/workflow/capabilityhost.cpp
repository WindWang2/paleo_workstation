// 层：功能
#include "capabilityhost.h"
#include "capabilitytaskrunner.h"
#include "services/paleotaskservice.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QThread>
#include <QUuid>

namespace paleo::capability {
namespace {
QByteArray digest(const ExecuteRequest &r) {
  const QJsonObject value{{"capability", r.capabilityId}, {"version", r.contractVersion},
    {"document", r.documentId}, {"caller", r.callerScope}, {"request", r.clientRequestId},
    {"intent", r.publishIntent}, {"parameters", QJsonObject::fromVariantMap(r.parameters)},
    {"inputs", QJsonObject::fromVariantMap(r.inputs)}, {"bindingMode", r.bindingMode},
    {"binding", r.bindingId}, {"frozenHead", r.frozenHead}, {"currentHead", r.currentHead},
    {"operationDir", r.operationDir}, {"projectDir", r.projectDir}, {"present", r.presentOk}};
  return QCryptographicHash::hash(QJsonDocument(value).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
}
ExecuteResult rejected(const QString &code) {
  ExecuteResult result; result.code = code; result.execution = QStringLiteral("Rejected");
  result.commitState = QStringLiteral("not_requested"); return result;
}
} // namespace

CapabilityHost::CapabilityHost(QObject *parent) : QObject(parent) {}
void CapabilityHost::setBindingProvider(BindingProvider provider) { m_binding = std::move(provider); invalidate(); }
void CapabilityHost::setTaskService(PaleoTaskService *tasks) { m_tasks = tasks; }
void CapabilityHost::setCallerScope(host::Caller caller, const QString &scope) { m_callers[int(caller)] = scope; }
QString CapabilityHost::callerScope(host::Caller caller) const { return m_callers.value(int(caller)); }
void CapabilityHost::invalidate() { m_prepared.clear(); }
HostBinding CapabilityHost::binding() const {
  return QThread::currentThread() == thread() && m_binding ? m_binding() : HostBinding{};
}
host::CallContext CapabilityHost::context() const { return binding().context; }
QString CapabilityHost::preparedCommand(const QString &handle) const {
  const auto it = m_prepared.constFind(handle);
  return it == m_prepared.cend() ? QString() : it->commandId;
}

HostPrepared CapabilityHost::prepare(const ExecuteRequest &intent, host::Caller caller,
                                      const QStringList &requiredContext, const QString &commandId,
                                      std::function<QString()> executionGuard) {
  HostPrepared result;
  const auto fail = [&](const QString &code) { result.code = code; return result; };
  auto live = binding();
  if (!intent.documentId.isEmpty() && intent.documentId != live.context.documentId)
    return fail(QStringLiteral("document_mismatch"));
  const unsigned effects = host::computeEffects(intent.publishIntent);
  if (!effects) return fail(QStringLiteral("unsupported_intent"));
  // W5 supplies an owner-side receipt/CAS publication path. The old gateway
  // performs synchronous compute+publish and is not a product-host shortcut.
  if (intent.publishIntent == QLatin1String("publish"))
    return fail(QStringLiteral("publication_not_ready"));
  QString code = host::checkContext(live.context, live.context, effects);
  if (code.isEmpty()) code = host::checkRequiredContext(live.context, requiredContext);
  if (!code.isEmpty()) return fail(code);
  const QString scope = callerScope(caller);
  if (scope.isEmpty()) return fail(QStringLiteral("caller_unbound"));
  if (caller != host::Caller::Ui && intent.clientRequestId.trimmed().isEmpty())
    return fail(QStringLiteral("client_request_id_required"));
  const QString id = CapabilityRegistry::resolveAlias(intent.capabilityId, &code);
  const auto spec = CapabilityRegistry::spec(id, intent.contractVersion);
  if (spec.capabilityId.isEmpty() || spec.exposure != CapabilityExposure::Callable)
    return fail(QStringLiteral("capability_not_callable"));
  Prepared saved;
  saved.request = intent;
  saved.request.capabilityId = id;
  saved.request.documentId = live.context.documentId;
  saved.request.callerScope = scope;
  if (saved.request.clientRequestId.isEmpty())
    saved.request.clientRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
  saved.request.catalog = live.catalog;
  saved.request.projectDir = live.projectDir;
  saved.request.operationDir = live.operationDir;
  saved.request.currentHead.clear(); // Never trust the caller's alleged current state.
  saved.request.cancelled = {}; saved.request.onRunning = {};
  if (live.freeze) code = live.freeze(saved.request); else code.clear();
  if (code.isEmpty() && live.validateInputs) code = live.validateInputs(saved.request);
  if (!code.isEmpty()) return fail(code);
  auto parameters = saved.request.parameters;
  const bool resolvedInterval = saved.request.seismicTrace && !parameters.contains(QStringLiteral("SAMPLE_INTERVAL_MS"));
  if (resolvedInterval) parameters.insert(QStringLiteral("SAMPLE_INTERVAL_MS"), 1.0);
  result.effectiveParameters = effectiveScalarParams(spec.algorithmId, spec.algorithmContractVersion, parameters, &code);
  if (code != QLatin1String("ok")) return fail(code);
  // SDK preflight supplies the real interval; do not display a placeholder as a default.
  if (resolvedInterval) result.effectiveParameters.remove(QStringLiteral("SAMPLE_INTERVAL_MS"));
  saved.context = live.context; saved.effects = effects; saved.required = requiredContext;
  saved.commandId = commandId;
  saved.executionGuard = std::move(executionGuard);
  saved.intentDigest = digest(intent);
  saved.expiresAt = QDateTime::currentMSecsSinceEpoch() + 5 * 60 * 1000;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  for (auto it = m_prepared.begin(); it != m_prepared.end(); )
    if (it->expiresAt < now || it->context.documentId != live.context.documentId ||
        it->context.epoch != live.context.epoch) it = m_prepared.erase(it); else ++it;
  if (m_prepared.size() >= 128) return fail(QStringLiteral("prepared_capacity_exceeded"));
  result.handle = QUuid::createUuid().toString(QUuid::WithoutBraces);
  m_prepared.insert(result.handle, std::move(saved)); result.ok = true; result.code = QStringLiteral("ok");
  return result;
}

QString CapabilityHost::check(const Prepared &saved, const ExecuteRequest &intent, HostBinding &live) const {
  if (saved.intentDigest != digest(intent)) return QStringLiteral("prepared_tampered");
  if (saved.expiresAt < QDateTime::currentMSecsSinceEpoch()) return QStringLiteral("prepared_expired");
  live = binding();
  QString code = host::checkContext(live.context, saved.context, saved.effects);
  if (code.isEmpty() && saved.executionGuard) code = saved.executionGuard();
  if (code.isEmpty()) code = host::checkRequiredContext(live.context, saved.required);
  if (code.isEmpty() && live.validateInputs) code = live.validateInputs(saved.request);
  return code;
}

ExecuteResult CapabilityHost::execute(const QString &handle, const ExecuteRequest &intent) {
  const auto it = m_prepared.constFind(handle);
  if (it == m_prepared.cend()) return rejected(QStringLiteral("prepared_unknown"));
  const Prepared saved = *it;
  HostBinding live;
  const QString code = check(saved, intent, live);
  if (!code.isEmpty()) return rejected(code);
  return rejected(QStringLiteral("async_execution_required"));
}

PaleoTask *CapabilityHost::start(const QString &handle, const ExecuteRequest &intent,
                                QObject *receiver, const QString &title,
                                std::function<void(ExecuteResult)> finished, QString *error) {
  const auto it = m_prepared.constFind(handle);
  QString code;
  HostBinding live;
  if (it == m_prepared.cend()) code = QStringLiteral("prepared_unknown");
  else code = check(*it, intent, live);
  if (!code.isEmpty()) { if (error) *error = code; return nullptr; }
  const Prepared saved = *it;
  QPointer<CapabilityHost> guard(this);
  auto *task = startCapabilityTask(m_tasks, receiver, title, saved.request,
    [guard, saved, finished = std::move(finished)](ExecuteResult result) {
      if (!finished) return;
      if (!guard) { finished(rejected(QStringLiteral("stale_session"))); return; }
      const auto current = guard->binding();
      const QString invalid = host::checkContext(current.context, saved.context, saved.effects);
      if (!invalid.isEmpty() && result.commitState != QLatin1String("committed")) {
        result.ok = false; result.code = invalid;
      }
      finished(std::move(result));
    }, error);
  if (task) m_prepared.remove(handle);
  return task;
}

} // namespace paleo::capability
