// 层：测试壳
#include <QtTest>
#include "../src/services/toolresultprotocol.h"
#include "../src/workflow/aiconversationpresentation.h"
#include "../src/workflow/chatinvocationbinding.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using paleo::chat::ToolResultFacts;

// G5 W2/W3：只读投影层单测（无 UI）。
//   · projectToolResult：raw/envelope/坏 JSON/数组/operation 包裹的归一事实；
//   · projectSessionTools：消息顺序、配对、绑定归属、歧义、未应答；
//   · decodeUtf8Display：跨页 UTF-8 边界。
// oracle 独立于实现：期望值手写自协议事实（paleo.tool-result.v1 /
// chatToolResponse 透传键），不复制被测函数输出。
namespace {
QString envelopeJson(const QJsonObject &native, bool ok) {
  return QString::fromUtf8(paleo::chat::compactJson(
    paleo::chat::resultEnvelope(native, ok)));
}
ChatMessage userMessage(const QString &turnId = {}) {
  ChatMessage message;
  message.role = ChatRole::User;
  message.content = QStringLiteral("查一下井");
  message.hostTurnId = turnId;
  return message;
}
ChatMessage assistantWithCall(const QString &callId,
                              const QString &tool = QStringLiteral("paleo.query_project")) {
  ChatMessage message;
  message.role = ChatRole::Assistant;
  message.content = QStringLiteral("我来查");
  ChatToolCall call;
  call.id = callId;
  call.name = tool;
  call.argumentsJson = QStringLiteral("{\"topic\":\"wells\"}");
  message.toolCalls.append(call);
  return message;
}
ChatMessage toolMessage(const QString &callId, const QString &content) {
  ChatMessage message;
  message.role = ChatRole::Tool;
  message.toolCallId = callId;
  message.content = content;
  return message;
}
} // namespace

class TestG5ConversationProjection : public QObject {
  Q_OBJECT
private slots:
  void rawNativePayloadKeepsDataObject();
  void completeEnvelopeUnwrapsDataAndTransport();
  void incompleteEnvelopeKeepsFactsWithoutData();
  void unparseableTextIsNeverAnEmptySuccess();
  void arrayRootIsPresentedAsNonObject();
  void operationWrappedRawPayloadCarriesThreeAxes();
  void envelopeKeepsOperationFactsAndCommitOutcome();
  void unknownStatusStaysUnknownOutcome();
  void historyPairingAndAttribution();
  void historyLegacyTurnAndMissingBindingStayUnverified();
  void historyAmbiguousCallIdFlagged();
  void historyUnansweredCallListedWithoutResult();
  void historyCallIdsAreLocalToTheTurn();
  void historyRefusesMismatchedBindingAndFutureCall();
  void utf8BoundariesSplitAcrossPages();
};

void TestG5ConversationProjection::rawNativePayloadKeepsDataObject() {
  const QString json = QStringLiteral(
    R"({"topic":"wells","wells":[{"id":"w1","name":"A1"}],"count":1})");
  const auto facts = paleo::chat::projectToolResult(
    QStringLiteral("paleo.query_project"), json, true, true);
  QCOMPARE(facts.source, ToolResultFacts::Source::RawPayload);
  QVERIFY(facts.hasData);
  QVERIFY(facts.dataIsObject);
  QCOMPARE(facts.data.toObject().value(QStringLiteral("count")).toInt(), 1);
  QCOMPARE(facts.outcome, ToolResultFacts::Outcome::Succeeded);
  QVERIFY(facts.outcomeKnown);
  QVERIFY(!facts.operationFacts);
  QVERIFY(!facts.transportKnown); // raw 帧没有 transport 段（协议事实）
}

void TestG5ConversationProjection::completeEnvelopeUnwrapsDataAndTransport() {
  const auto native = QJsonObject{{"topic", "summary"},
                                  {"entities", QJsonObject{{"well", 3}}}};
  const auto facts = paleo::chat::projectToolResult(
    QStringLiteral("paleo.query_project"), envelopeJson(native, true));
  QCOMPARE(facts.source, ToolResultFacts::Source::Envelope);
  QCOMPARE(facts.status, QStringLiteral("succeeded"));
  QCOMPARE(facts.transportStatus, QStringLiteral("complete"));
  QVERIFY(facts.coverageKnown);
  QVERIFY(facts.coverageComplete);
  QVERIFY(facts.hasData);
  QCOMPARE(facts.data.toObject().value(QStringLiteral("topic")).toString(),
           QStringLiteral("summary"));
  QCOMPARE(facts.outcome, ToolResultFacts::Outcome::Succeeded);
  QCOMPARE(facts.payloadContract, QStringLiteral("legacy.unversioned"));
}

void TestG5ConversationProjection::incompleteEnvelopeKeepsFactsWithoutData() {
  // 大结果走 transportResult：data 移除、transport incomplete、引用式留存。
  QJsonObject native;
  native.insert(QStringLiteral("contract"), QStringLiteral("paleo.well-facies.result.v2"));
  native.insert(QStringLiteral("inputOperationId"), QStringLiteral("op-123"));
  auto envelope = paleo::chat::resultEnvelope(native, true);
  envelope = paleo::chat::transportResult(
    envelope, QStringLiteral("result_referenced"),
    QJsonObject{{"kind", "object"}, {"encodedBytes", 90000}});
  envelope.insert(QStringLiteral("resultRefs"),
                  QJsonArray{QJsonObject{{"kind", "chat-result-v1"},
                                         {"ref", "abc123"}}});
  const auto facts = paleo::chat::projectToolResult(
    QStringLiteral("paleo.well_facies_prediction"),
    QString::fromUtf8(paleo::chat::compactJson(envelope)));
  QCOMPARE(facts.source, ToolResultFacts::Source::Envelope);
  QCOMPARE(facts.transportCode, QStringLiteral("result_referenced"));
  QVERIFY(!facts.coverageComplete);
  QVERIFY(facts.retryAdviceNoRepeat);
  QVERIFY(!facts.hasData); // 留存不含 data：呈现层不得伪造空成功
  QCOMPARE(facts.resultRefs.size(), 1);
  QCOMPARE(facts.encodedBytesNote, QStringLiteral("90000"));
  // transport incomplete ≠ 业务失败：outcome 以 envelope status 为准。
  QCOMPARE(facts.outcome, ToolResultFacts::Outcome::Succeeded);
}

void TestG5ConversationProjection::unparseableTextIsNeverAnEmptySuccess() {
  const auto facts = paleo::chat::projectToolResult(
    QStringLiteral("paleo.tile_classification_v2"),
    QStringLiteral("<html>半截输出"), true, true);
  QCOMPARE(facts.source, ToolResultFacts::Source::Unparseable);
  QVERIFY(!facts.hasData);
  QVERIFY(facts.rawText.contains(QStringLiteral("半截输出")));
  QVERIFY(!facts.rawTruncated);
  // 调用侧 ok 仍保留为事实，但内容不可解析不是「成功空结果」。
  QCOMPARE(facts.outcome, ToolResultFacts::Outcome::Succeeded);
  const auto noEvidence = paleo::chat::projectToolResult(
    QStringLiteral("paleo.tile_classification_v2"), QStringLiteral("{oops"));
  QCOMPARE(noEvidence.outcome, ToolResultFacts::Outcome::Unknown);
  QVERIFY(!noEvidence.outcomeKnown);
}

void TestG5ConversationProjection::arrayRootIsPresentedAsNonObject() {
  const auto facts = paleo::chat::projectToolResult(
    QStringLiteral("paleo.query_project"), QStringLiteral("[1,2,3]"), true, true);
  QCOMPARE(facts.source, ToolResultFacts::Source::RawPayload);
  QVERIFY(facts.hasData);
  QVERIFY(!facts.dataIsObject);
  QCOMPARE(facts.data.toArray().size(), 3);
}

void TestG5ConversationProjection::operationWrappedRawPayloadCarriesThreeAxes() {
  // chatToolResponse 形态（capabilitychatprotocol.cpp:117 透传键）。
  const QString json = QStringLiteral(
    R"({"contract":"paleo.operation.v2","ok":true,"code":"ok",)"
    R"("operationId":"op-abc","execution":"succeeded","commitState":"committed",)"
    R"("presentation":"pending","revision":"7","computeCount":2,)"
    R"("cancelRequested":false,"commitReceiptRef":"rcpt-1",)"
    R"("resultRef":{"kind":"operation-result-v2","operationId":"op-abc"},)"
    R"("rows":12})");
  const auto facts = paleo::chat::projectToolResult(
    QStringLiteral("paleo.tile_classification_v2"), json, true, true);
  QVERIFY(facts.operationFacts);
  QCOMPARE(facts.operationId, QStringLiteral("op-abc"));
  QCOMPARE(facts.execution, QStringLiteral("succeeded"));
  QCOMPARE(facts.commitState, QStringLiteral("committed"));
  QCOMPARE(facts.presentation, QStringLiteral("pending"));
  QCOMPARE(facts.revision, QStringLiteral("7"));
  QCOMPARE(facts.computeCount, 2.0);
  QVERIFY(facts.computeCountKnown);
  QVERIFY(!facts.cancelRequested);
  QCOMPARE(facts.commitReceiptRef, QStringLiteral("rcpt-1"));
  QVERIFY(facts.responseOkKnown);
  QVERIFY(facts.responseOk); // 查询 ok=true：呈现层不得当业务完成（W4 红线）
  // operation 包裹不吞 data（summary 字段仍在）。
  QVERIFY(facts.hasData);
  QCOMPARE(facts.data.toObject().value(QStringLiteral("rows")).toInt(), 12);
}

void TestG5ConversationProjection::envelopeKeepsOperationFactsAndCommitOutcome() {
  // 持久化路径：resultEnvelope 把 operation 键复制到顶层（toolloop 落盘形态）。
  const auto native = QJsonObject{{"contract", "paleo.operation.v2"},
                                  {"operationId", "op-9"},
                                  {"execution", "succeeded"},
                                  {"commitState", "committed"},
                                  {"presentation", "presented"},
                                  {"revision", "3"}};
  const auto facts = paleo::chat::projectToolResult(
    QStringLiteral("paleo.operation_get"), envelopeJson(native, true));
  QCOMPARE(facts.source, ToolResultFacts::Source::Envelope);
  QVERIFY(facts.operationFacts);
  QCOMPARE(facts.commitState, QStringLiteral("committed"));
  // status=committed → 已知成功（已提交事实不被 transport 状态抹掉）。
  QCOMPARE(facts.outcome, ToolResultFacts::Outcome::Succeeded);
}

void TestG5ConversationProjection::unknownStatusStaysUnknownOutcome() {
  const auto facts = paleo::chat::projectToolResult(
    QStringLiteral("paleo.operation_get"),
    QString::fromUtf8(paleo::chat::compactJson(paleo::chat::resultError(
      QStringLiteral("commit_uncertain")))));
  QCOMPARE(facts.status, QStringLiteral("unknown"));
  QVERIFY(!facts.outcomeKnown);
  QCOMPARE(facts.outcome, ToolResultFacts::Outcome::Unknown);
  QCOMPARE(facts.transportCode, QStringLiteral("commit_uncertain"));
}

void TestG5ConversationProjection::historyPairingAndAttribution() {
  ChatSession session;
  session.id = QStringLiteral("sess-1");
  session.messages << userMessage(QStringLiteral("turn-1"))
                   << assistantWithCall(QStringLiteral("call-1"))
                   << toolMessage(QStringLiteral("call-1"),
                                  envelopeJson(QJsonObject{{"topic", "summary"}}, true));
  auto &calls = session.messages[1].toolCalls;
  QVERIFY(paleo::chat::bindInvocations(session, "turn-1", "doc-A", calls).isEmpty());
  const auto entries = paleo::chat::projectSessionTools(session);
  QCOMPARE(entries.size(), 1);
  const auto &entry = entries.first();
  QCOMPARE(entry.toolCallId, QStringLiteral("call-1"));
  QCOMPARE(entry.toolName, QStringLiteral("paleo.query_project"));
  QVERIFY(entry.callPaired);
  QVERIFY(!entry.ambiguousCallId);
  QCOMPARE(entry.turnId, QStringLiteral("turn-1"));
  QVERIFY(entry.bindingKnown);
  QCOMPARE(entry.bindingDocumentId, QStringLiteral("doc-A"));
  QVERIFY(entry.hasResult);
  QCOMPARE(entry.facts.source, ToolResultFacts::Source::Envelope);
  QCOMPARE(entry.argumentsJson, QStringLiteral("{\"topic\":\"wells\"}"));
}

void TestG5ConversationProjection::historyLegacyTurnAndMissingBindingStayUnverified() {
  ChatSession session;
  session.id = QStringLiteral("sess-2");
  session.messages << userMessage() // 旧记录：无 hostTurnId
                   << assistantWithCall(QStringLiteral("call-x"))
                   << toolMessage(QStringLiteral("call-x"),
                                  envelopeJson(QJsonObject{{"topic", "summary"}}, true));
  const auto entries = paleo::chat::projectSessionTools(session);
  QCOMPARE(entries.size(), 1);
  QVERIFY(entries.first().turnId.isEmpty()); // 归属未验证，不猜
  QVERIFY(!entries.first().bindingKnown);
}

void TestG5ConversationProjection::historyAmbiguousCallIdFlagged() {
  ChatSession session;
  session.id = QStringLiteral("sess-3");
  session.messages << userMessage(QStringLiteral("turn-2"))
                   << assistantWithCall(QStringLiteral("dup"))
                   << toolMessage(QStringLiteral("dup"), QStringLiteral("{}"))
                   << assistantWithCall(QStringLiteral("dup"));
  const auto entries = paleo::chat::projectSessionTools(session);
  QCOMPARE(entries.size(), 1);
  QVERIFY(entries.first().ambiguousCallId); // 同 id 两个声明位：如实标歧义
  QVERIFY(entries.first().callPaired);
}

void TestG5ConversationProjection::historyUnansweredCallListedWithoutResult() {
  ChatSession session;
  session.id = QStringLiteral("sess-4");
  session.messages << userMessage(QStringLiteral("turn-3"))
                   << assistantWithCall(QStringLiteral("call-never"));
  const auto entries = paleo::chat::projectSessionTools(session);
  QCOMPARE(entries.size(), 1);
  QVERIFY(entries.first().callPaired);
  QVERIFY(!entries.first().hasResult); // 结果缺失：如实列出，不补写
  QCOMPARE(entries.first().turnId, QStringLiteral("turn-3"));
}

void TestG5ConversationProjection::historyCallIdsAreLocalToTheTurn() {
  ChatSession session;
  session.id = QStringLiteral("reused-call");
  session.messages << userMessage("one") << assistantWithCall("id", "paleo.query_project")
                   << toolMessage("id", "{}") << userMessage("two")
                   << assistantWithCall("id", "paleo.operation_get");
  QVERIFY(paleo::chat::bindInvocations(session, "one", "doc-A",
                                      session.messages[1].toolCalls).isEmpty());
  QVERIFY(paleo::chat::bindInvocations(session, "two", "doc-B",
                                      session.messages[4].toolCalls).isEmpty());

  const auto entries = paleo::chat::projectSessionTools(session);
  QCOMPARE(entries.size(), 2);
  QCOMPARE(entries[0].toolName, QStringLiteral("paleo.query_project"));
  QCOMPARE(entries[0].bindingDocumentId, QStringLiteral("doc-A"));
  QVERIFY(entries[0].bindingKnown && !entries[0].ambiguousCallId);
  QCOMPARE(entries[1].toolName, QStringLiteral("paleo.operation_get"));
  QCOMPARE(entries[1].bindingDocumentId, QStringLiteral("doc-B"));
  QVERIFY(entries[1].bindingKnown && !entries[1].hasResult);
}

void TestG5ConversationProjection::historyRefusesMismatchedBindingAndFutureCall() {
  ChatSession session;
  session.id = QStringLiteral("changed-call");
  session.messages << userMessage("one") << toolMessage("early", "{}")
                   << assistantWithCall("id") << toolMessage("id", "{}")
                   << assistantWithCall("early");
  QVERIFY(paleo::chat::bindInvocations(session, "one", "doc-A",
                                      session.messages[2].toolCalls).isEmpty());
  session.messages[2].toolCalls[0].argumentsJson = QStringLiteral(R"({"topic":"assets"})");

  const auto entries = paleo::chat::projectSessionTools(session);
  QCOMPARE(entries.size(), 3);
  QVERIFY(!entries[0].callPaired && !entries[0].bindingKnown);
  QVERIFY(entries[1].callPaired && !entries[1].bindingKnown);
  QVERIFY(entries[2].callPaired && !entries[2].hasResult);
}

void TestG5ConversationProjection::utf8BoundariesSplitAcrossPages() {
  // 三字节「砂」(E7 A0 82) 跨页：前两字节必须留给下一页拼接。
  const QByteArray sand("\xe7\xa0\x82");
  const auto first = paleo::chat::decodeUtf8Display(sand.left(2));
  QCOMPARE(first.trailingBytes, 2);
  QVERIFY(first.text.isEmpty());
  const auto second = paleo::chat::decodeUtf8Display(sand.left(2) + sand.right(1) + "岩");
  QCOMPARE(second.trailingBytes, 0);
  QVERIFY(second.text.startsWith(QString::fromUtf8("砂岩")));
  QVERIFY(!second.invalid);
  // 四字节 emoji（F0 9F 98 80）跨页 + 完整页。
  const QByteArray emoji("\xf0\x9f\x98\x80");
  QCOMPARE(paleo::chat::decodeUtf8Display(emoji).trailingBytes, 0);
  QCOMPARE(paleo::chat::decodeUtf8Display(emoji.left(3)).trailingBytes, 3);
  // 非法序列：替换字符 + invalid 标记，不崩。
  const auto broken = paleo::chat::decodeUtf8Display(QByteArray("\xff\xfe"));
  QVERIFY(broken.invalid);
  const auto stray = paleo::chat::decodeUtf8Display(QByteArray("A\x80"));
  QCOMPARE(stray.trailingBytes, 0);
  QVERIFY(stray.invalid);
  const auto overlong = paleo::chat::decodeUtf8Display(QByteArray("\xe0\x80"));
  QCOMPARE(overlong.trailingBytes, 0);
  QVERIFY(overlong.invalid);
  QVERIFY(!paleo::chat::decodeUtf8Display(QByteArray("\xef\xbf\xbd")).invalid);
  // 末页（holdTail=false）：残缺尾序列不再切尾吞字节，按替换呈现并置 invalid。
  const auto tail = paleo::chat::decodeUtf8Display(sand.left(2), /*holdTail=*/false);
  QCOMPARE(tail.trailingBytes, 0);
  QVERIFY(tail.invalid); // 截尾的半个「砂」以替换字符如实呈现
  QVERIFY(tail.text.contains(QChar::ReplacementCharacter));
}

QTEST_APPLESS_MAIN(TestG5ConversationProjection)
#include "tst_g5_conversation_projection.moc"
