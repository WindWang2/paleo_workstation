// 层：功能
#include "aichatcontroller.h"

#include <QPointer>
#include <utility>

void AiChatController::flushImportReceipts() {
  const auto pending = std::exchange(m_pendingImportReceipts, {});
  const QPointer<AiChatController> guard(this);
  for (const auto &[sessionId, detail] : pending) {
    QString reason;
    if (!recordImportReceipt(sessionId, detail, &reason) && guard)
      emit errorOccurred(reason);
    if (!guard) return;
  }
}

bool AiChatController::recordImportReceipt(const QString &sessionId,
                                           const QString &summary, QString *error) {
  const QString detail = summary.trimmed();
  if (detail.isEmpty()) {
    if (error) *error = tr("文件导入记录为空");
    return false;
  }
  const bool current = sessionId == m_session.id;
  if (current && streaming()) {
    // Keep the streaming assistant and its outstanding tool replies adjacent.
    m_pendingImportReceipts.append({sessionId, detail});
    return true;
  }

  ChatSession target;
  if (current) target = m_session;
  else if (!ChatSessionStore::load(sessionId, &target, error)) return false;

  ChatMessage receipt;
  receipt.role = ChatRole::User; // User-selected attachment context on the wire.
  receipt.localImportReceipt = true;
  receipt.timestamp = QDateTime::currentDateTime();
  receipt.content = tr("文件导入记录\n%1").arg(detail);
  if (target.messages.isEmpty() && target.title.isEmpty())
    target.title = detail.section(QLatin1Char('\n'), 0, 0).left(40);
  target.messages.append(receipt);
  target.updatedAt = receipt.timestamp;
  if (!ChatSessionStore::save(target, error)) return false;

  const QPointer<AiChatController> guard(this);
  if (current) {
    m_session = std::move(target);
    m_saveArrears = false;
    m_saveArrearsError.clear();
    emit messageAppended(receipt);
  }
  if (guard) emit sessionListChanged();
  return true;
}
