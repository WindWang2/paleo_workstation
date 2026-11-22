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
#include <QSet>
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
  // G1：绝对总时限（毫秒）——一次发送从开始到结束的硬上限，不因任何到货
  // （含 SSE 注释心跳、非 JSON 滴答）延长。向后兼容：旧 JSON 缺该字段按
  // 默认 300000（5 分钟）取；合法域 [1000, 3600000]（load 时钳位）。
  // 只进 JSON 配置面，不新增 UI 配置项；测试直接改字段注入。
  int totalTimeoutMs = 300000;
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
  // 封印（sealed）：arguments 已拼成非空合法 JSON 对象（含预算内）。此后该
  // index 再到任何非空 arguments 分片都是协议翻覆——解析器报错，绝不改写已
  // 封印内容。封印发生在事件处理期（与 TCP 切分无关），封印序保证「任意
  // 拆包 == 一次喂入」的事件序列不变量。
  bool argumentsComplete = false;
  qint64 sealSequence = -1;
  bool hasIdentity() const { return index >= 0 && !name.isEmpty(); }
};

// SSE 增量解析器（带状态，纯逻辑，可单测）。G1 重写为真·增量行/事件解析
// （WHATWG event-stream 口径：https://html.spec.whatwg.org/multipage/
// server-sent-events.html#parsing-an-event-stream）。
//   feed()：喂任意切分的字节流。行界按 LF/CRLF/裸 CR 三个字节序列增量判定
//           （CRLF 跨块拆开只算一次换行）；只剥字段名后**单个**前导空格；
//           流首一次性剥 UTF-8 BOM；注释行（':' 开头）与无关字段忽略；
//           同一事件多条 data 行按规范以 '\n' 连接。
//   takeCompleteCalls()：流中取出 arguments 已拼成**非空**合法 JSON 对象、
//           且未被推翻的调用帧（按 index 升序交付，同位次以封印序稳定——
//           与旧实现的 index 稳定排序口径一致）。已封印/已产出的 index
//           再收到分片 = 协议翻覆 → feed 置 Malformed 错误，绝不无声改写
//           已交付内容（W4）。空 arguments 不算完成（#278）。
//           流终局（[DONE] 或 markTerminal）后调用则做终局校验：空实参按
//           无参调用产出，非空残缺/非对象 → 错误（不得改造成 {}）。
//   设计取舍（W4「malformed 流不触发实际工具」的口径）：同一 TCP 块里
//   「已封印完整帧 + 畸形事件」时，完整帧**先交付**、随后整轮报 Malformed。
//   已封印帧在畸形被发现前就已完整（无法合理撤回），且 LlmClient 的单一
//   终态保证错误路径不会走到 controller 的工具执行相——畸形流不会触发
//   实际工具执行，只会留下一张未执行卡片（错误路径按既有口径补
//   「未执行」应答，协议完整性不受影响）。
//   markTerminal()/flush()：流结束（[DONE] 或 EOF 兜底）收尾。flush =
//           markTerminal + 终局 takeCompleteCalls。终局后不再产出任何
//           文本/工具（同一 TCP chunk 内 [DONE] 之后的内容一并忽略）。
//   本客户端请求恒为单一回答：choices 里只有 index==0（或缺省按 0）参与，
//           其余 choice 的文本/工具一律丢弃（禁止混拼）。
class LlmStreamParser {
public:
  void feed(const QByteArray &chunk, QStringList *deltas, bool *done,
            QString *error);
  // 旧签名（错误丢弃）保留——现有调用面兼容；新逻辑用带 error 的重载。
  QVector<ChatToolCall> takeCompleteCalls();
  QVector<ChatToolCall> takeCompleteCalls(QString *error);
  QVector<ChatToolCall> flush();
  QVector<ChatToolCall> flush(QString *error);
  // EOF 无 [DONE] 的兜底收尾：把残余缓冲按最后一行处理并进入终局。
  void markTerminal();
  bool terminal() const { return m_terminal; }
  // choice 0 的最后一个非空 finish_reason（透传给 finished；未见时为空，
  // 由调用方按兼容口径退化，不改写 length/content_filter 等语义）。
  QString finishReason() const { return m_finishReason; }

  int promptTokens = 0;      // usage 出现时更新（取最后一次）
  int completionTokens = 0;

private:
  void processLine(const QByteArray &line, QStringList *deltas, bool *done,
                   QString *error);
  void dispatchEvent(QStringList *deltas, bool *done, QString *error);
  void accumulateChoice(const QJsonObject &choice, QStringList *deltas,
                        QString *error);

  QByteArray m_pending;       // 未构成完整行的字节（可能含半个 CRLF/BOM 前缀）
  bool m_bomChecked = false;  // 流首 BOM 判定是否已定（需 ≥1 或 3 个字节）
  bool m_terminal = false;    // [DONE]/markTerminal 后：输入与产出一并停止
  QString m_data;             // 当前事件累积的 data 载荷（WHATWG：value+'\n'）
  bool m_sawDataLine = false; // 当前事件是否出现过 data 字段
  QHash<int, LlmToolDraft> m_drafts;
  QSet<int> m_emitted;        // 已交付调用的 index：再收到分片即协议翻覆
  qint64 m_sealCounter = 0;   // 封印序发生器（事件处理期递增：切分不变）
  QString m_finishReason;
};

// ai/chat — LLM 客户端（异步；零阻塞，世代号作废迟到应答）。
//
// 用法：setConfig(config) → send(messages) → 监听 deltaReceived/toolCallReceived
// /finished/errorOccurred。全程异步：不嵌套事件循环等应答（方向51 对
// remotepredictrouter 提的同一条纪律，新代码不许重现）。
//
// G1 请求冻结（attempt 所有权）：每次 send 冻结一份私有请求快照（endpoint/
// model/stream/密钥/双时限）。在途请求的一切——网络信号、两个计时器、解析
// 器、尾字节、预算计数与清理——只认发起时的快照；期间 setConfig 只影响后续
// 请求。旧 attempt 不得关闭新 reply、停止新计时器或产生新请求的终态；
// delta/tool/error/finished 的同步订阅者在槽内再 send/cancel/delete client
// 时，每个继续处理点前重查对象存活与世代号。
class LlmClient : public QObject {
  Q_OBJECT
public:
  explicit LlmClient(QObject *parent = nullptr);
  // 测试注入口：注入自定义 QNetworkAccessManager（FakeReply 制造确定性
  // readyRead/finished/tail 序列）；生产一律用默认构造。
  explicit LlmClient(QNetworkAccessManager *networkAccessManagerForTesting,
                     QObject *parent = nullptr);
  ~LlmClient() override;

  void setConfig(const LlmConfig &config);
  LlmConfig config() const { return m_config; }

  // 发起一轮补全。messages 里 system/user/assistant/tool 均可（由 ChatMessage
  // 直接序列化）。tools 非空时请求体带 tools[]（领域工具表）与
  // tool_choice="auto"——模型自决是否调用（含不调工具直接作答）。
  // 未配置时立即 emit errorOccurred(NotConfigured)——不静默。
  // 有效 send 会中止并作废上一轮在途请求（其信号一律不再发出）。
  void send(const QVector<ChatMessage> &messages,
            const QVector<AiToolSpec> &tools = QVector<AiToolSpec>());
  void cancel();
  bool busy() const { return m_reply != nullptr; }

  // 双时限（W2）：timeoutMs 保持 idle 语义（真实到货即重置）；totalTimeoutMs
  // 为绝对总时限（单调时钟、单发不重置）。两者都释放 reply 并失效世代，
  // 都报 LlmErrorKind::Timeout，文案以「空闲超时/总时限」区分。
  int totalTimeoutMs() const { return m_config.totalTimeoutMs; }

signals:
  void deltaReceived(const QString &text);
  void toolCallReceived(const ChatToolCall &call);
  void finished(const QString &finishReason, int promptTokens,
                int completionTokens);
  void errorOccurred(LlmErrorKind kind, const QString &message);

private:
  // 一次实际发送的冻结快照 + 在途状态。m_generation 是唯一 attempt 身份。
  struct Attempt {
    int generation = 0;
    LlmConfig config;        // 冻结配置（URL/模型/流式/密钥/双时限全出自这里）
  };

  bool alive(int generation) const { return generation == m_generation; }
  void installDeadlineTimers(); // 双构造共用的计时器接线
  void releaseReply();
  // 统一失败终态：旧 attempt 的迟到失败静默丢弃；新 attempt 不被旧失败
  // 误伤；消息先按 attempt 密钥脱敏再出网。
  void fail(int generation, LlmErrorKind kind, const QString &message);
  void finishUp(int generation, const QString &reason, int promptTokens,
                int completionTokens);
  // 流终局（[DONE] 或 EOF）：终局校验 → 完整工具帧 → finished，一条路径。
  void finalizeStream(int generation);
  // readyRead/finished 共用的字节预算：溢出安全累加，超 8 MiB 即 fail(TooLarge)。
  bool countBytes(int generation, qint64 bytes);
  void handleNonStreamReply(int generation, const QByteArray &payload);
  QNetworkReply *issueRequest(const QVector<ChatMessage> &messages,
                              const QVector<AiToolSpec> &tools,
                              const LlmConfig &frozen);

  LlmConfig m_config;
  QNetworkAccessManager *m_nam = nullptr;
  // QPointer 而非裸指针：reply 由 m_nam 拥有并会自行 deleteLater，
  // 裸指针会在「流已收完但连接还没关」那段时间里悬垂——析构时解引用即崩
  // （实测：tst_aichatcontroller 的 streaming 用例曾在 ~LlmClient 段错误）。
  QPointer<QNetworkReply> m_reply = nullptr;
  QTimer *m_timer = nullptr;       // idle（到货重置）
  QTimer *m_totalTimer = nullptr;  // 绝对总时限（单发，不重置）
  Attempt m_attempt;
  QByteArray m_body;        // 原始应答累积体（非流式解析 + 非 SSE 判别共用；
                            //  一旦确认真流式即释放，见 W6 内存口径）
  qint64 m_received = 0;    // 已收字节数（readyRead 与 finished tail 统一口径）
  LlmStreamParser m_parser;
  int m_generation = 0;     // 世代号：cancel/send/终态后作废迟到信号
  // 本轮是否真见过 SSE 事件（增量/工具帧/[DONE]/解析过的坏事件；注释心跳
  // 不算）：stream=true 却收到普通 JSON 应答时的判别依据（#588）——没见过
  // 事件又确是 chat completions 形状，按非流式解析，不把零增量当成功。
  bool m_sawStreamEvent = false;
};
