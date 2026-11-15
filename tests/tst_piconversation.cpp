// 层：测试壳
#include "services/piconversation.h"
#include "services/piprotocol.h"
#include <QtTest>
#include <QJsonDocument>
#include <thread>

using namespace paleo::pi;
namespace {
ConversationIdentity identity() { return {"host-session", "18446744073709551615", "host-turn", QString(32, 'a')}; }
QJsonObject peer(int sequence = 1) {
  const auto value = identity();
  return {{"protocol", QLatin1String(ConversationProtocol)}, {"type", "opened"}, {"payload", QJsonObject{}},
    {"sessionId", value.sessionId}, {"generation", value.generation}, {"turnId", value.turnId},
    {"nonce", value.nonce}, {"sequence", QString::number(sequence)}};
}
}
class TestPiConversation : public QObject {
  Q_OBJECT
private slots:
  void hostIdentityAndCounterRoundTripWithoutNumericLoss() {
    ConversationChannel channel(identity()); QVERIFY(channel.send("open")); auto bytes = channel.takeOutgoing();
    bool complete = false; QString error; QJsonObject value;
    QVERIFY(takeFrame(bytes, value, complete, error)); QVERIFY(complete); QVERIFY(bytes.isEmpty());
    QCOMPARE(value.value("generation").toString(), QString("18446744073709551615"));
    QCOMPARE(value.value("sequence").toString(), QString("1")); QCOMPARE(value.value("protocol").toString(), QLatin1String(ConversationProtocol));
  }
  void forgedIdentityAndSequenceNeverYieldAnOwnerEvent_data() {
    QTest::addColumn<QString>("field");
    for (const auto field : {"sessionId", "generation", "turnId", "nonce", "sequence", "protocol", "extra"}) QTest::newRow(field) << QString(field);
  }
  void forgedIdentityAndSequenceNeverYieldAnOwnerEvent() {
    QFETCH(QString, field); ConversationChannel channel(identity()); auto value = peer(); value[field] = "forged";
    bool notify = false; QVERIFY(!channel.acceptPeer(value, notify)); QVERIFY(!notify);
    QVector<QJsonObject> frames; QVERIFY(channel.takePeer(frames)); QVERIFY(frames.isEmpty());
    QCOMPARE(channel.error(), QString("pi_conversation_identity"));
  }
  void notificationsAreCoalescedAndBothDirectionsAreBounded() {
    ConversationChannel channel(identity()); bool notify = false;
    for (int i = 1; i <= ConversationQueueFrames; ++i) { QVERIFY(channel.acceptPeer(peer(i), notify)); QCOMPARE(notify, i == 1); }
    QVector<QJsonObject> frames; QVERIFY(channel.takePeer(frames)); QCOMPARE(frames.size(), ConversationQueueFrames);
    QVERIFY(channel.acceptPeer(peer(ConversationQueueFrames + 1), notify)); QVERIFY(notify);
    for (int i = 0; i < ConversationOutgoingFrames; ++i) QVERIFY(channel.send("model_delta", {{"text", "bounded"}}));
    QVERIFY(!channel.send("model_delta", {{"text", "overflow"}})); QCOMPARE(channel.error(), QString("pi_outgoing_budget"));
    ConversationChannel incoming(identity());
    for (int i = 1; i <= ConversationQueueFrames; ++i) QVERIFY(incoming.acceptPeer(peer(i), notify));
    QVERIFY(!incoming.acceptPeer(peer(ConversationQueueFrames + 1), notify)); QCOMPARE(incoming.error(), QString("pi_incoming_budget"));
  }
  void oversizedFramesFailWholeAndExplicitCancelCanUpgradeInterruption() {
    ConversationChannel channel(identity()); QVERIFY(!channel.send("model_delta", {{"text", QString(MaxFrameBytes, 'x')}}));
    QVERIFY(channel.takeOutgoing().isEmpty()); channel.requestStop(false); QCOMPARE(channel.stopReason(), 2);
    channel.requestStop(true); QCOMPARE(channel.stopReason(), 1); channel.requestStop(false); QCOMPARE(channel.stopReason(), 1);
  }
  void concurrentControlAndModelProducersDoNotLoseFramesOnMutexContention() {
    ConversationChannel channel(identity()); quint64 expected = 1;
    for (int batch = 0; batch < 32; ++batch) {
      std::atomic_bool accepted{true};
      const auto produce = [&] { for (int i = 0; i < 8; ++i) if (!channel.send("model_delta", {{"text", "bounded"}})) accepted = false; };
      std::thread model(produce), control(produce); model.join(); control.join(); QVERIFY(accepted.load());
      for (int index = 0; index < 16; ++index) {
        auto bytes = channel.takeOutgoing(); bool complete = false; QString error; QJsonObject value;
        QVERIFY(takeFrame(bytes, value, complete, error)); QVERIFY(complete); QVERIFY(bytes.isEmpty());
        QCOMPARE(value.value("sequence").toString(), QString::number(expected++));
      }
      QVERIFY(channel.takeOutgoing().isEmpty()); QVERIFY(channel.error().isEmpty());
    }
  }
  void fullRetryPreservesTheFrameWithoutPoisoningOrConsumingASequence() {
    ConversationChannel channel(identity());
    for (int i = 0; i < ConversationOutgoingFrames; ++i) QVERIFY(channel.trySend("model_delta", {{"text", "bounded"}}));
    QVERIFY(!channel.trySend("model_end")); QVERIFY(channel.error().isEmpty());
    QVERIFY(!channel.takeOutgoing().isEmpty()); QVERIFY(channel.trySend("model_end"));
    for (int i = 0; i < ConversationOutgoingFrames - 1; ++i) QVERIFY(!channel.takeOutgoing().isEmpty());
    auto bytes = channel.takeOutgoing(); bool complete = false; QString error; QJsonObject value;
    QVERIFY(takeFrame(bytes, value, complete, error)); QVERIFY(complete);
    QCOMPARE(value.value("type").toString(), QString("model_end"));
    QCOMPARE(value.value("sequence").toString(), QString::number(ConversationOutgoingFrames + 1));
  }
  void disabledAndOwnerCallsStartNoOptionalRuntime() {
    ConversationRequest request; QCOMPARE(runConversation(request).code, QString("pi_disabled"));
    request.enabled = true; QCOMPARE(runConversation(request).code, QString("pi_worker_required"));
  }
};
QTEST_GUILESS_MAIN(TestPiConversation)
#include "tst_piconversation.moc"
