// 层：功能
#pragma once
#include <QCoreApplication>

namespace paleo::chat {
// 普通用户消息的一段可见正文，随当前问题计入上下文与会话归档预算。
inline QString workspaceContextSeparator()
{
  return QStringLiteral("\n\n") + QCoreApplication::translate("ChatWorkspaceContext", "发送时的工作区") + QStringLiteral("\n");
}
inline QString questionTitleText(const QString &message)
{
  return message.section(workspaceContextSeparator(), 0, 0).trimmed();
}
}
