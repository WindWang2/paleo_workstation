// 层：功能
#include "chatinvocationbinding.h"
#include "ai/chat/domaintools.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QSet>

namespace paleo::chat {
namespace {
constexpr int MaxBindings = 4096;
QString digest(const QJsonObject &value) {
  return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(value).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}
bool hash(const QString &value) {
  const auto bytes = value.toLatin1(); return bytes.size() == 64 && QByteArray::fromHex(bytes).toHex() == bytes;
}
bool safeId(const QString &value) {
  if (value.isEmpty() || value.size() > 128) return false;
  for (auto character : value)
    if (!character.isLetterOrNumber() || character.unicode() > 127)
      if (character != '-' && character != '_') return false;
  return true;
}
QString requestId(const QString &session, const QJsonObject &row) {
  return "pi-" + digest({{"sessionId", session}, {"turnId", row.value("turnId")},
      {"callId", row.value("callId")}, {"documentId", row.value("documentId")}});
}
QString key(const QJsonObject &row) {
  return row.value("turnId").toString() + ':' + row.value("callId").toString();
}
bool hasHostRequestId(const QString &tool) {
  return aiToolSpec(tool).toJsonSchema().value("properties").toObject()
      .contains("client_request_id");
}
QString admittedDigest(const QString &tool, const QJsonObject &arguments) {
  return digest({{"name", tool}, {"arguments", arguments}});
}
} // namespace

bool invocationMatchesBinding(const ChatToolCall &call, const QJsonObject &row) {
  if (row.value("callId") != call.id || row.value("toolName") != call.name ||
      !call.argumentsWithinBudget())
    return false;
  const auto parsed = QJsonDocument::fromJson(call.argumentsJson.toUtf8());
  if (!parsed.isObject())
    return false;
  auto arguments = parsed.object();
  if (hasHostRequestId(call.name) &&
      arguments.take("client_request_id") != row.value("clientRequestId"))
    return false;
  return row.value("admittedSha256") == admittedDigest(call.name, arguments);
}

QString validateInvocationBindings(const ChatSession &session) {
  if (!session.hostBindingsValid || !safeId(session.id) || session.hostToolBindings.size() > MaxBindings) return "pi_invocation_binding_invalid";
  QSet<QString> seen;
  for (const auto &value : session.hostToolBindings) {
    const auto row = value.toObject();
    if (row.keys() != QStringList{"admittedSha256", "callId", "clientRequestId", "documentId", "toolName", "turnId"} ||
        !safeId(row.value("turnId").toString()) || !safeId(row.value("callId").toString()) ||
        !row.value("documentId").isString() || row.value("documentId").toString().size() > 256 ||
        !row.value("toolName").isString() || row.value("toolName").toString().size() > 128 ||
        !hash(row.value("admittedSha256").toString()) || row.value("clientRequestId") != requestId(session.id, row) || seen.contains(key(row)))
      return "pi_invocation_binding_invalid";
    seen.insert(key(row));
  }
  return {};
}
QString validateInvocationRetry(const ChatSession &session, qsizetype userIndex) {
  if (userIndex < 0 || userIndex >= session.messages.size() || session.messages[userIndex].role != ChatRole::User)
    return "pi_invocation_turn_unavailable";
  const auto integrity = validateInvocationBindings(session); if (!integrity.isEmpty()) return integrity;
  const auto turn = session.messages[userIndex].hostTurnId;
  for (qsizetype index = userIndex + 1; index < session.messages.size(); ++index)
    for (const auto &call : session.messages[index].toolCalls) {
      const auto spec = aiToolSpec(call.name);
      const auto arguments = QJsonDocument::fromJson(call.argumentsJson.toUtf8()).object();
      const auto effects = spec.effectsForArguments(arguments);
      // Resume may execute a frozen write even though its listing spec is Read;
      // without a binding, use the explicit original-operation recovery path.
      const bool resumes = call.name == QLatin1String("paleo.operation_resume");
      if (!spec.name.isEmpty() && !resumes && effects == unsigned(paleo::host::Read)) continue;
      bool found = false;
      for (const auto &value : session.hostToolBindings) {
        const auto row = value.toObject();
        if (!turn.isEmpty() && row.value("turnId") == turn && row.value("callId") == call.id) { found = true; break; }
      }
      if (!found) return "chat_legacy_retry_requires_operation_recovery";
    }
  return {};
}
QString bindInvocations(ChatSession &session, const QString &turn, const QString &document,
                        QVector<ChatToolCall> &calls) {
  const auto code = validateInvocationBindings(session); if (!code.isEmpty()) return code;
  if (!safeId(turn) || document.size() > 256 || calls.size() > 8) return "pi_invocation_binding_invalid";
  bool knownTurn = false;
  for (const auto &message : session.messages) if (message.role == ChatRole::User && message.hostTurnId == turn) knownTurn = true;
  if (!knownTurn) return "pi_invocation_turn_unavailable";
  auto bindings = session.hostToolBindings; auto normalized = calls; QSet<QString> ids;
  for (auto &call : normalized) {
    if (!safeId(call.id) || ids.contains(call.id) || !call.argumentsWithinBudget() || call.name.size() > 128)
      return "pi_invocation_binding_invalid";
    ids.insert(call.id); QJsonParseError error;
    const auto parsed = QJsonDocument::fromJson(call.argumentsJson.isEmpty() ? QByteArray("{}") : call.argumentsJson.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !parsed.isObject()) return "invalid_tool_arguments";
    auto arguments = parsed.object();
    // The authoritative host schema owns this field. Future persistent tools
    // participate without a second name catalogue; control tools have no field.
    const bool operation = hasHostRequestId(call.name);
    if (operation) arguments.remove("client_request_id");
    QJsonObject row{{"admittedSha256", admittedDigest(call.name, arguments)}, {"callId", call.id},
      {"documentId", document}, {"toolName", call.name}, {"turnId", turn}};
    const auto stable = requestId(session.id, row); row.insert("clientRequestId", stable);
    bool existing = false;
    for (const auto &value : bindings) if (key(value.toObject()) == key(row)) {
      if (value.toObject() != row) return "pi_invocation_binding_conflict"; existing = true; break;
    }
    if (!existing) { if (bindings.size() >= MaxBindings) return "pi_invocation_binding_budget"; bindings.append(row); }
    if (operation) arguments.insert("client_request_id", stable);
    call.argumentsJson = QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact));
    if (!call.argumentsWithinBudget()) return "request_budget_exceeded";
  }
  session.hostToolBindings = bindings; calls = normalized;
  return {};
}
} // namespace paleo::chat
