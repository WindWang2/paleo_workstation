// 层：功能
#include "aichattoolrunner.h"
#include "capabilityhost.h"
#include "ai/onnxpredictionservice.h"
#include "services/projectquerytopics.h"
#include <QJsonDocument>

void AiChatToolRunner::runLocalModels(const QJsonObject &args) {
  const auto fail=[&](const QString &code,const QString &field={}){
    QJsonObject payload{{"contract","paleo.local_models.page.v1"},{"code",code}};
    if(!field.isEmpty())payload.insert("field",field);
    finishActive(false,payload);
  };
  QString field;
  const auto argumentError = paleo::query::validateQueryTopicArguments(args, &field);
  if (!argumentError.isEmpty()) return fail(argumentError, field);
#if !PALEO_HAVE_ORT
  return fail("ort_unavailable");
#else
  if(!m_context.onnx)return fail("ort_provider_unavailable");
  if(!m_context.capabilityHost || m_resultSession.isEmpty())return fail("query_scope_unavailable");
  const auto live=m_context.capabilityHost->context();auto expected=live;
  expected.documentId=m_context.documentId;expected.epoch=m_context.documentEpoch;
  const auto policy=paleo::host::checkContext(live,expected,paleo::host::Read);
  if(!policy.isEmpty())return fail(policy);
  const auto caller=m_context.capabilityHost->callerScope(paleo::host::Caller::Agent,m_resultSession);
  if(caller.isEmpty())return fail("caller_unbound");
  paleo::query::QueryTopicRule rule;
  if (!paleo::query::queryTopicRule(QStringLiteral("local_models"), rule)) return fail("query_topic_unavailable");
  const int size = args.value("page_size").toInt(rule.pageSizeDefault);
  const int bytes = args.value("max_bytes").toInt(rule.maxBytesDefault);
  const QJsonObject scope{{"caller",caller},{"document",live.documentId},{"epoch",QString::number(live.epoch)}};
  const auto result=m_context.onnx->modelContractPage(args.value("model_name").toString(),args.value("cursor").toString(),
    QString::fromUtf8(QJsonDocument(scope).toJson(QJsonDocument::Compact)),size,bytes);
  finishActive(result.value("ok").toBool(),result);
#endif
}
