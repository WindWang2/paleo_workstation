// 层：功能
#pragma once
#include "../ai/chat/chatmessage.h"
#include "../ai/chat/chatsession.h"
#include "../ai/chat/domaintools.h"
#include "../ai/chat/llmclient.h"
#include <QObject>
#include <QString>
#include <QVector>
#include <functional>

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
  // 方向62：图形化配置的应用编排（对话框只出表单值，落盘/钥匙串在这里）。
  //   · config：表单值（端点/模型/开关等，密钥字段无效——密钥走 newKey/clearKey）
  //   · newKey 非空 = 写入钥匙串并启用；clearKey 且 newKey 空 = 清除；
  //     两者都空 = 密钥维持现状（沿用内存态）。
  //   · done(ok, error)：在 callbackContext 仍存活时回调（钥匙串异步）；
  //     ok 时 m_config 已更新，error 为空。
  void applyConfig(const LlmConfig &config, const QByteArray &newKey,
                   bool clearKey, QObject *callbackContext,
                   std::function<void(bool ok, const QString &error)> done);
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
  // 方向62：会话重命名（任意会话，按 id；改标题不改消息）。
  bool renameSession(const QString &sessionId, const QString &title,
                     QString *error = nullptr);
  // 方向62：导出当前会话为 .md（显式路径；导出前先落盘，保证与磁盘一致）。
  bool exportCurrentSession(const QString &filePath, QString *error = nullptr);
  // 方向62：可重发的唯一判据——不在流式中且历史里有用户消息。
  bool canRetry() const;
  // 方向62：重发最后一条用户消息——截掉其后的助手应答（含工具帧），
  // 按既有历史重新发起请求（不重复追加用户消息）。
  void retryLast();

  void sendUserText(const QString &text);
  void cancel();

signals:
  void sessionChanged();
  // 会话列表/标题/排序变化（打开/删除/重命名/落盘后；UI 侧刷新下拉）。
  void sessionListChanged();
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
