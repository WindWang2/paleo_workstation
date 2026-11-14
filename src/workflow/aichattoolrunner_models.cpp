// 层：功能
#include "aichattoolrunner.h"
#include "capabilityhost.h"
#include "ai/onnxpredictionservice.h"
#include <QJsonDocument>
#include <cmath>

void AiChatToolRunner::runLocalModels(const QJsonObject &args) {
  const auto fail=[&](const QString &code){finishActive(false,{{"contract","paleo.local_models.page.v1"},{"code",code}});};
  const QStringList allowed{"topic","model_name","cursor","page_size","max_bytes"};
  for(auto it=args.begin();it!=args.end();++it)if(!allowed.contains(it.key()))return fail("unsupported_topic_argument");
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
  const auto integer=[&](const char *key,int fallback,int minimum,int maximum,int &out) {
    const auto value=args.value(QLatin1String(key));if(value.isUndefined()){out=fallback;return true;}
    const double number=value.toDouble();
    if(!value.isDouble() || !std::isfinite(number) || number!=std::floor(number) || number<minimum || number>maximum)return false;
    out=int(number);return true;
  };
  int size=0,bytes=0;
  if(!integer("page_size",16,1,64,size) || !integer("max_bytes",8192,1024,32768,bytes))return fail("invalid_query_budget");
  const QJsonObject scope{{"caller",caller},{"document",live.documentId},{"epoch",QString::number(live.epoch)}};
  const auto result=m_context.onnx->modelContractPage(args.value("model_name").toString(),args.value("cursor").toString(),
    QString::fromUtf8(QJsonDocument(scope).toJson(QJsonDocument::Compact)),size,bytes);
  finishActive(result.value("ok").toBool(),result);
#endif
}
