// 层：功能
#include "aichattoolrunner.h"
#include "../services/toolresultprotocol.h"

void AiChatToolRunner::setResultSession(const QString &session) {
  m_resultSession = session;
  if (m_querySessionId != session) {
    m_querySessionId = session;
    rebindQueryScope();
  }
  refreshResultScope();
}

void AiChatToolRunner::refreshResultScope() {
  if (m_resultTaskSession != taskSession()) {
    m_resultTaskSession = taskSession();
    ++m_resultEpoch;
  }
  m_results.setScope(m_resultSession, m_context.documentId, m_resultEpoch);
}

void AiChatToolRunner::discardDeliveredReadResult(const QString &callId) {
  if (callId != m_deliveredReadCallId) return;
  m_deliveredReadCallId.clear();
  m_deliveredReadQuota = 0;
}

QJsonObject AiChatToolRunner::modelResult(const ChatToolCall &call, const QString &rawJson,
                                        bool ok, int maxBytes) {
  refreshResultScope();
  const bool read = call.name == QLatin1String("paleo.read_result");
  if (read && ok && call.id == m_deliveredReadCallId) {
    // H7-W3/W7：这次读页已经按生效预算生成；最终化只做一次复核，不重新
    // 查缓存、不重选页。额度被压小意味着该页无法原样送达——给最小合法
    // envelope 明确失败（同一 cursor 可换更小 max_bytes 重试，不重跑业务），
    // 而不是让 encode 静默删 data/丢 cursor。
    const int used = m_deliveredReadQuota;
    m_deliveredReadCallId.clear();
    m_deliveredReadQuota = 0;
    if (used > 0 && maxBytes < used)
      return paleo::chat::boundedResultError(QStringLiteral("tool_reply_budget_changed"),
                                             maxBytes, QStringLiteral("retry_read_same_cursor"));
    return m_results.encode(rawJson, true, used > 0 ? qMin(maxBytes, used) : maxBytes, false);
  }
  return m_results.encode(rawJson, ok, maxBytes, !read);
}
