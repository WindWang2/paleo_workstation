// 层：功能
#pragma once
#include "chatmessage.h"
#include "domaintools.h"
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QUrl>
#include <QVector>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

// ai/chat — LLM 客户端配置（方向51）。
//
// 协议面：OpenAI 兼容的 chat completions（POST <endpoint>/chat/completions，
// 可选 SSE 流式）**公开文档口径**（https://platform.openai.com/docs/api-reference/chat
// 与 https://platform.openai.com/docs/guides/streaming-responses）——
// 不引第三方 SDK，只用 QtNetwork。
//
// 诚实与安全三条：
//   · 无缺省第三方端点：端点必须显式配置，缺省 = 禁用态（UI 显示禁用原因）。
//   · 只接受 https://；http:// 仅 loopback 或显式开关（密钥明文风险）。
//   · 密钥不进 JSON：由 LlmKeyStore 走系统钥匙串；配置文件只存端点/模型/开关。
struct LlmConfig {
  QUrl endpoint;          // 例：https://api.openai.com/v1
  QString model;          // 例：gpt-4o-mini
  bool stream = true;     // SSE 流式增量
  int timeoutMs = 30000;  // 空闲超时（收到数据即重置）
  int maxTokens = 1024;
  bool allowInsecureHttp = false;
  QByteArray apiKey;      // 内存态密钥（不在 JSON 里落盘）

  static QString path();
  static LlmConfig load();              // JSON + 环境变量（密钥由 KeyStore 另行补齐）
  bool save(QString *error) const;      // 不写密钥
  // 空 = 可用；非空 = 禁用原因（面向用户，已翻译）。
  QString validate() const;
  bool enabled() const { return validate().isEmpty(); }
  // 真实请求地址：<endpoint>/chat/completions（端点尾斜杠容错）。
  QUrl completionsUrl() const;
  static bool isLoopbackHost(const QString &host);
  // 直接构造（测试/程序化配置用）：不读盘、不读环境变量。
  static LlmConfig fromParts(const QUrl &endpoint, const QString &model,
                             const QByteArray &apiKey = QByteArray(),
                             bool stream = true);
};

Q_DECLARE_METATYPE(LlmConfig)

// 错误分类（UI 按类给提示；测试断言分类而不匹配整句——文案可变，语义不可变）。
enum class LlmErrorKind {
  None,
  NotConfigured,  // 端点/模型缺失或禁用（UI 呈禁用态）
  Unauthorized,   // HTTP 401/403：密钥无效或无权
  RateLimited,    // HTTP 429：限流
  ServerError,    // HTTP 5xx
  BadRequest,     // HTTP 4xx（除 401/403/429）
  Network,        // 连接/解析层失败
  Timeout,        // 空闲超时
  Malformed,      // 应答不是合法 JSON / 缺必要字段
  TooLarge,       // 超过响应体上限
  Cancelled,      // 用户/调用方取消
};
QString llmErrorLabel(LlmErrorKind kind);

// SSE 流式工具调用的半成品（SSE 把 tool_calls 按 index 切成多段下发：id/name
// 一般只在首段，arguments 逐段拼接）。
struct LlmToolDraft {
  int index = -1;
  QString id;
  QString name;
  QString arguments;
  bool hasIdentity() const { return index >= 0 && !name.isEmpty(); }
};

// SSE 增量解析器（带状态，纯逻辑，可单测）。
//   feed()：喂任意切分的字节流（TCP 不保证按事件边界到货），产出本块里已经
//           完整的文本增量。协议按上述 streaming-responses 文档口径：事件以
//           空行分隔、`data:` 行是载荷、单独一行 `data: [DONE]` 收尾。
//   takeCompleteCalls()：取出 arguments 已拼成**非空**合法 JSON 的调用帧
//           （取出即从表移除，天然防重复 emit）。实参空串不算完成——那是
//           分片未齐，不是无参调用（#278）；无参调用只在流结束时经 flush()
//           产出。
//   flush()：流结束时兜底——id+name 齐即算一个调用（arguments 可残缺/为空）。
class LlmStreamParser {
public:
  void feed(const QByteArray &chunk, QStringList *deltas, bool *done,
            QString *error);
  QVector<ChatToolCall> takeCompleteCalls();
  QVector<ChatToolCall> flush();

  int promptTokens = 0;      // usage 出现时更新（取最后一次）
  int completionTokens = 0;

private:
  QByteArray m_buffer;
  QHash<int, LlmToolDraft> m_drafts;
};

// ai/chat — LLM 客户端（异步；零阻塞，世代号作废迟到应答）。
//
// 用法：setConfig(config) → send(messages) → 监听 deltaReceived/toolCallReceived
// /finished/errorOccurred。全程异步：不嵌套事件循环等应答（方向51 对
// remotepredictrouter 提的同一条纪律，新代码不许重现）。
class LlmClient : public QObject {
  Q_OBJECT
public:
  explicit LlmClient(QObject *parent = nullptr);
  ~LlmClient() override;

  void setConfig(const LlmConfig &config);
  LlmConfig config() const { return m_config; }

  // 发起一轮补全。messages 里 system/user/assistant/tool 均可（由 ChatMessage
  // 直接序列化）。tools 非空时请求体带 tools[]（领域工具表）与
  // tool_choice="auto"——模型自决是否调用（含不调工具直接作答）。
  // 未配置时立即 emit errorOccurred(NotConfigured)——不静默。
  void send(const QVector<ChatMessage> &messages,
            const QVector<AiToolSpec> &tools = QVector<AiToolSpec>());
  void cancel();
  bool busy() const { return m_reply != nullptr; }

signals:
  void deltaReceived(const QString &text);
  void toolCallReceived(const ChatToolCall &call);
  void finished(const QString &finishReason, int promptTokens,
                int completionTokens);
  void errorOccurred(LlmErrorKind kind, const QString &message);

private:
  void fail(LlmErrorKind kind, const QString &message);
  void finishUp(const QString &reason, int promptTokens, int completionTokens);
  void handleNonStreamReply(const QByteArray &payload);
  QNetworkReply *issueRequest(const QVector<ChatMessage> &messages,
                              const QVector<AiToolSpec> &tools);

  LlmConfig m_config;
  QNetworkAccessManager *m_nam = nullptr;
  // QPointer 而非裸指针：reply 由 m_nam 拥有并会自行 deleteLater，
  // 裸指针会在「流已收完但连接还没关」那段时间里悬垂——析构时解引用即崩
  // （实测：tst_aichatcontroller 的 streaming 用例曾在 ~LlmClient 段错误）。
  QPointer<QNetworkReply> m_reply = nullptr;
  QTimer *m_timer = nullptr;
  QByteArray m_buffer;      // SSE 半包缓冲（fed to LlmStreamParser）
  QByteArray m_body;        // 非流式累积体
  qint64 m_received = 0;    // 已收字节数（流式与非流式共用，用于上限判定）
  LlmStreamParser m_parser;
  int m_generation = 0;     // 世代号：cancel/send 后作废迟到信号
  int m_promptTokens = 0;
  int m_completionTokens = 0;
};
