// 层：功能
#pragma once
// workflow/aiconversationpresentation.h — G5 会话只读投影（纯值，无 QtWidgets）。
//
// 职责边界：把「实时 raw 工具出参」与「历史持久化 envelope」归一成同一组
// 呈现事实（ToolResultFacts），并按消息/hostToolBindings 建立历史工具帧的
// 只读归属关联（HistoryToolCall）。本组件不执行工具、不发起查询、不改会话
// ——投影永远不构成执行授权；缺失字段如实标注未知，不猜测补对。
#include "../ai/chat/chatmessage.h"
#include "../ai/chat/chatsession.h"
#include <QByteArray>
#include <QJsonValue>
#include <QString>
#include <QVector>

namespace paleo::chat {

// W3 归一化结果：一份工具结果可呈现的全部已知事实。
struct ToolResultFacts {
  // 内容来源：实时/原始出参、持久化 paleo.tool-result.v1 envelope、不可解析文本。
  enum class Source { RawPayload, Envelope, Unparseable };
  // 业务结果（三态诚实口径：查询成功 ≠ 业务完成，坏 JSON ≠ 空成功）。
  enum class Outcome { Unknown, Succeeded, Failed };

  Source source = Source::RawPayload;
  Outcome outcome = Outcome::Unknown;
  bool outcomeKnown = false;

  QString tool;                       // 调用侧工具名（历史帧由 assistant.toolCalls 配对给出）
  // envelope 顶层事实（raw 帧取 payload 同名字段，缺失即未知）。
  QString status, code, payloadContract;
  QString transportStatus, transportCode;
  bool transportKnown = false;        // envelope 才有 transport 段
  bool coverageComplete = true;
  bool coverageKnown = false;
  bool retryAdviceNoRepeat = false;   // retryAdvice=do_not_repeat_execution
  QString encodedBytesNote;           // envelope summary.encodedBytes（有界摘要证据）

  bool hasData = false;
  bool dataIsObject = false;
  QJsonValue data;                    // envelope.data 或 raw 根值（可能是数组/标量）
  QJsonArray resultRefs;              // chat-result-v1 引用（可用性由引用自身决定）
  QStringList omittedFields;

  // operation 事实：contract=paleo.operation.v2 或 envelope 顶层透传。
  bool operationFacts = false;
  bool responseOk = false;            // 查询级 ok（≠业务完成）
  bool responseOkKnown = false;
  QString operationId, execution, commitState, presentation;
  QString revision, commitReceiptRef, requestHash;
  double computeCount = 0;
  bool computeCountKnown = false;
  bool cancelRequested = false;

  // 不可解析时的原始证据（有界保留，供复制/检视；永不参与解析）。
  QString rawText;
  bool rawTruncated = false;
};

// 归一化入口。实时帧 okKnown=true 携带 runner 的执行 ok；历史 envelope 帧
// okKnown=false，outcome 只从 envelope.status 推导（不做无证据判定）。
ToolResultFacts projectToolResult(const QString &tool, const QString &resultJson,
                                  bool okKnown = false, bool ok = false);

// ---- W2 历史归属关联（只读）----
struct HistoryToolCall {
  int toolMessageIndex = -1;      // role=Tool 消息下标（-1 = 调用无结果帧）
  int assistantMessageIndex = -1; // 配对 assistant.toolCalls 所在消息
  int turnUserIndex = -1;         // 所属回合的 user 消息下标（-1 = 缺 user 帧）
  QString toolCallId, toolName, argumentsJson;
  QString turnId;                 // 回合 user 消息的 hostTurnId（空 = 旧记录未记）
  bool callPaired = false;        // assistant.toolCalls 里找得到该 id
  bool ambiguousCallId = false;   // 同 id 被多个 assistant 帧声明
  bool bindingKnown = false;      // hostToolBindings 命中 (turnId, callId)
  QString bindingDocumentId;
  bool hasResult = false;
  ToolResultFacts facts;          // role=Tool 消息内容投影（hasResult=false 时为空）
};

// 按消息顺序建立 role=Tool 帧 → 调用/回合/绑定的只读关联。不改会话内容，
// 不修复缺绑定/未配对记录——这类事实原样给出（bindingKnown/callPaired=false）。
QVector<HistoryToolCall> projectSessionTools(const ChatSession &session);

// ---- W6 跨页 UTF-8 边界 ----
// 把一页字节解码为可显示文本：尾部不完整多字节序列切出留待下一页，
// 非法序列以替换字符呈现（invalid 标记）。holdTail=false 用于末页
//（hasMore=false）：不再切尾，残缺尾序列按非法替换呈现、不静默吞字节。
struct TextPage {
  QString text;
  int trailingBytes = 0; // 留给下一页拼接的尾部不完整序列长度
  bool invalid = false;  // 页内存在被替换的非法序列
};
TextPage decodeUtf8Display(const QByteArray &bytes, bool holdTail = true);

} // namespace paleo::chat
