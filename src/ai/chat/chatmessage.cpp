// 层：功能
#include "chatmessage.h"

#include <QJsonValue>

QString chatRoleKey(ChatRole role) {
  switch (role) {
  case ChatRole::System:
    return QStringLiteral("system");
  case ChatRole::User:
    return QStringLiteral("user");
  case ChatRole::Assistant:
    return QStringLiteral("assistant");
  case ChatRole::Tool:
    return QStringLiteral("tool");
  }
  return QStringLiteral("user"); // 枚举外值：按最低权限（用户）处理
}

bool chatRoleFromKey(const QString &key, ChatRole *out) {
  const bool ok = key == QLatin1String("system") || key == QLatin1String("user") ||
                  key == QLatin1String("assistant") ||
                  key == QLatin1String("tool");
  if (!ok)
    return false;
  if (!out)
    return true;
  if (key == QLatin1String("system"))
    *out = ChatRole::System;
  else if (key == QLatin1String("user"))
    *out = ChatRole::User;
  else if (key == QLatin1String("assistant"))
    *out = ChatRole::Assistant;
  else
    *out = ChatRole::Tool;
  return true;
}

bool ChatToolCall::hasValidArguments() const {
  if (argumentsJson.trimmed().isEmpty())
    return true; // 无参工具：协议允许省略 arguments
  QJsonParseError parseError;
  const QJsonDocument doc =
    QJsonDocument::fromJson(argumentsJson.toUtf8(), &parseError);
  return parseError.error == QJsonParseError::NoError && doc.isObject();
}

QJsonObject ChatToolCall::toJson() const {
  QJsonObject object;
  object.insert(QStringLiteral("id"), id);
  object.insert(QStringLiteral("type"), QStringLiteral("function"));
  QJsonObject function;
  function.insert(QStringLiteral("name"), name);
  function.insert(QStringLiteral("arguments"), argumentsJson);
  object.insert(QStringLiteral("function"), function);
  return object;
}

ChatToolCall ChatToolCall::fromJson(const QJsonObject &object) {
  ChatToolCall call;
  call.id = object.value(QStringLiteral("id")).toString();
  // 协议里 function.arguments 是字符串（不是对象）——这里照样按字符串取。
  const QJsonObject function =
    object.value(QStringLiteral("function")).toObject();
  call.name = function.value(QStringLiteral("name")).toString();
  const QJsonValue arguments = function.value(QStringLiteral("arguments"));
  call.argumentsJson = arguments.isString()
                         ? arguments.toString()
                         : QString::fromUtf8(
                             QJsonDocument(arguments.toVariant().toJsonObject())
                               .toJson(QJsonDocument::Compact));
  return call;
}

QJsonObject ChatMessage::toJson() const {
  QJsonObject object;
  object.insert(QStringLiteral("role"), chatRoleKey(role));
  // 协议要求 assistant 的工具调用消息里 content 可为 null；本实现统一写空串
  // （序列化往返稳定优先，null/空串在流式拼接处无差别）。
  object.insert(QStringLiteral("content"), content);
  if (!toolCallId.isEmpty())
    object.insert(QStringLiteral("tool_call_id"), toolCallId);
  if (!toolCalls.isEmpty()) {
    QJsonArray calls;
    for (const ChatToolCall &call : toolCalls)
      calls.append(call.toJson());
    object.insert(QStringLiteral("tool_calls"), calls);
  }
  if (timestamp.isValid())
    object.insert(QStringLiteral("timestamp"),
                  timestamp.toUTC().toString(Qt::ISODateWithMs));
  return object;
}

QJsonObject ChatMessage::toProtocolJson() const {
  // 与 toJson() 同形，只是**不写 timestamp**：请求体里的每条 message 只允许
  // 协议字段（role / content / tool_call_id / tool_calls）。
  QJsonObject object;
  object.insert(QStringLiteral("role"), chatRoleKey(role));
  object.insert(QStringLiteral("content"), content);
  if (!toolCallId.isEmpty())
    object.insert(QStringLiteral("tool_call_id"), toolCallId);
  if (!toolCalls.isEmpty()) {
    QJsonArray calls;
    for (const ChatToolCall &call : toolCalls)
      calls.append(call.toJson());
    object.insert(QStringLiteral("tool_calls"), calls);
  }
  return object;
}

ChatMessage ChatMessage::fromJson(const QJsonObject &object) {
  ChatMessage message;
  if (!chatRoleFromKey(object.value(QStringLiteral("role")).toString(),
                       &message.role))
    message.role = ChatRole::User; // 未知角色降级为用户（不冒充 assistant）
  message.content = object.value(QStringLiteral("content")).toString();
  message.toolCallId = object.value(QStringLiteral("tool_call_id")).toString();
  const QJsonArray calls = object.value(QStringLiteral("tool_calls")).toArray();
  for (const QJsonValue &value : calls)
    message.toolCalls.append(ChatToolCall::fromJson(value.toObject()));
  const QString stamp = object.value(QStringLiteral("timestamp")).toString();
  if (!stamp.isEmpty())
    message.timestamp = QDateTime::fromString(stamp, Qt::ISODateWithMs).toLocalTime();
  return message;
}

bool ChatMessage::operator==(const ChatMessage &other) const {
  return role == other.role && content == other.content &&
         toolCallId == other.toolCallId && toolCalls == other.toolCalls;
}
