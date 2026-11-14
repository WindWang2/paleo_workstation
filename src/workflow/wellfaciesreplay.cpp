// 层：功能
#include "wellfaciesreplay.h"
#include "capabilityhost.h"
#include "capabilityrecovery.h"
#include "domain/wellfaciesrequest.h"
#include <QJsonArray>

namespace paleo::wellfacies {
QJsonObject completedReplay(capability::CapabilityHost *host,const QString &chatSession,
                           const QJsonObject &args,QString &code) {
  code.clear();Request request;
  if(!requestFromJson(args,request,&code))return {};
  if(!host||!host->recovery()){code="well_execution_host_unavailable";return {};}
  const auto context=host->context();code=paleo::host::checkContext(context,context,paleo::host::Read);
  if(!code.isEmpty())return {};
  if(context.documentId!=request.well.documentId){code="well_document_mismatch";return {};}
  const auto scope=host->callerScope(paleo::host::Caller::Agent,chatSession);
  if(scope.isEmpty()||chatSession.isEmpty()){code="caller_scope_required";return {};}
  auto *recovery=host->recovery();const auto parent=recovery->lookup(scope,request.clientRequestId);
  if(parent.code=="not_found")return {};
  if(parent.code!="found"){code=parent.code;return {};}
  if(parent.record.requestHash!=capability::operationDigest(args)){code="idempotency_conflict";return {};}
  if(parent.record.capabilityId!="wellfacies.predict"||parent.record.contractVersion!=2){code="operation_contract_mismatch";return {};}
  const auto source=parent.record.frozenRequest.value("plan").toObject().value("sourceManifest").toObject();
  const auto segments=source.value("segments").toArray();
  if(segments.isEmpty()||segments.size()>64){code="invalid_frozen_segments";return {};}
  QJsonArray results;
  for(int index=0;index<segments.size();++index) {
    const auto requestId="well-segment-"+capability::operationDigest({{"parentRequestId",request.clientRequestId},{"segment",index}});
    const auto child=recovery->lookup(scope,requestId);
    if(child.code=="not_found")return {};
    if(child.code!="found"){code=child.code;return {};}
    if(child.record.execution!="succeeded"||child.record.externalOutcome!="succeeded"||child.record.artifactManifest.isEmpty())return {};
    if(child.record.frozenRequest.value("plan").toObject().value("parentOperationId").toString()!=parent.record.operationId||
       child.record.externalRequest.value("inputHash")!=source.value("inputHash")||
       child.record.externalRequest.value("requestHash")!=segments[index].toObject().value("requestHash")) {
      code="stored_input_plan_mismatch";return {};
    }
    const auto result=recovery->results(scope,child.record.operationId);
    if(result.value("ports").toArray().isEmpty())return {};
    results.append(QJsonObject{{"operationId",child.record.operationId},{"execution","succeeded"},{"result",result},
      {"resultAvailability","reference_unverified"},{"replayed",true}});
  }
  const auto occurrences=source.value("occurrences").toArray();int derived=0;
  for(const auto &value:occurrences)if(value.toObject().value("derivedDivisionGeometry").toBool())++derived;
  const auto skipped=source.value("skipped").toArray();
  return {{"contract","paleo.well-facies.result.v2"},{"code","succeeded"},{"status","succeeded"},{"execution","succeeded"},
    {"inputOperationId",parent.record.operationId},{"segments",results},{"replayed",true},
    {"executionScope","batch_summary"},
    {"inputOperation",QJsonObject{{"operationId",parent.record.operationId},{"role","frozen_input_snapshot"},
      {"execution",parent.record.execution},{"cancelRequested",parent.record.cancelRequested}}},
    {"resultAvailability","reference_unverified"},
    {"inputHash",source.value("inputHash")},{"featureSchemaHash",source.value("featureSchemaHash")},
    {"coverage",QJsonObject{{"scope",source.value("coverage")},{"subject","durable_execution_receipts"},
      {"resultBytesVerified",false},{"occurrenceCount",occurrences.size()},
      {"expectedSegmentCount",segments.size()},{"resultSegmentCount",results.size()},{"complete",skipped.isEmpty()},
      {"inputPartial",!skipped.isEmpty()},{"skipped",skipped},{"derivedDivisionWindows",derived}}}};
}
}
