// 层：功能
#include "aichatcontroller.h"

#include <QDateTime>
#include <QJsonDocument>

// workflow/ — AI 对话助手编排实现（方向51）。

AiChatController::AiChatController(QObject *parent)
  : QObject(parent), m_client(this) {
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
            // 卡片文案 = 分类标签 + 入口 + 原因（诚实：未接线就写未接线）。
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
            setStreaming(false);
            Q_UNUSED(reason)
            Q_UNUSED(promptTokens)
            Q_UNUSED(completionTokens)
            saveCurrentSession(); // 一轮结束即落盘（用户目录，不进工程）
          });
  connect(&m_client, &LlmClient::errorOccurred, this,
          [this](LlmErrorKind kind, const QString &message) {
            setStreaming(false);
            const QString text = llmErrorLabel(kind).isEmpty()
                                   ? message
                                   : QStringLiteral("%1：%2").arg(
                                       llmErrorLabel(kind), message);
            emit errorOccurred(text);
          });
}

QString AiChatController::statusText() const {
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
    "由解释员在工作流里确认后执行，你不能代做。\n"
    "  2. 数据不足就直说数据不足，不得编造井号、层位、深度或数值。\n"
    "  3. 涉及计算时说明所用方法与假设，不确定的地方标注不确定。\n"
    "可用的领域工具（目前只有描述与分发，尚未自动执行）：\n%1")
    .arg(tools);
}

QVector<ChatSession> AiChatController::recentSessions(int limit) const {
  return ChatSessionStore::loadRecent(limit);
}

void AiChatController::newSession() {
  m_session = ChatSession();
  m_session.id = ChatSessionStore::newSessionId();
  m_session.model = m_config.model;
  m_session.createdAt = QDateTime::currentDateTime();
  m_session.updatedAt = m_session.createdAt;
  emit sessionChanged();
}

bool AiChatController::loadSession(const QString &sessionId, QString *error) {
  ChatSession loaded;
  if (!ChatSessionStore::load(sessionId, &loaded, error))
    return false;
  m_session = loaded;
  emit sessionChanged();
  return true;
}

bool AiChatController::removeSession(const QString &sessionId, QString *error) {
  if (sessionId == m_session.id)
    newSession();
  return ChatSessionStore::remove(sessionId, error);
}

bool AiChatController::saveCurrentSession(QString *error) {
  if (m_session.messages.isEmpty())
    return true; // 空会话不落盘（避免用户目录里堆垃圾文件）
  m_session.model = m_config.model;
  m_session.updatedAt = QDateTime::currentDateTime();
  return ChatSessionStore::save(m_session, error);
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

QVector<ChatMessage> AiChatController::requestMessages() const {
  QVector<ChatMessage> out;
  ChatMessage system;
  system.role = ChatRole::System;
  system.content = systemPrompt();
  out.append(system);
  for (const ChatMessage &message : m_session.messages)
    out.append(message);
  return out;
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
  ChatMessage user;
  user.role = ChatRole::User;
  user.content = trimmed;
  user.timestamp = QDateTime::currentDateTime();
  appendMessage(user);

  ChatMessage placeholder; // 流式增量往这条上累积
  placeholder.role = ChatRole::Assistant;
  placeholder.timestamp = QDateTime::currentDateTime();
  appendMessage(placeholder);

  setStreaming(true);
  m_client.setConfig(m_config);
  m_client.send(requestMessages());
}

void AiChatController::cancel() {
  if (!m_streaming)
    return;
  m_client.cancel();
  setStreaming(false);
  // 取消后占位消息若仍为空就摘掉——不给用户留一条空白的"助手回答"。
  if (!m_session.messages.isEmpty() &&
      m_session.messages.last().role == ChatRole::Assistant &&
      m_session.messages.last().isEmpty())
    m_session.messages.removeLast();
  emit errorOccurred(tr("已取消本轮生成"));
}

void AiChatController::setConfig(const LlmConfig &config) {
  m_config = config;
  m_session.model = config.model;
  emit statusChanged(statusText());
}
