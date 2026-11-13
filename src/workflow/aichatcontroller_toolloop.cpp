// 层：功能
#include "aichatcontroller.h"
#include "../services/toolresultprotocol.h"
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

void AiChatController::rejectToolRound(const QString &code) {
  if (!m_session.messages.isEmpty() && m_session.messages.last().role == ChatRole::Assistant) {
    auto &message = m_session.messages.last();
    // This entire frame was refused before execution. It must not enter replay as unpaired calls.
    message.toolCalls.clear();
    message.content += QStringLiteral("[%1]").arg(code);
  }
  m_pendingToolResults = 0;
  m_unansweredCalls.clear();
  setStreaming(false);
  emit errorOccurred(code);
  saveCurrentSession();
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
    setStreaming(false);
    saveCurrentSession();
    return;
  }
  auto calls = normalizedCalls(m_session.messages.last().toolCalls);
  m_session.messages.last().toolCalls = calls;
  if (++m_toolRound > kMaxToolRounds) {
    padUnansweredToolCalls(tr("工具往返超过 %1 次上限，本调用未执行").arg(kMaxToolRounds));
    setStreaming(false);
    emit errorOccurred(tr("工具往返超过 %1 次上限，已停止本轮（模型反复点工具不给终答）").arg(kMaxToolRounds));
    saveCurrentSession();
    return;
  }
  if (!admitToolCalls(calls)) {
    rejectToolRound(QStringLiteral("tool_turn_budget_exceeded"));
    return;
  }
  m_unansweredCalls = calls;
  m_pendingToolResults = int(calls.size());
  emit statusChanged(statusText());
  // Snapshot: synchronous listeners may cancel/rebind; runner owns serial execution.
  for (const auto &call : calls) {
    if (!m_streaming) break;
    m_runner.run(call);
  }
}

void AiChatController::onToolFinished(const ChatToolCall &call, bool ok, const QString &resultJson) {
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
  // Publish completed state before signals: a listener may cancel or switch sessions synchronously.
  appendMessage(tool);
  // The UI summary remains independent of the model's transport contract.
  const auto native = QJsonDocument::fromJson(resultJson.toUtf8()).object();
  QString summary = ok ? resultJson : native.value("error").toString(resultJson);
  if (ok && resultJson.size() > 160) {
    int end = 160;
    if (resultJson[end - 1].isHighSurrogate() && resultJson[end].isLowSurrogate()) --end;
    summary = resultJson.left(end) + QStringLiteral("…");
  }
  emit toolResultReady(call, ok, summary);
  if (!m_streaming || m_pendingToolResults > 0) return;
  if (m_toolTokens >= kHistoryTokenBudget - kMinimumReplyTokens) {
    setStreaming(false);
    emit errorOccurred(QStringLiteral("tool_turn_budget_exceeded"));
    saveCurrentSession();
    return;
  }
  sendRound();
}

void AiChatController::padUnansweredToolCalls(const QString &reason) {
  if (m_session.messages.isEmpty() || m_session.messages.last().role != ChatRole::Assistant)
    return;
  if (m_roundRejected) {
    m_session.messages.last().toolCalls.clear();
    return;
  }
  const auto calls = normalizedCalls(m_session.messages.last().toolCalls);
  m_session.messages.last().toolCalls = calls;
  for (const auto &call : calls) {
    ChatMessage tool;
    tool.role = ChatRole::Tool;
    tool.toolCallId = call.id;
    const auto native = QJsonObject{{"code", "tool_not_executed"}, {"error", reason}};
    tool.content = QString::fromUtf8(paleo::chat::compactJson(paleo::chat::resultEnvelope(native, false)));
    tool.timestamp = QDateTime::currentDateTime();
    appendMessage(tool);
  }
}

void AiChatController::cancel() {
  if (!m_streaming) return;
  m_runner.cancel();
  m_client.cancel();
  while (!m_unansweredCalls.isEmpty()) {
    const auto call = m_unansweredCalls.takeFirst();
    ChatMessage tool;
    tool.role = ChatRole::Tool;
    tool.toolCallId = call.id;
    const auto native = QJsonObject{{"code", "tool_cancelled"},
        {"error", tr("用户取消：本调用未执行或结果已作废")}};
    tool.content = QString::fromUtf8(paleo::chat::compactJson(paleo::chat::resultEnvelope(native, false)));
    tool.timestamp = QDateTime::currentDateTime();
    appendMessage(tool);
  }
  padUnansweredToolCalls(tr("用户取消：本调用未执行或结果已作废"));
  m_pendingToolResults = 0;
  setStreaming(false);
  if (!m_session.messages.isEmpty() && m_session.messages.last().role == ChatRole::Assistant &&
      m_session.messages.last().isEmpty()) m_session.messages.removeLast();
  saveCurrentSession();
  emit errorOccurred(tr("已取消本轮生成"));
}
