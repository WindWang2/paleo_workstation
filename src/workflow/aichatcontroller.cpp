// 层：功能
#include "aichatcontroller.h"
#include "chatinvocationbinding.h"

#include "../services/toolresultprotocol.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QPointer>
#include <QUuid>

// workflow/ — AI 对话助手编排实现（方向51 骨架，方向61 工具闭环）。
//
// 工具闭环状态机（一个用户轮内）：
//   sendUserText → LLM 轮（流式增量/工具帧累积）→ finished：
//     · 末条 assistant 带 toolCalls → 工具相：runner 逐帧执行 → 每结果一条
//       role=tool 消息（toolCallId 对账）→ 全部到齐 → 新 assistant 占位 +
//       再发一轮（带 tools[]），直到模型不再要工具（终答）。
//     · 往返数超 kMaxToolRounds → 准入前整帧拒绝，不执行、不加入后续请求。
//   cancel → 作废（口径见 aichattoolrunner.h）：在途结果不回灌，未应答的
//   tool_call_id 合成「已取消」role=tool 应答——否则历史里 assistant 带
//   calls 而缺应答，下轮请求直接违反协议（OpenAI 400）。

AiChatController::AiChatController(QObject *parent)
  : QObject(parent), m_client(this), m_configCommitter(this), m_runner(this),
    m_piBridge(this) {
  newSession();
  connect(&m_client, &LlmClient::deltaReceived, this,
          [this](const QString &text) {
            if (!m_streaming) return;
            if (m_piBridge.busy() && !m_piBridge.modelDelta(text)) return;
            if (m_session.messages.isEmpty() ||
                m_session.messages.last().role != ChatRole::Assistant)
              return; // 没有占位消息就不往历史里写（防御：协议外增量）
            m_session.messages.last().content += text;
            emit assistantDelta(text);
          });
  connect(&m_client, &LlmClient::toolCallReceived, this,
          [this](const ChatToolCall &call) {
            if (!m_streaming) return;
            ++m_streamToolCalls;
            if (m_streamToolCalls > kMaxCallsPerResponse ||
                call.argumentsJson.toUtf8().size() > kMaxToolArgumentBytes ||
                call.id.size() > 256 || call.name.size() > 128)
              m_roundRejected = true;
            if (m_roundRejected)
              return;
            if (!call.id.isEmpty()) {
              if (m_providerCalls.contains(call.id)) return;
              m_providerCalls.insert(call.id);
            }
            ChatToolCall admitted = hostAdmittedCall(call);
            if (admitted.id.isEmpty())
              admitted.id = QUuid::createUuid().toString(QUuid::Id128);
            if (!m_session.messages.isEmpty() &&
                m_session.messages.last().role == ChatRole::Assistant) {
              const auto &prior = m_session.messages.last().toolCalls;
              for (const auto &existing : prior)
                if (existing.id == admitted.id) return;
              m_session.messages.last().toolCalls.append(admitted);
            }
            AiToolDispatch dispatch;
            if (call.argumentsWithinBudget()) {
              dispatch = dispatchAiTool(call.name,
                                        QJsonDocument::fromJson(call.argumentsJson.toUtf8()).object());
              if(dispatch.status==AiToolDispatchStatus::Routed) {
                const auto ready=m_runner.readiness(call.name,QJsonDocument::fromJson(call.argumentsJson.toUtf8()).object());
                if(!ready.ready) {dispatch.status=AiToolDispatchStatus::Disabled;dispatch.note=ready.code;}
              }
            } else {
              dispatch.status = AiToolDispatchStatus::Disabled;
              dispatch.note = QStringLiteral("request_budget_exceeded");
            }
            // 卡片文案 = 分类标签 + 入口 + 原因（诚实：能不能路由、缺什么都
            // 写明；执行状态由 toolExecutionStarted/toolResultReady 后续更新）。
            const QString text =
              aiToolDispatchLabel(dispatch.status) +
              (dispatch.target.isEmpty() ? QString()
                                         : QStringLiteral(" · %1").arg(dispatch.target)) +
              (dispatch.note.isEmpty() ? QString()
                                       : QStringLiteral(" · %1").arg(dispatch.note));
            emit toolCallDispatched(admitted, text);
          });
  connect(&m_client, &LlmClient::finished, this,
          [this](const QString &reason, int promptTokens, int completionTokens) {
            Q_UNUSED(promptTokens)
            Q_UNUSED(completionTokens)
            onRoundFinished(reason);
          });
  connect(&m_client, &LlmClient::errorOccurred, this,
          [this](LlmErrorKind kind, const QString &message) {
            handleModelError(kind, message);
          });
  connect(&m_runner, &AiChatToolRunner::toolStarted, this,
          [this](const ChatToolCall &call) {
            emit toolExecutionStarted(call);
          });
  connect(&m_runner, &AiChatToolRunner::toolFinished, this,
          [this](const ChatToolCall &call, bool ok, const QString &resultJson) {
            onToolFinished(call, ok, resultJson);
          });
  connect(&m_piBridge, &PiChatBridge::modelRequested, this, [this] { if (m_streaming) sendRound(); else m_piBridge.abort(false); });
  connect(&m_piBridge, &PiChatBridge::toolRequested, this, [this](const ChatToolCall &call) {
    if (!m_streaming || !m_unansweredCalls.contains(call)) { m_piBridge.abort(false); return; }
    bool declared = false; for (const auto &tool : m_piBridge.toolSnapshot()) if (tool.name == call.name) declared = true;
    if (!declared) {
      onToolFinished(call, false, QString::fromUtf8(QJsonDocument(QJsonObject{{"code", "tool_not_declared"},
        {"error", "tool_not_declared"}}).toJson(QJsonDocument::Compact))); return;
    }
    m_runner.run(call);
  });
  connect(&m_piBridge, &PiChatBridge::failed, this, &AiChatController::piFailure);
  connect(&m_piBridge, &PiChatBridge::finished, this, [this](const QString &code) {
    if (!m_streaming) return;
    if (code != "ok") { piFailure(code); return; }
    const QPointer<AiChatController> guard(this); const auto sequence = m_turnSequence;
    setStreaming(false); if (guard && sequence == m_turnSequence) saveCurrentSession();
  });
}

// 协议完整性（见文件头注释）：尾部 assistant 的 tool_calls 若没有对应
// role=tool 应答，下一轮请求就违反「每个 tool_call 必有一条应答」
// （OpenAI 400，且该会话每轮复现，直到该轮滑出历史窗口）。按 reason 合成
// 错误应答补齐缺口——取消 / 请求出错 / 往返超限三条停轮路径共用同一补齐，
// 缺一条都会把协议违规留给后续每一轮。
QString AiChatController::statusText() const {
  if (m_streaming && m_pendingToolResults > 0)
    return tr("正在执行领域工具…");
  if (m_streaming)
    return tr("正在生成回答…");
  const QString invalid = m_config.validate();
  if (!invalid.isEmpty())
    return invalid; // 禁用原因原样给 UI（「尚未配置 API 密钥——助手处于禁用态」等）
  return tr("就绪：%1").arg(m_config.model);
}

QString AiChatController::systemPrompt() const {
  QString tools;
  for (const AiToolSpec &spec : this->tools())
    tools += QStringLiteral("  · %1 — %2\n").arg(spec.name, spec.description);
  QString prompt = tr(
    "你是 Paleo Workbench 里的地质解释助手。回答必须用中文，术语按石油地质/"
    "地震解释行业惯例。\n"
    "红线（不可越过）：\n"
    "  1. 按用户已授权的意图调用工具。查询只读；prepare 和 compute-only 写私有暂存；"
    "publish 会登记成果及呈现图层；测井相预测会向已配置服务传输输入。"
    "工具的 execution、commitState、presentation 分别报告执行、数据提交及呈现事实，"
    "不能把已提交但呈现待修复说成未提交。追踪建议仍须逐条接受后才写解释。\n"
    "  2. 数据不足就直说数据不足，不得编造井号、层位、深度或数值；工具报错时"
    "如实转述错误，不得虚构结果。\n"
    "  3. 涉及计算时说明所用方法与假设，不确定的地方标注不确定。\n"
    "当前可调用的领域工具（如实解读结果及副作用）：\n%1")
    .arg(tools);
  // 方向77：工程概况常驻轻注入（≤3 行，预算口径见 aichattoolrunner 的
  // projectBrief；数据以工具实查为准——这段只是开局的指北针）。
  if (!m_projectBrief.isEmpty())
    prompt += tr("\n当前工程概况（绑定时的 catalog 计数，实查请用 "
                 "paleo.query_project）：\n%1\n")
                .arg(m_projectBrief);
  return prompt;
}

QVector<ChatSession> AiChatController::recentSessions(int limit) const {
  return ChatSessionStore::loadRecent(limit);
}

void AiChatController::newSession() {
  ++m_turnSequence;
  const QPointer<AiChatController> guard(this);
  const auto sequence = m_turnSequence + (m_streaming ? 1 : 0);
  // 切换语义（方向62 钉死）：弃当前会话前先取消在飞请求（迟到增量
  // 不许写进新会话），再落盘——未保存的半轮回答不因「新会话」丢失；
  // 空会话不落盘（saveCurrentSession 内部判空）。
  if (m_streaming)
    interrupt();
  if (!guard || sequence != m_turnSequence) return;
  saveCurrentSession();
  if (!guard || sequence != m_turnSequence) return;
  m_session = ChatSession();
  m_session.id = ChatSessionStore::newSessionId();
  m_runner.setResultSession(m_session.id);
  if (!guard || sequence != m_turnSequence) return;
  m_session.model = m_config.model;
  m_session.createdAt = QDateTime::currentDateTime();
  m_session.updatedAt = m_session.createdAt;
  emit sessionChanged();
  if (guard && sequence == m_turnSequence) emit sessionListChanged();
}

bool AiChatController::loadSession(const QString &sessionId, QString *error) {
  ++m_turnSequence;
  const QPointer<AiChatController> guard(this);
  const auto sequence = m_turnSequence + (m_streaming ? 1 : 0);
  // 切换语义同 newSession：先取消在飞请求（防迟到增量污染目标会话），
  // 再落盘再换，未保存内容不丢。
  if (m_streaming)
    interrupt();
  if (!guard || sequence != m_turnSequence) return false;
  saveCurrentSession();
  if (!guard || sequence != m_turnSequence) return false;
  ChatSession loaded;
  if (!ChatSessionStore::load(sessionId, &loaded, error)) {
    // 失败也要发列表信号：UI 下拉可能已经把显示切到目标会话，
    // 刷新让它回到真实的当前会话（不许显示态与事实脱钩）。
    emit sessionListChanged();
    return false;
  }
  m_session = loaded;
  m_runner.setResultSession(m_session.id);
  if (!guard || sequence != m_turnSequence) return false;
  emit sessionChanged();
  if (guard && sequence == m_turnSequence) emit sessionListChanged();
  return true;
}

bool AiChatController::removeSession(const QString &sessionId, QString *error) {
  const QPointer<AiChatController> guard(this);
  if (sessionId == m_session.id)
    newSession();
  if (!guard) return false;
  const bool ok = ChatSessionStore::remove(sessionId, error);
  if (ok)
    emit sessionListChanged();
  return ok;
}

bool AiChatController::saveCurrentSession(QString *error) {
  if (m_session.messages.isEmpty())
    return true; // 空会话不落盘（避免用户目录里堆垃圾文件）
  m_session.model = m_config.model;
  m_session.updatedAt = QDateTime::currentDateTime();
  const bool ok = ChatSessionStore::save(m_session, error);
  if (ok)
    emit sessionListChanged(); // 标题/updatedAt 变化，列表要刷新
  return ok;
}

bool AiChatController::renameSession(const QString &sessionId,
                                     const QString &title, QString *error) {
  const QString trimmed = title.trimmed();
  if (trimmed.isEmpty()) {
    if (error)
      *error = tr("会话标题不能为空");
    return false;
  }
  if (sessionId == m_session.id) {
    // 当前会话：改内存态；空会话不落盘（守住「空会话不出文件」的既有
    // 纪律——标题跟着首次真实落盘一起持久化）。
    m_session.title = trimmed;
    if (m_session.messages.isEmpty()) {
      emit sessionListChanged();
      return true;
    }
    const bool ok = ChatSessionStore::save(m_session, error);
    if (ok)
      emit sessionListChanged();
    return ok;
  }
  ChatSession stored;
  if (!ChatSessionStore::load(sessionId, &stored, error))
    return false;
  stored.title = trimmed;
  const bool ok = ChatSessionStore::save(stored, error);
  if (ok)
    emit sessionListChanged();
  return ok;
}

bool AiChatController::exportCurrentSession(const QString &filePath,
                                            QString *error) {
  if (filePath.isEmpty()) {
    if (error)
      *error = tr("导出路径为空");
    return false;
  }
  // 先落盘：导出内容与磁盘会话一致（落盘失败如实中断导出，不静默分叉）。
  if (!saveCurrentSession(error))
    return false;
  return ChatSessionStore::exportMarkdown(m_session, filePath, error);
}

void AiChatController::applyConfig(
  const LlmConfig &config, const QByteArray &newKey, bool clearKey,
  QObject *callbackContext, std::function<void(bool, const QString &)> done) {
  // G1（W7）：提交协议收编进 LlmConfigCommitter（串行链 + 文件快照补偿，
  // 细节见 llmconfigcommit.h）。对外签名与语义不变：结构校验失败同步拒绝；
  // 密钥语义保持 newKey 优先 / clearKey 清除 / 沿用当前；成功仍经 setConfig
  // 生效；done(ok) 时配置已生效，callbackContext 先亡则回调静默丢弃。
  m_configCommitter.submit(config, newKey, clearKey, m_config,
                           callbackContext, std::move(done),
                           [this](const LlmConfig &applied) {
                             setConfig(applied);
                           });
}

bool AiChatController::canRetry() const {
  if (m_streaming || m_piBridge.busy())
    return false;
  for (int i = m_session.messages.size() - 1; i >= 0; --i)
    if (m_session.messages.at(i).role == ChatRole::User)
      return true;
  return false;
}

void AiChatController::retryLast() {
  if (!canRetry())
    return;
  if (!enabled()) {
    emit errorOccurred(m_config.validate()); // 与 sendUserText 同一诚实口径
    return;
  }
  const QPointer<AiChatController> guard(this); const auto sequence = ++m_turnSequence; const auto sessionId = m_session.id;
  int lastUser = -1;
  for (int i = m_session.messages.size() - 1; i >= 0; --i) {
    if (m_session.messages.at(i).role == ChatRole::User) {
      lastUser = i;
      break;
    }
  }
  const auto retryCode = paleo::chat::validateInvocationRetry(m_session, lastUser);
  if (!retryCode.isEmpty()) { emit errorOccurred(retryCode); return; }
  // 截掉旧回答（含其携带的工具帧），历史回到「刚发完这条用户消息」。
  m_session.messages.resize(lastUser + 1);
  emit sessionChanged(); // 历史被截断：整页重画（UI 不做增量反演）
  if (!guard || sequence != m_turnSequence || sessionId != m_session.id) return;

  // 与 sendUserText 同口径重置工具往返状态：否则上一轮打满 kMaxToolRounds
  // 后重试，本轮首个工具请求即超限——重试轮被永久禁用工具。
  m_toolRound = 0;
  m_turnCalls = 0;
  m_toolTokens = estimateTokens(systemPrompt()) + qMin(m_config.maxTokens, kHistoryTokenBudget / 2);
  for (int i = m_session.messages.size() - 1; i >= 0; --i)
    if (m_session.messages[i].role == ChatRole::User) {
      m_toolTokens += estimateTokens(m_session.messages[i].content);
      break;
    }
  m_pendingToolResults = 0;
  m_unansweredCalls.clear();
  setStreaming(true);
  if (!guard || sequence != m_turnSequence || !m_streaming || sessionId != m_session.id) return;
  // C-1（full-audit-20261021）：收编进 sendRound——此前手工复制占位+上送
  // 序列时漏了 tools() 实参，重试轮发出的请求不含领域工具，模型静默失去
  // 工具调用能力（与上方注释自述的「重试轮恢复工具」意图相悖）。
  startEngineTurn();
}

void AiChatController::appendMessage(const ChatMessage &message) {
  m_session.messages.append(message);
  m_session.updatedAt = QDateTime::currentDateTime();
  if (m_session.messages.size() == 1 && message.role == ChatRole::User &&
      m_session.title.isEmpty())
    m_session.title = message.content.trimmed().left(40);
  emit messageAppended(message);
}

void AiChatController::setStreaming(bool streaming) {
  if (m_streaming == streaming)
    return;
  m_streaming = streaming;
  const QPointer<AiChatController> guard(this);
  emit streamingChanged(streaming);
  if (!guard) return;
  emit statusChanged(statusText());
}

int AiChatController::estimateTokens(const QString &text) {
  // 字符近似（非精确 tokenizer）：ASCII ≈ 4 字符/token，其余（CJK 为主）
  // ≈ 1 字符/token。只用于历史开窗的保守预算，不进任何用户可见的
  // 「精确 token 数」声明。
  int ascii = 0, wide = 0;
  for (const QChar &c : text) {
    if (c.unicode() < 0x80)
      ++ascii;
    else
      ++wide;
  }
  return ascii / 4 + wide;
}

QVector<ChatMessage> AiChatController::requestMessages() const {
  QVector<ChatMessage> out;
  ChatMessage system;
  system.role = ChatRole::System;
  system.content = systemPrompt();
  out.append(system);

  // 历史按「轮」开窗（一轮 = 一条 User 消息起到下一条 User 前）：协议要求
  // assistant 的 tool_calls 与其后所有 role=tool 应答同进同出，从轮边界切
  // 才不会产生孤儿 tool 消息。窗口 = 近 kMaxHistoryTurns 轮且估算 token 不
  // 超 kHistoryTokenBudget（口径见 estimateTokens）；超预算从最旧整轮丢弃，
  // 并在 system 后插一条截断标记（对模型与请求体断言都可见）。
  QVector<QVector<ChatMessage>> turns;
  for (const ChatMessage &message : m_session.messages) {
    if (message.role == ChatRole::User || turns.isEmpty())
      turns.append(QVector<ChatMessage>());
    turns.last().append(message);
  }
  QVector<QVector<ChatMessage>> kept;
  int budget = kHistoryTokenBudget;
  bool dropped = false;
  for (int i = turns.size() - 1; i >= 0; --i) {
    if (int(kept.size()) < kMaxHistoryTurns) {
      int cost = 0;
      for (const ChatMessage &message : turns[i]) {
        cost += estimateTokens(message.content);
        for (const ChatToolCall &call : message.toolCalls)
          cost += estimateTokens(call.argumentsJson);
      }
      // 最新一轮永远保留（用户问题必须上送），哪怕单轮超预算。
      if (cost <= budget || kept.isEmpty()) {
        budget -= qMin(cost, budget);
        kept.prepend(turns[i]);
        continue;
      }
    }
    dropped = true; // 超轮数上限或预算耗尽：整轮丢弃（从最旧开始）
  }
  if (dropped) {
    ChatMessage marker;
    marker.role = ChatRole::System;
    marker.content = tr("（更早的对话已按上下文预算截断）");
    out.append(marker);
  }
  for (const QVector<ChatMessage> &turn : kept)
    for (ChatMessage message : turn) {
      if (message.role == ChatRole::Tool) {
        const auto value = QJsonDocument::fromJson(message.content.toUtf8()).object();
        if (!paleo::chat::isResultEnvelope(value)) {
          const bool valid = !value.isEmpty();
          paleo::chat::ChatResultStore transient;
          const auto wrapped = valid ? transient.encode(message.content, !value.contains("error"),
                                                        paleo::chat::kResultPageBytes, false)
                                     : paleo::chat::resultError(QStringLiteral("legacy_result_unavailable"));
          message.content = QString::fromUtf8(paleo::chat::compactJson(wrapped));
        }
      }
      out.append(message);
    }
  return out;
}

void AiChatController::sendRound() {
  const QPointer<AiChatController> guard(this); const auto session = m_session.id;
  const auto sequence = m_turnSequence;
  ++m_modelRound;
  m_roundRejected = false;
  m_streamToolCalls = 0;
  m_providerCalls.clear();
  ChatMessage placeholder; // 流式增量往这条上累积
  placeholder.role = ChatRole::Assistant;
  placeholder.timestamp = QDateTime::currentDateTime();
  appendMessage(placeholder);
  if (!guard || !m_streaming || m_session.id != session || sequence != m_turnSequence) return;
  m_client.setConfig(m_config);
  m_client.send(requestMessages(), m_piBridge.busy() ? m_piBridge.toolSnapshot() : tools());
}

void AiChatController::sendUserText(const QString &text) {
  const QString trimmed = text.trimmed();
  if (trimmed.isEmpty())
    return;
  if (!enabled()) {
    // 禁用态：如实报错，不冒充回答。
    emit errorOccurred(m_config.validate());
    return;
  }
  if (m_streaming || m_runner.busy() || m_piBridge.busy()) {
    emit errorOccurred(tr("上一轮仍在进行（生成或工具执行中），请先停止"));
    return;
  }
  const QPointer<AiChatController> guard(this); const auto sequence = ++m_turnSequence; const auto sessionId = m_session.id;
  ChatMessage user;
  user.role = ChatRole::User;
  user.content = trimmed;
  user.timestamp = QDateTime::currentDateTime();
  appendMessage(user);
  if (!guard || sequence != m_turnSequence || sessionId != m_session.id) return;

  m_toolRound = 0;
  m_turnCalls = 0;
  m_toolTokens = estimateTokens(systemPrompt()) + qMin(m_config.maxTokens, kHistoryTokenBudget / 2);
  for (int i = m_session.messages.size() - 1; i >= 0; --i)
    if (m_session.messages[i].role == ChatRole::User) {
      m_toolTokens += estimateTokens(m_session.messages[i].content);
      break;
    }
  m_pendingToolResults = 0;
  m_unansweredCalls.clear();
  setStreaming(true);
  if (!guard || sequence != m_turnSequence || !m_streaming || sessionId != m_session.id) return;
  startEngineTurn();
}

void AiChatController::setConfig(const LlmConfig &config) {
  m_config = config;
  m_session.model = config.model;
  emit statusChanged(statusText());
}
