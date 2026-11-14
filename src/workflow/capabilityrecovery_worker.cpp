// 层：功能
// workflow/capabilityrecovery_worker.cpp — 数值/拷贝/校验只在池内；Catalog 只在 owner 提交。
#include "capabilityrecovery_internal.h"
#include "services/operationretention.h"
#include "capabilitypublication_internal.h"
#include "services/paleotaskservice.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QUuid>
#include <QTimer>
#include <QScopeGuard>
#include <QThread>
#include <algorithm>
namespace paleo::capability {
namespace recovery_detail {
namespace {
ExecuteResult invalidDelivery(ExecuteResult result,const QString &code) {
  result.ok=false;result.code=code;result.summary.clear();
  result.responseJson=QJsonDocument(QJsonObject{{"contract",QLatin1String(OperationContract)},{"ok",false},{"code",code},
    {"operationId",result.operationId},{"execution",result.execution},{"commitState",result.commitState}}).toJson(QJsonDocument::Compact);
  return result;
}
bool diagnosticPort(const AlgorithmSpec &spec,const QString &id) {
  for(const auto &port:spec.outputs)if(port.id==id)return port.constraints.value("diagnostic").toBool();
  return false;
}
QString validateDiagnostics(const OperationWorkContext &context,const AlgorithmRunResult &diagnostics) {
  if(diagnostics.artifacts.isEmpty())return {};
  if(!diagnostics.ok || diagnostics.artifacts.size()>8)return "invalid_diagnostic_output";
  const auto root=QFileInfo(context.privateDir).canonicalFilePath()+'/';qint64 bytes=0;
  for(const auto &artifact:diagnostics.artifacts) {
    const QFileInfo file(artifact.absolutePath);
    if(!diagnosticPort(context.outputSpec,artifact.portId) || !file.isFile() || file.isSymLink() ||
        !file.canonicalFilePath().startsWith(root) || file.size()<0 || file.size()>4*1024*1024-bytes)
      return "invalid_diagnostic_output";
    bytes+=file.size();
  }
  return {};
}
}
OperationWorkResult run(const RecoveryHost &h,OperationLog::Record r,OperationWorker worker,OperationInputValidator validator,
    const std::shared_ptr<ActiveAttempt> &attempt,const std::function<void(const QString&)> &checkpoint) {
  const auto finished=qScopeGuard([attempt]{attempt->workFinished=true;});
  OperationWorkResult out; QString error;
  const auto stop=[attempt]{
    return attempt->cancelled.load() || attempt->interrupted.load() ||
      (attempt->contextInvalidated && attempt->contextInvalidated->load());
  };
  OperationLog log(h.operationDir,h.projectDir,h.writeClaim);
  const auto fail=[&](const QString &code) {
    OperationWorkResult failed;failed.code=code;auto current=log.recordById(r.operationId);
    if(current.ownerAttempt==r.ownerAttempt && current.execution==QLatin1String("running")) {
      current.execution=attempt->cancelled.load()?"cancelled":stop()?"interrupted":"failed";
      current.code=stop() && !attempt->cancelled.load()?QStringLiteral("interrupted"):code;
      store(log,current,current.execution=="interrupted"?OperationLog::Transition::Reconcile:OperationLog::Transition::Advance);
    }
    attempt->workFinished=true;return failed;
  };
  OperationWorkContext context;context.operationId=r.operationId;context.attemptId=r.ownerAttempt;
  if(stop())return fail("cancelled");
  context.projectDir=h.projectDir;
  context.frozenPlan=r.frozenRequest.value("plan").toObject();context.cancelled=stop;
  context.privateDir=QDir(h.operationDir).filePath(r.operationId+'/'+r.ownerAttempt);
  if(!QDir().mkpath(context.privateDir) || !publication_detail::specFromJson(r.stagePlan.value("outputSpec").toObject(),context.outputSpec) ||
      !publication_detail::targetsFromJson(h.projectDir,r.stagePlan,context.targets)) return fail("invalid_frozen_plan");
  if(validator){const auto code=validator(context);if(!code.isEmpty())return fail(code);}
  if(!r.preparedManifest.isEmpty()) {
    attempt->workFinished=true; // Verification may stop, but already-frozen data is not a running computation.
    out.ok=publication_detail::preparedFromJson(h.projectDir,r,out.prepared,&error,stop);
    out.code=out.ok?"prepared":error;return out;
  }
  const auto sourceId=r.frozenRequest.value("sourceOperationId").toString();
  if(!sourceId.isEmpty() && r.frozenRequest.value("phase").toString()==QLatin1String("publish")) {
    const auto source=authorized(log,h,r.callerScope,sourceId);
    if((source.execution!=QLatin1String("succeeded") && source.commitState!=QLatin1String("committed")) ||
       !publication_detail::outputsFromManifest(h.projectDir,source.operationId,source.artifactManifest,out.outputs,&error,stop,true,managedTargets(source,h.projectDir))) {
      return fail(error.isEmpty()?"source_result_unavailable":error);
    }
    // A new publication consumes numerical outputs only; original logs remain with its source operation.
    out.outputs.artifacts.erase(std::remove_if(out.outputs.artifacts.begin(),out.outputs.artifacts.end(),
      [&](const auto &artifact){return diagnosticPort(context.outputSpec,artifact.portId);}),out.outputs.artifacts.end());
    out.ok=true;
    if(source.artifactManifest.contains("childOperations"))out.summary.insert("child_operations",source.artifactManifest.value("childOperations"));
  } else if(worker)out=worker(context);else return fail("execution_adapter_unavailable");
  if(out.ok && stop()){out.ok=false;out.code="cancelled";}
  const auto diagnosticError=validateDiagnostics(context,out.diagnostics);
  if(!diagnosticError.isEmpty()){out.ok=false;out.code=diagnosticError;out.diagnostics={};}
  if(out.ok && r.frozenRequest.value("phase").toString()==QLatin1String("publish")) {
    out.ok=prepareAlgorithmOutputSet(context.outputSpec,out.outputs,context.targets,out.prepared,&error,stop);
    if(out.ok) {
      auto &p=out.prepared;p.publicationSchema=2;p.id=r.stagePlan.value("outputSetId").toString();
      p.documentId=r.documentId;p.operationId=r.operationId;p.requestHash=r.requestHash;p.planHash=r.planHash;p.storeGeneration=r.storeGeneration;
      out.outputs=publication_detail::preparedOutputs(p);
    }
  }
  const auto allowed=managedTargets(r,h.projectDir);
  auto retained=out.ok?out.outputs:AlgorithmRunResult{};
  retained.ok=true;
  for(const auto &artifact:out.diagnostics.artifacts)retained.artifacts.append(artifact);
  auto manifest=retained.artifacts.isEmpty()?QJsonObject{}:publication_detail::artifactManifest(h.projectDir,r.operationId,
    context.privateDir,retained,&error,out.ok?std::function<bool()>(stop):std::function<bool()>{},allowed);
  if(!out.ok && !manifest.isEmpty())manifest.insert("diagnosticsOnly",true);
  if(!manifest.isEmpty() && !out.summary.value("child_operations").toArray().isEmpty())manifest.insert("childOperations",out.summary.value("child_operations"));
  if(out.ok && manifest.isEmpty()){out.ok=false;out.code=error.isEmpty()?"invalid_artifact_manifest":error;}
  for(int retry=0;retry<3;++retry) {
    auto current=log.recordById(r.operationId);
    if(current.ownerAttempt!=r.ownerAttempt || current.execution!=QLatin1String("running")){out.ok=false;out.code="stale_attempt";return out;}
    current.cancelRequested=current.cancelRequested || attempt->cancelled.load();
    current.execution=out.ok?(out.prepared.members.isEmpty()?"succeeded":"prepared"):
      current.cancelRequested?"cancelled":stop()?"interrupted":"failed";
    current.code=out.ok?"computed":current.execution=="interrupted"?"interrupted":(out.code.isEmpty()?error:out.code);
    if(out.ok || !manifest.isEmpty()){
      current.artifactManifest=manifest;current.summaryJson=QJsonDocument(out.summary).toJson(QJsonDocument::Compact);
      if(current.summaryJson.size()>16*1024)current.summaryJson=QJsonDocument(QJsonObject{{"summaryOmitted",true},{"resultsAvailable",true}}).toJson(QJsonDocument::Compact);
    }
    if(out.ok && !out.prepared.members.isEmpty())current.preparedManifest=publication_detail::preparedJson(out.prepared);
    const auto code=store(log,current,current.execution=="interrupted"?OperationLog::Transition::Reconcile:OperationLog::Transition::Advance);
    if(code==QLatin1String("ok")){attempt->workFinished=true;if(out.ok && checkpoint)checkpoint("prepared");return out;}
    if(code!=QLatin1String("operation_state_conflict")){out.ok=false;out.code=code;return out;}
  }
  out.ok=false;out.code="operation_state_conflict";return out;
}
}

PaleoTask *CapabilityRecovery::start(const QString &scope,const QString &id,QObject *subscriber,const QString &title,
    std::function<void(ExecuteResult)> finished,bool resume,QString *error,bool quiet) {
  const auto reject=[&](const QString &code)->PaleoTask*{if(error)*error=code;return nullptr;};
  if(QThread::currentThread()!=thread())return reject("owner_thread_required");
  if(subscriber && subscriber->thread()!=thread())return reject("subscriber_thread_mismatch");
  const QPointer<CapabilityRecovery> guard(this);const QPointer<QObject> listener(subscriber);
  const auto h=live();OperationLog log(h.operationDir,h.projectDir,h.writeClaim);
  auto r=reconcile(h,recovery_detail::authorized(log,h,scope,id));
  if(r.operationId.isEmpty())return reject("operation_not_found");
  if(r.planHash!=storage::operationPlanHash(r.frozenRequest,r.stagePlan,r.presentationIntent,r.externalRequest))
    return reject("frozen_plan_corrupt");
  if(!m_tasks || !subscriber)return reject("task_service_unavailable");
  const auto tasks=m_tasks;
  const auto knownResult=[&](const OperationLog::Record &record)->PaleoTask* {
    auto *created=tasks->beginExternal(title,{},quiet);const QPointer<PaleoTask> task(created);
    if(!guard){if(task)task->finishExternal("document_unavailable");return created;}
    QObject::connect(guard.data(),&QObject::destroyed,created,[task]{if(task)task->finishExternal("document_unavailable");},Qt::SingleShotConnection);
    QTimer::singleShot(0,guard.data(),[guard,listener,task,record,expected=h.context,finished=std::move(finished)] {
      auto result=recovery_detail::result(record);
      const auto code=guard?host::checkContext(guard->live().context,expected,host::Read):QStringLiteral("document_unavailable");
      if(!code.isEmpty())result=recovery_detail::invalidDelivery(std::move(result),code);
      else if(task && task->cancelRequested() && (!task->explicitCancelRequested() || result.commitState!="committed"))
        result=recovery_detail::invalidDelivery(std::move(result),task->explicitCancelRequested()?"cancelled":"stale_session");
      if(listener && finished)finished(std::move(result));if(task)task->finishExternal(code);
    });return created;
  };
  if(r.cancelRequested && r.commitState!="committed")return reject("cancelled");
  if(r.commitState==QLatin1String("committed") || r.execution==QLatin1String("succeeded")) {
    if(resume && r.commitState==QLatin1String("committed"))r=presentCommitted(h,r);
    if(!guard)return reject("document_unavailable");
    return knownResult(r);
  }
  const auto effects=recovery_detail::operationEffects(r);
  if(!effects)return reject("invalid_execution_effects");
  QString code=host::checkContext(h.context,h.context,effects);
  if(!code.isEmpty())return reject(code);
  if(r.commitState==QLatin1String("publishing"))return reject("commit_uncertain");
  if(r.execution==QLatin1String("external_unknown"))return reject("external_reconciliation_required");
  if(r.frozenRequest.value("phase").toString()==QLatin1String("prepare")) {
    r.execution="prepared";r.code="prepared";
    if(recovery_detail::store(log,r)!=QLatin1String("ok"))return reject("persist_failed");
    return knownResult(r);
  }
  const bool prepared=!r.preparedManifest.isEmpty();
  if(!prepared && r.execution!=QLatin1String("queued") && !(resume && r.execution==QLatin1String("interrupted")))return reject("explicit_resume_required");
  if(h.validateFrozen){code=h.validateFrozen(r);if(!code.isEmpty())return reject(code);}
  OperationInputValidator validator=m_runtimeHooks.value(id).inputValidator;
  if(!validator && h.restoreInputValidator)validator=h.restoreInputValidator(r,&code);
  if(!code.isEmpty())return reject(code);
  if(r.frozenRequest.value("inputValidationRequired").toBool() && !validator)return reject("input_validator_unavailable");
  OperationWorker worker=m_runtimeHooks.value(id).worker;
  const bool copySource=r.frozenRequest.value("phase").toString()==QLatin1String("publish") && !r.frozenRequest.value("sourceOperationId").toString().isEmpty();
  if(!prepared && !copySource && !worker && h.restoreWorker)worker=h.restoreWorker(r,&code);
  if(!prepared && !worker && (r.frozenRequest.value("sourceOperationId").toString().isEmpty() ||
      r.frozenRequest.value("phase").toString()!=QLatin1String("publish")))return reject(code.isEmpty()?"execution_adapter_unavailable":code);
  const auto attempt=recovery_detail::acquire(h.projectDir,id);if(!attempt)return reject("operation_running");
  attempt->contextInvalidated=m_executionInvalidated;
  if(!prepared) {
    const auto oldAttempt=r.ownerAttempt;r.ownerAttempt=QUuid::createUuid().toString(QUuid::WithoutBraces);
    r.ownerProcess=QString::number(QCoreApplication::applicationPid());r.execution="running";r.code="running";
    if(!copySource)++r.computeCount;
    const auto proposal=r;code=log.saveDurable(proposal,r.stateRevision,oldAttempt,&r,resume?OperationLog::Transition::Resume:OperationLog::Transition::Advance);
    if(code!=QLatin1String("ok")){recovery_detail::release(h.projectDir,id);return reject(code);}
  }
  if(m_checkpoint)m_checkpoint("running");
  if(!guard)return reject("document_unavailable");
  auto output=std::make_shared<OperationWorkResult>();
  auto *created=tasks->start(title,[h,r,worker,validator,attempt,output,checkpoint=m_checkpoint](PaleoTask *task) {
    const auto cancel=[attempt,task] {
      if(task->explicitCancelRequested())attempt->cancelled=true;
      else attempt->interrupted=true;
    };
    QObject::connect(task,&PaleoTask::cancelAsked,task,cancel,Qt::DirectConnection);
    if(task->cancelRequested())cancel();
    *output=recovery_detail::run(h,r,worker,validator,attempt,checkpoint);return output->ok?QString():output->code;
  },QString(),quiet);
  const QPointer<PaleoTask> task(created);
  QObject::connect(created,&PaleoTask::finished,created,[guard,root=h.projectDir,id]{
    if(!guard)recovery_detail::release(root,id);
  },Qt::SingleShotConnection);
  if(!guard){attempt->interrupted=true;return created;}
  QObject::connect(guard.data(),&QObject::destroyed,created,[attempt]{attempt->interrupted=true;},Qt::SingleShotConnection);
  auto *watch=new QTimer(guard.data());watch->setInterval(25);
  QObject::connect(watch,&QTimer::timeout,guard.data(),[guard,expected=h.context,effects,attempt,task,watch] {
    if(!guard)return;
    if(attempt->workFinished.load()){watch->stop();return;}
    const auto current=guard->live();
    if((attempt->contextInvalidated && attempt->contextInvalidated->load()) ||
        !host::checkContext(current.context,expected,effects).isEmpty()) {
      attempt->interrupted=true;watch->stop();
    }
  });
  QObject::connect(created,&PaleoTask::finished,watch,[watch]{watch->stop();watch->deleteLater();},Qt::SingleShotConnection);watch->start();
  QObject::connect(created,&QObject::destroyed,watch,[watch]{watch->stop();watch->deleteLater();},Qt::SingleShotConnection);
  QObject::connect(created,&PaleoTask::finished,guard.data(),[guard,task,host=RecoveryHost(h),r,output,lease=attempt,listener,finished=std::move(finished)]() mutable {
    // A task can be cancelled after the worker persisted prepared but before this owner callback.
    const bool cancelled=lease->cancelled.load() || (task && task->explicitCancelRequested());
    const bool interrupted=lease->interrupted.load() || (lease->contextInvalidated && lease->contextInvalidated->load()) ||
      (task && task->cancelRequested() && !cancelled);
    if(guard && cancelled)guard->cancel(r.callerScope,r.operationId);
    if(interrupted){output->ok=false;output->code="stale_session";}
    auto result=guard?guard->finish(host,r,*output):ExecuteResult{};
    if(guard) {
      auto code=host::checkContext(guard->live().context,host.context,recovery_detail::operationEffects(r));
      if(code.isEmpty() && lease->contextInvalidated && lease->contextInvalidated->load())code="stale_session";
      if(code.isEmpty() && interrupted)code="stale_session";
      const bool lateCancellation=cancelled || lease->cancelled.load() || (task && task->explicitCancelRequested());
      if(code.isEmpty() && lateCancellation && result.commitState!="committed")code="cancelled";
      if(!code.isEmpty())result=recovery_detail::invalidDelivery(std::move(result),code);
    }
    if(!result.ok && task && task->state()==PaleoTask::State::Succeeded) {
      if(result.code=="stale_session" || result.code=="cancelled")task->discardPublication();
      else task->failPublication(result.code.isEmpty()?QStringLiteral("operation_failed"):result.code);
    }
    recovery_detail::release(host.projectDir,r.operationId);
    if(guard) {
      if(result.execution=="prepared") {
        const auto hooks=guard->m_runtimeHooks.find(r.operationId);
        if(hooks!=guard->m_runtimeHooks.end()) {
          hooks->worker={};
          if(!hooks->inputValidator)guard->m_runtimeHooks.erase(hooks);
        }
      } else if(result.execution=="succeeded" || result.execution=="failed" || result.execution=="cancelled")guard->m_runtimeHooks.remove(r.operationId);
    }
    host={};lease.reset();output.reset();r={}; // retained task rows must not retain project ownership or numerical buffers.
    if(guard && listener && finished)finished(std::move(result));
    finished={};
  },Qt::SingleShotConnection);return created;
}
} // namespace paleo::capability
