// 层：功能
// workflow/capabilityrecovery_external.cpp — POST 许可在日志之后；不确定结果只按原远程任务核对。
#include "capabilityrecovery_internal.h"
#include <QCoreApplication>
#include <QUuid>
#include <QRegularExpression>
#include "capabilitypublication_internal.h"
#include "services/paleotaskservice.h"
#include <QDir>
namespace paleo::capability {
CapabilityRecovery::Accepted CapabilityRecovery::beginExternal(const FrozenOperation &plan) {
  if(plan.externalRequest.isEmpty())return {false,"external_request_required",{}};
  auto accepted=accept(plan);if(!accepted.ok)return accepted;
  auto &r=accepted.record;
  if(r.execution==QLatin1String("succeeded") || r.execution==QLatin1String("failed")){accepted.code="Terminal";return accepted;}
  const auto h=live();
  const auto permitted=host::checkContext(h.context,h.context,host::Read|host::PrivateStage|host::ExternalTransfer);
  if(!permitted.isEmpty())return {false,permitted,r};
  if(!r.remoteJobId.isEmpty()){accepted.code="Query";return accepted;}
  if(r.execution!=QLatin1String("queued")){accepted.code="Unknown";return accepted;}
  OperationLog log(h.operationDir,h.projectDir,h.writeClaim);
  const auto before=r;r.execution="running";r.code="external_sending";
  r.ownerAttempt=QUuid::createUuid().toString(QUuid::WithoutBraces);r.ownerProcess=QString::number(QCoreApplication::applicationPid());++r.computeCount;
  const auto code=log.saveDurable(r,before.stateRevision,before.ownerAttempt,&r);
  if(code==QLatin1String("ok")) {
    if(auto active=recovery_detail::acquire(h.projectDir,r.operationId)){
      active->claim=h.writeClaim;m_externalActive.insert(r.operationId,active);m_externalHosts.insert(r.operationId,h);
    }
  }
  accepted.ok=code==QLatin1String("ok");accepted.code=accepted.ok?"Send":code;return accepted;
}
QString CapabilityRecovery::noteRemoteAccepted(const QString &scope,const QString &id,const QString &jobId) {
  const auto h=m_externalHosts.contains(id)?m_externalHosts.value(id):live();OperationLog log(h.operationDir,h.projectDir,h.writeClaim);
  auto r=recovery_detail::authorized(log,h,scope,id);
  if(r.operationId.isEmpty())return "operation_not_found";
  if(r.externalRequest.isEmpty() || jobId.isEmpty() || jobId.toUtf8().size()>1024)return "invalid_remote_job";
  if(!r.remoteJobId.isEmpty())return r.remoteJobId==jobId?"ok":"remote_job_conflict";
  if(r.execution!=QLatin1String("running") && r.execution!=QLatin1String("external_unknown"))return "invalid_external_state";
  r.remoteJobId=jobId;r.code="remote_accepted";return recovery_detail::store(log,r);
}
QString CapabilityRecovery::reportExternal(const QString &scope,const QString &id,const QString &jobId,const QString &outcome,const QJsonObject &proof,
                                           bool retainOwnershipForResults) {
  const auto h=m_externalHosts.contains(id)?m_externalHosts.value(id):live();OperationLog log(h.operationDir,h.projectDir,h.writeClaim);
  auto r=recovery_detail::authorized(log,h,scope,id);
  if(r.operationId.isEmpty())return "operation_not_found";
  if(r.externalRequest.isEmpty() || outcome!="succeeded" && outcome!="failed" && outcome!="unknown")return "invalid_external_outcome";
  if(!jobId.isEmpty() && !r.remoteJobId.isEmpty() && jobId!=r.remoteJobId)return "remote_job_conflict";
  if(!proof.isEmpty()) {
    static const QRegularExpression sha("^[0-9a-f]{64}$");
    if(outcome!="succeeded" || proof.size()!=3 ||
        proof.value("requestHash")!=r.externalRequest.value("requestHash") || !sha.match(proof.value("requestHash").toString()).hasMatch() ||
        !sha.match(proof.value("responseHash").toString()).hasMatch() || proof.value("modelVersion").toString().isEmpty() ||
        proof.value("modelVersion")!=r.externalRequest.value("modelVersion"))return "remote_job_conflict";
    if(!r.remoteCompletionProof.isEmpty() && r.remoteCompletionProof!=proof)return "external_completion_conflict";
  }
  if(r.execution==QLatin1String("succeeded") || r.execution==QLatin1String("failed")) {
    if(r.externalOutcome!=outcome)return "external_outcome_conflict";
    if(!proof.isEmpty() && r.remoteCompletionProof.isEmpty()) {
      r.remoteCompletionProof=proof;const auto saved=recovery_detail::store(log,r);if(saved!="ok")return saved;
    }
    if(outcome=="succeeded" && retainOwnershipForResults && r.artifactManifest.isEmpty()) {
      const auto allowed=host::checkContext(h.context,h.context,host::Read|host::PrivateStage);
      if(!allowed.isEmpty())return allowed;
      if(!log.ownsWrites())return "project_write_ownership_required";
      m_externalHosts.insert(id,h);
    }
    return "ok";
  }
  if(outcome=="unknown" && r.remoteJobId.isEmpty() && !jobId.isEmpty() && jobId.toUtf8().size()<=1024)r.remoteJobId=jobId;
  if(outcome!="unknown" && (jobId.isEmpty() || jobId!=r.remoteJobId) &&
      (outcome!="succeeded" || r.execution!="running" || !r.remoteJobId.isEmpty() || !jobId.isEmpty() || proof.isEmpty()))return "remote_job_conflict";
  if(!proof.isEmpty())r.remoteCompletionProof=proof;
  r.externalOutcome=outcome;r.execution=outcome=="unknown"?"external_unknown":outcome;
  r.code=outcome=="unknown"?"external_outcome_unknown":"remote_"+outcome;
  const auto code=recovery_detail::store(log,r,OperationLog::Transition::Reconcile);
  if(code==QLatin1String("ok")){
    m_externalActive.remove(id);
    if(outcome!="succeeded" || !retainOwnershipForResults || !r.artifactManifest.isEmpty())m_externalHosts.remove(id);
  }
  return code;
}
PaleoTask *CapabilityRecovery::attachExternalResults(const QString &scope,const QString &id,OperationWorker worker,
    QObject *subscriber,std::function<void(ExecuteResult)> finished,QString *error) {
  const auto h=m_externalHosts.contains(id)?m_externalHosts.value(id):live();OperationLog log(h.operationDir,h.projectDir,h.writeClaim);
  const auto r=recovery_detail::authorized(log,h,scope,id);
  const auto reject=[&](const QString &code)->PaleoTask*{if(error)*error=code;return nullptr;};
  if(r.operationId.isEmpty())return reject("operation_not_found");
  if(r.externalRequest.isEmpty() || r.execution!="succeeded" || !worker)return reject("external_result_not_ready");
  if(!log.ownsWrites() || !m_tasks || !subscriber)return reject("project_write_ownership_required");
  if(!r.artifactManifest.isEmpty())return reject("external_result_already_attached");
  auto manifest=std::make_shared<QJsonObject>();auto code=std::make_shared<QString>();
  auto *task=m_tasks->start("Store external result",[h,r,worker,manifest,code](PaleoTask *task) {
    OperationWorkContext context;context.operationId=r.operationId;context.attemptId=r.ownerAttempt;
    context.projectDir=h.projectDir;
    context.privateDir=QDir(h.operationDir).filePath(r.operationId+'/'+r.ownerAttempt+"/result-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    context.frozenPlan=r.frozenRequest.value("plan").toObject();context.cancelled=[task]{return task->cancelRequested();};
    if(!QDir().mkpath(context.privateDir)){*code="stage_failed";return *code;}
    const auto result=worker(context);if(!result.ok){*code=result.code;return *code;}
    *manifest=publication_detail::artifactManifest(h.projectDir,r.operationId,context.privateDir,result.outputs,code.get(),context.cancelled);
    if(manifest->isEmpty() && code->isEmpty())*code="invalid_artifact_manifest";return *code;
  });
  const QPointer<QObject> listener(subscriber);
  QObject::connect(task,&PaleoTask::finished,this,[this,host=RecoveryHost(h),r,manifest,code,listener,finished=std::move(finished)]() mutable {
    OperationLog journal(host.operationDir,host.projectDir,host.writeClaim);auto current=journal.recordById(r.operationId);
    if(code->isEmpty() && !manifest->isEmpty()) {
      current.artifactManifest=*manifest;*code=recovery_detail::store(journal,current);
      if(*code=="ok"){m_externalHosts.remove(r.operationId);m_externalActive.remove(r.operationId);}
    }
    auto result=recovery_detail::result(current);if(!code->isEmpty() && *code!="ok"){result.ok=false;result.code=*code;}
    host={};manifest.reset();code.reset();
    if(listener && finished)finished(std::move(result));
    finished={};
  });return task;
}
} // namespace paleo::capability
