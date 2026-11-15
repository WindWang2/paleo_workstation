// 层：数据
#pragma once
#include "piruntime.h"
#include <QByteArray>
#include <QMutex>
#include <QQueue>
#include <QVector>
#include <array>
#include <atomic>

namespace paleo::pi {
inline constexpr char ConversationProtocol[] = "paleo.pi-conversation.v1";
inline constexpr qsizetype ConversationQueueBytes = 256 * 1024;
inline constexpr int ConversationQueueFrames = 32;
struct ConversationIdentity {
  QString sessionId, generation, turnId, nonce;
  bool valid() const;
};
// Shared bounded mailbox. Owner operations never wait on a worker lock. A full
// queue is an explicit failure, not truncated JSON or an unbounded Qt event queue.
class ConversationChannel {
public:
  explicit ConversationChannel(ConversationIdentity);
  const ConversationIdentity &identity() const { return m_identity; }
  bool send(const QString &type, const QJsonObject &payload = {});
  bool takePeer(QVector<QJsonObject> &); // false means retry later; never waits.
  void requestStop(bool explicitUser);
  int stopReason() const { return m_stop.load(); } // 0 live, 1 user, 2 host.
  QString error() const;
  // Transport-only producer/consumer; neither method grants tool authority.
  QByteArray takeOutgoing();
  bool acceptPeer(const QJsonObject &, bool &notify);
private:
  ConversationIdentity m_identity;
  mutable QMutex m_mutex;
  struct OutgoingSlot {
    std::atomic<quint64> sequence{0};
    QByteArray bytes;
    qsizetype reservedBytes = 0;
  };
  std::array<OutgoingSlot, ConversationQueueFrames> m_outgoing;
  std::atomic<quint64> m_writePosition{0};
  quint64 m_readPosition = 0; // Transport is the only consumer.
  std::atomic<qsizetype> m_outgoingBytes{0};
  std::atomic_bool m_outgoingFailed{false};
  QQueue<QJsonObject> m_incoming;
  qsizetype m_incomingBytes = 0;
  quint64 m_peerSequence = 0;
  bool m_notified = false;
  QString m_error;
  std::atomic_int m_stop{0};
};
struct ConversationRequest {
  bool enabled = false;
  std::shared_ptr<const VerifiedRuntime> runtime;
  std::shared_ptr<ConversationChannel> channel;
  std::function<void()> incoming; // Coalesced notification; no QObject/UI from the worker.
  std::function<bool()> cancelled;
};
struct ConversationResult {
  QString code;
  QByteArray diagnostics;
  bool ok() const { return code == QLatin1String("ok"); }
};
// Existing TaskService worker only. Fixed entry/env; no arguments/paths from Pi.
// Not an OS sandbox or a new thread pool; cleanup has a bounded grace period.
ConversationResult runConversation(const ConversationRequest &);
} // namespace paleo::pi
