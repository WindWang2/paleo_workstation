// 层：功能
#pragma once
#include "../ai/chat/chatmessage.h"
#include "../ai/chat/chatsession.h"
#include "../ai/chat/domaintools.h"
#include "../ai/chat/llmclient.h"
#include "aichattoolrunner.h"
#include <QObject>
#include <QString>
#include <QVector>
#include <functional>

// workflow/ — AI 地质对话助手编排（方向51 骨架，方向61 工具闭环）。
//
// 分层站位：视图（src/ui/ai/aiassistdock）只发意图、只渲染；编排在这里——
// 会话历史、系统提示、流式增量累积、工具调用帧的执行编排（经
// AiChatToolRunner 转既有执行面）、结果按 role=tool 回灌下轮请求、
// 会话落盘（经 ChatSessionStore，写用户目录，不进工程）。
//
// 诚实面（硬约束）：
//   · 未配置端点/模型/密钥 → enabled()=false，sendUserText() 直接报错，
//     不假装"已回答"；UI 呈禁用态。
//   · 工具执行失败 / 未绑定上下文 → 错误结果如实回灌（{"error": ...}），
//     UI 呈失败卡片——不冒充成功。
//   · token/上下文预算是字符近似口径（非精确 tokenizer），注释钉死。
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
  // 面板状态一行字（禁用原因 / 就绪 / 流式中 / 工具执行中）。
  QString statusText() const;
  // 系统提示：领域工具表 + 地质解释红线（建议不自动落库）。
  QString systemPrompt() const;
  QVector<AiToolSpec> tools() const { return builtinAiToolSpecs(); }
  // 工具执行器（组合成员；装配根/测试经此绑定执行面与数据上下文）。
  AiChatToolRunner *toolRunner() { return &m_runner; }

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

  // 工具结果回灌前的截断（超长结果截尾 + 如实标记原始长度）。公开静态：
  // 预算口径的单测面。
  static QString clampToolResult(const QString &content);

  // 上下文预算（估算口径：字符近似，非精确 tokenizer——ASCII 记 0.25
  // token/字符、其余（CJK 为主）记 1 token/字符；只求保守不求对齐词表）。
  static int estimateTokens(const QString &text);
  static constexpr int kMaxHistoryTurns = 12;      // 近 N 轮开窗
  static constexpr int kHistoryTokenBudget = 6000; // 历史估算 token 上限
  static constexpr int kToolResultCharLimit = 4000; // 单条工具结果字符上限
  static constexpr int kMaxToolRounds = 5;          // 每个用户轮的工具往返上限

signals:
  void sessionChanged();
  // 会话列表/标题/排序变化（打开/删除/重命名/落盘后；UI 侧刷新下拉）。
  void sessionListChanged();
  void messageAppended(const ChatMessage &message);
  void assistantDelta(const QString &text); // 流式增量（接到当前 assistant 消息尾）
  void toolCallDispatched(const ChatToolCall &call, const QString &statusText);
  void toolExecutionStarted(const ChatToolCall &call);
  void toolResultReady(const ChatToolCall &call, bool ok, const QString &summary);
  void streamingChanged(bool streaming);
  void statusChanged(const QString &text);
  void errorOccurred(const QString &message);

private:
  void appendMessage(const ChatMessage &message);
  void setStreaming(bool streaming);
  void sendRound();               // 占位 + 上送（带 tools[]）
  void onRoundFinished(const QString &reason);
  void onToolFinished(const ChatToolCall &call, bool ok, const QString &resultJson);
  QVector<ChatMessage> requestMessages() const; // system + 预算窗内历史

  LlmConfig m_config;
  LlmClient m_client;
  AiChatToolRunner m_runner;
  ChatSession m_session;
  bool m_streaming = false;
  int m_pendingToolResults = 0; // 工具相：等待回灌的未应答帧数
  int m_toolRound = 0;          // 本用户轮内已发生的模型↔工具往返数
  QVector<ChatToolCall> m_unansweredCalls; // 本轮尚未有 role=tool 应答的帧
};
