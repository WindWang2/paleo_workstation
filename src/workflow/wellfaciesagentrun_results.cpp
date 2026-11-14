// 层：功能
#include "wellfaciesagentrun.h"
#include "services/wellfaciesinputstore.h"
#include <QJsonDocument>
#include <QJsonArray>

void WellFaciesAgentRun::resultUnavailable(const QString &reason) {
  if(m_recovery)m_recovery->reportExternal(m_scope,m_segment.record.operationId,m_remoteJobId,"succeeded",
    m_segment.record.remoteCompletionProof,false);
  m_externalActive=false;
  finish(false,"remote_succeeded_result_unavailable",{{"reason",reason},{"remoteOutcome","succeeded"},{"resultAvailability","unavailable"},
    {"childOperationId",m_segment.record.operationId},{"retryAdvice","recover_original_result_only"}});
}
void WellFaciesAgentRun::restoreCompletedResult() {
  const auto retained=m_recovery->reportExternal(m_scope,m_segment.record.operationId,m_remoteJobId,"succeeded",
    m_segment.record.remoteCompletionProof,true);
  if(retained!="ok"){resultUnavailable(retained);return;}
  m_resultRecoveryOnly=true;
  auto bytes=std::make_shared<QByteArray>();auto error=std::make_shared<QString>();
  const auto project=m_projectDir,hash=m_segment.record.remoteCompletionProof.value("responseHash").toString();
  const auto reference=m_parentPlan.value("storedInput").toObject();const auto index=m_index;
  m_worker=m_tasks->start(tr("恢复已完成测井相结果"),[bytes,error,project,hash,reference,index](PaleoTask *task) {
    if(task->cancelRequested())return QStringLiteral("cancelled");
    *bytes=paleo::wellfacies::loadResultCache(project,reference,index,hash,*error);return QString();
  });
  if(m_done){m_worker->requestCancel();return;}
  connect(m_worker,&PaleoTask::finished,this,[this,bytes,error] {
    if(m_done)return;
    if(!bytes->isEmpty()) {
      const auto result=QJsonDocument::fromJson(*bytes).object();
      if(result.value("contract")!="paleo.well-facies.output.v2"||result.value("inputHash").toString()!=m_input->inputHash||
         result.value("requestHash").toString()!=m_input->requestHashes[m_index]){resultUnavailable("external_result_cache_mismatch");return;}
      m_resultReceived=true;setParent(m_recovery);attachBytes(*bytes,result.value("intervals").toArray().size());return;
    }
    if(m_remoteJobId.isEmpty()){resultUnavailable(*error);return;}
    if(!current().isEmpty()){resultUnavailable(current());return;}
    m_externalActive=true;m_send=false;
    m_service->queryFrozen(m_input->model,m_input->wellName,m_input->segments[m_index],m_input->requestHashes[m_index],m_remoteJobId);
  });
}
