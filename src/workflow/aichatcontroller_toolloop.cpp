// 层：功能
#include "aichatcontroller.h"
#include "../services/toolresultprotocol.h"
#include "chatinvocationbinding.h"
#include <QDateTime>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>

namespace {
// For the existing character estimator, valid UTF-8 JSON costs at most one estimated token
// per two bytes (including a surrogate pair). This is not a model tokenizer guarantee.
constexpr int kMinimumReplyTokens = (paleo::chat::kMinimumEnvelopeBytes + 1) / 2;
QVector<ChatToolCall> normalizedCalls(const QVector<ChatToolCall> &calls) {
  QVector<ChatToolCall> out;
  QSet<QString> ids;
  for (auto call : calls) {
    if (call.id.isEmpty()) call.id = QUuid::createUuid().toString(QUuid::Id128);
    if (ids.contains(call.id)) continue;
    ids.insert(call.id);
    out.append(call);
  }
  return out;
}
} // namespace

void AiChatController::rejectToolRound(const QString &code, const QString &message) {
  const QPointer<AiChatController> guard(this);
  const auto sequence = m_turnSequence;
  if (m_piBridge.busy()) m_piBridge.modelFailed(code);
  if (!guard || sequence != m_turnSequence) return;
  if (!m_session.messages.isEmpty() && m_session.messages.last().role == ChatRole::Assistant) {
    auto &assistant = m_session.messages.last();
    // This entire frame was refused before execution. It must not enter replay as unpaired calls.
    assistant.toolCalls.clear();
    assistant.content += QStringLiteral("[%1]").arg(code);
  }
  m_pendingToolResults = 0;
  m_unansweredCalls.clear();
  setStreaming(false);
  if (!guard || sequence != m_turnSequence) return;
  emit errorOccurred(message.isEmpty() ? code : message);
  if (!guard || sequence != m_turnSequence) return;
  saveCurrentSession();
  if (!guard || sequence != m_turnSequence) return;
  emit sessionChanged();
}

bool AiChatController::admitToolCalls(const QVector<ChatToolCall> &calls) {
  if (calls.size() > kMaxCallsPerResponse || m_turnCalls + calls.size() > kMaxCallsPerTurn)
    return false;
  const int assistantTokens = estimateTokens(m_session.messages.last().content);
  int cost = int(calls.size()) * kMinimumReplyTokens + assistantTokens;
  for (const auto &call : calls) {
    if (call.argumentsJson.toUtf8().size() > kMaxToolArgumentBytes) return false;
    cost += estimateTokens(call.argumentsJson);
  }
  if (cost > kHistoryTokenBudget - m_toolTokens) return false;
  m_toolTokens += assistantTokens;
  for (const auto &call : calls) m_toolTokens += estimateTokens(call.argumentsJson);
  m_turnCalls += int(calls.size());
  return true;
}

void AiChatController::onRoundFinished(const QString &reason) {
  const QPointer<AiChatController> guard(this);
  const auto sequence = m_turnSequence;
  Q_UNUSED(reason)
  if (!m_streaming) return;
  if (m_roundRejected) {
    rejectToolRound(QStringLiteral("tool_call_budget_exceeded"));
    return;
  }
  const bool wantsTools = !m_session.messages.isEmpty() &&
      m_session.messages.last().role == ChatRole::Assistant &&
      !m_session.messages.last().toolCalls.isEmpty();
  if (!wantsTools) {
    if (m_piBridge.busy()) { m_piBridge.modelFinished({}, reason); return; }
    setStreaming(false);
    if (!guard || sequence != m_turnSequence) return;
    saveCurrentSession();
    return;
  }
  auto calls = normalizedCalls(m_session.messages.last().toolCalls);
  const bool pi = m_piBridge.busy();
  auto bound = m_session;
  if (m_runner.hostContextIdentity() != m_turnContext) { piFailure("interrupted"); return; }
  const auto code = paleo::chat::bindInvocations(bound, m_hostTurn, m_turnContext.value("documentId").toString(), calls);
  if (!code.isEmpty()) { rejectToolRound(code); return; }
  m_session.messages.last().toolCalls = calls;
  if (++m_toolRound > kMaxToolRounds) {
    rejectToolRound(QStringLiteral("tool_round_limit_exceeded"),
        tr("工具往返超过 %1 次上限，已停止本轮（模型反复点工具不给终答）").arg(kMaxToolRounds));
    return;
  }
  if (!admitToolCalls(calls)) {
    rejectToolRound(QStringLiteral("tool_turn_budget_exceeded"));
    return;
  }
  m_unansweredCalls = calls;
  m_pendingToolResults = int(calls.size());
  m_session.hostToolBindings = bound.hostToolBindings;
  const auto session = m_session.id, turn = m_hostTurn; QString error;
  // Both engines use the same durable admitted calls before any tool dispatch.
  if (!saveCurrentSession(&error)) { if (guard) rejectToolRound("pi_invocation_save_failed"); return; }
  if (!guard || !m_streaming || sequence != m_turnSequence || m_session.id != session || m_hostTurn != turn) return;
  if (pi) { m_piBridge.modelFinished(calls, reason); return; }
  emit statusChanged(statusText());
  if (!guard || sequence != m_turnSequence) return;
  // Snapshot: synchronous listeners may cancel/rebind; runner owns serial execution.
  for (const auto &call : calls) {
    if (!guard || !m_streaming || sequence != m_turnSequence) break;
    m_runner.run(call);
  }
}

void AiChatController::onToolFinished(const ChatToolCall &call, bool ok, const QString &resultJson) {
  const QPointer<AiChatController> guard(this); const auto sessionId = m_session.id;
  const auto sequence = m_turnSequence;
  if (m_pendingToolResults <= 0) return;
  int match = -1;
  for (int i = 0; i < m_unansweredCalls.size(); ++i)
    if (m_unansweredCalls[i].id == call.id) { match = i; break; }
  if (match < 0) return;
  const int reserved = (m_pendingToolResults - 1) * kMinimumReplyTokens;
  const int available = kHistoryTokenBudget - m_toolTokens - reserved;
  const int bytes = qBound(paleo::chat::kMinimumEnvelopeBytes, available * 2, paleo::chat::kResultPageBytes);
  const auto envelope = m_runner.modelResult(call, resultJson, ok, bytes);
  ChatMessage tool;
  tool.role = ChatRole::Tool;
  tool.toolCallId = call.id;
  tool.content = QString::fromUtf8(paleo::chat::compactJson(envelope));
  tool.timestamp = QDateTime::currentDateTime();
  m_toolTokens += estimateTokens(tool.content);
  m_unansweredCalls.removeAt(match);
  --m_pendingToolResults;
  const bool pi = m_piBridge.busy();
  // Publish completed state before signals: a listener may cancel or switch sessions synchronously.
  appendMessage(tool);
  if (!guard || sessionId != m_session.id || sequence != m_turnSequence) return;
  const bool piBudgetLimit = pi && m_pendingToolResults == 0 && m_toolTokens >= kHistoryTokenBudget - kMinimumReplyTokens;
  if (pi && m_streaming && !piBudgetLimit) {
    const auto sent = m_piBridge.toolFinished(call, ok, tool.content);
    if (!guard) return;
    if (!sent) { piFailure("pi_tool_result_state"); return; }
  }
  // The UI summary remains independent of the model's transport contract.
  const auto native = QJsonDocument::fromJson(resultJson.toUtf8()).object();
  QString summary = ok ? resultJson : native.value("error").toString(resultJson);
  if (ok && resultJson.size() > 160) {
    int end = 160;
    if (resultJson[end - 1].isHighSurrogate() && resultJson[end].isLowSurrogate()) --end;
    summary = resultJson.left(end) + QStringLiteral("…");
  }
  emit toolResultReady(call, ok, summary);
  if (!guard || sessionId != m_session.id || sequence != m_turnSequence) return;
  if (!m_streaming || m_pendingToolResults > 0) return;
  if (piBudgetLimit) { piFailure("tool_turn_budget_exceeded"); return; }
  if (m_toolTokens >= kHistoryTokenBudget - kMinimumReplyTokens) {
    setStreaming(false);
    if (!guard || sequence != m_turnSequence) return;
    emit errorOccurred(QStringLiteral("tool_turn_budget_exceeded"));
    if (!guard || sequence != m_turnSequence) return;
    saveCurrentSession();
    return;
  }
  if (pi) return;
  sendRound();
}

QVector<ChatMessage> AiChatController::pairUnansweredToolCalls(const QString &reason, const QString &pendingCode) {
  QSet<QString> pending, answered;
  for (const auto &call : m_unansweredCalls) pending.insert(call.id);
  m_pendingToolResults = 0; m_unansweredCalls.clear();
  qsizetype assistant = -1;
  for (qsizetype i = m_session.messages.size(); i > 0; --i) {
    if (m_session.messages[i - 1].role == ChatRole::User) break;
    if (m_session.messages[i - 1].role == ChatRole::Assistant) { assistant = i - 1; break; }
  }
  if (assistant < 0) return {};
  if (m_roundRejected) { m_session.messages[assistant].toolCalls.clear(); return {}; }
  const auto calls = normalizedCalls(m_session.messages[assistant].toolCalls);
  m_session.messages[assistant].toolCalls = calls;
  for (qsizetype i = assistant + 1; i < m_session.messages.size(); ++i)
    if (m_session.messages[i].role == ChatRole::Tool) answered.insert(m_session.messages[i].toolCallId);
  QVector<ChatMessage> completed;
  for (const auto &call : calls) {
    if (answered.contains(call.id)) continue;
    ChatMessage tool;
    tool.role = ChatRole::Tool;
    tool.toolCallId = call.id;
    const auto native = QJsonObject{{"code", pending.contains(call.id) ? pendingCode : QStringLiteral("tool_not_executed")}, {"error", reason}};
    tool.content = QString::fromUtf8(paleo::chat::compactJson(paleo::chat::resultEnvelope(native, false)));
    tool.timestamp = QDateTime::currentDateTime();
    completed.append(tool);
  }
  // The whole paired state precedes every reentrant observer notification.
  for (const auto &tool : completed) m_session.messages.append(tool);
  if (!completed.isEmpty()) m_session.updatedAt = QDateTime::currentDateTime();
  return completed;
}
void AiChatController::handleModelError(LlmErrorKind kind, const QString &message) {
  if (!m_streaming) return;
  const QPointer<AiChatController> guard(this); const auto session = m_session.id;
  const auto sequence = m_turnSequence;
  if (m_piBridge.busy()) m_piBridge.modelFailed(QStringLiteral("llm_error_%1").arg(int(kind)));
  if (!guard || !m_streaming || session != m_session.id) return;
  const auto completed = pairUnansweredToolCalls(tr("请求出错：本调用未执行"), "tool_not_executed");
  if (!m_session.messages.isEmpty() && m_session.messages.last().role == ChatRole::Assistant && m_session.messages.last().isEmpty())
    m_session.messages.removeLast();
  m_streaming = false;
  saveCurrentSession(); if (!guard || m_streaming || session != m_session.id) return;
  emit streamingChanged(false); if (!guard || m_streaming || session != m_session.id) return;
  emit statusChanged(statusText()); if (!guard || m_streaming || session != m_session.id) return;
  for (const auto &tool : completed) { emit messageAppended(tool); if (!guard || m_streaming || sequence != m_turnSequence || session != m_session.id) return; }
  const QString text = llmErrorLabel(kind).isEmpty() ? message : QStringLiteral("%1：%2").arg(llmErrorLabel(kind), message);
  emit errorOccurred(text);
}

void AiChatController::cancel() {
  stopCurrentRound(true);
}
void AiChatController::interrupt() {
  stopCurrentRound(false);
}
void AiChatController::stopCurrentRound(bool explicitRequest) {
  ++m_turnSequence;
  if (!m_streaming) return;
  const QPointer<AiChatController> guard(this); const auto session = m_session.id;
  const auto sequence = m_turnSequence;
  const auto reason=explicitRequest?tr("用户取消：本调用未执行或结果已作废"):QStringLiteral("stale_session");
  const auto completed = pairUnansweredToolCalls(reason, explicitRequest ? "tool_cancelled" : "stale_session");
  m_streaming = false; // Logical state precedes cancellation signals and prevents reentrant old-round work.
  m_piBridge.abort(explicitRequest);
  if(explicitRequest)m_runner.cancel();else m_runner.interrupt();
  if (!guard) return;
  m_client.cancel();
  if (!guard || m_streaming) return;
  if (session != m_session.id) return;
  if (!m_session.messages.isEmpty() && m_session.messages.last().role == ChatRole::Assistant &&
      m_session.messages.last().isEmpty()) m_session.messages.removeLast();
  saveCurrentSession();
  if (!guard || m_streaming || session != m_session.id) return;
  emit streamingChanged(false); if (!guard || m_streaming || session != m_session.id) return;
  emit statusChanged(statusText()); if (!guard || m_streaming || session != m_session.id) return;
  for (const auto &tool : completed) { emit messageAppended(tool); if (!guard || m_streaming || sequence != m_turnSequence || session != m_session.id) return; }
  if(explicitRequest)emit errorOccurred(tr("已取消本轮生成"));
}
