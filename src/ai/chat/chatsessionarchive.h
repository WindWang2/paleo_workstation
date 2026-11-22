// 层：功能
#pragma once
// H2（agent-session-archive，2026-11-22）：本机私有会话档案的合同层——
// 结构/身份校验（W1）、有界读取（W2）与对称容量预算（W3）的单点定义。
//
// 站位：会话 JSON 仍是权威历史；本组件只回答「这个档案能不能信、能不能读、
// 还装不装得下一轮」。它不授予任何工具执行 / 跨工程读取 / 发布权限——
// 绑定行的深校验与授权语义仍在 workflow/chatinvocationbinding（宿主准入），
// 档案层只做形状校验（形状合法 ≠ 已授权）。
#include "chatmessage.h"
#include "chatsession.h"
#include <QJsonObject>
#include <QString>
#include <QVector>

class QIODevice;

namespace paleo::chat {

// ---- 既有业务上限的档案侧登记（单点；controller 同名常量引用此处）----
// 每用户轮（一次 sendUserText / retryLast）内可能发生的最大轮次形状：
//   · 模型完成次数 ≤ 1 + kArchiveMaxToolRounds（开场 + 每次工具往返续轮）；
//   · 每轮准入的调用总数 ≤ kArchiveMaxCallsPerTurn（admitToolCalls 收口，
//     超限整帧拒绝后清空，不落盘）；
//   · 单条模型响应网络字节 ≤ kArchiveMaxModelResponseBytes（LlmClient 传输
//     上限，非 maxTokens 理想值）。
inline constexpr int kArchiveMaxToolRounds = 5;
inline constexpr int kArchiveMaxCallsPerTurn = 16;
inline constexpr int kArchiveMaxToolArgumentBytes = ChatToolCall::kMaxArgumentBytes;
inline constexpr qint64 kArchiveMaxModelResponseBytes = 8ll * 1024 * 1024;

// 档案拒绝分类（load 失败的诚实口径；不同类别的用户可行动性不同）。
enum class ChatArchiveReject {
  None = 0,
  UnsafeId,         // id 非单段安全名（沿用 pathFor 判空）
  NotFound,         // 文件不存在
  NotRegularFile,   // 目录/设备/FIFO/叶子符号链接伪装
  OutsideRoot,      // 解析后真实路径不在会话根内（中间链接/junction 穿越）
  IoError,          // 打开/读取失败（权限等）
  SizeExceeded,     // 打开前 size 已超档案容量上限（超限旧档案：字节保留、仅元信息）
  ReadLimitExceeded,// 读取过程中累计超上限（打开后在读窗口内增长）
  JsonSyntax,       // 不是合法 JSON
  NotObject,        // 顶层不是 JSON object
  SchemaMissing,    // 缺 schema 字段（writer 自首版起恒写 schema=1）
  SchemaUnsupported,// 未来 schema：明确不可完整载入，不擅自解释后保存
  StructureInvalid, // 字段类型/形状/长度不符（toObject/toArray 默认值不再掩盖坏结构）
  IdentityMismatch, // 文件名 sessionId 与 JSON 内部 id 不一致（拒绝，不「修复」）
  MessageBudget,    // 消息数超结构上限（防超小元素洪泛内存）
};
// 稳定英文码（诊断/测试断言用；用户可见文案由调用方 tr()）。
QString chatArchiveRejectCode(ChatArchiveReject reject);

// W1：结构 + 身份校验。identity 非空时同时核对文件名 id 与 JSON id 一致
// （不做 trim/大小写折叠/字符替换式「修复」）。合法旧档案（无 bindings、
// 无标题、旧时间格式、对象形态 arguments）继续可读；未知未来 schema 与
// 身份不一致明确拒绝。未知多余键容忍（前向读取不炸）。
ChatArchiveReject validateChatArchiveObject(const QJsonObject &object,
                                             const QString &identity = {},
                                             QString *detail = nullptr);

// W2：从已打开设备累计读取，总量超 limit 即停（打开后增长的文件也被拦住，
// 不只看打开前的 size）。失败时 *reject 置分类并返回空串。
QByteArray readChatArchiveBytes(QIODevice *device, qint64 limit,
                                ChatArchiveReject *reject);

// ---- W3：对称容量预算 ----
// 推导口径（每项都是「已准入回合内可能落盘的最大增量」，不混用模型响应
// 计数与工具计数，不拿 maxTokens 理想值代替 8 MiB 实际响应上限）：
//   · 模型回答：(1 + toolRounds) 条 assistant，每条 content 码元数 ≤
//     maxResponseBytes（UTF-8 网络字节数 ≥ UTF-16 码元数），JSON 转义最坏
//     escapeWorstFactor 字节/码元（控制字符 \uXXXX 6 字节是数学上界）；
//   · 工具结果：callsPerTurn 条 role=tool，每条 content 是 result envelope
//     （≤ resultPageBytes 页 + 信封字段），同按最坏转义放缩；
//   · 工具调用帧：callsPerTurn 个 arguments（≤ toolArgumentBytes）+ 名字/id；
//   · host 绑定行：callsPerTurn 行定长字段（~0.5 KiB/行）；
//   · 元字段：消息时间戳/host_turn_id、轮中合法 rename 的标题上限
//     （titleReserve——合法改名不消耗本轮响应余量）。
struct ChatArchiveBudget {
  qint64 maxArchiveBytes = 0;   // 读/写共用的硬上限（save 与 load 对称）
  int maxMessages = 0;          // 结构性消息数上限（内存护栏）
  int maxTitleChars = 0;        // 新标题上限（rename 准入；UTF-16 码元）
  int maxTitleCharsLoad = 0;    // 旧档案标题容忍上限（更长的旧标题仍可载入）
  int toolRounds = kArchiveMaxToolRounds;
  qint64 maxResponseBytes = kArchiveMaxModelResponseBytes;
  int callsPerTurn = kArchiveMaxCallsPerTurn;
  int toolArgumentBytes = kArchiveMaxToolArgumentBytes;
  int resultPageBytes = 8 * 1024;
  int escapeWorstFactor = 6;
  qint64 contentSlack = 1024;   // 每内容块的标点/字段名/信封余量
  qint64 callMetaSlack = 2560;  // 每调用帧的 id/type/name 元字段余量（含最坏转义）
  qint64 messageMetaSlack = 1600;// 每条消息的时间戳/tool_call_id 等元字段余量（含最坏转义）
  qint64 bindingRowBytes = 3072;// 每行 host 绑定的定长字段上界（documentId ≤256、toolName ≤128，均含最坏转义）

  static ChatArchiveBudget production();
  qint64 titleReserve() const;
  qint64 turnReserve() const;   // 一轮最坏增量（不含用户消息本体与标题）
  // 一轮内可能追加的最大消息条数：1 占位外的首答开场 + (1+toolRounds) 条
  // assistant（开场 + 续轮）+ callsPerTurn 条 role=tool 结果 + 配对合成余量。
  qsizetype messagesPerTurn() const {
    return 2 + toolRounds + callsPerTurn + 2;
  }
};
// 当前生效预算（production，或测试注入的缩放档）。线程安全（互斥读快照）。
const ChatArchiveBudget chatArchiveBudget();
void setChatArchiveBudgetForTesting(const ChatArchiveBudget &budget);

// 诊断/测试用：进程内权威正文读取/解析次数（无论后续校验成败）。热列表
// 路径不应使它增长——这是「checkpoint 不触发全库正文解析」的可靠
// I/O/解析计数口径（冷修复每个可读候选恰好 +1；超限候选只 stat 不计）。
qint64 chatArchiveFullLoadCount();
void resetChatArchiveFullLoadCountForTesting();
// 供 ChatSessionStore::load 成功路径回拨（勿在别处调用）。
void noteChatArchiveFullLoad();

// 序列化成本（Compact JSON 精确字节；W3 准入与 save 上限共用同一度量）。
qint64 chatArchiveSerializedBytes(const ChatSession &session);
// 追加一条消息后的档案例（增量 = messageBytes + 1 个逗号，JSON 数组拼接可加）。
qint64 chatArchiveMessageBytes(const ChatMessage &message);
qint64 chatArchiveSerializedBytesWithMessage(const ChatSession &session,
                                             const ChatMessage &message);
// 截至第 messageCount 条（不含其后）的前缀例——retry 准入用：前缀 + 完整
// bindings/元字段 + 一轮最坏预留，不得先 resize 再判断。
qint64 chatArchiveSerializedPrefixBytes(const ChatSession &session,
                                        qsizetype messageCount);

// 纯准入判断（无副作用：不 append/不 resize/不落盘/不改绑定）。
bool chatArchiveAcceptsTurn(const ChatSession &session, const QString &userText,
                            const ChatArchiveBudget &budget, QString *reason = nullptr);
bool chatArchiveAcceptsRetryPrefix(const ChatSession &session, qsizetype lastUser,
                                   const ChatArchiveBudget &budget, QString *reason = nullptr);
bool chatArchiveAcceptsTitle(const ChatSession &session, const QString &title,
                             const ChatArchiveBudget &budget, QString *reason = nullptr);

} // namespace paleo::chat
