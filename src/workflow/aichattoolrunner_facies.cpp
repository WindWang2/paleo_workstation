// 层：功能
#include "aichattoolrunner.h"
#include "wellfaciesagentrun.h"
#include "wellfaciesreplay.h"
#include "capabilityhost.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>

QString AiChatToolRunner::wellFaciesReadiness(const QJsonObject &args,bool forListing) const {
  if(!forListing) {
    QString code;const auto replay=paleo::wellfacies::completedReplay(m_context.capabilityHost,m_resultSession,args,code);
    if(!code.isEmpty()||!replay.isEmpty())return code;
  }
  const auto endpoint=WellFaciesConfig::validateUrl(m_faciesConfig.baseUrl,m_faciesConfig.allowInsecureHttp);
  if(!endpoint.isEmpty()||m_faciesConfig.apiKey.trimmed().isEmpty()||m_faciesConfig.apiKey.contains('\r')||m_faciesConfig.apiKey.contains('\n'))
    return "well_endpoint_unavailable";
  if(!m_taskService)return "task_service_unavailable";
  if(!m_context.frozenFaciesInput)return "well_provider_unavailable";
  if(!m_context.capabilityHost||!m_context.capabilityHost->recovery())return "well_execution_host_unavailable";
  const auto context=m_context.capabilityHost->context();
  const auto code=paleo::host::checkContext(context,context,paleo::host::Read|paleo::host::PrivateStage|paleo::host::ExternalTransfer);
  if(!code.isEmpty())return code;
  if(!forListing){paleo::wellfacies::Request request;QString invalid;
    if(!paleo::wellfacies::requestFromJson(args,request,&invalid))return invalid;
    const auto endpoint=m_faciesConfig.baseUrl.adjusted(QUrl::NormalizePathSegments|QUrl::StripTrailingSlash).toEncoded();
    if(request.endpointId!=QString::fromLatin1(QCryptographicHash::hash(endpoint,QCryptographicHash::Sha256).toHex()))
      return "frozen_endpoint_unavailable";}
  return {};
}

void AiChatToolRunner::runFacies(const QJsonObject &args) {
  QString code;const auto replay=paleo::wellfacies::completedReplay(m_context.capabilityHost,m_resultSession,args,code);
  if(!code.isEmpty()){finishActive(false,{{"code",code}});return;}
  if(!replay.isEmpty()){finishActive(true,replay);return;}
  const auto readiness=wellFaciesReadiness(args,false);
  if(!readiness.isEmpty()){finishActive(false,{{"code",readiness}});return;}
  m_activeIsFacies=true;const auto generation=m_generation;
  const auto faciesEpoch=m_faciesEpoch;
  const auto chatSession=m_resultSession;
  const QPointer<AiChatToolRunner> guarded=this;
  auto *run=new WellFaciesAgentRun(m_context,m_facies,m_taskService,m_faciesConfig,chatSession,[guarded,generation,chatSession,faciesEpoch] {
    return guarded&&generation==guarded->m_generation&&faciesEpoch==guarded->m_faciesEpoch&&
      guarded->activeContextIsCurrent()&&chatSession==guarded->m_resultSession?
      QString():QStringLiteral("stale_session");
  },this);
  connect(run,&WellFaciesAgentRun::finished,this,[this,generation](bool ok,const QJsonObject &payload) {
    if(generation==m_generation&&m_active)finishActive(ok,payload);
  });
  auto *phase=run->start(args);
  if(generation!=m_generation){if(phase&&phase->running())phase->requestCancel();return;}
  m_activeTask=phase;
  if(m_activeTask&&!activeContextIsCurrent())m_activeTask->requestCancel();
}
void AiChatToolRunner::runFaciesModels(const QJsonObject &args) {
  const auto endpointError=WellFaciesConfig::validateUrl(m_faciesConfig.baseUrl,m_faciesConfig.allowInsecureHttp);
  if(!endpointError.isEmpty()||m_faciesConfig.apiKey.trimmed().isEmpty()) {
    finishActive(false,{{"code","well_endpoint_unavailable"}});return;
  }
  if(!m_taskService||!m_context.capabilityHost){finishActive(false,{{"code","well_execution_host_unavailable"}});return;}
  const auto expected=m_context.capabilityHost->context();
  const auto policy=paleo::host::checkContext(expected,expected,paleo::host::Read|paleo::host::ExternalTransfer);
  if(!policy.isEmpty()){finishActive(false,{{"code",policy}});return;}
  m_activeIsFacies=true;const auto generation=m_generation;const auto faciesEpoch=m_faciesEpoch;
  auto *phase=m_taskService->beginExternal(tr("查询测井相模型契约"));
  if(generation!=m_generation){phase->requestCancel();phase->finishExternal("cancelled");return;}
  if(faciesEpoch!=m_faciesEpoch){phase->requestCancel();phase->finishExternal("facies_config_changed");finishActive(false,{{"code","facies_config_changed"}});return;}
  m_activeTask=phase;
  if(!activeContextIsCurrent()){phase->requestCancel();phase->finishExternal("stale_session");finishActive(false,{{"code","stale_session"}});return;}
  connect(phase,&PaleoTask::cancelAsked,this,[this,generation,phase] {
    if(generation!=m_generation)return;m_facies->cancel();phase->finishExternal("cancelled");finishActive(false,{{"code","cancelled"}});
  });
  connect(m_facies,&WellFaciesService::modelsReady,this,[this,generation,args,phase,expected](const QVector<WellFaciesModel> &models) {
    if(generation!=m_generation||!m_active)return;
    const auto guard=m_context.capabilityHost;
    const auto policy=guard?paleo::host::checkContext(guard->context(),expected,paleo::host::Read|paleo::host::ExternalTransfer):QStringLiteral("stale_session");
    if(!policy.isEmpty()){phase->finishExternal(policy);finishActive(false,{{"code",policy}});return;}
    QJsonArray list;const auto filter=args.value("model_id").toString();QHash<QString,int> ids;
    for(const auto &model:models)++ids[model.id];
    for(const auto &model:models) {
      if(!filter.isEmpty()&&model.id!=filter)continue;
      auto readiness=paleo::wellfacies::modelReadiness(model,model.id,model.version);
      if(readiness.isEmpty()&&ids.value(model.id)>1)readiness="ambiguous_model_version_id";
      QJsonObject item{{"id",model.id},{"version",model.version},{"name",model.name},
        {"ready",readiness.isEmpty()},{"readinessCode",readiness},{"readinessScope","model_contract_only"}};
      if(!filter.isEmpty())item.insert("inputContract",paleo::wellfacies::frozenModelJson(model));
      list.append(item);
    }
    QJsonObject payload{{"contract","paleo.well-facies.models.v2"},{"models",list},{"returnedCount",list.size()},
      {"totalCount",list.size()},{"catalogCount",models.size()},
      {"coverage",QJsonObject{{"complete",true},{"scope",filter.isEmpty()?"all_models":"exact_model_id"}}}};
    const auto endpoint=m_faciesConfig.baseUrl.adjusted(QUrl::NormalizePathSegments|QUrl::StripTrailingSlash).toEncoded();
    payload.insert("endpoint_id",QString::fromLatin1(QCryptographicHash::hash(endpoint,QCryptographicHash::Sha256).toHex()));
    const auto bytes=QJsonDocument(payload).toJson(QJsonDocument::Compact);
    if(m_faciesConfig.apiKey.size()>=8&&bytes.contains(m_faciesConfig.apiKey)) {
      phase->finishExternal("model_catalog_contains_credential");finishActive(false,{{"code","model_catalog_contains_credential"}});return;
    }
    if(bytes.size()>1024*1024) {
      phase->finishExternal("model_catalog_budget_exceeded");
      finishActive(false,{{"code","model_catalog_budget_exceeded"},{"retryAdvice","query_exact_model_id"}});
    }else{phase->finishExternal();finishActive(true,payload);}
  });
  connect(m_facies,&WellFaciesService::failed,this,[this,generation,phase](const QString &reason) {
    if(generation==m_generation&&m_active){phase->finishExternal(reason);finishActive(false,{{"code","model_catalog_unavailable"},{"error",reason}});}
  });
  m_facies->fetchModelContracts();
  if(!activeContextIsCurrent())phase->requestCancel();
}
