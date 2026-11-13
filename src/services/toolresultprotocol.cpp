// 层：数据
#include "toolresultprotocol.h"
#include <QJsonArray>
#include <QJsonDocument>

namespace paleo::chat {
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
                  {"payloadContract", native.value("contractVersion").toString("legacy.unversioned")},
                  {"transport", QJsonObject{{"status", "complete"}, {"code", "ok"}}},
                  {"coverage", coverage}, {"data", value}};
  for (const QString &key : {QStringLiteral("operationId"), QStringLiteral("commitReceiptRef"),
                            QStringLiteral("execution"), QStringLiteral("commit"),
                            QStringLiteral("commitState"), QStringLiteral("presentation"),
                            QStringLiteral("receipt"), QStringLiteral("resultRef")})
    if (native.contains(key))
      out.insert(key, native.value(key));
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
                            QStringLiteral("payloadContract"), QStringLiteral("operationId")})
    props.insert(key, QJsonObject{{"type", "string"}});
  props.insert("contractVersion", QJsonObject{{"const", "paleo.tool-result.v1"}});
  props.insert("data", nativeSchema);
  for (const QString &key : {QStringLiteral("transport"), QStringLiteral("coverage")})
    props.insert(key, QJsonObject{{"type", "object"}});
  return {{"type", "object"}, {"properties", props},
          {"required", QJsonArray{"contractVersion", "status", "code", "transport", "coverage"}}};
}
} // namespace paleo::chat
