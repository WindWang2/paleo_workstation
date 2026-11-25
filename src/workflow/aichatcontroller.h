// 层：功能
#pragma once
#include "../ai/chat/chatmessage.h"
#include "../ai/chat/chatsession.h"
#include "../ai/chat/chatsessionarchive.h"
#include "../ai/chat/chatsessionindex.h"
#include "../ai/chat/domaintools.h"
#include "../ai/chat/llmclient.h"
#include "../ai/chat/llmconfigcommit.h"
#include "aichattoolrunner.h"
#include "pichatbridge.h"
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>
#include <functional>

class PythonEnvironmentController;
class AiChatRecentSummaries;

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
  ~AiChatController() override;
  void setPiRuntime(const paleo::pi::RuntimeConfig &); // Host opt-in; default remains native.
  paleo::pi::RuntimeConfig piRuntime() const { return m_piConfiguration; }

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
  // G1（W7）测试注入口：确定性伪造文件/钥匙串存储（真实生产走
  // defaultLlmConfigCommitHooks——LlmConfig::save + LlmKeyStore）。
  void setLlmConfigCommitHooksForTesting(const LlmConfigCommitHooks &hooks) {
    m_configCommitter.setHooks(hooks);
  }
  // 可发请求的唯一判据（端点/模型/密钥齐且地址合法）。
  bool enabled() const { return m_config.enabled(); }
  bool streaming() const { return m_streaming; }
  // 面板状态一行字（禁用原因 / 就绪 / 流式中 / 工具执行中）。
  QString statusText() const;
  // 系统提示：领域工具表 + 地质解释红线（建议不自动落库）。
  QString systemPrompt() const;
  // 方向77：工程摘要常驻轻注入（井数/层位数/资产计数一行式，由装配根经
  // AiChatToolRunner::projectBrief 算好送入；空 = 无工程/未注入，不占预算）。
  // G5：赋值变化时发 projectBriefChanged（展示层刷新上下文摘要；不影响注入）。
  void setProjectBrief(const QString &brief) {
    if (m_projectBrief == brief)
      return;
    m_projectBrief = brief;
    emit projectBriefChanged();
  }
  QString projectBrief() const { return m_projectBrief; }
  // G5 展示快照（const、按值返回，不构成任何权限）：当前回合的宿主持久
  // 归属 id 与回合发起时的冻结身份。空 = 未在回合中/未接入宿主。
  QString hostTurnId() const { return m_hostTurn; }
  QJsonObject turnContext() const { return m_turnContext; }
  QVector<AiToolSpec> tools() const { return m_runner.availableTools(); }
  // 工具执行器（组合成员；装配根/测试经此绑定执行面与数据上下文）。
  AiChatToolRunner *toolRunner() { return &m_runner; }
  // G2：可选 Pi 引擎桥（测试经此注入受控 peer；运行接线不变）。
  PiChatBridge *piBridge() { return &m_piBridge; }
  PythonEnvironmentController *createPythonEnvironmentController(QObject *owner);

  ChatSession session() const { return m_session; }
  QVector<ChatMessage> messages() const { return m_session.messages; }
  QVector<ChatSession> recentSessions(int limit = 5) const;
  // H2：轻量最近会话元信息查询（列表不载入正文；热路径只读元数据/缓存，
  // 冷修复经既有任务服务异步分批重建）。结果经 recentSessionSummariesReady
  // 发布；complete=false 表示仍有待修复条目或扫描截断——那是有限扫描的
  // 部分结果，修复完成后会再次发布。
  void requestRecentSessionSummaries(int limit = 20);
  bool recentSummaryRepairInFlight() const;

  void newSession();
  bool loadSession(const QString &sessionId, QString *error = nullptr);
  bool removeSession(const QString &sessionId, QString *error = nullptr);
  bool saveCurrentSession(QString *error = nullptr);
  // A local file selection or import result belongs to the initiating chat.
  // Recording it does not contact a model or grant access to the source file.
  bool recordImportReceipt(const QString &sessionId, const QString &summary,
                           QString *error = nullptr);
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
  void interrupt();

  // 上下文预算（估算口径：字符近似，非精确 tokenizer——ASCII 记 0.25
  // token/字符、其余（CJK 为主）记 1 token/字符；只求保守不求对齐词表）。
  static int estimateTokens(const QString &text);
  static constexpr int kMaxHistoryTurns = 12;      // 近 N 轮开窗
  static constexpr int kHistoryTokenBudget = 6000; // 历史估算 token 上限
  // H2：每用户轮的轮次形状上限单点登记在 ai/chat/chatsessionarchive.h
  //（档案容量预留推导与之共用；此处仅引用，不另立第二份取值）。
  static constexpr int kMaxToolRounds = paleo::chat::kArchiveMaxToolRounds;
  static constexpr int kMaxCallsPerResponse = 8;
  static constexpr int kMaxCallsPerTurn = paleo::chat::kArchiveMaxCallsPerTurn;
  static constexpr int kMaxToolArgumentBytes = paleo::chat::kArchiveMaxToolArgumentBytes;

signals:
  void sessionChanged();
  // 会话列表/标题/排序变化（打开/删除/重命名/落盘后；UI 侧刷新下拉）。
  void sessionListChanged();
  // H2：轻量最近会话元信息（按 updatedAt、无效回退 createdAt 倒序，同值
  // 按 id 升序）。元信息不是读取工具或执行计划的授权——载入仍走
  // loadSession 的权威校验，缓存命中不绕过身份/结构检查。
  void recentSessionSummariesReady(
    const QVector<paleo::chat::ChatSessionSummary> &summaries, bool complete);
  void messageAppended(const ChatMessage &message);
  void assistantDelta(const QString &text); // 流式增量（接到当前 assistant 消息尾）
  void toolCallDispatched(const ChatToolCall &call, const QString &statusText);
  void toolExecutionStarted(const ChatToolCall &call);
  void toolResultReady(const ChatToolCall &call, bool ok, const QString &summary);
  void streamingChanged(bool streaming);
  void statusChanged(const QString &text);
  void configChanged(const LlmConfig &config);
  void errorOccurred(const QString &message);
  // G5：工程摘要变化（setProjectBrief 实际改变值时；空→空不发）。
  void projectBriefChanged();

private:
  void stopCurrentRound(bool explicitRequest);
  void flushImportReceipts();
  void appendMessage(const ChatMessage &message);
  void setStreaming(bool streaming);
  void sendRound();               // 占位 + 上送（带 tools[]）
  QString buildSystemPrompt(const QVector<AiToolSpec> &tools, const QString &brief) const;
  bool ensureTurnContext();
  QString negotiatedToolError(const ChatToolCall &call) const;
  void dispatchTurnTool(ChatToolCall call);
  void onRoundFinished(const QString &reason);
  void onToolFinished(const ChatToolCall &call, bool ok, const QString &resultJson);
  // 历史→请求投影由 aichatcontextprojection 只读组件承担（H1）；sendRound
  // 在装配接缝消费其结果——投影不可发送即零 HTTP、走既有停轮路径。
  // 给尾部 assistant 的未应答 tool_calls 合成一条 role=tool 错误应答
  // （reason 说明为何没执行）：取消/出错两条停轮路径共用，
  // 保证历史里「每个 tool_call 必有一条应答」的协议完整性。
  QVector<ChatMessage> pairUnansweredToolCalls(const QString &reason, const QString &pendingCode);
  void handleModelError(LlmErrorKind, const QString &message);
  bool admitToolCalls(const QVector<ChatToolCall> &calls);
  void rejectToolRound(const QString &code, const QString &message = {});
  void startEngineTurn(const QJsonObject &context);
  void failTurn(const QString &);
  ChatToolCall hostAdmittedCall(const ChatToolCall &) const;
  // ---- G2（回合执行闭环）：本轮冻结快照与保存欠账 ----
  // W1：一轮用户请求的执行归属（模型配置/协商工具表）在 startEngineTurn
  // 定格；续轮不因中途 setConfig 或工具表变化悄悄换身份，下一轮重新定格。
  QString currentSessionModel() const;               // 本轮开场模型（未冻结=当前配置）
  bool settleSaveArrears(QString *error = nullptr);  // 先清保存欠账，成功才允许续轮
  void failTurnForSaveArrears(const QString &error); // 保存失败统一结算本轮（单次上报）

  LlmConfig m_config;
  LlmClient m_client;
  // G1（W7）配置保存串行提交：applyConfig 的私有协作者（世代/排队/补偿
  // 细节见 llmconfigcommit.h；成功仍经上方 setConfig 生效）。
  LlmConfigCommitter m_configCommitter;
  AiChatToolRunner m_runner;
  PiChatBridge m_piBridge;
  paleo::pi::RuntimeConfig m_piConfiguration;
  QString m_hostTurn;
  quint64 m_modelRound = 0;
  quint64 m_turnSequence = 0;
  QJsonObject m_turnContext;
  QSet<QString> m_providerCalls; // Response-only deduplication; never durable identity.
  ChatSession m_session;
  QList<QPair<QString, QString>> m_pendingImportReceipts;
  QString m_projectBrief; // 方向77：工程概况常驻段（system prompt 尾部轻注入）
  bool m_streaming = false;
  int m_pendingToolResults = 0; // 工具相：等待回灌的未应答帧数
  int m_toolRound = 0;          // 本用户轮内已发生的模型↔工具往返数
  int m_turnCalls = 0;
  int m_toolTokens = 0;
  int m_streamToolCalls = 0;
  bool m_roundRejected = false;
  QVector<ChatToolCall> m_unansweredCalls; // 本轮尚未有 role=tool 应答的帧
  // G2-W1：本轮冻结的模型配置与协商工具表（原生/Pi 同源），见上块注释。
  LlmConfig m_turnConfig;
  QVector<AiToolSpec> m_turnTools;
  QString m_turnPrompt;
  bool m_turnFrozen = false;
  // G2-W3：保存欠账 = 已进入内存但尚未成功落盘的对话内容；未清账前拒绝
  // 无声切换/删除当前会话（薄门见 newSession/loadSession/removeSession）。
  bool m_saveArrears = false;
  QString m_saveArrearsError;
  // H2 私有：最近会话元信息查询协调器（子对象；任务服务在每次刷新时现取
  // runner 的既有绑定，不新增装配/全局线程池）。
  AiChatRecentSummaries *m_recentSummaries = nullptr;
};
