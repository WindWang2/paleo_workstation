// 层：功能
#include "aichatcontextprojection.h"
#include "../services/toolresultprotocol.h"
#include "chatresultstore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QtGlobal>

namespace paleo::chat {
namespace {

// 饱和字符估算（口径同 AiChatController::estimateTokens：ASCII ≈4 字符/token、
// 其余（CJK 为主）≈1 字符/token；保守近似，不宣称等于任意 tokenizer）。
// 到 ceiling 即提前返回：预算比较只需要「是否超限」，超长输入不必全扫，
// 累加面也因此不会溢出（每条 ≤ ceiling，求和再夹一次 ceiling）。
int estimateTokensSaturated(const QString &text, int ceiling) {
  if (ceiling <= 0)
    return 0;
  qint64 ascii = 0, wide = 0;
  for (const QChar &character : text) {
    if (character.unicode() < 0x80)
      ++ascii;
    else
      ++wide;
    if (ascii / 4 + wide >= ceiling)
      return ceiling;
  }
  return int(qMin<qint64>(ascii / 4 + wide, ceiling));
}

// 历史 tool 内容 → 最终发送 envelope。与既有 requestMessages 适配规则一致：
// 已是 envelope 原样保留（resultRef 等精确值不重写、不刷新 TTL、不注册
// 新引用）；合法 JSON 对象走 encode 的 allowCache=false 确定性分支（小载荷
// 内联 data，大载荷确定性降级——不产生可读引用，不冒称可读）；其余按
// legacy_result_unavailable 如实报错。无时钟、无随机 id：重复投影恒等。
QString adaptToolContentForWire(const QString &content) {
  const auto value = QJsonDocument::fromJson(content.toUtf8()).object();
  if (isResultEnvelope(value))
    return content;
  const bool valid = !value.isEmpty();
  ChatResultStore transient;
  const auto wrapped =
    valid ? transient.encode(content, !value.contains("error"), kResultPageBytes, false)
          : resultError(QStringLiteral("legacy_result_unavailable"));
  return QString::fromUtf8(compactJson(wrapped));
}

// 一轮的投影产物：发送形态消息 + 协议判定 + 最终形态计价。
struct TurnView {
  QVector<ChatMessage> wire;
  QString violation; // 非空 = 协议损坏（机器码，不含用户内容）
  int cost = 0;      // 最终协议形态估算 token（饱和在预算上限）
};

// 轮内协议闭合（W2）：assistant tool_calls 与紧随其后的 role=tool 应答必须
// 配对完整——重复/缺失 call id、孤儿/重复/跨轮应答、文本插进未完工具组、
// 轮尾未完成工具组都判损坏。User 边界只是候选分组，配对校验在这里补齐。
// 同时完成发送形态归一：剔 System、剔空 assistant 占位、tool 内容转 envelope。
// 损坏即返回：不做「部分保留」——半条 tool pair 进请求同样是协议违规。
TurnView prepareTurn(const QVector<ChatMessage> &messages, int begin, int end,
                     int ceiling, ContextProjectionDiagnostics *diagnostics) {
  TurnView out;
  QSet<QString> pending; // 已声明、等应答的 call id
  QSet<QString> seen;    // 本轮声明过的 call id（查重复声明）
  const auto fail = [&out](const char *code) {
    out.violation = QString::fromLatin1(code);
    out.wire.clear();
    out.cost = 0;
  };
  int cost = 0;
  for (int index = begin; index < end; ++index) {
    const ChatMessage &message = messages.at(index);
    if (message.role == ChatRole::System) {
      // 存储中的 System（旧档案残留）不进请求、不拼回宿主提示——存储历史
      // 不是新的高优先级指令来源。原档案不动，仅计数。
      ++diagnostics->strippedSystemMessages;
      continue;
    }
    if (message.role == ChatRole::Assistant && message.content.isEmpty() &&
        message.toolCalls.isEmpty()) {
      // 空 assistant（sendRound 的流式占位/历史残留）：本地落点，不是协议
      // 消息，不发送。带 toolCalls 的空 content assistant 是合法帧，保留。
      ++diagnostics->droppedEmptyAssistants;
      continue;
    }
    if (message.role == ChatRole::Assistant && !message.toolCalls.isEmpty()) {
      if (!pending.isEmpty()) {
        fail("assistant_interrupts_pending_tool_group");
        return out;
      }
      for (const ChatToolCall &call : message.toolCalls) {
        if (call.id.isEmpty()) {
          fail("tool_call_id_missing");
          return out;
        }
        if (seen.contains(call.id)) {
          fail("duplicate_tool_call_id");
          return out;
        }
        seen.insert(call.id);
        pending.insert(call.id);
      }
    } else if (message.role == ChatRole::Assistant) {
      if (!pending.isEmpty()) {
        fail("text_inside_tool_group");
        return out;
      }
    } else if (message.role == ChatRole::Tool) {
      // 无对应在等 call id 的应答：孤儿、重复应答、跨轮匹配同一判定。
      if (message.toolCallId.isEmpty() || !pending.contains(message.toolCallId)) {
        fail("orphan_tool_reply");
        return out;
      }
      pending.remove(message.toolCallId);
    } else if (message.role == ChatRole::User) {
      if (!pending.isEmpty()) {
        fail("text_inside_tool_group");
        return out;
      }
    }
    ChatMessage wire = message;
    if (message.role == ChatRole::Tool)
      wire.content = adaptToolContentForWire(message.content);
    out.wire.append(wire);
    cost = int(qMin<qint64>(qint64(cost) + estimateProtocolTokens(wire, ceiling),
                            ceiling));
  }
  if (!pending.isEmpty()) {
    fail("incomplete_tool_group");
    return out;
  }
  out.cost = cost;
  return out;
}

QByteArray toolDeclarationsJson(const QVector<AiToolSpec> &tools) {
  // 与 LlmClient::issueRequest 的 tools[] 同形（{type:function,
  // function:{…}}；空表不上送）——声明字节闸与 byte oracle 都以此为尺。
  QJsonArray array;
  for (const AiToolSpec &spec : tools) {
    QJsonObject entry;
    entry.insert(QStringLiteral("type"), QStringLiteral("function"));
    entry.insert(QStringLiteral("function"), spec.toChatFunction());
    array.append(entry);
  }
  return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

} // namespace

int estimateProtocolTokens(const ChatMessage &message, int ceiling) {
  return estimateTokensSaturated(
    QString::fromUtf8(compactJson(message.toProtocolJson())), ceiling);
}

QByteArray serializeChatRequestBody(const QVector<ChatMessage> &messages,
                                    const QVector<AiToolSpec> &tools,
                                    const LlmConfig &config) {
  QJsonArray array;
  for (const ChatMessage &message : messages)
    array.append(message.toProtocolJson());
  QJsonObject body;
  body.insert(QStringLiteral("model"), config.model);
  body.insert(QStringLiteral("messages"), array);
  body.insert(QStringLiteral("stream"), config.stream);
  body.insert(QStringLiteral("max_tokens"), config.maxTokens);
  if (!tools.isEmpty()) {
    const QByteArray declarations = toolDeclarationsJson(tools);
    body.insert(QStringLiteral("tools"),
                QJsonDocument::fromJson(declarations).array());
    body.insert(QStringLiteral("tool_choice"), QStringLiteral("auto"));
  }
  return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

ContextProjection projectChatRequest(const ChatSession &session,
                                     const QString &systemPrompt,
                                     const QVector<AiToolSpec> &tools,
                                     const LlmConfig &config,
                                     const ContextProjectionLimits &limits) {
  ContextProjection result;
  ContextProjectionDiagnostics &diagnostics = result.diagnostics;

  // ---- 固定成本分账（W6）：不挤历史窗，只如实报告 ----
  diagnostics.systemPromptTokens =
    estimateTokensSaturated(systemPrompt, limits.historyTokenBudget);
  diagnostics.maxTokensReserved = config.maxTokens;
  diagnostics.toolDeclarationBytes =
    tools.isEmpty() ? 0 : toolDeclarationsJson(tools).size();
  if (diagnostics.toolDeclarationBytes > limits.maxToolDeclarationsBytes) {
    // 冻结声明过大 = 宿主配置问题：明确拒绝，不静默删协商工具或改 schema。
    diagnostics.rejectionCode = QStringLiteral("tool_declarations_exceed_budget");
    return result;
  }

  // ---- 尾部有界扫描（W7）：从最后一条向前找 User 轮界，最多确认
  // maxHistoryTurns+1 个轮起点即停——窗外更旧内容只记「存在」，不扫描、
  // 不复制。扫描量 = 最新 N 轮消息 + 1 条边界，不随全史线性增长。 ----
  struct Range {
    int begin;
    int end;
  };
  QVector<Range> ranges; // 最新轮在前
  const int total = int(session.messages.size());
  int boundary = total;
  for (int index = total - 1; index >= 0; --index) {
    ++diagnostics.scannedMessages;
    if (session.messages.at(index).role == ChatRole::User) {
      ranges.append({index, boundary});
      boundary = index;
      if (int(ranges.size()) > limits.maxHistoryTurns)
        break; // 窗外还有更旧内容：到此为止
    }
  }
  if (boundary > 0 && int(ranges.size()) <= limits.maxHistoryTurns)
    ranges.append({0, boundary}); // 首个 User 之前的引导消息（旧档案形态）
  diagnostics.olderContentBeyondScan = int(ranges.size()) > limits.maxHistoryTurns;

  // ---- 连续后缀选择（W4）：锚在最新轮，从新到旧逐轮纳入；遇「无法纳入」
  // （预算耗尽/轮数满/协议损坏）即停——绝不跳过较近轮去捞更旧小轮。
  // 计价上限取 budget+1：饱和计数仍保留「恰好等于」与「超过」的区分——
  // cost==budget 可纳入、cost==budget+1 判超限（累加面有界，比较不失真）。 ----
  QVector<TurnView> kept; // 新→旧收集，装配时倒序
  qint64 remaining = limits.historyTokenBudget;
  const int tokenCeiling = limits.historyTokenBudget + 1;
  for (int round = 0; round < ranges.size(); ++round) {
    const bool currentTurn = round == 0;
    if (!currentTurn && int(kept.size()) >= limits.maxHistoryTurns) {
      ++diagnostics.droppedTurns; // 轮数满：更旧轮整轮截断（含未扫部分）
      break;
    }
    const Range &range = ranges.at(round);
    const TurnView turn =
      prepareTurn(session.messages, range.begin, range.end,
                  tokenCeiling, &diagnostics);
    if (!turn.violation.isEmpty()) {
      if (currentTurn) {
        // 当前必需轮损坏：不允许丢掉真实结果继续，明确拒绝整个请求。
        diagnostics.rejectionCode =
          QStringLiteral("current_turn_protocol_violation");
        diagnostics.rejectionDetail = turn.violation;
        return result;
      }
      ++diagnostics.excludedTurns; // 损坏旧轮整轮排除，且不得跨过它捞更旧轮
      break;
    }
    if (turn.cost > remaining) {
      if (currentTurn) {
        diagnostics.rejectionCode =
          QStringLiteral("current_turn_exceeds_history_budget");
        return result;
      }
      ++diagnostics.droppedTurns; // 预算耗尽：整轮截断
      break;
    }
    remaining -= turn.cost;
    kept.append(turn);
  }
  diagnostics.historyTokens =
    int(qMax<qint64>(0, limits.historyTokenBudget - remaining));
  diagnostics.keptTurns = int(kept.size());

  // ---- 装配：唯一宿主 System（固定提示 + 有界截断说明合并）+ 连续后缀 ----
  ChatMessage system;
  system.role = ChatRole::System;
  system.content = systemPrompt;
  diagnostics.truncated = diagnostics.droppedTurns > 0 ||
                          diagnostics.excludedTurns > 0 ||
                          diagnostics.olderContentBeyondScan;
  if (diagnostics.truncated) {
    // 标记只说明实际省略的范围/原因，合并进唯一 System 块；不写内容摘要
    // （模型不得编造未保留内容），重复投影不叠加（每次从原史重新推导）。
    QString note = QStringLiteral("（更早的对话已按上下文预算截断");
    if (diagnostics.excludedTurns > 0)
      note += QStringLiteral("；%1 轮历史记录不完整，未随请求发送")
                .arg(diagnostics.excludedTurns);
    note += QLatin1Char('）');
    system.content += QStringLiteral("\n") + note;
  }
  result.messages.append(system);
  for (int index = kept.size() - 1; index >= 0; --index)
    for (const ChatMessage &message : kept.at(index).wire)
      result.messages.append(message);

  // ---- 请求体字节闸（W6）：最终序列化形态度量，超限拒绝（零发送）。 ----
  diagnostics.requestBodyBytes =
    serializeChatRequestBody(result.messages, tools, config).size();
  if (diagnostics.requestBodyBytes > limits.maxRequestBodyBytes) {
    result.messages.clear();
    diagnostics.rejectionCode = QStringLiteral("request_body_exceeds_byte_budget");
    return result;
  }
  diagnostics.sendable = true;
  return result;
}

} // namespace paleo::chat
