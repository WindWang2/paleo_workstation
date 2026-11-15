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
ConversationChannel::ConversationChannel(ConversationIdentity identity) : m_identity(std::move(identity)) {
  for (quint64 index = 0; index < ConversationQueueFrames; ++index) m_outgoing[index].sequence = index;
}
bool ConversationChannel::send(const QString &type, const QJsonObject &payload) {
  if (!m_identity.valid() || m_outgoingFailed.load()) return false;
  QJsonObject value{{"protocol", QLatin1String(ConversationProtocol)}, {"type", type}, {"sessionId", m_identity.sessionId},
    {"generation", m_identity.generation}, {"turnId", m_identity.turnId}, {"nonce", m_identity.nonce},
    {"sequence", QString::number(std::numeric_limits<quint64>::max())}, {"payload", payload}};
  // Reserve the longest possible counter encoding before claiming a slot. The
  // fixed sequence-numbered ring has bounded storage and no worker-held mutex;
  // owner and transport cancellation may both produce, but only transport reads.
  const auto reserved = encodeFrame(value).size();
  if (!reserved || m_outgoingBytes.fetch_add(reserved) > ConversationQueueBytes - reserved) {
    if (reserved) m_outgoingBytes.fetch_sub(reserved);
    m_outgoingFailed = true; return false;
  }
  auto position = m_writePosition.load();
  for (;;) {
    if (position >= std::numeric_limits<quint64>::max() - ConversationQueueFrames) {
      m_outgoingBytes.fetch_sub(reserved); m_outgoingFailed = true; return false;
    }
    if (m_outgoing[position % ConversationQueueFrames].sequence.load(std::memory_order_acquire) != position) {
      const auto current = m_writePosition.load();
      if (current != position) { position = current; continue; }
      m_outgoingBytes.fetch_sub(reserved); m_outgoingFailed = true; return false;
    }
    if (m_writePosition.compare_exchange_weak(position, position + 1)) break;
  }
  auto &slot = m_outgoing[position % ConversationQueueFrames];
  value["sequence"] = QString::number(position + 1); slot.bytes = encodeFrame(value); slot.reservedBytes = reserved;
  slot.sequence.store(position + 1, std::memory_order_release); return true;
}
QByteArray ConversationChannel::takeOutgoing() {
  auto &slot = m_outgoing[m_readPosition % ConversationQueueFrames];
  if (slot.sequence.load(std::memory_order_acquire) != m_readPosition + 1) return {};
  auto frame = std::move(slot.bytes); m_outgoingBytes.fetch_sub(slot.reservedBytes);
  slot.sequence.store(m_readPosition + ConversationQueueFrames, std::memory_order_release);
  ++m_readPosition; return frame;
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
  if (m_outgoingFailed.load()) return "pi_outgoing_budget";
  if (!m_mutex.tryLock()) return "pi_conversation_busy";
  const auto unlock = qScopeGuard([this] { m_mutex.unlock(); }); return m_error;
}
void ConversationChannel::requestStop(bool explicitUser) {
  if (explicitUser) m_stop = 1;
  else { int expected = 0; m_stop.compare_exchange_strong(expected, 2); }
}
} // namespace paleo::pi
