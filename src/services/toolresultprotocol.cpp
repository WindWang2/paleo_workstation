// 层：数据
#include "toolresultprotocol.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>

namespace paleo::chat {
QJsonObject boundedWellResultMetadata(const QJsonObject &native) {
  if(native.value("contract")!="paleo.well-facies.result.v2")return {};
  QJsonObject facts;
  for(const auto &field:{"inputOperationId","childOperationId"}) {
    const auto value=native.value(QLatin1String(field));const auto text=value.toString();bool safe=value.isString()&&!text.isEmpty()&&text.size()<=128;
    for(const auto &ch:text)if(ch.unicode()<0x21||ch.unicode()>0x7e||ch=='"'||ch=='\\')safe=false;
    if(safe)facts.insert(QLatin1String(field),value);
  }
  for(const auto &field:{"executionScope","resultAvailability","remoteOutcome"}) {
    const auto value=native.value(QLatin1String(field)).toString();
    const QStringList allowed=QString(field)=="executionScope"?QStringList{"batch_summary"}:
      QString(field)=="resultAvailability"?QStringList{"reference_unverified","unavailable"}:
      QStringList{"succeeded","failed","unknown"};
    if(allowed.contains(value))facts.insert(QLatin1String(field),value);
  }
  return facts;
}
QByteArray compactJson(const QJsonValue &value) {
  if (value.isObject())
    return QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
  if (value.isArray())
    return QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact);
  const QByteArray wrapped = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
  return wrapped.mid(1, wrapped.size() - 2);
}

QString jsonValueKind(const QJsonValue &value) {
  if (value.isArray()) return QStringLiteral("array");
  if (value.isObject()) return QStringLiteral("object");
  if (value.isString()) return QStringLiteral("string");
  if (value.isBool()) return QStringLiteral("boolean");
  if (value.isDouble()) return QStringLiteral("number");
  return QStringLiteral("null");
}

QJsonObject resultEnvelope(const QJsonValue &value, bool ok) {
  const QJsonObject native = value.toObject();
  const bool truncated = native.contains("truncated") && !native.value("truncated").isNull() &&
      native.value("truncated") != QJsonValue(false);
  const QJsonObject coverage = native.value("coverage").isObject()
      ? native.value("coverage").toObject()
      : QJsonObject{{"complete", !truncated && !native.value("hasMore").toBool() &&
                                            !native.value("valueFragment").toBool()}};
  const auto commitState = native.value("commitState").toString(
      native.value("commit").toObject().value("state").toString());
  const QString status = commitState == QLatin1String("committed") ? QStringLiteral("committed")
      : commitState == QLatin1String("publishing") ? QStringLiteral("commit_uncertain")
      : native.value("status").toString(ok ? "succeeded" : "failed");
  QJsonObject out{{"contractVersion", "paleo.tool-result.v1"},
                  {"status", status},
                  {"code", native.value("code").toString(ok ? "ok" : "tool_failed")},
                  {"payloadContract", native.value("contractVersion").toString(
                      native.value("contract").toString("legacy.unversioned"))},
                  {"transport", QJsonObject{{"status", "complete"}, {"code", "ok"}}},
                  {"coverage", coverage}, {"data", value}};
  for (const QString &key : {QStringLiteral("operationId"), QStringLiteral("commitReceiptRef"),
                            QStringLiteral("execution"), QStringLiteral("commit"),
                            QStringLiteral("commitState"), QStringLiteral("presentation"),
                            QStringLiteral("receipt"), QStringLiteral("resultRef")})
    if (native.contains(key))
      out.insert(key, native.value(key));
  const auto well=boundedWellResultMetadata(native);
  for(auto it=well.begin();it!=well.end();++it)out.insert(it.key(),it.value());
  return out;
}

QJsonObject transportResult(const QJsonObject &facts, const QString &code,
                            const QJsonValue &summary) {
  QJsonObject out = facts;
  out.remove("data");
  out.remove("resultRefs");
  out.insert("transport", QJsonObject{{"status", "incomplete"}, {"code", code}});
  out.insert("coverage", QJsonObject{{"complete", false}});
  out.insert("retryAdvice", QStringLiteral("do_not_repeat_execution"));
  if (!summary.isUndefined() && !summary.isNull())
    out.insert("summary", summary);
  return out;
}

QJsonObject resultError(const QString &code) {
  auto out = resultEnvelope(QJsonObject{{"code", code}}, false);
  out.insert("status", "unknown");
  return transportResult(out, code);
}

bool isResultEnvelope(const QJsonObject &value) {
  return value.value("contractVersion") == QJsonValue("paleo.tool-result.v1") &&
         value.value("status").isString() && value.value("code").isString() &&
         value.value("transport").isObject() && value.value("coverage").isObject();
}

QJsonObject toolResultEnvelopeSchema(const QJsonObject &nativeSchema) {
  QJsonObject props;
  for (const QString &key : {QStringLiteral("status"), QStringLiteral("code"),
                            QStringLiteral("payloadContract"), QStringLiteral("operationId"),
                            QStringLiteral("inputOperationId"), QStringLiteral("childOperationId"),
                            QStringLiteral("executionScope"), QStringLiteral("resultAvailability"), QStringLiteral("remoteOutcome")})
    props.insert(key, QJsonObject{{"type", "string"}});
  props.insert("contractVersion", QJsonObject{{"const", "paleo.tool-result.v1"}});
  props.insert("data", nativeSchema);
  for (const QString &key : {QStringLiteral("transport"), QStringLiteral("coverage")})
    props.insert(key, QJsonObject{{"type", "object"}});
  return {{"type", "object"}, {"properties", props},
          {"required", QJsonArray{"contractVersion", "status", "code", "transport", "coverage"}}};
}
} // namespace paleo::chat
