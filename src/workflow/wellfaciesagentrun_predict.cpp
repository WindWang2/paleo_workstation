// 层：功能
#include "wellfaciesagentrun.h"
#include "services/wellfaciesinputstore.h"
#include "domain/faciestaxonomy.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSaveFile>
#include <QDir>

namespace cap=paleo::capability;
void WellFaciesAgentRun::next() {
  if(m_done)return;
  if(!current().isEmpty()){finish(false,current());return;}
  if(m_index>=m_input->segments.size()){finish(true,"succeeded");return;}
  const auto parent=m_recovery->lookup(m_scope,m_request.clientRequestId);
  if(parent.code!="found"||parent.record.operationId!=m_parentId){finish(false,"input_operation_unavailable");return;}
  m_parentCancelled=parent.record.cancelRequested;
  const auto requestHash=m_input->requestHashes[m_index];
  cap::FrozenOperation plan;plan.capabilityId="wellfacies.predict";plan.contractVersion=2;
  plan.implementationVersion="well-facies-agent-v2";plan.callerScope=m_scope;
  plan.clientRequestId="well-segment-"+cap::operationDigest({{"parentRequestId",m_request.clientRequestId},{"segment",m_index}});
  plan.intent={{"parentRequestId",m_request.clientRequestId},{"segment",m_index},{"requestHash",requestHash}};
  plan.frozenPlan={{"parentOperationId",m_parentId},{"segment",m_index},
    {"storedInput",m_parentPlan.value("storedInput")},{"requestHash",requestHash}};
  plan.phase="compute-only";
  plan.externalRequest={{"endpointId",m_endpointId},{"modelRef",paleo::wellfacies::frozenModelJson(m_input->model)},
    {"modelVersion",m_input->model.version},{"featureSchemaHash",m_input->featureSchemaHash},
    {"inputHash",m_input->inputHash},{"requestHash",requestHash}};
  const auto prior=m_recovery->lookup(m_scope,plan.clientRequestId);
  if(m_parentCancelled&&(prior.code=="not_found"||
      (prior.code=="found"&&prior.record.execution=="queued"&&prior.record.remoteJobId.isEmpty()))) {
    finish(false,"cancelled_before_next_segment");return;
  }
  if(!m_modelVerified&&(prior.code=="not_found"||(prior.code=="found"&&prior.record.execution=="queued"))) {
    verifyStoredModel();return; // Metadata-only check before any newly issued POST; accepted jobs query by their original ID.
  }
  m_segment=m_recovery->beginExternal(plan);
  if(!m_segment.ok){finish(false,m_segment.code);return;}
  m_remoteJobId=m_segment.record.remoteJobId;
  if(m_segment.code=="Terminal") {
    if(m_segment.record.externalOutcome=="succeeded"&&m_segment.record.artifactManifest.isEmpty()) {
      restoreCompletedResult();return;
    }
    m_results.append(QJsonObject{{"operationId",m_segment.record.operationId},{"execution",m_segment.record.execution},
      {"result",m_recovery->results(m_scope,m_segment.record.operationId)},{"replayed",true}});
    if(m_segment.record.externalOutcome!="succeeded"){finish(false,"remote_failed");return;}
    ++m_index;next();return;
  }
  if(m_segment.code=="Unknown") {
    finish(false,"ExternalOutcomeUnknown",{{"childOperationId",m_segment.record.operationId},
      {"remoteJobId",m_remoteJobId},{"retryAdvice","query_original_job_only"}});return;
  }
  m_externalActive=true;m_send=m_segment.code=="Send";
  if(m_send)m_service->predictFrozen(m_input->model,m_input->wellName,m_input->segments[m_index],requestHash,m_input->requestBodies[m_index]);
  else if(m_segment.code=="Query")m_service->queryFrozen(m_input->model,m_input->wellName,m_input->segments[m_index],requestHash,m_remoteJobId);
  else unknown("invalid_external_permission");
}
void WellFaciesAgentRun::completed(const WellFaciesResult &result) {
  if(m_done||!m_externalActive)return;
  m_resultReceived=true;
  if(m_recovery)setParent(m_recovery);
  auto bytes=std::make_shared<QByteArray>();auto hash=std::make_shared<QString>();
  const auto input=m_input;const auto index=m_index;
  const auto project=m_projectDir,remoteJob=m_remoteJobId;
  const auto reference=m_parentPlan.value("storedInput").toObject();
  const auto expected=m_resultRecoveryOnly?m_segment.record.remoteCompletionProof.value("responseHash").toString():QString();
  m_worker=m_tasks->start(tr("冻结测井相结果"),[bytes,hash,result,input,index,project,reference,remoteJob,expected](PaleoTask *){
    QJsonArray intervals,predictions;
    for(int i=0;i<result.intervals.size();++i) {
      const auto &interval=result.intervals[i];
      const auto code=faciesCodeForLabel(input->segments[index].facies,interval.text);
      if(code<=0)return QStringLiteral("invalid_frozen_result_class");
      intervals.append(QJsonObject{{"topMdMetres",interval.topDepth},{"bottomMdMetres",interval.bottomDepth},
        {"faciesCode",code}});
    }
    for(const auto &value:result.response.value("predictions").toArray()) {
      const auto row=value.toObject();if(row.value("wellName").toString()!=input->wellName)continue;
      const auto code=faciesCodeForLabel(input->segments[index].facies,row.value("label").toString());
      if(code<=0)return QStringLiteral("invalid_frozen_result_class");
      predictions.append(QJsonObject{{"wellName",input->wellName},
        {"depthMdMetres",row.value("depth")},{"faciesCode",code},{"confidence",row.value("confidence")}});
    }
    const QJsonObject safe{{"contract","paleo.well-facies.output.v2"},{"model",paleo::wellfacies::frozenModelJson(input->model)},
      {"inputHash",input->inputHash},{"requestHash",input->requestHashes[index]},
      {"sourceManifest",input->sourceManifest},{"occurrence",input->segmentOccurrences[index]},
      {"wellName",input->wellName},{"remoteJobId",remoteJob},{"depthAxis","MD"},{"depthUnit","m"},
      {"intervals",intervals},{"predictions",predictions},{"facies",QJsonArray::fromVariantList(input->segments[index].facies)}};
    *bytes=QJsonDocument(safe).toJson(QJsonDocument::Compact);
    if(bytes->size()>32*1024*1024){bytes->clear();return QStringLiteral("external_result_budget_exceeded");}
    *hash=QString::fromLatin1(QCryptographicHash::hash(*bytes,QCryptographicHash::Sha256).toHex());
    if(!expected.isEmpty()&&*hash!=expected){bytes->clear();return QStringLiteral("external_completion_conflict");}
    QString cacheCode;paleo::wellfacies::storeResultCache(project,reference,index,*bytes,cacheCode);
    return QString();
  });
  m_worker->setCancellable(false); // Receipt persistence is independent of a chat subscription.
  connect(m_worker,&PaleoTask::finished,this,[this,result,bytes,hash] {
    if(m_done)return;
    if(bytes->isEmpty()||hash->isEmpty()){unknown(m_worker->errorText());return;}
    attach(result,*bytes,*hash);
  });
}
void WellFaciesAgentRun::attach(const WellFaciesResult &result,const QByteArray &bytes,const QString &hash) {
  const QJsonObject proof{{"requestHash",m_input->requestHashes[m_index]},
    {"responseHash",hash},{"modelVersion",result.modelVersion}};
  const auto code=m_recovery->reportExternal(m_scope,m_segment.record.operationId,m_remoteJobId,"succeeded",proof,true);
  if(code!="ok"){unknown(code);return;}
  m_externalActive=false;m_send=false;
  attachBytes(bytes,result.intervals.size());
}
void WellFaciesAgentRun::attachBytes(const QByteArray &bytes,int count) {
  QString error;
  m_worker=m_recovery->attachExternalResults(m_scope,m_segment.record.operationId,
    [bytes](const cap::OperationWorkContext &context) {
      cap::OperationWorkResult result;
      if(context.cancelled&&context.cancelled()){result.code="cancelled";return result;}
      const auto path=QDir(context.privateDir).filePath("prediction.json");QSaveFile file(path);
      if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit()){result.code="result_store_failed";return result;}
      result.outputs.ok=true;result.outputs.artifacts.append({"result",path,"well_facies_result","Well facies result",{}, {}});
      result.ok=true;return result;
    },this,[this,count](cap::ExecuteResult stored) {
      if(m_done)return;
      if(!stored.ok){resultUnavailable(stored.code);return;}
      m_results.append(QJsonObject{{"operationId",m_segment.record.operationId},{"execution","succeeded"},
        {"intervals",count},{"result",m_recovery->results(m_scope,m_segment.record.operationId)}});
      m_resultReceived=false;
      m_resultRecoveryOnly=false;
      if(m_cancelAfterResult){finish(false,"cancelled_after_remote_completion");return;}
      ++m_index;next();
    },&error);
  if(m_worker)m_worker->setCancellable(false);
  else resultUnavailable(error);
}
