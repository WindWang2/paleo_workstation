// 层：功能
// workflow/capabilityrecovery_worker.cpp — 数值/拷贝/校验只在池内；Catalog 只在 owner 提交。
#include "capabilityrecovery_internal.h"
#include "capabilitypublication_internal.h"
#include "services/paleotaskservice.h"
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QUuid>
#include <QTimer>
namespace paleo::capability {
namespace recovery_detail {
OperationWorkResult run(const RecoveryHost &h,OperationLog::Record r,OperationWorker worker,OperationInputValidator validator,
    const std::shared_ptr<ActiveAttempt> &attempt,const std::function<void(const QString&)> &checkpoint) {
  OperationWorkResult out; QString error;
  const auto stop=[attempt]{return attempt->cancelled.load();};
  OperationLog log(h.operationDir,h.projectDir,h.writeClaim);
  const auto fail=[&](const QString &code) {
    OperationWorkResult failed;failed.code=code;auto current=log.recordById(r.operationId);
    if(current.ownerAttempt==r.ownerAttempt && current.execution==QLatin1String("running")) {
      current.execution=stop()?"cancelled":"failed";current.code=code;store(log,current);
    }
    return failed;
  };
  OperationWorkContext context;context.operationId=r.operationId;context.attemptId=r.ownerAttempt;
  context.projectDir=h.projectDir;
  context.frozenPlan=r.frozenRequest.value("plan").toObject();context.cancelled=stop;
  context.privateDir=QDir(h.operationDir).filePath(r.operationId+'/'+r.ownerAttempt);
  if(!QDir().mkpath(context.privateDir) || !publication_detail::specFromJson(r.stagePlan.value("outputSpec").toObject(),context.outputSpec) ||
      !publication_detail::targetsFromJson(h.projectDir,r.stagePlan,context.targets)) return fail("invalid_frozen_plan");
  if(validator){const auto code=validator(context);if(!code.isEmpty())return fail(code);}
  if(!r.preparedManifest.isEmpty()) {
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
    out.ok=true;
  } else if(worker)out=worker(context);else return fail("execution_adapter_unavailable");
  if(out.ok && stop()){out.ok=false;out.code="cancelled";}
  if(out.ok && r.frozenRequest.value("phase").toString()==QLatin1String("publish")) {
    out.ok=prepareAlgorithmOutputSet(context.outputSpec,out.outputs,context.targets,out.prepared,&error,stop);
    if(out.ok) {
      auto &p=out.prepared;p.publicationSchema=2;p.id=r.stagePlan.value("outputSetId").toString();
      p.documentId=r.documentId;p.operationId=r.operationId;p.requestHash=r.requestHash;p.planHash=r.planHash;p.storeGeneration=r.storeGeneration;
      out.outputs=publication_detail::preparedOutputs(p);
    }
  }
  const auto allowed=managedTargets(r,h.projectDir);
  const auto manifest=out.ok?publication_detail::artifactManifest(h.projectDir,r.operationId,context.privateDir,out.outputs,&error,stop,allowed):QJsonObject{};
  if(out.ok && manifest.isEmpty()){out.ok=false;out.code=error.isEmpty()?"invalid_artifact_manifest":error;}
  for(int retry=0;retry<3;++retry) {
    auto current=log.recordById(r.operationId);
    if(current.ownerAttempt!=r.ownerAttempt || current.execution!=QLatin1String("running")){out.ok=false;out.code="stale_attempt";return out;}
    current.cancelRequested=current.cancelRequested || stop();
    current.execution=out.ok?(out.prepared.members.isEmpty()?"succeeded":"prepared"):(current.cancelRequested?"cancelled":"failed");
    current.code=out.ok?"computed":(out.code.isEmpty()?error:out.code);
    if(out.ok){
      current.artifactManifest=manifest;current.summaryJson=QJsonDocument(out.summary).toJson(QJsonDocument::Compact);
      if(current.summaryJson.size()>16*1024)current.summaryJson=QJsonDocument(QJsonObject{{"summaryOmitted",true},{"resultsAvailable",true}}).toJson(QJsonDocument::Compact);
    }
    if(out.ok && !out.prepared.members.isEmpty())current.preparedManifest=publication_detail::preparedJson(out.prepared);
    const auto code=store(log,current);
    if(code==QLatin1String("ok")){if(out.ok && checkpoint)checkpoint("prepared");return out;}
    if(code!=QLatin1String("operation_state_conflict")){out.ok=false;out.code=code;return out;}
  }
  out.ok=false;out.code="operation_state_conflict";return out;
}
}

PaleoTask *CapabilityRecovery::start(const QString &scope,const QString &id,QObject *subscriber,const QString &title,
    std::function<void(ExecuteResult)> finished,bool resume,QString *error) {
  const auto h=live();OperationLog log(h.operationDir,h.projectDir,h.writeClaim);
  auto r=reconcile(h,recovery_detail::authorized(log,h,scope,id));
  const auto reject=[&](const QString &code)->PaleoTask*{if(error)*error=code;return nullptr;};
  if(r.operationId.isEmpty())return reject("operation_not_found");
  if(r.planHash!=operationDigest({{"frozen",r.frozenRequest},{"stage",r.stagePlan},{"presentation",r.presentationIntent},{"external",r.externalRequest}}))
    return reject("frozen_plan_corrupt");
  if(!m_tasks || !subscriber)return reject("task_service_unavailable");
  if(r.commitState==QLatin1String("committed") || r.execution==QLatin1String("succeeded")) {
    if(resume && r.commitState==QLatin1String("committed"))r=presentCommitted(h,r);
    auto *task=m_tasks->beginExternal(title);const QPointer<QObject> listener(subscriber);
    QTimer::singleShot(0,this,[task,r,listener,finished=std::move(finished)] {
      if(listener && finished)finished(recovery_detail::result(r));task->finishExternal();
    });return task;
  }
  QString code=host::checkContext(h.context,h.context,host::computeEffects(r.frozenRequest.value("phase").toString()));
  if(!code.isEmpty())return reject(code);
  if(r.frozenRequest.value("phase").toString()==QLatin1String("prepare")) {
    r.execution="prepared";r.code="prepared";
    if(recovery_detail::store(log,r)!=QLatin1String("ok"))return reject("persist_failed");
    auto *task=m_tasks->beginExternal(title);const QPointer<QObject> listener(subscriber);
    QTimer::singleShot(0,this,[task,r,listener,finished=std::move(finished)] {
      if(listener && finished)finished(recovery_detail::result(r));task->finishExternal();
    });return task;
  }
  if(r.commitState==QLatin1String("publishing"))return reject("commit_uncertain");
  if(r.execution==QLatin1String("external_unknown"))return reject("external_reconciliation_required");
  if(r.cancelRequested)return reject("cancelled");
  const bool prepared=!r.preparedManifest.isEmpty();
  if(!prepared && r.execution!=QLatin1String("queued") && !(resume && r.execution==QLatin1String("interrupted")))return reject("explicit_resume_required");
  if(h.validateFrozen){code=h.validateFrozen(r);if(!code.isEmpty())return reject(code);}
  OperationInputValidator validator=m_frozen.value(id).inputValidator;
  if(!validator && h.restoreInputValidator)validator=h.restoreInputValidator(r,&code);
  if(!code.isEmpty())return reject(code);
  if(r.frozenRequest.value("inputValidationRequired").toBool() && !validator)return reject("input_validator_unavailable");
  OperationWorker worker=m_frozen.value(id).worker;
  const bool copySource=r.frozenRequest.value("phase").toString()==QLatin1String("publish") && !r.frozenRequest.value("sourceOperationId").toString().isEmpty();
  if(!prepared && !copySource && !worker && h.restoreWorker)worker=h.restoreWorker(r,&code);
  if(!prepared && !worker && (r.frozenRequest.value("sourceOperationId").toString().isEmpty() ||
      r.frozenRequest.value("phase").toString()!=QLatin1String("publish")))return reject(code.isEmpty()?"execution_adapter_unavailable":code);
  const auto attempt=recovery_detail::acquire(h.projectDir,id);if(!attempt)return reject("operation_running");
  if(!prepared) {
    const auto oldAttempt=r.ownerAttempt;r.ownerAttempt=QUuid::createUuid().toString(QUuid::WithoutBraces);
    r.ownerProcess=QString::number(QCoreApplication::applicationPid());r.execution="running";r.code="running";
    if(!copySource)++r.computeCount;
    const auto proposal=r;code=log.saveDurable(proposal,r.stateRevision,oldAttempt,&r,resume?OperationLog::Transition::Resume:OperationLog::Transition::Advance);
    if(code!=QLatin1String("ok")){recovery_detail::release(h.projectDir,id);return reject(code);}
  }
  if(m_checkpoint)m_checkpoint("running");
  auto output=std::make_shared<OperationWorkResult>();const QPointer<CapabilityRecovery> guard(this);const QPointer<QObject> listener(subscriber);
  auto *task=m_tasks->start(title,[h,r,worker,validator,attempt,output,checkpoint=m_checkpoint](PaleoTask *task) {
    QObject::connect(task,&PaleoTask::cancelAsked,task,[attempt]{attempt->cancelled=true;},Qt::DirectConnection);
    if(task->cancelRequested())attempt->cancelled=true;
    *output=recovery_detail::run(h,r,worker,validator,attempt,checkpoint);return output->ok?QString():output->code;
  });
  QObject::connect(task,&PaleoTask::finished,this,[guard,host=RecoveryHost(h),r,output,lease=attempt,listener,finished=std::move(finished)]() mutable {
    auto result=guard?guard->finish(host,r,*output):ExecuteResult{};
    recovery_detail::release(host.projectDir,r.operationId);
    if(guard) {
      if(result.execution=="prepared")guard->m_frozen[r.operationId].worker={};
      else if(result.execution=="succeeded" || result.execution=="failed" || result.execution=="cancelled")guard->m_frozen.remove(r.operationId);
    }
    host={};lease.reset();output.reset();r={}; // retained task rows must not retain project ownership or numerical buffers.
    if(listener && finished)finished(std::move(result));
    finished={};
  });return task;
}
} // namespace paleo::capability
