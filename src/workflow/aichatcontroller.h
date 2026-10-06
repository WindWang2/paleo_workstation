// 层：功能
#pragma once
#include "../ai/chat/chatmessage.h"
#include "../ai/chat/chatsession.h"
#include "../ai/chat/domaintools.h"
#include "../ai/chat/llmclient.h"
#include <QObject>
#include <QString>
#include <QVector>

// workflow/ — AI 地质对话助手编排（方向51）。
//
// 分层站位：视图（src/ui/ai/aiassistdock）只发意图、只渲染；编排在这里——
// 会话历史、系统提示、流式增量累积、工具调用帧的分发与「未接线」说明、
// 会话落盘（经 ChatSessionStore，写用户目录，不进工程）。
//
// 诚实面（本方向的硬约束）：
//   · 未配置端点/模型/密钥 → enabled()=false，sendUserText() 直接报错，
//     不假装"已回答"；UI 呈禁用态。
//   · 工具调用帧只做「描述 + 分发结论」，不执行、不回填结果——
//     function calling 闭环递延（TODOS.md）。
class AiChatController : public QObject {
  Q_OBJECT
public:
  explicit AiChatController(QObject *parent = nullptr);

  void setConfig(const LlmConfig &config);
  LlmConfig config() const { return m_config; }
  // 可发请求的唯一判据（端点/模型/密钥齐且地址合法）。
  bool enabled() const { return m_config.enabled(); }
  bool streaming() const { return m_streaming; }
  // 面板状态一行字（禁用原因 / 就绪 / 流式中）。
  QString statusText() const;
  // 系统提示：领域工具表 + 地质解释红线（建议不自动落库）。
  QString systemPrompt() const;
  QVector<AiToolSpec> tools() const { return builtinAiToolSpecs(); }

  ChatSession session() const { return m_session; }
  QVector<ChatMessage> messages() const { return m_session.messages; }
  QVector<ChatSession> recentSessions(int limit = 5) const;

  void newSession();
  bool loadSession(const QString &sessionId, QString *error = nullptr);
  bool removeSession(const QString &sessionId, QString *error = nullptr);
  bool saveCurrentSession(QString *error = nullptr);

  void sendUserText(const QString &text);
  void cancel();

signals:
  void sessionChanged();
  void messageAppended(const ChatMessage &message);
  void assistantDelta(const QString &text); // 流式增量（接到当前 assistant 消息尾）
  void toolCallDispatched(const ChatToolCall &call, const QString &statusText);
  void streamingChanged(bool streaming);
  void statusChanged(const QString &text);
  void errorOccurred(const QString &message);

private:
  void appendMessage(const ChatMessage &message);
  void setStreaming(bool streaming);
  QVector<ChatMessage> requestMessages() const; // system + 历史（不含空占位）

  LlmConfig m_config;
  LlmClient m_client;
  ChatSession m_session;
  bool m_streaming = false;
};
