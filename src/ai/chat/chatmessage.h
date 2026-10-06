// 层：功能
#pragma once
// ai/chat — 对话域模型（方向51：LLM 地质对话助手骨架）。
//
// 纯值类型 + JSON 序列化：不含 QtWidgets，不碰网络，不知道 HTTP 协议。
// 消息/工具调用帧的形状按 OpenAI 兼容 chat completions 的公开协议口径定义
// （官方文档：https://platform.openai.com/docs/api-reference/chat），
// 不引入第三方 SDK：序列化是唯一的横向依赖面。
//
// 诚实边界：本方向只做「描述 + 分发」的工具表（domaintools.h），
// ChatToolCall 只是协议上的调用帧记录，**不**代表工具已经被执行——
// 真正的 function-calling 闭环递延（TODOS.md 登记）。
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QVector>

// 对话角色（与协议 "role" 字段一一对应）。
enum class ChatRole { System, User, Assistant, Tool };

QString chatRoleKey(ChatRole role);
// 容错：未知 key 返回 false（调用方如实拒绝，不猜角色）。
bool chatRoleFromKey(const QString &key, ChatRole *out);

// 一次工具调用请求（assistant 消息携带）。
struct ChatToolCall {
  QString id;            // 协议要求的调用 id（tool call id）
  QString name;          // 领域工具名（domaintools.h 的注册表 key）
  QString argumentsJson; // 实参 JSON 原文（不解析——本方向不执行）
  bool hasValidArguments() const; // 空串 = 无参工具，视为合法
  QJsonObject toJson() const;
  static ChatToolCall fromJson(const QJsonObject &object);
  bool operator==(const ChatToolCall &other) const {
    return id == other.id && name == other.name &&
           argumentsJson == other.argumentsJson;
  }
};

struct ChatMessage {
  ChatRole role = ChatRole::User;
  QString content;
  // role==Tool：回指被应答的那个调用 id（协议语义）。
  QString toolCallId;
  // role==Assistant 且模型要求调用工具：调用帧列表（可多个）。
  QVector<ChatToolCall> toolCalls;
  QDateTime timestamp;

  bool isEmpty() const { return content.isEmpty() && toolCalls.isEmpty(); }
  // 落盘形态（会话文件）：带 timestamp 等本机元数据。
  QJsonObject toJson() const;
  // 上线形态（请求体 messages[]）：**只含协议字段**（role / content /
  // tool_call_id / tool_calls）。timestamp 是本机元数据，协议里没有——
  // 混进去等于给服务端塞未知字段，严格实现会直接拒收。
  QJsonObject toProtocolJson() const;
  static ChatMessage fromJson(const QJsonObject &object);
  bool operator==(const ChatMessage &other) const;
};
