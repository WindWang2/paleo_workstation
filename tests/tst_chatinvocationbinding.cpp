// 层：测试壳
#include "workflow/chatinvocationbinding.h"
#include <QtTest>

namespace {
ChatSession session() {
  ChatSession value; value.id = "host-session"; ChatMessage user; user.role = ChatRole::User;
  user.content = "host intent"; user.hostTurnId = "host-turn"; value.messages.append(user); return value;
}
ChatToolCall call() { return {"host-call", "paleo.python_compute", "{\"client_request_id\":\"model-controlled\",\"parameters\":{}}"}; }
}
class TestChatInvocationBinding : public QObject {
  Q_OBJECT
private slots:
  void hostTupleOverridesModelIdAndReplaysExactly() {
    auto value = session(); QVector<ChatToolCall> calls{call()};
    QCOMPARE(paleo::chat::bindInvocations(value, "host-turn", "doc", calls), QString());
    const auto stable = QJsonDocument::fromJson(calls.first().argumentsJson.toUtf8()).object().value("client_request_id").toString();
    QVERIFY(stable.startsWith("pi-")); QVERIFY(stable != "model-controlled"); QCOMPARE(value.hostToolBindings.size(), 1);
    const auto first = calls; QCOMPARE(paleo::chat::bindInvocations(value, "host-turn", "doc", calls), QString());
    QCOMPARE(calls, first); QCOMPARE(value.hostToolBindings.size(), 1);
    auto restored = ChatSession::fromJson(value.toJson()); QCOMPARE(paleo::chat::validateInvocationBindings(restored), QString());
    QCOMPARE(restored.hostToolBindings, value.hostToolBindings); QCOMPARE(restored.messages.first().hostTurnId, QString("host-turn"));
    QVERIFY(!restored.messages.first().toProtocolJson().contains("host_turn_id"));
  }
  void canonicalArgumentsAreOrderIndependentAndChangedCallsConflict() {
    auto value = session(); QVector<ChatToolCall> calls{call()};
    QCOMPARE(paleo::chat::bindInvocations(value, "host-turn", "doc", calls), QString());
    calls[0].argumentsJson = "{\"parameters\":{},\"client_request_id\":\"different-model-value\"}";
    QCOMPARE(paleo::chat::bindInvocations(value, "host-turn", "doc", calls), QString());
    const auto prior = value.hostToolBindings; calls[0].argumentsJson = "{\"parameters\":{\"changed\":true}}";
    QCOMPARE(paleo::chat::bindInvocations(value, "host-turn", "doc", calls), QString("pi_invocation_binding_conflict"));
    QCOMPARE(value.hostToolBindings, prior);
  }
  void everyPersistentToolUsesTheHostRequestIdButControlToolsKeepTheirSchema() {
    for (const auto &name : {"paleo.python_compute", "paleo.tile_classification_v2", "paleo.well_facies_prediction"}) {
      auto value = session(); auto item = call(); item.name = QLatin1String(name); QVector<ChatToolCall> calls{item};
      QCOMPARE(paleo::chat::bindInvocations(value, "host-turn", "doc", calls), QString());
      QVERIFY(QJsonDocument::fromJson(calls.first().argumentsJson.toUtf8()).object().value("client_request_id").toString().startsWith("pi-"));
    }
    auto value = session(); QVector<ChatToolCall> calls{{"host-call", "paleo.operation_get", "{\"operation_id\":\"existing\"}"}};
    QCOMPARE(paleo::chat::bindInvocations(value, "host-turn", "doc", calls), QString());
    QVERIFY(!QJsonDocument::fromJson(calls.first().argumentsJson.toUtf8()).object().contains("client_request_id"));
  }
  void requestIdDependsOnTheHostTupleRatherThanTheChangedArguments() {
    auto original = session(), missingRow = session(); QVector<ChatToolCall> first{call()}, changed{call()};
    changed[0].argumentsJson = "{\"parameters\":{\"changed\":true}}";
    QCOMPARE(paleo::chat::bindInvocations(original, "host-turn", "doc", first), QString());
    QCOMPARE(paleo::chat::bindInvocations(missingRow, "host-turn", "doc", changed), QString());
    const auto before = original.hostToolBindings.first().toObject(), after = missingRow.hostToolBindings.first().toObject();
    QCOMPARE(before.value("clientRequestId"), after.value("clientRequestId")); QVERIFY(before.value("admittedSha256") != after.value("admittedSha256"));
  }
  void peerSuppliedTurnOrCorruptedBindingCannotCreateAuthority() {
    auto value = session(); QVector<ChatToolCall> calls{call()};
    QCOMPARE(paleo::chat::bindInvocations(value, "peer-turn", "doc", calls), QString("pi_invocation_turn_unavailable"));
    QVERIFY(value.hostToolBindings.isEmpty()); QCOMPARE(paleo::chat::bindInvocations(value, "host-turn", "doc", calls), QString());
    auto row = value.hostToolBindings.first().toObject(); row["clientRequestId"] = "forged"; value.hostToolBindings[0] = row;
    QCOMPARE(paleo::chat::validateInvocationBindings(value), QString("pi_invocation_binding_invalid"));
  }
  void oldSessionsRemainReadableButMalformedNewMetadataIsRejected() {
    auto old = session().toJson(); old.remove("host_tool_bindings"); auto restored = ChatSession::fromJson(old);
    QVERIFY(restored.hostToolBindings.isEmpty()); QCOMPARE(paleo::chat::validateInvocationBindings(restored), QString());
    old["host_tool_bindings"] = QJsonObject{{"peer", "cannot-overwrite"}};
    QCOMPARE(paleo::chat::validateInvocationBindings(ChatSession::fromJson(old)), QString("pi_invocation_binding_invalid"));
  }
};
QTEST_GUILESS_MAIN(TestChatInvocationBinding)
#include "tst_chatinvocationbinding.moc"
