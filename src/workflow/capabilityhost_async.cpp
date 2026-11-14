// 层：功能
#include "capabilityhost.h"
#include "services/paleotaskservice.h"
#include <QJsonDocument>
#include <QTimer>
#include <QThread>
#include <QScopeGuard>

namespace paleo::capability {
namespace {
struct PreparationState {
  std::atomic_bool stopped{false};
  std::atomic_bool preparationFinished{false};
  QPointer<CapabilityHost> owner;
  QPointer<QObject> receiver;
  QPointer<PaleoTask> parent,phase;
  std::shared_ptr<std::atomic_bool> invalidated;
  std::function<void(ExecuteResult)> callback;
  FrozenOperation frozen;
  QString code;
  bool delivered=false;
};
ExecuteResult preparationFailure(const QString &code) {
  ExecuteResult result;result.code=code;result.execution="rejected";result.commitState="not_requested";
  result.responseJson=QJsonDocument(QJsonObject{{"contract",QLatin1String(OperationContract)},
    {"ok",false},{"code",code},{"execution",result.execution},{"commitState",result.commitState}}).toJson(QJsonDocument::Compact);
  return result;
}
void completePreparation(const std::shared_ptr<PreparationState> &state,ExecuteResult result) {
  if(state->delivered)return;
  state->delivered=true;if(state->preparationFinished.load())state->frozen={};
  auto callback=std::move(state->callback);
  if(state->parent)state->parent->finishExternal(result.ok?QString():result.code);
  if(state->owner && state->receiver && callback)callback(std::move(result));
}
void stopPreparation(const std::shared_ptr<PreparationState> &state) {
  state->stopped=true;
  if(state->phase) {
    if(state->parent && state->parent->explicitCancelRequested())state->phase->requestCancel();
    else state->phase->requestInterruption();
  }
}
}

PaleoTask *CapabilityHost::startPreparing(const QString &handle,const Prepared &saved,const ExecuteRequest &intent,
    QObject *receiver,const QString &title,std::function<void(ExecuteResult)> finished,QString *error) {
  const auto reject=[&](const QString &code)->PaleoTask*{if(error)*error=code;return nullptr;};
  if(QThread::currentThread()!=thread())return reject("owner_thread_required");
  if(!m_tasks || !receiver)return reject("task_service_unavailable");
  if(receiver->thread()!=thread())return reject("subscriber_thread_mismatch");
  const auto key=saved.request.callerScope+QChar(0)+saved.request.clientRequestId;
  if(m_pendingPreparations.contains(key))return reject(m_pendingPreparations.value(key)==saved.intentDigest?
    QStringLiteral("operation_preparing"):QStringLiteral("idempotency_conflict"));
  const QPointer<CapabilityHost> guard(this);const auto tasks=m_tasks;
  auto state=std::make_shared<PreparationState>();state->owner=guard;state->receiver=receiver;
  state->invalidated=m_preparationInvalidated;state->callback=std::move(finished);
  m_pendingPreparations.insert(key,saved.intentDigest);m_prepared.remove(handle);
  auto *created=tasks->beginExternal(title);state->parent=created;
  if(!guard){completePreparation(state,preparationFailure("document_unavailable"));return created;}
  const std::weak_ptr<PreparationState> weak=state;
  connect(created,&PaleoTask::cancelAsked,created,[weak]{if(auto state=weak.lock())state->stopped=true;},Qt::DirectConnection);
  connect(created,&PaleoTask::cancelAsked,created,[weak]{if(auto state=weak.lock())stopPreparation(state);},Qt::QueuedConnection);
  connect(guard.data(),&QObject::destroyed,created,[state]{stopPreparation(state);completePreparation(state,preparationFailure("document_unavailable"));},Qt::SingleShotConnection);
  if(state->receiver)connect(state->receiver.data(),&QObject::destroyed,created,[weak]{
    if(auto state=weak.lock()){stopPreparation(state);if(state->parent)state->parent->requestInterruption();}
  },Qt::SingleShotConnection);
  if(!state->receiver || (state->parent && state->parent->cancelRequested()))stopPreparation(state);
  const auto stopped=[state] {return state->stopped.load() || state->invalidated->load();};
  auto *phase=tasks->start(title,[state,worker=saved.preparation.worker,stopped](PaleoTask *task) {
    const auto done=qScopeGuard([state]{state->preparationFinished=true;});
    connect(task,&PaleoTask::cancelAsked,task,[state]{state->stopped=true;},Qt::DirectConnection);
    if(task->cancelRequested())state->stopped=true;
    if(stopped()){state->code="stale_session";return state->code;}
    state->frozen=worker(stopped,&state->code);return state->code;
  },QString(),true);
  state->phase=phase;
  if(!guard){stopPreparation(state);return created;}
  if(stopped())stopPreparation(state);
  auto *watch=new QTimer(guard.data());watch->setInterval(25);
  connect(watch,&QTimer::timeout,guard.data(),[guard,state,saved,intent] {
    HostBinding live;
    if(state->invalidated->load() || !guard->check(saved,intent,live).isEmpty()) {
      stopPreparation(state);if(state->parent)state->parent->requestInterruption();
    }
  });
  connect(phase,&PaleoTask::finished,watch,[watch]{watch->stop();watch->deleteLater();},Qt::SingleShotConnection);watch->start();
  connect(phase,&PaleoTask::finished,guard.data(),[guard,state,saved,intent,key,title]() mutable {
    auto fail=[&](const QString &code){guard->m_pendingPreparations.remove(key);completePreparation(state,preparationFailure(code));};
    if(state->delivered)return;
    const bool explicitCancel=state->parent && state->parent->explicitCancelRequested();
    if(state->stopped || state->invalidated->load())return fail(explicitCancel?"cancelled":"stale_session");
    HostBinding live;auto code=guard->check(saved,intent,live);
    if(code.isEmpty() && saved.preparation.validate)code=saved.preparation.validate();
    if(!guard){stopPreparation(state);return;}
    if(code.isEmpty())code=state->code;
    if(code.isEmpty() && (!state->phase || state->phase->state()!=PaleoTask::State::Succeeded))code="preparation_failed";
    if(!code.isEmpty())return fail(code);
    code=host::checkContext(live.context,saved.context,saved.effects|state->frozen.requiredEffects);
    if(!code.isEmpty())return fail(code);
    const auto accepted=guard->m_recovery->accept(state->frozen);state->frozen={};
    if(!guard){stopPreparation(state);return;}
    if(!accepted.ok)return fail(accepted.code);
    auto *running=guard->m_recovery->start(saved.request.callerScope,accepted.record.operationId,guard.data(),title,
      [guard,state,key](ExecuteResult result) {
        if(guard)guard->m_pendingPreparations.remove(key);
        completePreparation(state,std::move(result));
      },false,&code,true);
    state->phase=running;
    if(!guard){stopPreparation(state);return;}
    if(!running)return fail(code);
    if(state->stopped || state->invalidated->load())stopPreparation(state);
  },Qt::SingleShotConnection);
  return created;
}
} // namespace paleo::capability
