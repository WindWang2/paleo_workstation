// 层：功能
#include "aichatcontroller.h"

#include "../ai/chat/llmkeystore.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QPointer>

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
