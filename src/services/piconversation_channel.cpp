// 层：数据
#include "piconversation.h"
#include "piprotocol.h"
#include <QJsonDocument>
#include <QMutexLocker>
#include <QScopeGuard>
#include <limits>

namespace paleo::pi {
bool ConversationIdentity::valid() const {
  const auto bytes = nonce.toLatin1();
  return validSession(sessionId) && validSession(turnId) && validCounter(generation) &&
      bytes.size() == 32 && QByteArray::fromHex(bytes).toHex() == bytes;
}
ConversationChannel::ConversationChannel(ConversationIdentity identity) : m_identity(std::move(identity)) {}
bool ConversationChannel::send(const QString &type, const QJsonObject &payload) {
  if (!m_mutex.tryLock()) return false;
  const auto unlock = qScopeGuard([this] { m_mutex.unlock(); });
  if (!m_identity.valid() || !m_error.isEmpty() || m_hostSequence == std::numeric_limits<quint64>::max()) return false;
  const QJsonObject value{{"protocol", QLatin1String(ConversationProtocol)}, {"type", type}, {"sessionId", m_identity.sessionId},
    {"generation", m_identity.generation}, {"turnId", m_identity.turnId}, {"nonce", m_identity.nonce},
    {"sequence", QString::number(m_hostSequence + 1)}, {"payload", payload}};
  const auto bytes = encodeFrame(value);
  if (bytes.isEmpty() || m_outgoing.size() >= ConversationQueueFrames || bytes.size() > ConversationQueueBytes - m_outgoingBytes) {
    m_error = "pi_outgoing_budget"; return false;
  }
  ++m_hostSequence; m_outgoingBytes += bytes.size(); m_outgoing.enqueue(bytes); return true;
}
QByteArray ConversationChannel::takeOutgoing() {
  QMutexLocker lock(&m_mutex);
  if (m_outgoing.isEmpty()) return {};
  const auto frame = m_outgoing.dequeue(); m_outgoingBytes -= frame.size(); return frame;
}
bool ConversationChannel::acceptPeer(const QJsonObject &frame, bool &notify) {
  QMutexLocker lock(&m_mutex); notify = false;
  const auto fail = [&](const char *code) { m_error = QLatin1String(code); return false; };
  if (!m_identity.valid() || !m_error.isEmpty() || frame.keys() != QStringList{"generation", "nonce", "payload", "protocol", "sequence", "sessionId", "turnId", "type"} ||
      frame.value("protocol") != QLatin1String(ConversationProtocol) || frame.value("sessionId") != m_identity.sessionId ||
      frame.value("generation") != m_identity.generation || frame.value("turnId") != m_identity.turnId || frame.value("nonce") != m_identity.nonce ||
      !frame.value("type").isString() || !frame.value("payload").isObject() || m_peerSequence == std::numeric_limits<quint64>::max() ||
      frame.value("sequence").toString() != QString::number(m_peerSequence + 1)) return fail("pi_conversation_identity");
  const auto size = QJsonDocument(frame).toJson(QJsonDocument::Compact).size() + 4;
  if (size > MaxFrameBytes + 4 || m_incoming.size() >= ConversationQueueFrames || size > ConversationQueueBytes - m_incomingBytes)
    return fail("pi_incoming_budget");
  ++m_peerSequence; m_incomingBytes += size; m_incoming.enqueue(frame);
  if (!m_notified) { m_notified = true; notify = true; } return true;
}
bool ConversationChannel::takePeer(QVector<QJsonObject> &frames) {
  frames.clear(); if (!m_mutex.tryLock()) return false;
  const auto unlock = qScopeGuard([this] { m_mutex.unlock(); });
  frames.reserve(m_incoming.size());
  while (!m_incoming.isEmpty()) frames.append(m_incoming.dequeue());
  m_incomingBytes = 0; m_notified = false; return true;
}
QString ConversationChannel::error() const {
  if (!m_mutex.tryLock()) return "pi_conversation_busy";
  const auto unlock = qScopeGuard([this] { m_mutex.unlock(); }); return m_error;
}
void ConversationChannel::requestStop(bool explicitUser) {
  if (explicitUser) m_stop = 1;
  else { int expected = 0; m_stop.compare_exchange_strong(expected, 2); }
}
} // namespace paleo::pi
