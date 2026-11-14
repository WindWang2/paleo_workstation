// 层：功能
#include "wellfaciesservice.h"
#include "domain/wellfaciesrequest.h"

namespace {
bool hashShape(const QString &value) {
  if(value.size()!=64)return false;
  for(const auto &ch:value)
    if(!QStringLiteral("0123456789abcdef").contains(ch))return false;
  return true;
}
}
void WellFaciesService::predictFrozen(const WellFaciesModel &model,const QString &wellName,
    const WellFaciesInput &input,const QString &requestHash,const QByteArray &requestBody) {
  const auto readiness=paleo::wellfacies::modelReadiness(model,model.id,model.version);
  if(!readiness.isEmpty())return fail(readiness);
  if(!hashShape(requestHash)||requestBody.isEmpty()||requestBody.size()>32*1024*1024||
      !input.ready()||input.modelLabels!=model.labels||input.facies.isEmpty())
    return fail(QStringLiteral("frozen_well_input_required"));
  beginPrediction(model,wellName,input,requestHash,requestBody,true);
}
void WellFaciesService::queryFrozen(const WellFaciesModel &model,const QString &wellName,
    const WellFaciesInput &input,const QString &requestHash,const QString &remoteJobId) {
  const auto readiness=paleo::wellfacies::modelReadiness(model,model.id,model.version);
  if(!readiness.isEmpty())return fail(readiness);
  if(!hashShape(requestHash)||remoteJobId.isEmpty()||remoteJobId.size()>256||remoteJobId.contains(QChar(0))||
      !input.ready()||input.modelLabels!=model.labels||input.facies.isEmpty())
    return fail(QStringLiteral("frozen_remote_job_required"));
  const auto previous=m_accepted.value(requestHash);
  if(!previous.isEmpty()&&previous!=remoteJobId)return fail(QStringLiteral("remote_job_conflict"));
  rememberAcceptedJob(requestHash,remoteJobId);
  beginPrediction(model,wellName,input,requestHash,{},true);
}
bool WellFaciesService::observeJob(const QJsonObject &json) {
  if(json.contains("jobId")&&!json.value("jobId").isString()){fail(QStringLiteral("invalid_remote_job_id"));return false;}
  const auto id=json.value("jobId").toString();
  if(id.isEmpty())return true; // Synchronous completed responses may not allocate a remote job.
  if(id.size()>256||id.contains(QChar(0))) {fail(QStringLiteral("invalid_remote_job_id"));return false;}
  if(!m_jobId.isEmpty()&&m_jobId!=id){fail(QStringLiteral("remote_job_conflict"));return false;}
  if(m_jobId==id)return true;
  m_jobId=id;
  rememberAcceptedJob(m_key,id);
  const auto generation=m_generation;
  emit jobAccepted(m_key,id);
  return generation==m_generation; // The durable observer may cancel if recording the job failed.
}
