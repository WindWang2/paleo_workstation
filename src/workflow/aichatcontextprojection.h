// 层：功能
#pragma once
// H1（agent-context-budget，2026-11-22）：完整会话 → 当轮只读上下文投影。
//
// 纯函数组件：输入（会话历史、宿主 system prompt、冻结工具表、冻结模型
// 配置），输出「实际可发送的 messages + 预算/丢弃/拒绝诊断」。不改原
// messages/bindings/时间戳/operation 事实；相同输入必得相同投影（不借
// 时钟、网络或随机 id——legacy 结果适配走 ChatResultStore::encode 的
// allowCache=false 确定性分支，绝不注册可读引用）。
//
// 职责边界（docs/progress/h1-agent-context-budget-20261122.md）：
//   · 请求只保留一个宿主生成的 System 块（固定提示 + 有界截断说明合并）；
//     存储里的旧 System 消息不进请求、不拼回宿主提示。
//   · 历史按「轮」（User 消息起界）做 tool_call/role=tool 应答配对校验：
//     孤儿/重复/跨轮应答、缺 id、未完成工具组、文本插进未完工具组都判
//     损坏；损坏旧轮整轮排除且不得跨过它再捞更旧轮（连续后缀）；当前
//     必需轮损坏或超限 → 整个投影不可发送（零 HTTP 交给调用方停轮）。
//   · 历史工具结果先适配成最终 envelope 形态再计价（content、工具名/ID、
//     arguments、tool_call_id 与协议结构开销都算），杜绝「按短字符串收费、
//     按大 envelope 发送」。
//   · 6000 历史估算 token 预算语义不变（仍只管历史消息窗）；宿主固定
//     prompt、冻结工具声明、输出 maxTokens 单独分账，只报告不挤历史窗。
//   · 独立字节保护：请求体 ≤ maxRequestBodyBytes、工具声明序列化 ≤
//     maxToolDeclarationsBytes——超限拒绝并报告宿主配置问题，不为过门
//     削减协商工具或改写 schema。
#include "../ai/chat/chatmessage.h"
#include "../ai/chat/chatsession.h"
#include "../ai/chat/domaintools.h"
#include "../ai/chat/llmclient.h"

#include <QByteArray>
#include <QString>
#include <QVector>

namespace paleo::chat {

// 与 AiChatController 的公开预算常量同值的组件默认（controller 显式传
//自己的常量，tst_h1_contextprojection 钉两者相等防漂移）。
struct ContextProjectionLimits {
  int maxHistoryTurns = 12;
  int historyTokenBudget = 6000;
  // 请求体字节闸默认 256 KiB：历史窗 6000 估算 token（≤ ~24 KiB 字节）、
  // 单工具实参 ≤16 KiB×16、envelope ≤8 KiB/条且总量被 token 预算压住、
  // 内置工具表实测几十 KiB——256 KiB 是合法组合的 ≥8 倍余量，仍远小于
  // 8 MiB 应答上限（应答限额不是请求额度）。
  qint64 maxRequestBodyBytes = 256 * 1024;
  // 工具声明序列化字节闸默认 128 KiB：当前内置工具表实测 ~72 KiB
  //（tst_h1_contextprojection::toolDeclarationByteGuard 钉住余量）；超限说明
  // 宿主声明面失控（schema 漂移/误注入），应拒绝并修配置，而非静默裁剪。
  qint64 maxToolDeclarationsBytes = 128 * 1024;
};

struct ContextProjectionDiagnostics {
  bool sendable = false;
  // 机器稳定码（!sendable 时非空）：
  //   tool_declarations_exceed_budget / request_body_exceeds_byte_budget /
  //   current_turn_protocol_violation / current_turn_exceeds_history_budget
  QString rejectionCode;
  // current_turn_protocol_violation 时的有界补充（机器码，不含用户内容）。
  QString rejectionDetail;
  // 有界扫描 oracle：本次投影实际检视的消息条数（尾部开窗，不随全史线性涨）。
  int scannedMessages = 0;
  int keptTurns = 0;    // 进入请求的历史轮数（含当前轮）
  int droppedTurns = 0; // 因轮数/预算被截掉的更旧轮数
  int excludedTurns = 0; // 因协议损坏被排除的轮数（含导致停扫的那轮）
  // 尾部有界扫描确认「窗外还有更旧内容」即停（不扫描更早消息）。
  bool olderContentBeyondScan = false;
  // 有内容未随请求发送（丢弃/排除/窗外）→ system 块内合并截断说明。
  bool truncated = false;
  // 历史（含当前轮）最终协议形态的估算 token；饱和在 historyTokenBudget。
  int historyTokens = 0;
  // 分账（只报告，不占历史窗）：宿主固定 prompt / 冻结工具声明序列化
  // 字节 / 配置输出预留（max_tokens 原值，不宣称等于模型 token window）。
  int systemPromptTokens = 0;
  qint64 toolDeclarationBytes = 0;
  int maxTokensReserved = 0;
  // 最终请求体序列化字节（含 messages+tools+标量字段）；sendable 时有效。
  qint64 requestBodyBytes = 0;
  // 请求中被剔除的存储 System 消息条数与空 assistant 消息条数（只计数）。
  int strippedSystemMessages = 0;
  int droppedEmptyAssistants = 0;
};

struct ContextProjection {
  // sendable：[0] 唯一 System（宿主 prompt + 有界截断说明）+ 连续完整历史
  // 后缀（已按最终发送形态适配）；不含空 assistant 占位。!sendable：空表。
  QVector<ChatMessage> messages;
  ContextProjectionDiagnostics diagnostics;
};

ContextProjection projectChatRequest(const ChatSession &session,
                                     const QString &systemPrompt,
                                     const QVector<AiToolSpec> &tools,
                                     const LlmConfig &config,
                                     const ContextProjectionLimits &limits = {});

// 与 LlmClient::issueRequest 同形的纯序列化（messages + tools + 标量字段，
// Compact JSON）。字节闸与测试 byte oracle 共用；不触网络代码。
// 等价性由 tst_h1_requestassembly 以真实 POST body 钉住。
QByteArray serializeChatRequestBody(const QVector<ChatMessage> &messages,
                                    const QVector<AiToolSpec> &tools,
                                    const LlmConfig &config);

// 单条消息按最终协议形态（toProtocolJson 紧凑序列化）的估算 token，
// 含结构开销；上限饱和在 ceiling（预算比较只需「是否超」，防累加溢出）。
int estimateProtocolTokens(const ChatMessage &message, int ceiling);

} // namespace paleo::chat
