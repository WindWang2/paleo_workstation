// 层：功能
#include "wellfaciesagentrun.h"
#include "capabilityhost.h"
#include "services/wellfaciesinputstore.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>

namespace wf=paleo::wellfacies;
namespace cap=paleo::capability;
WellFaciesAgentRun::WellFaciesAgentRun(AiChatToolContext context,WellFaciesService *service,PaleoTaskService *tasks,
    WellFaciesConfig config,QString trustedChatSession,std::function<QString()> validate,QObject *parent)
  :QObject(parent),m_context(std::move(context)),m_config(std::move(config)),m_service(service),m_tasks(tasks),
   m_validate(std::move(validate)),m_chatSession(std::move(trustedChatSession)) {
  if(m_context.capabilityHost)m_recovery=m_context.capabilityHost->recovery();
}
QString WellFaciesAgentRun::current() const {
  if(!m_tasks||!m_recovery||!m_service)return "well_execution_host_unavailable";
  if(m_validate){const auto code=m_validate();if(!code.isEmpty())return code;}
  if(m_ownerValidation){const auto code=m_ownerValidation();if(!code.isEmpty())return code;}
  return {};
}
PaleoTask *WellFaciesAgentRun::start(const QJsonObject &args) {
  m_args=args;QString code;
  if(!wf::requestFromJson(args,m_request,&code)){finish(false,code);return nullptr;}
  if(!m_context.frozenFaciesInput){finish(false,"well_provider_unavailable");return nullptr;}
  if(!current().isEmpty()){finish(false,current());return nullptr;}
  if(m_chatSession.isEmpty()){finish(false,"caller_session_required");return nullptr;}
  m_scope=m_context.capabilityHost->callerScope(paleo::host::Caller::Agent,m_chatSession);
  if(m_scope.isEmpty()){finish(false,"caller_scope_required");return nullptr;}
  const auto endpoint=m_config.baseUrl.adjusted(QUrl::NormalizePathSegments|QUrl::StripTrailingSlash).toEncoded();
  m_endpointId=QString::fromLatin1(QCryptographicHash::hash(endpoint,QCryptographicHash::Sha256).toHex());
  if(m_endpointId!=m_request.endpointId){finish(false,"frozen_endpoint_unavailable");return nullptr;}
  m_phase=m_tasks->beginExternal(tr("测井相预测"));
  if(!current().isEmpty()){m_phase->requestCancel();finish(false,current());return m_phase;}
  connect(m_phase,&PaleoTask::cancelAsked,this,&WellFaciesAgentRun::cancel);
  connect(m_service,&WellFaciesService::completed,this,&WellFaciesAgentRun::completed);
  connect(m_service,&WellFaciesService::failed,this,[this](const QString &reason){
    if(m_resultRecoveryOnly)resultUnavailable(reason);
    else if(m_externalActive)unknown(reason);
    else finish(false,m_remoteFailed?"remote_failed":"well_service_unavailable",{{"error",reason},
      {"childOperationId",m_remoteFailed?m_segment.record.operationId:QString()}});
  });
  connect(m_service,&WellFaciesService::remoteFailed,this,[this](const QString &hash,const QString &id) {
    if(m_resultRecoveryOnly){m_service->cancel();resultUnavailable("remote_completion_changed");return;}
    if(!m_externalActive||hash!=m_input->requestHashes.value(m_index)||id!=m_remoteJobId) {
      m_service->cancel();unknown("remote_job_conflict");return;
    }
    const auto code=m_recovery->reportExternal(m_scope,m_segment.record.operationId,id,"failed");
    if(code!="ok"){m_service->cancel();unknown(code);return;}
    m_externalActive=false;m_remoteFailed=true;
  });
  connect(m_service,&WellFaciesService::requestSending,this,[this](const QString &hash){
    const auto parent=m_recovery->lookup(m_scope,m_request.clientRequestId);
    if(parent.code!="found"||parent.record.operationId!=m_parentId||parent.record.cancelRequested) {
      m_parentCancelled=parent.record.cancelRequested;m_service->cancel();unknown("cancelled_before_external_post");return;
    }
    if(!m_send||!m_input||hash!=m_input->requestHashes.value(m_index)||!current().isEmpty()) {
      m_service->cancel();unknown("external_send_guard_failed");
    }
  });
  connect(m_service,&WellFaciesService::jobAccepted,this,[this](const QString &hash,const QString &id){
    if(!m_externalActive||hash!=m_input->requestHashes.value(m_index)){m_service->cancel();unknown("remote_job_conflict");return;}
    m_remoteJobId=id;
    const auto code=m_recovery->noteRemoteAccepted(m_scope,m_segment.record.operationId,id);
    if(code!="ok"){m_service->cancel();unknown(code);return;}
  });
  const auto prior=m_recovery->lookup(m_scope,m_request.clientRequestId);
  if(prior.code=="found") {
    if(prior.record.requestHash!=cap::operationDigest(args)){finish(false,"idempotency_conflict");return m_phase;}
    m_parentId=prior.record.operationId;m_parentPlan=prior.record.frozenRequest.value("plan").toObject();
    m_parentCancelled=prior.record.cancelRequested;
    if(m_parentPlan.value("endpointId").toString()!=m_endpointId){finish(false,"frozen_endpoint_unavailable");return m_phase;}
    restore(m_parentPlan.value("storedInput").toObject());
  }else if(prior.code=="not_found")resolveModel();
  else finish(false,prior.code);
  return m_phase;
}
void WellFaciesAgentRun::resolveModel() {
  connect(m_service,&WellFaciesService::modelsReady,this,[this](const QVector<WellFaciesModel> &models){
    if(m_done)return;
    const WellFaciesModel *chosen=nullptr;
    for(const auto &model:models)if(model.id==m_request.modelId) {
      if(chosen){finish(false,"ambiguous_model_version_id");return;}chosen=&model;
    }
    if(!chosen||chosen->version!=m_request.modelVersion){finish(false,"exact_model_unavailable");return;}
    const auto code=wf::modelReadiness(*chosen,m_request.modelId,m_request.modelVersion);
    if(!code.isEmpty()){finish(false,code);return;}
    m_modelVerified=true;
    prepare(*chosen);
  });
  m_service->fetchModelContracts();
}
void WellFaciesAgentRun::verifyStoredModel() {
  connect(m_service,&WellFaciesService::modelsReady,this,[this](const QVector<WellFaciesModel> &models) {
    if(m_done)return;const WellFaciesModel *actual=nullptr;
    for(const auto &model:models)if(model.id==m_input->model.id) {
      if(actual){finish(false,"ambiguous_model_version_id");return;}actual=&model;
    }
    if(!actual||!wf::modelReadiness(*actual,m_input->model.id,m_input->model.version).isEmpty()||
       wf::frozenModelJson(*actual)!=wf::frozenModelJson(m_input->model)) {
      finish(false,"frozen_model_contract_changed");return;
    }
    m_modelVerified=true;next();
  },Qt::SingleShotConnection);
  m_service->fetchModelContracts();
}
void WellFaciesAgentRun::prepare(const WellFaciesModel &model) {
  if(!current().isEmpty()){finish(false,current());return;}
  AiChatToolRunner::FrozenFaciesInput provided;QString code;
  if(!m_context.frozenFaciesInput(m_request,model,provided,code)){finish(false,code);return;}
  m_ownerValidation=provided.ownerValidation;
  if(!current().isEmpty()){finish(false,current());return;}
  auto input=std::make_shared<wf::PreparedInput>();auto ref=std::make_shared<QJsonObject>();
  const auto project=provided.job.projectDir;
  m_projectDir=project;
  m_worker=m_tasks->start(tr("准备冻结井曲线"),[job=provided.job,input,ref,project](PaleoTask *task){
    const auto cancelled=[task]{return task->cancelRequested();};
    *input=wf::prepareInput(job,cancelled);if(!input->ok)return input->code;
    QString error;*ref=wf::storeInput(project,*input,error,cancelled);return error;
  });
  if(m_done||!current().isEmpty()){m_worker->requestCancel();if(!m_done)finish(false,current());return;}
  connect(m_worker,&PaleoTask::finished,this,[this,input,ref]{
    if(m_done)return;
    if(m_worker->state()!=PaleoTask::State::Succeeded){finish(false,m_worker->errorText());return;}
    inputReady(input,*ref,false);
  });
}
void WellFaciesAgentRun::restore(const QJsonObject &reference) {
  WellFaciesModel frozen;
  if(!wf::frozenModelFromJson(m_parentPlan.value("sourceManifest").toObject().value("model").toObject(),frozen)) {
    finish(false,"invalid_frozen_model");return;
  }
  AiChatToolRunner::FrozenFaciesInput provided;QString code;
  if(!m_context.frozenFaciesInput(m_request,frozen,provided,code)){finish(false,code);return;}
  m_ownerValidation=provided.ownerValidation;
  if(!current().isEmpty()){finish(false,current());return;}
  auto input=std::make_shared<wf::PreparedInput>();const auto project=provided.job.projectDir;
  m_projectDir=project;
  m_worker=m_tasks->start(tr("恢复冻结井曲线"),[input,reference,project](PaleoTask *task){
    *input=wf::loadInput(project,reference,[task]{return task->cancelRequested();});return input->ok?QString():input->code;
  });
  if(m_done||!current().isEmpty()){m_worker->requestCancel();if(!m_done)finish(false,current());return;}
  connect(m_worker,&PaleoTask::finished,this,[this,input,reference]{
    if(m_done)return;
    if(m_worker->state()!=PaleoTask::State::Succeeded){finish(false,m_worker->errorText());return;}
    if(input->sourceManifest!=m_parentPlan.value("sourceManifest").toObject()){finish(false,"stored_input_plan_mismatch");return;}
    inputReady(input,reference,true);
  });
}
void WellFaciesAgentRun::inputReady(std::shared_ptr<wf::PreparedInput> input,const QJsonObject &reference,bool replay) {
  if(!current().isEmpty()){finish(false,current());return;}
  m_input=std::move(input);
  if(!replay) {
    m_parentPlan={{"storedInput",reference},{"endpointId",m_endpointId},{"sourceManifest",m_input->sourceManifest}};
    cap::FrozenOperation plan;plan.capabilityId="wellfacies.predict";plan.contractVersion=2;
    plan.implementationVersion="well-facies-agent-v2";plan.callerScope=m_scope;
    plan.clientRequestId=m_request.clientRequestId;plan.intent=m_args;plan.frozenPlan=m_parentPlan;plan.phase="prepare";
    const auto accepted=m_recovery->accept(plan);
    if(!accepted.ok){finish(false,accepted.code);return;}m_parentId=accepted.record.operationId;
    if(accepted.code=="replay"&&accepted.record.frozenRequest.value("plan").toObject()!=m_parentPlan) {
      finish(false,"concurrent_input_preparation");return;
    }
  }
  next();
}
void WellFaciesAgentRun::cancel() {
  if(m_done)return;
  if(m_resultReceived) {
    // Remote completion is already known. Keep its receipt/artifact work alive under the recovery owner.
    m_cancelAfterResult=true;m_ignoreService=true;if(m_service)m_service->disconnect(this);return;
  }
  if(m_resultRecoveryOnly) {
    if(m_worker&&m_worker->running())m_worker->requestCancel();
    if(m_service)m_service->cancel();resultUnavailable("cancelled");return;
  }
  if(m_worker&&m_worker->running())m_worker->requestCancel();
  if(m_service)m_service->cancel();
  if(m_externalActive)unknown("cancelled_after_external_send");else finish(false,"cancelled");
}
void WellFaciesAgentRun::unknown(const QString &reason) {
  if(m_resultRecoveryOnly){resultUnavailable(reason);return;}
  QString persistence;
  if(m_externalActive&&m_recovery)persistence=m_recovery->reportExternal(m_scope,m_segment.record.operationId,m_remoteJobId,"unknown");
  m_externalActive=false;
  finish(false,"ExternalOutcomeUnknown",{{"reason",reason},{"childOperationId",m_segment.record.operationId},
    {"remoteJobId",m_remoteJobId},{"recording",persistence},{"retryAdvice","query_original_job_only"}});
}
void WellFaciesAgentRun::finish(bool ok,const QString &code,const QJsonObject &extra) {
  if(m_done)return;m_done=true;
  if(m_service){m_service->disconnect(this);if(!m_ignoreService)m_service->cancel();}
  if(m_phase&&m_phase->running())m_phase->finishExternal(ok?QString():code);
  QJsonObject result=extra;result.insert("contract","paleo.well-facies.result.v2");
  result.insert("code",code);result.insert("inputOperationId",m_parentId);result.insert("segments",m_results);
  result.insert("executionScope","batch_summary");
  if(!m_parentId.isEmpty())result.insert("inputOperation",QJsonObject{{"operationId",m_parentId},
    {"role","frozen_input_snapshot"},{"execution","prepared"},{"cancelRequested",m_parentCancelled}});
  result.insert("status",ok?"succeeded":code=="ExternalOutcomeUnknown"?"external_unknown":
      code=="remote_succeeded_result_unavailable"?"result_unavailable":"failed");
  result.insert("execution",ok?"succeeded":code=="ExternalOutcomeUnknown"?"external_unknown":
      code=="remote_succeeded_result_unavailable"||!m_results.isEmpty()?"partial":"failed");
  if(m_input){result.insert("inputHash",m_input->inputHash);result.insert("featureSchemaHash",m_input->featureSchemaHash);
    int derived=0;for(const auto &window:m_input->occurrences)if(window.derivedDivisionGeometry)++derived;
    int completed=0;for(const auto &child:m_results)if(child.toObject().value("execution")=="succeeded")++completed;
    result.insert("coverage",QJsonObject{{"scope",m_input->sourceManifest.value("coverage")},
      {"occurrenceCount",m_input->sourceManifest.value("occurrences").toArray().size()},
      {"expectedSegmentCount",m_input->segments.size()},{"resultSegmentCount",completed},
      {"complete",ok&&completed==m_input->segments.size()&&m_input->skipped.isEmpty()},
      {"inputPartial",!m_input->skipped.isEmpty()},
      {"skipped",QJsonArray::fromStringList(m_input->skipped)},{"derivedDivisionWindows",derived}});}
  emit finished(ok,result);deleteLater();
}
