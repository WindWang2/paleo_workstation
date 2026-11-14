// 层：功能
#include "capabilityhost.h"
#include "capabilitysourcepublication.h"
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

CapabilityHost::CapabilityHost(NativeAdapterFactory nativeFactory, QObject *parent)
  : QObject(parent), m_nativeAdapterFactory(std::move(nativeFactory)), m_recovery(new CapabilityRecovery(this)) {
  m_recovery->setHostProvider([this] {
    const QPointer<CapabilityHost> guard(this);const auto invalidated=m_preparationInvalidated;
    const auto current=[guard,invalidated]{return guard && !invalidated->load();};
    const auto b=binding();RecoveryHost h;if(!current())return h;
    h.context=b.context;h.projectDir=b.projectDir;h.operationDir=b.operationDir;
    h.catalog=b.catalog;h.writeClaim=b.writeClaim;
    h.presentationState=m_presentationState;h.present=m_present;
    h.restoreInputValidator=[guard,b,current](const OperationLog::Record &r,QString *error) {
      const auto unavailable=[error,guard]{if(error)*error=guard?"stale_session":"document_unavailable";return OperationInputValidator{};};
      if(!current())return unavailable();
      if(guard->m_adapters.contains(r.capabilityId)) {
        const auto restore=guard->m_adapters.value(r.capabilityId).restoreInputValidator;
        auto validator=restore?restore(r,error):OperationInputValidator{};
        return current()?validator:unavailable();
      }
      const auto restore=guard->m_restoreInputValidator;const auto factory=guard->m_nativeAdapterFactory;
      if(restore) {
        auto validator=restore(r,error);if(!current())return unavailable();if(validator)return validator;
      }
      const auto native=factory?factory(b):CapabilityExecutionAdapter{};if(!current())return unavailable();
      if(native.restoreInputValidator) {
        auto validator=native.restoreInputValidator(r,error);return current()?validator:unavailable();
      }
      if(error)*error="execution_adapter_unavailable";return OperationInputValidator{};
    };
    h.validateFrozen=[guard,b,current](const OperationLog::Record &r) {
      const auto unavailable=[guard]{return guard?QStringLiteral("stale_session"):QStringLiteral("document_unavailable");};
      if(!current())return unavailable();
      if(guard->m_adapters.contains(r.capabilityId)) {
        const auto validate=guard->m_adapters.value(r.capabilityId).validateFrozen;
        const auto code=validate?validate(r):QStringLiteral("execution_adapter_unavailable");
        return current()?code:unavailable();
      }
      const auto validate=guard->m_validateFrozen;const auto factory=guard->m_nativeAdapterFactory;
      if(validate) {
        const auto code=validate(r);if(!current())return unavailable();if(!code.isEmpty())return code;
      }
      const auto native=factory?factory(b):CapabilityExecutionAdapter{};if(!current())return unavailable();
      const auto code=native.validateFrozen?native.validateFrozen(r):QStringLiteral("execution_adapter_unavailable");
      return current()?code:unavailable();
    };
    h.restoreWorker=[guard,b,current](const OperationLog::Record &r,QString *error) {
      const auto unavailable=[error,guard]{if(error)*error=guard?"stale_session":"document_unavailable";return OperationWorker{};};
      if(!current())return unavailable();
      if(guard->m_adapters.contains(r.capabilityId)) {
        const auto restore=guard->m_adapters.value(r.capabilityId).restoreWorker;
        if(restore){auto worker=restore(r,error);return current()?worker:unavailable();}
        if(error)*error="execution_adapter_unavailable";return OperationWorker{};
      }
      const auto restore=guard->m_restoreWorker;const auto factory=guard->m_nativeAdapterFactory;
      if(restore) {
        auto worker=restore(r,error);if(!current())return unavailable();
        if(worker || (error && !error->isEmpty()))return worker;
      }
      const auto native=factory?factory(b):CapabilityExecutionAdapter{};if(!current())return unavailable();
      if(native.restoreWorker){auto worker=native.restoreWorker(r,error);return current()?worker:unavailable();}
      if(error)*error="execution_adapter_unavailable";return OperationWorker{};
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
  if(QThread::currentThread()!=thread())return {};
  const auto provider=m_binding;return provider?provider():HostBinding{};
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
  const QPointer<CapabilityHost> guard(this);const auto invalidated=m_preparationInvalidated;
  const auto interrupted=[guard,invalidated] {
    return !guard?QStringLiteral("document_unavailable"):invalidated->load()?QStringLiteral("stale_session"):QString();
  };
  auto live = binding();
  if(const auto code=interrupted();!code.isEmpty())return fail(code);
  prunePrepared(live.context);
  if (!intent.documentId.isEmpty() && intent.documentId != live.context.documentId)
    return fail(QStringLiteral("document_mismatch"));
  QString resolutionError;const auto resolved=CapabilityRegistry::resolveAlias(intent.capabilityId,&resolutionError);
  const auto adapter=m_adapters.value(resolved);
  const auto nativeFactory=m_nativeAdapterFactory;const auto native=nativeFactory?nativeFactory(live):CapabilityExecutionAdapter{};
  if(const auto code=interrupted();!code.isEmpty())return fail(code);
  if(intent.seismicTrace && !adapter.intent && !native.intent)return fail("execution_adapter_unavailable");
  const auto original=adapter.intent?adapter.intent(intent):native.intent?native.intent(intent):capabilityIntent(intent);
  if(const auto code=interrupted();!code.isEmpty())return fail(code);
  const unsigned phaseEffects=host::computeEffects(intent.publishIntent);
  if(!phaseEffects)return fail(QStringLiteral("unsupported_intent"));
  const unsigned effects=phaseEffects|CapabilityRegistry::executionEffects(resolved,intent.contractVersion);
  const QString scope = callerScope(caller,trustedChatSessionId);
  if (scope.isEmpty()) return fail(QStringLiteral("caller_unbound"));
  if (caller != host::Caller::Ui && intent.clientRequestId.trimmed().isEmpty())
    return fail(QStringLiteral("client_request_id_required"));
  const auto prior=m_recovery->lookup(scope,intent.clientRequestId);
  if(const auto code=interrupted();!code.isEmpty())return fail(code);
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
    if(const auto stopped=interrupted();!stopped.isEmpty())return fail(stopped);
    if(!code.isEmpty() || !saved.preparation.worker)return fail(code.isEmpty()?QStringLiteral("preparation_adapter_unavailable"):code);
    result.effectiveParameters=saved.request.parameters;
  } else if(adapter.freeze) {
    saved.frozen=adapter.freeze(saved.request,original,&code);
    if(const auto stopped=interrupted();!stopped.isEmpty())return fail(stopped);
    if(!code.isEmpty())return fail(code);
    result.effectiveParameters=saved.request.parameters;
  } else {
  if(!native.freeze || !native.normalize)return fail("execution_adapter_unavailable");
  code=native.normalize(saved.request,result.effectiveParameters);
  if(const auto stopped=interrupted();!stopped.isEmpty())return fail(stopped);
  if(!code.isEmpty())return fail(code);
  const auto nativeRequestIntent=native.intent?native.intent(intent):original;
  if(const auto stopped=interrupted();!stopped.isEmpty())return fail(stopped);
  saved.frozen=native.freeze(saved.request,nativeRequestIntent,&code);
  if(const auto stopped=interrupted();!stopped.isEmpty())return fail(stopped);
  if(!code.isEmpty())return fail(code);
  }
  saved.context = live.context; saved.effects = effects|saved.frozen.requiredEffects; saved.required = requiredContext;
  saved.commandId = commandId;
  saved.executionGuard = std::move(executionGuard);
  saved.intentDigest = digest(intent);
  saved.expiresAt = QDateTime::currentMSecsSinceEpoch() + 5 * 60 * 1000;
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
