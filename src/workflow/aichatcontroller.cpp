// 层：功能
#include "aichatcontroller.h"

#include "../ai/chat/llmkeystore.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QPointer>

// workflow/ — AI 对话助手编排实现（方向51 骨架，方向61 工具闭环）。
//
// 工具闭环状态机（一个用户轮内）：
//   sendUserText → LLM 轮（流式增量/工具帧累积）→ finished：
//     · 末条 assistant 带 toolCalls → 工具相：runner 逐帧执行 → 每结果一条
//       role=tool 消息（toolCallId 对账）→ 全部到齐 → 新 assistant 占位 +
//       再发一轮（带 tools[]），直到模型不再要工具（终答）。
//     · 往返数超 kMaxToolRounds → 如实报错停轮（防模型无限点工具）。
//   cancel → 作废（口径见 aichattoolrunner.h）：在途结果不回灌，未应答的
//   tool_call_id 合成「已取消」role=tool 应答——否则历史里 assistant 带
//   calls 而缺应答，下轮请求直接违反协议（OpenAI 400）。

AiChatController::AiChatController(QObject *parent)
  : QObject(parent), m_client(this), m_runner(this) {
  newSession();
  connect(&m_client, &LlmClient::deltaReceived, this,
          [this](const QString &text) {
            if (m_session.messages.isEmpty() ||
                m_session.messages.last().role != ChatRole::Assistant)
              return; // 没有占位消息就不往历史里写（防御：协议外增量）
            m_session.messages.last().content += text;
            emit assistantDelta(text);
          });
  connect(&m_client, &LlmClient::toolCallReceived, this,
          [this](const ChatToolCall &call) {
            if (!m_session.messages.isEmpty() &&
                m_session.messages.last().role == ChatRole::Assistant)
              m_session.messages.last().toolCalls.append(call);
            const AiToolDispatch dispatch =
              dispatchAiTool(call.name,
                             QJsonDocument::fromJson(call.argumentsJson.toUtf8())
                               .object());
            // 卡片文案 = 分类标签 + 入口 + 原因（诚实：能不能路由、缺什么都
            // 写明；执行状态由 toolExecutionStarted/toolResultReady 后续更新）。
            const QString text =
              aiToolDispatchLabel(dispatch.status) +
              (dispatch.target.isEmpty() ? QString()
                                         : QStringLiteral(" · %1").arg(dispatch.target)) +
              (dispatch.note.isEmpty() ? QString()
                                       : QStringLiteral(" · %1").arg(dispatch.note));
            emit toolCallDispatched(call, text);
          });
  connect(&m_client, &LlmClient::finished, this,
          [this](const QString &reason, int promptTokens, int completionTokens) {
            Q_UNUSED(promptTokens)
            Q_UNUSED(completionTokens)
            onRoundFinished(reason);
          });
  connect(&m_client, &LlmClient::errorOccurred, this,
          [this](LlmErrorKind kind, const QString &message) {
            setStreaming(false);
            m_pendingToolResults = 0;
            m_unansweredCalls.clear();
            const QString text = llmErrorLabel(kind).isEmpty()
                                   ? message
                                   : QStringLiteral("%1：%2").arg(
                                       llmErrorLabel(kind), message);
            emit errorOccurred(text);
          });
  connect(&m_runner, &AiChatToolRunner::toolStarted, this,
          [this](const ChatToolCall &call) {
            emit toolExecutionStarted(call);
          });
  connect(&m_runner, &AiChatToolRunner::toolFinished, this,
          [this](const ChatToolCall &call, bool ok, const QString &resultJson) {
            onToolFinished(call, ok, resultJson);
          });
}

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
  for (const AiToolSpec &spec : builtinAiToolSpecs())
    tools += QStringLiteral("  · %1 — %2\n").arg(spec.name, spec.description);
  return tr(
    "你是 Paleo Workbench 里的地质解释助手。回答必须用中文，术语按石油地质/"
    "地震解释行业惯例。\n"
    "红线（不可越过）：\n"
    "  1. 你给出的是**建议**，不是结论——任何落库、改参数、写解释的动作都必须"
    "由解释员在工作流里确认后执行，你不能代做。你调用的领域工具同样只产出"
    "草稿态结果（派生资产登记为 DERIVED 草稿、追踪建议进待裁决队列），"
    "不会自动写任何解释。\n"
    "  2. 数据不足就直说数据不足，不得编造井号、层位、深度或数值；工具报错时"
    "如实转述错误，不得虚构结果。\n"
    "  3. 涉及计算时说明所用方法与假设，不确定的地方标注不确定。\n"
    "可调用的领域工具（结果会以草稿态回传给你，由你向解释员解读）：\n%1")
    .arg(tools);
}

QVector<ChatSession> AiChatController::recentSessions(int limit) const {
  return ChatSessionStore::loadRecent(limit);
}

void AiChatController::newSession() {
  // 切换语义（方向62 钉死）：弃当前会话前先取消在飞请求（迟到增量
  // 不许写进新会话），再落盘——未保存的半轮回答不因「新会话」丢失；
  // 空会话不落盘（saveCurrentSession 内部判空）。
  if (m_streaming)
    cancel();
  saveCurrentSession();
  m_session = ChatSession();
  m_session.id = ChatSessionStore::newSessionId();
  m_session.model = m_config.model;
  m_session.createdAt = QDateTime::currentDateTime();
  m_session.updatedAt = m_session.createdAt;
  emit sessionChanged();
  emit sessionListChanged();
}

bool AiChatController::loadSession(const QString &sessionId, QString *error) {
  // 切换语义同 newSession：先取消在飞请求（防迟到增量污染目标会话），
  // 再落盘再换，未保存内容不丢。
  if (m_streaming)
    cancel();
  saveCurrentSession();
  ChatSession loaded;
  if (!ChatSessionStore::load(sessionId, &loaded, error)) {
    // 失败也要发列表信号：UI 下拉可能已经把显示切到目标会话，
    // 刷新让它回到真实的当前会话（不许显示态与事实脱钩）。
    emit sessionListChanged();
    return false;
  }
  m_session = loaded;
  emit sessionChanged();
  emit sessionListChanged();
  return true;
}

bool AiChatController::removeSession(const QString &sessionId, QString *error) {
  if (sessionId == m_session.id)
    newSession();
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
  // 结构校验（地址/模型）：密钥单查——密钥缺失允许保存（助手保持禁用态，
  // 状态行如实说明），地址/模型非法则不落盘。
  LlmConfig probe = config;
  probe.apiKey = QByteArrayLiteral("probe-placeholder");
  const QString structural = probe.validate();
  if (!structural.isEmpty()) {
    done(false, structural);
    return;
  }
  LlmConfig next = config;
  QString error;
  if (!next.save(&error)) {
    done(false, tr("配置写入失败：%1").arg(error));
    return;
  }
  // 密钥语义：newKey 优先（显式输入新密钥 = 替换，clear 冗余忽略）；
  // clearKey 且无新密钥 = 清除；都无 = 沿用内存态密钥。
  // 回调经 QPointer 双守卫：callbackContext（钥匙串侧）+ 控制器自身
  // （关停次序反转时任一侧先走都不悬垂）。
  QPointer<AiChatController> self(this);
  if (!newKey.isEmpty()) {
    LlmKeyStore::write(callbackContext, newKey,
                       [self, next, newKey, done](bool ok,
                                                  const QString &err) mutable {
                         if (!self)
                           return;
                         if (!ok) {
                           done(false, self->tr("密钥写入钥匙串失败：%1").arg(err));
                           return;
                         }
                         next.apiKey = newKey;
                         self->setConfig(next);
                         done(true, QString());
                       });
    return;
  }
  if (clearKey) {
    LlmKeyStore::clear(callbackContext,
                       [self, next, done](bool ok,
                                          const QString &err) mutable {
                         if (!self)
                           return;
                         if (!ok) {
                           done(false, self->tr("清除密钥失败：%1").arg(err));
                           return;
                         }
                         next.apiKey = QByteArray();
                         self->setConfig(next);
                         done(true, QString());
                       });
    return;
  }
  next.apiKey = m_config.apiKey; // 密钥维持现状
  setConfig(next);
  done(true, QString());
}

bool AiChatController::canRetry() const {
  if (m_streaming)
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
  int lastUser = -1;
  for (int i = m_session.messages.size() - 1; i >= 0; --i) {
    if (m_session.messages.at(i).role == ChatRole::User) {
      lastUser = i;
      break;
    }
  }
  // 截掉旧回答（含其携带的工具帧），历史回到「刚发完这条用户消息」。
  m_session.messages.resize(lastUser + 1);
  emit sessionChanged(); // 历史被截断：整页重画（UI 不做增量反演）

  ChatMessage placeholder; // 流式增量往这条上累积（与 sendUserText 同形）
  placeholder.role = ChatRole::Assistant;
  placeholder.timestamp = QDateTime::currentDateTime();
  appendMessage(placeholder);

  setStreaming(true);
  m_client.setConfig(m_config);
  m_client.send(requestMessages());
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
  emit streamingChanged(streaming);
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

QString AiChatController::clampToolResult(const QString &content) {
  if (content.size() <= kToolResultCharLimit)
    return content;
  return content.left(kToolResultCharLimit) +
         tr("…[结果超长已截断：原始 %1 字符]").arg(content.size());
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
    out.append(turn);
  return out;
}

void AiChatController::sendRound() {
  ChatMessage placeholder; // 流式增量往这条上累积
  placeholder.role = ChatRole::Assistant;
  placeholder.timestamp = QDateTime::currentDateTime();
  appendMessage(placeholder);
  m_client.setConfig(m_config);
  m_client.send(requestMessages(), tools());
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
  if (m_streaming || m_runner.busy()) {
    emit errorOccurred(tr("上一轮仍在进行（生成或工具执行中），请先停止"));
    return;
  }
  ChatMessage user;
  user.role = ChatRole::User;
  user.content = trimmed;
  user.timestamp = QDateTime::currentDateTime();
  appendMessage(user);

  m_toolRound = 0;
  m_pendingToolResults = 0;
  m_unansweredCalls.clear();
  setStreaming(true);
  sendRound();
}

void AiChatController::onRoundFinished(const QString &reason) {
  Q_UNUSED(reason)
  if (!m_streaming)
    return; // 取消/出错后的迟到收尾：不再驱动状态机
  const bool wantsTools =
    !m_session.messages.isEmpty() &&
    m_session.messages.last().role == ChatRole::Assistant &&
    !m_session.messages.last().toolCalls.isEmpty();
  if (!wantsTools) {
    setStreaming(false);
    saveCurrentSession(); // 一轮结束即落盘（用户目录，不进工程）
    return;
  }
  if (++m_toolRound > kMaxToolRounds) {
    setStreaming(false);
    emit errorOccurred(tr("工具往返超过 %1 次上限，已停止本轮（模型反复点"
                          "工具不给终答）").arg(kMaxToolRounds));
    saveCurrentSession();
    return;
  }
  // 工具相：streaming 保持 true（输入框继续锁定，停止按钮可取消），状态行
  // 切「执行中」；逐帧入队，串行执行（runner 的正确性约束）。
  m_unansweredCalls = m_session.messages.last().toolCalls;
  m_pendingToolResults = int(m_unansweredCalls.size());
  emit statusChanged(statusText());
  for (const ChatToolCall &call : m_unansweredCalls)
    m_runner.run(call);
}

void AiChatController::onToolFinished(const ChatToolCall &call, bool ok,
                                      const QString &resultJson) {
  if (m_pendingToolResults <= 0)
    return; // 非本轮在途结果（取消/换轮后的迟到帧）：丢弃
  ChatMessage tool;
  tool.role = ChatRole::Tool;
  tool.toolCallId = call.id;
  tool.content = clampToolResult(resultJson);
  tool.timestamp = QDateTime::currentDateTime();
  appendMessage(tool);
  for (int i = 0; i < m_unansweredCalls.size(); ++i)
    if (m_unansweredCalls[i].id == call.id)
      m_unansweredCalls.removeAt(i);
  // 卡片摘要：成功给首 160 字符；失败给 error 字段原文（不吞）。
  QString summary;
  if (ok) {
    summary = resultJson.size() > 160 ? resultJson.left(160) + QStringLiteral("…")
                                      : resultJson;
  } else {
    const QJsonObject payload =
      QJsonDocument::fromJson(resultJson.toUtf8()).object();
    summary = payload.value(QStringLiteral("error")).toString(resultJson);
  }
  emit toolResultReady(call, ok, summary);
  if (--m_pendingToolResults > 0)
    return;
  // 全部应答到齐：续轮（新 assistant 占位 + 带 tools 再发）。
  sendRound();
}

void AiChatController::cancel() {
  if (!m_streaming)
    return;
  // 取消 = 作废：先作废执行器（在途结果不再回灌；tile 任务协作取消不写
  // 产品），再中止 LLM 请求，最后给未应答的帧补「已取消」应答（协议完整
  // 性，见文件头注释）。
  m_runner.cancel();
  m_client.cancel();
  while (!m_unansweredCalls.isEmpty()) {
    const ChatToolCall call = m_unansweredCalls.takeFirst();
    QJsonObject payload;
    payload.insert(QStringLiteral("tool"), call.name); // 模型可控：经 JSON 序列化转义
    payload.insert(QStringLiteral("error"),
                   tr("用户取消：本调用未执行或结果已作废"));
    ChatMessage tool;
    tool.role = ChatRole::Tool;
    tool.toolCallId = call.id;
    tool.content = QString::fromUtf8(
      QJsonDocument(payload).toJson(QJsonDocument::Compact));
    tool.timestamp = QDateTime::currentDateTime();
    appendMessage(tool);
  }
  m_pendingToolResults = 0;
  setStreaming(false);
  // 取消后占位消息若仍为空就摘掉——不给用户留一条空白的"助手回答"。
  // （工具相里占位带 toolCalls，非空，保留——历史需要它对账 tool_call_id。）
  if (!m_session.messages.isEmpty() &&
      m_session.messages.last().role == ChatRole::Assistant &&
      m_session.messages.last().isEmpty())
    m_session.messages.removeLast();
  saveCurrentSession();
  emit errorOccurred(tr("已取消本轮生成"));
}

void AiChatController::setConfig(const LlmConfig &config) {
  m_config = config;
  m_session.model = config.model;
  emit statusChanged(statusText());
}
