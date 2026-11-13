// 层：功能
// workflow/capabilitychatprotocol.cpp — 原生执行事实到聊天传输；大结果用稳定引用。
#include "capabilitygateway.h"
#include "capabilityrecovery.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <cmath>
namespace paleo::capability {
namespace {
bool safeTag(const QJsonValue &value,int maximum=64) {
  if(!value.isString() || value.toString().isEmpty() || value.toString().size()>maximum)return false;
  for(const auto &ch:value.toString())if(ch.unicode()<0x20 || ch.unicode()>0x7e || ch=='"' || ch=='\\' || ch=='/')return false;
  return true;
}
bool opaqueId(const QJsonValue &value) {
  if(!safeTag(value,256))return false;
  for(const auto &ch:value.toString())if(!ch.isLetterOrNumber() && ch!='-' && ch!='_')return false;
  return true;
}
bool hexHash(const QJsonValue &value) {
  const auto text=value.toString();if(!value.isString() || text.size()!=64)return false;
  for(const auto &ch:text)if(!((ch>='0' && ch<='9') || (ch>='a' && ch<='f')))return false;return true;
}
bool decimalRevision(const QJsonValue &value) {
  const auto text=value.toString();if(!value.isString() || text.isEmpty() || text.size()>20)return false;
  for(const auto &ch:text)if(ch<'0' || ch>'9')return false;return true;
}
QByteArray boundOperation(const QJsonObject &object,int budgetBytes,qsizetype originalBytes) {
  QJsonObject page;
  const auto add=[&](const QString &key,const QJsonValue &value) {
    if(value.isUndefined())return;auto candidate=page;candidate.insert(key,value);
    if(QJsonDocument(candidate).toJson(QJsonDocument::Compact).size()<=budgetBytes)page=std::move(candidate);
  };
  add("contract",QLatin1String(OperationContract));
  add("code",safeTag(object.value("code"))?object.value("code"):QJsonValue("native_details_by_reference"));
  for(const auto &key:{"commitState","presentation","execution"})if(safeTag(object.value(QLatin1String(key))))add(QLatin1String(key),object.value(QLatin1String(key)));
  if(object.value("ok").isBool())add("ok",object.value("ok"));
  add("transport",QJsonObject{{"status","incomplete"},{"code","response_budget_exceeded"}});
  add("coverage",QJsonObject{{"complete",false}});
  const auto id=object.value("operationId");
  if(opaqueId(id)) {
    add("operationId",id);add("resultRef",QJsonObject{{"kind","operation-result-v2"},{"operationId",id}});
  }
  auto receipt=object.value("commitReceiptRef");
  if(!opaqueId(receipt))receipt=object.value("receipt").toObject().value("outputSetId");
  if(opaqueId(receipt))add("commitReceiptRef",receipt);
  if(hexHash(object.value("requestHash")))add("requestHash",object.value("requestHash"));
  if(decimalRevision(object.value("revision")))add("revision",object.value("revision"));
  if(object.value("computeCount").isDouble() && std::isfinite(object.value("computeCount").toDouble()) &&
      object.value("computeCount").toDouble()>=0 && object.value("computeCount").toDouble()<=2147483647.0 &&
      std::floor(object.value("computeCount").toDouble())==object.value("computeCount").toDouble())add("computeCount",object.value("computeCount"));
  add("byteSize",originalBytes);add("budgetBytes",budgetBytes);add("retryAdvice","do_not_repeat_execution");
  return QJsonDocument(page).toJson(QJsonDocument::Compact);
}
}
QByteArray boundToolResponse(const QJsonObject &object, int budgetBytes) {
  const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
  if (bytes.size() <= budgetBytes)
    return bytes;
  if(object.value("contract")==QLatin1String(OperationContract))return boundOperation(object,qMax(2,budgetBytes),bytes.size());
  QJsonObject page;
  page.insert(QStringLiteral("ok"), object.value(QStringLiteral("ok")));
  page.insert(QStringLiteral("status"), object.value(QStringLiteral("code")));
  page.insert(QStringLiteral("operationId"), object.value(QStringLiteral("operationId")));
  for (const auto &key : {QStringLiteral("execution"), QStringLiteral("commitState"), QStringLiteral("presentation")})
    if (object.contains(key))
      page.insert(key, object.value(key));
  page.insert(QStringLiteral("code"), QStringLiteral("response_budget_exceeded"));
  page.insert(QStringLiteral("byteSize"), bytes.size());
  page.insert(QStringLiteral("budgetBytes"), budgetBytes);
  const QString receipt = object.value(QStringLiteral("receipt")).toString();
  if (!receipt.isEmpty())
    page.insert(QStringLiteral("resultRef"), receipt);
  const QString resultPath = object.value(QStringLiteral("result_path")).toString();
  if (!resultPath.isEmpty())
    page.insert(QStringLiteral("result_path"), resultPath);
  return QJsonDocument(page).toJson(QJsonDocument::Compact);
}

static QJsonObject decodeChatToolRequest(ExecuteRequest &request, const QString &toolName, const QJsonObject &arguments,
                                   const QString &hostScope, const QString &documentId, const QString &operationDir,
                                   const QString &hostHead,bool hosted) {
  request = {};
  if (arguments.contains(QStringLiteral("actor")) || arguments.contains(QStringLiteral("approved")) ||
      arguments.contains(QStringLiteral("caller_scope"))) {
    return QJsonObject{{QStringLiteral("code"), QStringLiteral("untrusted_authority")}};
  }
  const QString intent = arguments.value(QStringLiteral("publish_intent")).toString(QStringLiteral("compute-only"));
  if (!hosted && intent == QLatin1String("publish") && hostHead.isEmpty())
    return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("code"), QStringLiteral("host_head_required")}};
  request.capabilityId = toolName;
  request.contractVersion = arguments.value(QStringLiteral("contract_version")).toInt(1);
  request.documentId = documentId;
  request.callerScope = hostScope;
  request.clientRequestId = arguments.value(QStringLiteral("client_request_id")).toString();
  request.bindingMode = arguments.value(QStringLiteral("binding_mode")).toString();
  request.bindingId = arguments.value(QStringLiteral("binding_id")).toString();
  request.frozenHead = arguments.value(QStringLiteral("frozen_head")).toString();
  request.currentHead = hostHead;
  request.publishIntent = intent;
  request.parameters = arguments.value(QStringLiteral("parameters")).toObject().toVariantMap();
  request.inputs = arguments.value(QStringLiteral("inputs")).toObject().toVariantMap();
  request.inputs.remove(QStringLiteral("parent_version_ids"));
  request.inputs.remove(QStringLiteral("drop_output_port"));
  request.inputs.remove(QStringLiteral("grid_wkt"));
  request.operationDir = operationDir;
  return {};
}
QJsonObject prepareChatToolRequest(ExecuteRequest &request,const QString &name,const QJsonObject &arguments,
    const QString &scope,const QString &documentId,const QString &directory,const QString &head) {
  return decodeChatToolRequest(request,name,arguments,scope,documentId,directory,head,false);
}
QJsonObject prepareHostedChatToolRequest(ExecuteRequest &request,const QString &name,const QJsonObject &arguments) {
  return decodeChatToolRequest(request,name,arguments,{},{},{},{},true);
}

QJsonObject chatToolResponse(const ExecuteResult &result) {
  auto payload = QJsonObject::fromVariantMap(result.summary);
  payload.remove(QStringLiteral("result_path"));
  const auto control=QJsonDocument::fromJson(result.responseJson).object();
  if(control.value("contract")==QLatin1String(OperationContract)) {
    payload.insert("contract",QLatin1String(OperationContract));payload.remove("contractVersion");
    for(const auto &key:{"revision","computeCount","cancelRequested","commitReceiptRef"})
      if(control.contains(QLatin1String(key)))payload.insert(QLatin1String(key),control.value(QLatin1String(key)));
    const auto reference=control.value("resultRef").toObject();
    if(reference.value("kind")==QLatin1String("operation-result-v2") && reference.value("operationId").toString()==result.operationId && opaqueId(reference.value("operationId")))
      payload.insert("resultRef",QJsonObject{{"kind","operation-result-v2"},{"operationId",result.operationId}});
  }
  payload.insert(QStringLiteral("code"), result.code);
  payload.insert(QStringLiteral("ok"), result.ok);
  if (!result.operationId.isEmpty())
    payload.insert(QStringLiteral("operationId"), result.operationId);
  if(!result.execution.isEmpty())payload.insert("execution",result.execution);
  if(!result.commitState.isEmpty())payload.insert("commitState",result.commitState);
  if(!result.presentation.isEmpty())payload.insert("presentation",result.presentation);
  if(!result.requestHash.isEmpty())payload.insert("requestHash",result.requestHash);
  return QJsonDocument::fromJson(boundToolResponse(payload)).object();
}

QJsonObject runChatTool(const QString &toolName, const QJsonObject &arguments, const QString &hostScope,
                        const QString &documentId, const QString &operationDir, const QString &hostHead,
                        const std::function<bool()> &cancelled, DataCatalog *catalog, const QString &projectDir) {
  ExecuteRequest request;
  const auto error = prepareChatToolRequest(request, toolName, arguments, hostScope, documentId, operationDir, hostHead);
  if (!error.isEmpty())
    return error;
  request.catalog = catalog;
  request.projectDir = projectDir;
  request.cancelled = cancelled;
  return chatToolResponse(execute(request));
}

} // namespace paleo::capability
