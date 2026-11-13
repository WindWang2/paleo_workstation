// 层：功能
#include "chatresultstore.h"
#include "../services/toolresultprotocol.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QUuid>
#include <cmath>
#include <chrono>
#include <utility>

namespace paleo::chat {
namespace {
bool smallStateTag(const QJsonValue &value) {
  if (!value.isString() || value.toString().size() > 64) return false;
  for (const auto &ch : value.toString())
    if (ch.unicode() < 0x20 || ch.unicode() > 0x7e || ch == '"' || ch == '\\') return false;
  return true;
}
QJsonObject boundedMetadata(const QJsonObject &original, const QJsonObject &partial, int maxBytes) {
  auto minimal = resultError(QStringLiteral("native_metadata_requires_reference"));
  minimal.insert("transport", partial.value("transport"));
  minimal.insert("coverage", QJsonObject{{"complete", false}, {"metadataOmitted", true}});
  const auto commit = original.value("commit").toObject().value("state");
  if (original.value("commitState") == QJsonValue("committed") ||
      original.value("status") == QJsonValue("committed") || commit == QJsonValue("committed"))
    minimal.insert("status", "committed");
  else if (smallStateTag(original.value("status")))
    minimal.insert("status", original.value("status"));
  for (const auto &field : {"execution", "commitState", "presentation", "code"}) {
    if (!smallStateTag(original.value(QLatin1String(field)))) continue;
    auto candidate = minimal;
    candidate.insert(QLatin1String(field), original.value(QLatin1String(field)));
    if (compactJson(candidate).size() <= maxBytes) minimal = candidate;
  }
  const auto contract=original.value("contractVersion").toString(original.value("contract").toString());
  if(smallStateTag(QJsonValue(contract))) {
    auto candidate=minimal;candidate.insert("payloadContract",contract);
    if(compactJson(candidate).size()<=maxBytes)minimal=candidate;
  }
  if (smallStateTag(commit)) {
    auto candidate = minimal;
    candidate.insert("commit", QJsonObject{{"state", commit}});
    if (compactJson(candidate).size() <= maxBytes) minimal = candidate;
  }
  // Every optional field is admitted as a whole after measuring its escaped JSON representation.
  for (const auto &field : {"resultRefs", "operationId", "commitReceiptRef", "receipt", "resultRef"}) {
    const auto key = QLatin1String(field);
    if (!partial.contains(key) && !original.contains(key)) continue;
    auto candidate = minimal;
    candidate.insert(key, partial.contains(key) ? partial.value(key) : original.value(key));
    if (compactJson(candidate).size() <= maxBytes) minimal = candidate;
  }
  return minimal;
}
} // namespace

ChatResultStore::ChatResultStore() : ChatResultStore(Limits{}) {}
ChatResultStore::ChatResultStore(Limits limits, Clock clock)
  : m_limits(limits), m_clock(clock ? std::move(clock) : Clock([] {
      return std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
    })) {
  const Limits ceiling;
  m_limits.resultBytes = qBound(0, m_limits.resultBytes, ceiling.resultBytes);
  m_limits.totalBytes = qBound(0, m_limits.totalBytes, ceiling.totalBytes);
  m_limits.handles = qBound(0, m_limits.handles, ceiling.handles);
  m_limits.ttlMs = qBound(qint64(0), m_limits.ttlMs, ceiling.ttlMs);
  setScope({}, {}, 0);
}

void ChatResultStore::setScope(const QString &session, const QString &document, quint64 epoch) {
  if (!m_scope.isEmpty() && session == m_session && document == m_document && epoch == m_epoch)
    return;
  m_session = session;
  m_document = document;
  m_epoch = epoch;
  m_scope = QUuid::createUuid().toString(QUuid::Id128);
  m_secret = QUuid::createUuid().toRfc4122() + QUuid::createUuid().toRfc4122();
  m_values.clear();
  m_expired.clear();
  m_bytes = 0;
}

void ChatResultStore::expire() {
  const auto now = m_clock();
  for (auto it = m_values.begin(); it != m_values.end();) {
    if (it->expires > now) { ++it; continue; }
    m_bytes -= it->bytes;
    m_expired.insert(it.key(), true);
    it = m_values.erase(it);
  }
  while (m_expired.size() > m_limits.handles)
    m_expired.erase(m_expired.begin());
}

QString ChatResultStore::put(const QJsonValue &value, const QByteArray &json, QString *error) {
  expire();
  if (m_limits.ttlMs <= 0)
    *error = QStringLiteral("result_cache_disabled");
  else if (json.size() > m_limits.resultBytes)
    *error = QStringLiteral("result_too_large");
  else if (m_values.size() >= m_limits.handles || json.size() > m_limits.totalBytes - m_bytes)
    *error = QStringLiteral("result_cache_full");
  else {
    const QString reference = QStringLiteral("chat-result-v1:%1:%2")
        .arg(m_scope, QUuid::createUuid().toString(QUuid::Id128));
    m_values.insert(reference, {value, int(json.size()), m_clock() + m_limits.ttlMs});
    m_bytes += int(json.size());
    return reference;
  }
  return {};
}

QJsonObject ChatResultStore::encode(const QString &rawJson, bool ok, int maxBytes, bool allowCache) {
  maxBytes = qBound(kMinimumEnvelopeBytes, maxBytes, kResultHardBytes);
  QJsonParseError error;
  const auto doc = QJsonDocument::fromJson(rawJson.toUtf8(), &error);
  if (error.error != QJsonParseError::NoError || (!doc.isObject() && !doc.isArray()))
    return resultError(QStringLiteral("invalid_tool_result"));
  const QJsonValue value = doc.isObject() ? QJsonValue(doc.object()) : QJsonValue(doc.array());
  auto out = resultEnvelope(value, ok);
  auto transport = out.value("transport").toObject();
  transport.insert("maxBytes", maxBytes);
  out.insert("transport", transport);
  if (compactJson(out).size() <= maxBytes)
    return out;
  const QByteArray json = compactJson(value);
  const QJsonObject summary{{"kind", jsonValueKind(value)}, {"encodedBytes", json.size()}};
  QString code = QStringLiteral("page_budget_exceeded");
  const auto reference = allowCache ? put(value, json, &code) : QString();
  out = transportResult(out, reference.isEmpty() ? code : QStringLiteral("result_referenced"), summary);
  transport = out.value("transport").toObject();
  transport.insert("maxBytes", maxBytes);
  out.insert("transport", transport);
  if (!reference.isEmpty())
    out.insert("resultRefs", QJsonArray{QJsonObject{{"kind", "chat-result-v1"}, {"ref", reference}}});
  if (compactJson(out).size() > maxBytes)
    out.remove("summary");
  QJsonArray omitted;
  for (const QString &field : {QStringLiteral("execution"), QStringLiteral("commitReceiptRef"),
                               QStringLiteral("operationId"), QStringLiteral("commit")}) {
    if (compactJson(out).size() <= maxBytes) break;
    if (!out.contains(field)) continue;
    // Keep a known commit state even when its full native receipt does not fit.
    if (field == QLatin1String("commit") && out.value(field).isObject()) {
      const auto state = out.value(field).toObject().value("state").toString();
      if (!state.isEmpty() && state.size() <= 64) {
        out.insert(field, QJsonObject{{"state", state}});
        omitted.append("commit.details");
        continue;
      }
    }
    out.remove(field);
    omitted.append(field);
  }
  for (const QString &field : {QStringLiteral("payloadContract"), QStringLiteral("code"), QStringLiteral("status")})
    if (out.value(field).toString().toUtf8().size() > 128) {
      out.insert(field, field == QLatin1String("status") ? "unknown" : "native_value_requires_reference");
      omitted.append(field);
    }
  if (!omitted.isEmpty()) out.insert("coverage", QJsonObject{{"complete", false}, {"omittedFields", omitted}});
  if (compactJson(out).size() > maxBytes) {
    out.remove("payloadContract");
    out.insert("coverage", QJsonObject{{"complete", false}, {"metadataOmitted", true}});
  }
  if (compactJson(out).size() > maxBytes)
    out.insert("code", "native_metadata_requires_reference");
  if (compactJson(out).size() > maxBytes)
    out.insert("status", "unknown");
  if (compactJson(out).size() > maxBytes)
    out = boundedMetadata(value.toObject(), out, maxBytes);
  // IDs are omitted with coverage when unavoidable, never sliced into a different identity.
  return out;
}

QString ChatResultStore::cursor(const QString &reference, const QString &selector,
                                const QString &mode, int offset) const {
  const QByteArray body = compactJson(QJsonObject{{"v", 1}, {"ref", reference},
      {"selector", selector}, {"mode", mode}, {"offset", offset}}).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
  const auto mac = QMessageAuthenticationCode::hash(body, m_secret, QCryptographicHash::Sha256).toHex();
  return QString::fromLatin1(body + '.' + mac);
}

bool ChatResultStore::decodeCursor(const QString &token, const QString &reference,
                                   const QString &selector, Position *position) const {
  if (token.toUtf8().size() > 4096)
    return false;
  const auto parts = token.toLatin1().split('.');
  if (parts.size() != 2)
    return false;
  const auto mac = QMessageAuthenticationCode::hash(parts[0], m_secret, QCryptographicHash::Sha256).toHex();
  if (mac.size() != parts[1].size())
    return false;
  unsigned difference = 0;
  for (qsizetype i = 0; i < mac.size(); ++i)
    difference |= unsigned(uchar(mac[i]) ^ uchar(parts[1][i]));
  if (difference)
    return false;
  const auto body = QJsonDocument::fromJson(QByteArray::fromBase64(parts[0], QByteArray::Base64UrlEncoding)).object();
  if (body.value("v") != QJsonValue(1) || body.value("ref") != reference || body.value("selector") != selector)
    return false;
  const double offset = body.value("offset").toDouble(-1);
  if (offset < 0 || offset != std::floor(offset) || offset > m_limits.resultBytes)
    return false;
  position->offset = int(offset);
  position->mode = body.value("mode").toString();
  return !position->mode.isEmpty();
}

bool ChatResultStore::select(const QJsonValue &root, const QString &selector, QJsonValue *out) {
  *out = root;
  if (selector.isEmpty()) return true;
  if (!selector.startsWith('/') || selector.toUtf8().size() > 1024) return false;
  const auto tokens = selector.mid(1).split('/');
  for (QString token : tokens) {
    for (qsizetype i = 0; i < token.size(); ++i) {
      if (token[i] != '~') continue;
      if (++i >= token.size() || (token[i] != '0' && token[i] != '1')) return false;
    }
    token.replace("~1", "/");
    token.replace("~0", "~");
    if (out->isObject()) {
      const auto object = out->toObject();
      if (!object.contains(token)) return false;
      *out = object.value(token);
    } else if (out->isArray()) {
      bool valid = false;
      const auto index = token.toInt(&valid);
      const auto array = out->toArray();
      if (!valid || QString::number(index) != token || index < 0 || index >= array.size()) return false;
      *out = array[index];
    } else return false;
  }
  return true;
}

QJsonObject ChatResultStore::read(const QJsonObject &request) {
  const auto reference = request.value("result_ref").toString();
  const auto parts = reference.split(':');
  if (parts.size() != 3 || parts[0] != QLatin1String("chat-result-v1"))
    return {{"code", "unsupported_result_ref"}};
  if (parts[1] != m_scope) return {{"code", "result_scope_changed"}};
  auto it = m_values.constFind(reference);
  if (it == m_values.cend())
    return {{"code", m_expired.contains(reference) ? "result_expired" : "unknown_result_ref"}};
  if (it->expires <= m_clock()) return {{"code", "result_expired"}};
  const auto maximum = request.value("max_bytes");
  const double bytes = maximum.isUndefined() ? kResultPageBytes : maximum.toDouble(-1);
  if (bytes < kMinimumEnvelopeBytes || bytes > kResultHardBytes || bytes != std::floor(bytes))
    return {{"code", "invalid_page_budget"}};
  return readValue(request, reference, it->value, int(bytes));
}

QJsonObject ChatResultStore::readValue(const QJsonObject &request, const QString &reference,
                                      const QJsonValue &root, int maxBytes) {
  if (request.contains("selector") && !request.value("selector").isString())
    return {{"code", "invalid_result_selector"}};
  if (request.contains("cursor") && !request.value("cursor").isString())
    return {{"code", "invalid_result_cursor"}};
  if (request.contains("mode") && !request.value("mode").isString())
    return {{"code", "invalid_result_mode"}};
  const auto selector = request.value("selector").toString();
  QJsonValue value;
  if (!select(root, selector, &value)) return {{"code", "invalid_result_selector"}};
  const auto mode = request.value("mode").toString("auto");
  if (mode != QLatin1String("auto") && mode != QLatin1String("text_parts"))
    return {{"code", "invalid_result_mode"}};
  if (mode == QLatin1String("text_parts") && !value.isString())
    return {{"code", "invalid_result_mode"}};
  Position position{0, value.isString() ? QStringLiteral("text_parts") : jsonValueKind(value)};
  const auto token = request.value("cursor").toString();
  const auto expectedMode = position.mode;
  if (!token.isEmpty() && (!decodeCursor(token, reference, selector, &position) || position.mode != expectedMode))
    return {{"code", "invalid_result_cursor"}};
  // Leave room for the model envelope, so read_result cannot create a reference to another reference.
  const int pageBudget = maxBytes - 384;
  if (value.isString()) return stringPage(reference, selector, value.toString(), position, pageBudget);
  if (!value.isArray() && !value.isObject())
    return {{"code", "ok"}, {"kind", jsonValueKind(value)}, {"value", value}, {"hasMore", false}};
  const double pageSize = request.contains("page_size") ? request.value("page_size").toDouble(-1) : 32;
  if (pageSize < 1 || pageSize > 128 || pageSize != std::floor(pageSize))
    return {{"code", "invalid_page_size"}};
  return containerPage(reference, selector, value, position, int(pageSize), pageBudget);
}
} // namespace paleo::chat
