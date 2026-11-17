// 层：功能
#pragma once
#include "ai/chat/chatsession.h"

namespace paleo::chat {
// Owner-only: binds a complete already-admitted model response. Inputs come from
// the host session/controller, never from a Pi tool_request or model argument.
// The caller must persist the resulting session successfully before dispatch.
QString bindInvocations(ChatSession &, const QString &turn, const QString &document,
                        QVector<ChatToolCall> &calls);
QString validateInvocationBindings(const ChatSession &);
QString validateInvocationRetry(const ChatSession &, qsizetype userIndex);
} // namespace paleo::chat
