// 层：功能
#include "aichattoolrunner.h"

void AiChatToolRunner::setResultSession(const QString &session) {
  m_resultSession = session;
  refreshResultScope();
}

void AiChatToolRunner::refreshResultScope() {
  if (m_resultTaskSession != taskSession()) {
    m_resultTaskSession = taskSession();
    ++m_resultEpoch;
  }
  m_results.setScope(m_resultSession, m_context.documentId, m_resultEpoch);
}

QJsonObject AiChatToolRunner::modelResult(const ChatToolCall &call, const QString &rawJson,
                                        bool ok, int maxBytes) {
  refreshResultScope();
  return m_results.encode(rawJson, ok, maxBytes, call.name != QLatin1String("paleo.read_result"));
}
