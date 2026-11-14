// 层：功能
#include "capabilityhost.h"
#include "capabilitytaskrunner.h"
#include "capabilitynativeoperation.h"
#include "capabilitypublication_internal.h"
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
QByteArray CapabilityHost::requestDigest(const ExecuteRequest &request) { return digest(request); }

CapabilityHost::CapabilityHost(QObject *parent) : QObject(parent), m_recovery(new CapabilityRecovery(this)) {
  m_recovery->setHostProvider([this] {
    const auto b=binding();RecoveryHost h;h.context=b.context;h.projectDir=b.projectDir;h.operationDir=b.operationDir;
    h.catalog=b.catalog;h.writeClaim=b.writeClaim;
    h.presentationState=m_presentationState;h.present=m_present;
    h.restoreInputValidator=[this](const OperationLog::Record &r,QString *error) {
      if(m_adapters.contains(r.capabilityId)) {
        const auto adapter=m_adapters.value(r.capabilityId);
        return adapter.restoreInputValidator?adapter.restoreInputValidator(r,error):OperationInputValidator{};
      }
      if(m_restoreInputValidator)if(auto validator=m_restoreInputValidator(r,error))return validator;
      return nativeInputValidator(r.frozenRequest.value("plan").toObject());
    };
    h.validateFrozen=[this,b](const OperationLog::Record &r) {
      if(m_adapters.contains(r.capabilityId)) {
        const auto adapter=m_adapters.value(r.capabilityId);
        return adapter.validateFrozen?adapter.validateFrozen(r):QStringLiteral("execution_adapter_unavailable");
      }
      if(m_validateFrozen){const auto code=m_validateFrozen(r);if(!code.isEmpty())return code;}
      ExecuteRequest request;
      if(!nativeRequestFromJson(r.frozenRequest.value("plan").toObject(),request))return QString();
      if(!request.inputs.contains("seismic_selection"))return QString();
      request.projectDir=b.projectDir;
      QString code;if(b.freeze)code=b.freeze(request);if(code.isEmpty() && b.validateInputs)code=b.validateInputs(request);
      if(code.isEmpty() && nativeFrozenRequest(request).value("sourceVerification")!=r.frozenRequest.value("plan").toObject().value("sourceVerification"))code="source_revision_changed";
      return code;
    };
    h.restoreWorker=[this,b](const OperationLog::Record &r,QString *error) {
      if(m_adapters.contains(r.capabilityId)) {
        const auto adapter=m_adapters.value(r.capabilityId);
        if(adapter.restoreWorker)return adapter.restoreWorker(r,error);
        if(error)*error="execution_adapter_unavailable";return OperationWorker{};
      }
      if(m_restoreWorker) {
        auto worker=m_restoreWorker(r,error);
        if(worker || (error && !error->isEmpty()))return worker;
      }
      if(r.capabilityId==QLatin1String("workflow.plan"))return restoreNativePlan(r.frozenRequest.value("plan").toObject(),error);
      ExecuteRequest request;
      if(!nativeRequestFromJson(r.frozenRequest.value("plan").toObject(),request)){if(error)*error="invalid_frozen_plan";return OperationWorker{};}
      request.projectDir=b.projectDir;request.operationDir=b.operationDir;
      QString code;if(b.freeze)code=b.freeze(request);if(code.isEmpty() && b.validateInputs)code=b.validateInputs(request);
      if(!code.isEmpty()){if(error)*error=code;return OperationWorker{};}return nativeWorker(std::move(request));
    };return h;
  });
}
CapabilityHost::~CapabilityHost(){m_preparationInvalidated->store(true);}
void CapabilityHost::setWorkerRestorer(std::function<OperationWorker(const OperationLog::Record &,QString*)> restorer) {
  m_restoreWorker=std::move(restorer);
}
void CapabilityHost::setPresentationHandlers(std::function<QString(const QJsonObject&)> state,
    std::function<QString(const QJsonObject&,const PublicationReceipt&)> present) {
  m_presentationState=std::move(state);m_present=std::move(present);
}
void CapabilityHost::setInputValidatorRestorer(std::function<OperationInputValidator(const OperationLog::Record &,QString*)> restorer) {
  m_restoreInputValidator=std::move(restorer);
}
void CapabilityHost::setFrozenValidator(std::function<QString(const OperationLog::Record&)> validator) {m_validateFrozen=std::move(validator);}
void CapabilityHost::registerExecutionAdapter(const QString &id,CapabilityExecutionAdapter adapter) {
  if(!CapabilityRegistry::contains(id))return;
  if(m_adapters.contains(id)) {
    m_preparationInvalidated->store(true);m_preparationInvalidated=std::make_shared<std::atomic_bool>(false);
  }
  m_adapters.insert(id,std::move(adapter));
  for(auto it=m_prepared.begin();it!=m_prepared.end();)
    if(it->request.capabilityId==id)it=m_prepared.erase(it);else ++it;
}
void CapabilityHost::setBindingProvider(BindingProvider provider) {
  m_preparationInvalidated->store(true);m_preparationInvalidated=std::make_shared<std::atomic_bool>(false);
  m_binding=std::move(provider);m_prepared.clear();m_recovery->invalidateRuntimeHooks(false);
}
void CapabilityHost::setTaskService(PaleoTaskService *tasks) { m_tasks = tasks; m_recovery->setTaskService(tasks); }
void CapabilityHost::setCallerScope(host::Caller caller,const QString &scope) {
  if(m_callers.value(int(caller))==scope)return;
  m_callers[int(caller)]=scope;invalidate();
}
QString CapabilityHost::callerScope(host::Caller caller,const QString &session) const {
  const auto base=m_callers.value(int(caller));
  if(base.isEmpty())return {};
  return caller==host::Caller::Agent && !session.isEmpty()?base+":chat:"+QString::fromLatin1(QCryptographicHash::hash(session.toUtf8(),QCryptographicHash::Sha256).toHex()):base;
}
void CapabilityHost::invalidate() {
  m_prepared.clear();m_recovery->invalidateRuntimeHooks();
  m_preparationInvalidated->store(true);m_preparationInvalidated=std::make_shared<std::atomic_bool>(false);
}
void CapabilityHost::prunePrepared(const host::CallContext &context) {
  const auto now=QDateTime::currentMSecsSinceEpoch();
  for(auto it=m_prepared.begin();it!=m_prepared.end();)
    if(it->expiresAt<now || it->context.documentId!=context.documentId || it->context.epoch!=context.epoch)it=m_prepared.erase(it);else ++it;
}
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
                                      std::function<QString()> executionGuard, const QString &trustedChatSessionId) {
  HostPrepared result;
  const auto fail = [&](const QString &code) { result.code = code; return result; };
  auto live = binding();
  prunePrepared(live.context);
  if (!intent.documentId.isEmpty() && intent.documentId != live.context.documentId)
    return fail(QStringLiteral("document_mismatch"));
  QString resolutionError;const auto resolved=CapabilityRegistry::resolveAlias(intent.capabilityId,&resolutionError);
  const auto adapter=m_adapters.value(resolved);
  const auto original=adapter.intent?adapter.intent(intent):nativeIntent(intent);
  const unsigned phaseEffects=host::computeEffects(intent.publishIntent);
  if(!phaseEffects)return fail(QStringLiteral("unsupported_intent"));
  const unsigned effects=phaseEffects|CapabilityRegistry::executionEffects(resolved,intent.contractVersion);
  const QString scope = callerScope(caller,trustedChatSessionId);
  if (scope.isEmpty()) return fail(QStringLiteral("caller_unbound"));
  if (caller != host::Caller::Ui && intent.clientRequestId.trimmed().isEmpty())
    return fail(QStringLiteral("client_request_id_required"));
  const auto prior=m_recovery->lookup(scope,intent.clientRequestId);
  if(prior.code==QLatin1String("found")) {
    if(prior.record.schemaVersion!=2)return fail("legacy_unverifiable");
    if(prior.record.requestHash!=operationDigest(original))return fail("idempotency_conflict");
    if(m_prepared.size()>=128)return fail("prepared_capacity_exceeded");
    Prepared saved;saved.request=intent;saved.request.documentId=live.context.documentId;saved.request.callerScope=scope;
    saved.context=live.context;saved.effects=host::Read;saved.intentDigest=digest(intent);saved.recoveryId=prior.record.operationId;
    saved.commandId=commandId;saved.expiresAt=QDateTime::currentMSecsSinceEpoch()+5*60*1000;
    result.handle=QUuid::createUuid().toString(QUuid::WithoutBraces);m_prepared.insert(result.handle,std::move(saved));
    result.ok=true;result.code="replay";return result;
  }
  if(prior.code!=QLatin1String("not_found"))return fail(prior.code);
  QString code = host::checkContext(live.context, live.context, effects);
  if (code.isEmpty()) code = host::checkRequiredContext(live.context, requiredContext);
  if (!code.isEmpty()) return fail(code);
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
  const auto sourceId=intent.inputs.value("source_operation_id").toString();
  const bool publishSource=intent.publishIntent==QLatin1String("publish") && !sourceId.isEmpty();
  if(publishSource) {
    if(!intent.parameters.isEmpty() || intent.inputs.size()!=1 || intent.seismicTrace)return fail("mixed_source_operation_inputs");
    OperationLog log(live.operationDir,live.projectDir,live.writeClaim);
    saved.frozen=freezeSourcePublication(saved.request,original,log.recordById(sourceId),&code);
    if(!code.isEmpty())return fail(code);
  } else if(adapter.capture) {
    if(saved.request.bindingMode.isEmpty())saved.request.bindingMode="pinned-value";
    saved.preparation=adapter.capture(saved.request,original,&code);
    if(!code.isEmpty() || !saved.preparation.worker)return fail(code.isEmpty()?QStringLiteral("preparation_adapter_unavailable"):code);
    result.effectiveParameters=saved.request.parameters;
  } else if(adapter.freeze) {
    saved.frozen=adapter.freeze(saved.request,original,&code);
    if(!code.isEmpty())return fail(code);
    result.effectiveParameters=saved.request.parameters;
  } else {
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
  saved.request.parameters=result.effectiveParameters;
  code.clear();
  saved.frozen=freezeNativeOperation(saved.request,nativeIntent(intent),&code);
  if(!code.isEmpty())return fail(code);
  }
  saved.context = live.context; saved.effects = effects|saved.frozen.requiredEffects; saved.required = requiredContext;
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
  const QPointer<const CapabilityHost> guard(this);
  if (saved.intentDigest != digest(intent)) return QStringLiteral("prepared_tampered");
  if (saved.expiresAt < QDateTime::currentMSecsSinceEpoch()) return QStringLiteral("prepared_expired");
  live = binding();
  if(!guard)return QStringLiteral("document_unavailable");
  QString code = host::checkContext(live.context, saved.context, saved.effects);
  if (code.isEmpty() && saved.executionGuard) code = saved.executionGuard();
  if(!guard)return QStringLiteral("document_unavailable");
  if (code.isEmpty()) code = host::checkRequiredContext(live.context, saved.required);
  if (code.isEmpty() && saved.recoveryId.isEmpty() && saved.frozen.sourceOperationId.isEmpty() && live.validateInputs)
    code = live.validateInputs(saved.request);
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
  const QPointer<CapabilityHost> guard(this);
  const auto it = m_prepared.constFind(handle);
  if(it==m_prepared.cend()){if(error)*error="prepared_unknown";return nullptr;}
  const Prepared saved = *it; // Host callbacks can invalidate the prepared table.
  QString code;
  HostBinding live;
  code = check(saved, intent, live);
  if(!guard){if(error)*error="document_unavailable";return nullptr;}
  if(code.isEmpty() && !m_prepared.contains(handle))code="prepared_unknown";
  if (!code.isEmpty()) { if (error) *error = code; return nullptr; }
  if(saved.preparation.worker)return startPreparing(handle,saved,intent,receiver,title,std::move(finished),error);
  QString operationId=saved.recoveryId;
  if(operationId.isEmpty()) {
    const auto accepted=m_recovery->accept(saved.frozen);
    if(!guard){if(error)*error="document_unavailable";return nullptr;}
    if(!accepted.ok){if(error)*error=accepted.code;return nullptr;}operationId=accepted.record.operationId;
  }
  auto *task=m_recovery->start(saved.request.callerScope,operationId,receiver,title,std::move(finished),false,error);
  if (guard && task) m_prepared.remove(handle);
  return task;
}

} // namespace paleo::capability
