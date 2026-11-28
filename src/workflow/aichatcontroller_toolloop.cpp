// 层：功能
#include "aichatcontroller.h"
#include "../services/toolresultprotocol.h"
#include "chatinvocationbinding.h"
#include "toolreplybudget.h"
#include <QDateTime>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>
#include <algorithm>

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

// ---- G2（回合执行闭环）：冻结快照取值与保存欠账结算 ----

QString AiChatController::currentSessionModel() const {
  // 记录字段 = 上一轮实际开场的模型（startEngineTurn 定格，含该轮收尾保存）；
  // 下一轮重新定格前不跟随中途 setConfig 漂移——记录永远如实反映「这轮
  // 对话用哪个模型跑的」。未冻结（构造后首轮前）跟随当前配置。
  return m_turnFrozen ? m_turnConfig.model : m_config.model;
}

bool AiChatController::settleSaveArrears(QString *error) {
  // 无欠账直接放行；有欠账先真实落盘一次（不是只清标志）。
  if (!m_saveArrears || m_session.messages.isEmpty()) return true;
  return saveCurrentSession(error);
}

void AiChatController::failTurnForSaveArrears(const QString &error) {
  // 统一保存失败终态：欠账已由 saveCurrentSession 记账；这里只结算本轮——
  // 作废未开始的队列与续轮、补齐未应答帧、单次错误上报。已收到的真实结果
  // 与已提交操作事实不被改写、不改判 tool_failed（执行事实 ≠ 会话写成功）。
  const QPointer<AiChatController> guard(this);
  const auto sequence = m_turnSequence + 1;
  stopCurrentRound(false); // 宿主中断语义：保留可恢复的操作意图
  if (!guard || sequence != m_turnSequence) return;
  emit errorOccurred(tr("工具结果已收到，但会话保存失败（%1）。本轮已停止，内容保留在内存；请恢复存储后重发").arg(error));
}

void AiChatController::rejectToolRound(const QString &code, const QString &message) {
  const QPointer<AiChatController> guard(this);
  const auto sequence = m_turnSequence;
  m_piBridge.modelFailed(code);
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

QString AiChatController::negotiatedToolError(const ChatToolCall &call) const {
  const auto named = [&call](const AiToolSpec &spec) { return spec.name == call.name; };
  const auto declared = std::find_if(m_turnTools.cbegin(), m_turnTools.cend(), named);
  if (declared == m_turnTools.cend()) return QStringLiteral("tool_not_declared");
  if (call.name != QLatin1String("paleo.query_project")) return {};
  const auto args = QJsonDocument::fromJson(call.argumentsJson.toUtf8()).object();
  const auto topic = args.value("topic").toString();
  const auto topicParameter = std::find_if(declared->parameters.cbegin(), declared->parameters.cend(),
      [](const AiToolParamSpec &parameter) { return parameter.name == QLatin1String("topic"); });
  // The runner still diagnoses invalid topic values. This check is only for a
  // valid topic excluded from the schema offered when this turn started.
  if (topicParameter != declared->parameters.cend() && !topicParameter->enumValues.contains(topic) &&
      aiToolSpec(call.name).validateParameters(args).isEmpty())
    return QStringLiteral("query_topic_not_declared");
  return {};
}

void AiChatController::dispatchTurnTool(ChatToolCall call) {
  if (!ensureTurnContext()) return;
  // H7-W2：Pi 请求执行已准入的调用。派发即按当前回合
  // 额度冻结本次调用的最终回包配额，随 call 交给 runner；不预读、不消耗
  // 下一调用的份额，也不改 admitToolCalls 的准入与保存顺序。
  const int quota = paleo::chat::replyQuotaBytes(
      m_pendingToolResults, m_toolTokens, kHistoryTokenBudget, kMinimumReplyTokens);
  m_runner.run(call, quota);
}

void AiChatController::onRoundFinished(const QString &reason) {
  const QPointer<AiChatController> guard(this);
  const auto sequence = m_turnSequence;
  if (!ensureTurnContext()) return;
  if (reason == QLatin1String("length") || reason == QLatin1String("content_filter")) {
    // A parseable argument fragment does not make a truncated or filtered
    // response safe to execute. Preserve any partial text with its stop reason.
    rejectToolRound(reason == QLatin1String("length") ? QStringLiteral("model_output_truncated")
                                                       : QStringLiteral("model_output_filtered"));
    return;
  }
  if (m_roundRejected) {
    rejectToolRound(QStringLiteral("tool_call_budget_exceeded"));
    return;
  }
  const bool wantsTools = !m_session.messages.isEmpty() &&
      m_session.messages.last().role == ChatRole::Assistant &&
      !m_session.messages.last().toolCalls.isEmpty();
  if (!wantsTools) {
    if (!m_piBridge.modelFinished({}, reason) && guard && m_streaming && sequence == m_turnSequence)
      failTurn(QStringLiteral("pi_model_completion_rejected"));
    return;
  }
  auto calls = normalizedCalls(m_session.messages.last().toolCalls);
  for (const auto &call : calls) {
    const auto error = negotiatedToolError(call);
    if (!error.isEmpty()) { rejectToolRound(error); return; }
  }
  auto bound = m_session;
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
  // Checkpoint the admitted calls before Pi can request their execution.
  if (!saveCurrentSession(&error)) { if (guard) rejectToolRound("pi_invocation_save_failed"); return; }
  if (!guard || sequence != m_turnSequence || m_session.id != session || m_hostTurn != turn ||
      !ensureTurnContext()) return;
  emit statusChanged(statusText());
  if (!guard || sequence != m_turnSequence) return;
  if (!m_piBridge.modelFinished(calls, reason) && guard && m_streaming && sequence == m_turnSequence)
    failTurn(QStringLiteral("pi_model_completion_rejected"));
}

void AiChatController::onToolFinished(const ChatToolCall &call, bool ok, const QString &resultJson) {
  const QPointer<AiChatController> guard(this); const auto sessionId = m_session.id;
  const auto sequence = m_turnSequence;
  if (m_pendingToolResults <= 0) { m_runner.discardDeliveredReadResult(call.id); return; }
  int match = -1;
  for (int i = 0; i < m_unansweredCalls.size(); ++i)
    if (m_unansweredCalls[i].id == call.id) { match = i; break; }
  if (match < 0) { m_runner.discardDeliveredReadResult(call.id); return; }
  // H7-W2：最终化用同一纯值公式重算可用额度，交给 runner 复核「本次读页
  // 实际采用的 envelope 预算」。页生成时的交集（请求上限 vs 宿主配额）由
  // runner 记录，最终化不重选页、不重跑业务；若额度被压小，runner 返回
  // tool_reply_budget_changed，且不会把成功页降级成无 data/无 cursor。
  const int bytes = paleo::chat::replyQuotaBytes(
      m_pendingToolResults, m_toolTokens, kHistoryTokenBudget, kMinimumReplyTokens);
  const auto envelope = m_runner.modelResult(call, resultJson, ok, bytes);
  // 交付失败与 native tool_finished、Pi 外层 ok 和 UI 卡片必须是同一事实。
  // 任何工具的载荷被 transport 剥掉（读页预算、缓存满/过大，以及同一函数里
  // 去掉 data 与 resultRefs 的其它码）都不能仍报 ok=true。工具自己的成败留在
  // envelope 的 status/code，这里不改写。内联 data 或 resultRefs 仍算送达。
  const bool deliveryFailed = ok && !paleo::chat::toolResultPayloadDelivered(envelope);
  const bool deliveredOk = ok && !deliveryFailed;
  const QString transportCode = envelope.value(QStringLiteral("transport")).toObject()
                                    .value(QStringLiteral("code")).toString();
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
  if (!guard || sessionId != m_session.id || sequence != m_turnSequence) return;
  // G2-W2 提交点：真实结果先落盘（含此前已准入的全部状态），再回送 Pi 或发
  // 下一次模型请求——保存失败即停本轮：未开始的队列与续轮全部止步，已收到
  // 的结果保留在内存与欠账状态，不改判 tool_failed、不重执行工具补偿。
  QString saveError;
  if (!saveCurrentSession(&saveError)) {
    if (!guard || sessionId != m_session.id || sequence != m_turnSequence) return;
    failTurnForSaveArrears(saveError);
    return;
  }
  if (!guard || sessionId != m_session.id || sequence != m_turnSequence) return;
  const bool budgetExhausted = m_pendingToolResults == 0 && m_toolTokens >= kHistoryTokenBudget - kMinimumReplyTokens;
  if (m_streaming && !budgetExhausted) {
    const auto sent = m_piBridge.toolFinished(call, deliveredOk, tool.content);
    if (!guard) return;
    if (!sent) { failTurn("pi_tool_result_state"); return; }
  }
  // The UI summary remains independent of the model's transport contract.
  const auto native = QJsonDocument::fromJson(resultJson.toUtf8()).object();
  QString summary = deliveredOk ? resultJson
      : (deliveryFailed ? (transportCode.isEmpty() ? QStringLiteral("result_not_delivered")
                                                   : transportCode)
                        : native.value("error").toString(resultJson));
  if (deliveredOk && resultJson.size() > 160) {
    int end = 160;
    if (resultJson[end - 1].isHighSurrogate() && resultJson[end].isLowSurrogate()) --end;
    summary = resultJson.left(end) + QStringLiteral("…");
  }
  emit toolResultReady(call, deliveredOk, summary);
  if (!guard || sessionId != m_session.id || sequence != m_turnSequence) return;
  if (budgetExhausted && m_streaming) failTurn("tool_turn_budget_exceeded");
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
  m_piBridge.modelFailed(QStringLiteral("llm_error_%1").arg(int(kind)));
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
  if (!guard) return;
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
