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
  // G3/W3：contract_version 精确解码。此前 toInt(1) 把 "2"（文本）、2.5、
  // 越界值静默解释成默认版本 1——「校验通过但解码成另一值」。现在：
  // 缺席 → 1；在场必须是 JSON 原生整数（有限、整值、int 域、≥1），否则
  // 明确参数错误。不支持的整值版本不在此回退，交由网关按
  // unsupported_version 精确拒绝（capabilitygateway.cpp）。
  int contractVersion = 1;
  if (arguments.contains(QStringLiteral("contract_version"))) {
    const auto version = arguments.value(QStringLiteral("contract_version"));
    if (!version.isDouble() || !std::isfinite(version.toDouble()) ||
        std::floor(version.toDouble()) != version.toDouble() ||
        version.toDouble() < 1 || version.toDouble() > 2147483647.0)
      return QJsonObject{{QStringLiteral("ok"), false},
                         {QStringLiteral("code"), QStringLiteral("contract_version_invalid")}};
    contractVersion = int(version.toDouble());
  }
  // G3/W3：parameters/inputs 必须是 JSON object（若在场）。此前对非 object
  // 静默取空 map 继续执行。宿主授权字段仍由 forbiddenAuthority/gateway 把守。
  const auto parametersValue = arguments.value(QStringLiteral("parameters"));
  if (!parametersValue.isUndefined() && !parametersValue.isObject())
    return QJsonObject{{QStringLiteral("ok"), false},
                       {QStringLiteral("code"), QStringLiteral("invalid_request_shape")},
                       {QStringLiteral("field"), QStringLiteral("parameters")}};
  const auto inputsValue = arguments.value(QStringLiteral("inputs"));
  if (!inputsValue.isUndefined() && !inputsValue.isObject())
    return QJsonObject{{QStringLiteral("ok"), false},
                       {QStringLiteral("code"), QStringLiteral("invalid_request_shape")},
                       {QStringLiteral("field"), QStringLiteral("inputs")}};
  request.capabilityId = toolName;
  request.contractVersion = contractVersion;
  request.documentId = documentId;
  request.callerScope = hostScope;
  request.clientRequestId = arguments.value(QStringLiteral("client_request_id")).toString();
  request.bindingMode = arguments.value(QStringLiteral("binding_mode")).toString();
  request.bindingId = arguments.value(QStringLiteral("binding_id")).toString();
  request.frozenHead = arguments.value(QStringLiteral("frozen_head")).toString();
  request.currentHead = hostHead;
  request.publishIntent = intent;
  request.parameters = parametersValue.toObject().toVariantMap();
  request.inputs = inputsValue.toObject().toVariantMap();
  request.inputs.remove(QStringLiteral("parent_version_ids"));
  request.inputs.remove(QStringLiteral("drop_output_port"));
  request.inputs.remove(QStringLiteral("grid_wkt"));
  request.operationDir = operationDir;
  return {};
}
QJsonObject prepareChatToolRequest(ExecuteRequest &out, const QString &toolName, const QJsonObject &arguments,
    const QString &hostScope, const QString &documentId, const QString &operationDir, const QString &hostHead) {
  return decodeChatToolRequest(out,toolName,arguments,hostScope,documentId,operationDir,hostHead,false);
}
QJsonObject prepareHostedChatToolRequest(ExecuteRequest &out,const QString &toolName,const QJsonObject &arguments) {
  return decodeChatToolRequest(out,toolName,arguments,{},{},{},{},true);
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

} // namespace paleo::capability
