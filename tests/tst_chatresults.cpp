// 层：测试壳
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include "services/toolresultprotocol.h"
#include "workflow/chatresultstore.h"

using namespace paleo::chat;
namespace {
QString raw(const QJsonValue &value) { return QString::fromUtf8(compactJson(value)); }
QString resultRef(const QJsonObject &envelope) {
  const auto refs = envelope.value("resultRefs").toArray();
  return refs.isEmpty() ? QString() : refs.first().toObject().value("ref").toString();
}
QJsonArray rows(int count) {
  QJsonArray values;
  for (int i = 0; i < count; ++i)
    values.append(QJsonObject{{"id", QStringLiteral("井-%1-😀").arg(i)},
                             {"label", QString(30, QChar(0x5ca9)) + "\"\\"}});
  return values;
}
}

class TestChatResults : public QObject {
  Q_OBJECT
private slots:
  void legalEnvelopeAndSeparateSchema();
  void arrayPagesReconstructCompleteValues();
  void nestedObjectsAndEscapedPointers();
  void longUnicodeStringReconstructs();
  void scopeExpiryAndUnknownReferences();
  void capacityDoesNotEvictPromisedResults();
  void committedFactsSurviveTransportFailure();
  void cursorCannotChangeSelectorOrBeForged();
  void invalidInputAndBudgetsAreExplicit();
  void readResponsesNeverCreateReferences();
  void oversizedIdentityIsNotSliced();
  void escapedMetadataStillRespectsMinimumBudget();
  void actualGatewayCommitReplayFactsSurviveCacheFailure();
  void wellBatchRecoveryFactsSurviveAllTransportFallbacks();
};

void TestChatResults::legalEnvelopeAndSeparateSchema() {
  ChatResultStore store;
  const QJsonObject native{{"contractVersion", "native.strict.v1"}, {"id", "v1"}};
  const auto encoded = store.encode(raw(native), true, 8192);
  QCOMPARE(encoded.value("data").toObject(), native);
  QVERIFY(isResultEnvelope(encoded));
  QVERIFY(!isResultEnvelope(native));
  QCOMPARE(resultEnvelope(QJsonObject{{"contract", "paleo.project.query.page.v1"}}, true)
      .value("payloadContract"), QJsonValue("paleo.project.query.page.v1"));
  const QJsonObject nativeSchema{{"type", "object"}, {"additionalProperties", false}};
  QCOMPARE(toolResultEnvelopeSchema(nativeSchema).value("properties").toObject().value("data").toObject(), nativeSchema);
  QVERIFY(compactJson(encoded).size() <= 8192);
  QVERIFY(!resultEnvelope(QJsonObject{{"truncated", "first 50 of 100"}}, true)
      .value("coverage").toObject().value("complete").toBool());
}

void TestChatResults::arrayPagesReconstructCompleteValues() {
  ChatResultStore store;
  const auto values = rows(200);
  const auto envelope = store.encode(raw(values), true, 2048);
  const auto reference = resultRef(envelope);
  QVERIFY(!reference.isEmpty());
  QJsonObject request{{"result_ref", reference}, {"page_size", 7}, {"max_bytes", 2048}};
  QJsonArray assembled;
  for (int pages = 0; pages < 300; ++pages) {
    const auto page = store.read(request);
    QCOMPARE(page.value("code"), QJsonValue("ok"));
    QVERIFY(compactJson(page).size() <= 2048);
    QCOMPARE(page.value("offset").toInt(), assembled.size());
    for (const auto &entry : page.value("entries").toArray()) {
      QVERIFY(entry.toObject().contains("value"));
      assembled.append(entry.toObject().value("value"));
    }
    if (!page.value("hasMore").toBool()) break;
    request.insert("cursor", page.value("nextCursor"));
  }
  QCOMPARE(assembled, values);
}

void TestChatResults::nestedObjectsAndEscapedPointers() {
  ChatResultStore store;
  const QJsonObject value{{"a/b~c", rows(100)}, {"tiny", QJsonObject{{"n", 3}}}};
  const auto reference = resultRef(store.encode(raw(value), true, 1024));
  QVERIFY(!reference.isEmpty());
  const auto page = store.read({{"result_ref", reference}, {"max_bytes", 2048}});
  QCOMPARE(page.value("code"), QJsonValue("ok"));
  const auto first = page.value("entries").toArray().first().toObject();
  QCOMPARE(first.value("child").toObject().value("selector"), QJsonValue("/a~1b~0c"));
  const auto nested = store.read({{"result_ref", reference}, {"selector", "/a~1b~0c"}, {"page_size", 2}});
  QCOMPARE(nested.value("totalCount"), QJsonValue(100));
  QCOMPARE(nested.value("returnedCount"), QJsonValue(2));
}

void TestChatResults::longUnicodeStringReconstructs() {
  ChatResultStore store;
  const QString text = QStringLiteral("岩😀\"\\").repeated(18000);
  const auto reference = resultRef(store.encode(raw(QJsonObject{{"text", text}}), true, 1024));
  QVERIFY(!reference.isEmpty());
  QJsonObject request{{"result_ref", reference}, {"selector", "/text"}, {"mode", "text_parts"}, {"max_bytes", 4096}};
  QString assembled;
  for (int pages = 0; pages < 1000; ++pages) {
    const auto page = store.read(request);
    QCOMPARE(page.value("code"), QJsonValue("ok"));
    QVERIFY(compactJson(page).size() <= 4096);
    QVERIFY(page.value("valueFragment").toBool());
    const auto fragment = page.value("text").toString();
    QVERIFY(fragment.isEmpty() || !fragment.back().isHighSurrogate());
    assembled += fragment;
    if (!page.value("hasMore").toBool()) break;
    request.insert("cursor", page.value("nextCursor"));
  }
  QCOMPARE(assembled, text);
}

void TestChatResults::scopeExpiryAndUnknownReferences() {
  qint64 now = 100;
  ChatResultStore::Limits limits;
  limits.ttlMs = 10;
  ChatResultStore store(limits, [&now] { return now; });
  store.setScope("chat", "doc", 1);
  const auto reference = resultRef(store.encode(raw(rows(100)), true, 1024));
  now = 111;
  QCOMPARE(store.read({{"result_ref", reference}}).value("code"), QJsonValue("result_expired"));
  store.setScope("chat", "doc", 2);
  QCOMPARE(store.read({{"result_ref", reference}}).value("code"), QJsonValue("result_scope_changed"));
  const auto fresh = resultRef(store.encode(raw(rows(100)), true, 1024));
  auto unknown = fresh;
  unknown[unknown.size() - 1] = unknown.back() == 'a' ? 'b' : 'a';
  QCOMPARE(store.read({{"result_ref", unknown}}).value("code"), QJsonValue("unknown_result_ref"));
  store.setScope("other-chat", "doc", 2);
  QCOMPARE(store.read({{"result_ref", fresh}}).value("code"), QJsonValue("result_scope_changed"));
}

void TestChatResults::capacityDoesNotEvictPromisedResults() {
  ChatResultStore::Limits limits;
  limits.handles = 1;
  ChatResultStore store(limits);
  const auto first = resultRef(store.encode(raw(rows(100)), true, 1024));
  const auto second = store.encode(raw(rows(100)), true, 1024);
  QCOMPARE(second.value("transport").toObject().value("code"), QJsonValue("result_cache_full"));
  QVERIFY(resultRef(second).isEmpty());
  QCOMPARE(store.retainedResults(), 1);
  QCOMPARE(store.read({{"result_ref", first}}).value("code"), QJsonValue("ok"));
}

void TestChatResults::committedFactsSurviveTransportFailure() {
  ChatResultStore::Limits limits;
  limits.resultBytes = 2000;
  ChatResultStore store(limits);
  const QJsonObject native{{"status", "committed"}, {"code", "ok"}, {"operationId", "op-123"},
      {"commit", QJsonObject{{"state", "committed"}}}, {"commitReceiptRef", "receipt-123"}, {"rows", rows(100)}};
  const auto response = store.encode(raw(native), true, 1024);
  QCOMPARE(response.value("status"), QJsonValue("committed"));
  QCOMPARE(response.value("operationId"), QJsonValue("op-123"));
  QCOMPARE(response.value("commitReceiptRef"), QJsonValue("receipt-123"));
  QCOMPARE(response.value("commit"), native.value("commit"));
  QCOMPARE(response.value("transport").toObject().value("code"), QJsonValue("result_too_large"));
  QCOMPARE(response.value("retryAdvice"), QJsonValue("do_not_repeat_execution"));
}

void TestChatResults::cursorCannotChangeSelectorOrBeForged() {
  ChatResultStore store;
  const auto reference = resultRef(store.encode(raw(QJsonObject{{"a", rows(100)}, {"b", rows(100)}}), true, 1024));
  const auto first = store.read({{"result_ref", reference}, {"selector", "/a"}, {"page_size", 1}});
  auto cursor = first.value("nextCursor").toString();
  QVERIFY(!cursor.isEmpty());
  QCOMPARE(store.read({{"result_ref", reference}, {"selector", "/b"}, {"cursor", cursor}}).value("code"), QJsonValue("invalid_result_cursor"));
  cursor[cursor.size() - 1] = cursor.back() == 'a' ? 'b' : 'a';
  QCOMPARE(store.read({{"result_ref", reference}, {"selector", "/a"}, {"cursor", cursor}}).value("code"), QJsonValue("invalid_result_cursor"));
}

void TestChatResults::invalidInputAndBudgetsAreExplicit() {
  ChatResultStore store;
  QCOMPARE(store.encode("{broken", true, 1024).value("code"), QJsonValue("invalid_tool_result"));
  const auto reference = resultRef(store.encode(raw(rows(100)), true, 1024));
  QCOMPARE(store.read({{"result_ref", reference}, {"max_bytes", 1}}).value("code"), QJsonValue("invalid_page_budget"));
  QCOMPARE(store.read({{"result_ref", reference}, {"selector", "/01"}}).value("code"), QJsonValue("invalid_result_selector"));
  QCOMPARE(store.read({{"result_ref", reference}, {"page_size", -1}}).value("code"), QJsonValue("invalid_page_size"));
  QCOMPARE(store.read({{"result_ref", reference}, {"page_size", "2"}}).value("code"), QJsonValue("invalid_page_size"));
  QCOMPARE(store.read({{"result_ref", reference}, {"selector", 12}}).value("code"), QJsonValue("invalid_result_selector"));
  ChatResultStore::Limits invalid;
  invalid.handles = -1;
  invalid.ttlMs = -1;
  ChatResultStore disabled(invalid);
  const auto response = disabled.encode(raw(rows(100)), true, 1024);
  QVERIFY(resultRef(response).isEmpty());
  QCOMPARE(response.value("transport").toObject().value("code"), QJsonValue("result_cache_disabled"));
}

void TestChatResults::readResponsesNeverCreateReferences() {
  ChatResultStore store;
  const auto response = store.encode(raw(rows(100)), true, 1024, false);
  QVERIFY(resultRef(response).isEmpty());
  QCOMPARE(store.retainedResults(), 0);
  QCOMPARE(response.value("transport").toObject().value("code"), QJsonValue("page_budget_exceeded"));
}

void TestChatResults::oversizedIdentityIsNotSliced() {
  ChatResultStore store;
  const auto response = store.encode(raw(QJsonObject{{"status", "committed"},
      {"operationId", QString(10000, 'x')}, {"data", rows(100)}}), true, 1024);
  QVERIFY(compactJson(response).size() <= 1024);
  QVERIFY(!response.contains("operationId"));
  QVERIFY(!resultRef(response).isEmpty());
  QCOMPARE(response.value("status"), QJsonValue("committed"));
  QVERIFY(response.value("coverage").toObject().value("omittedFields").toArray().contains("operationId"));
}

void TestChatResults::escapedMetadataStillRespectsMinimumBudget() {
  for (const auto &label : {QString(128, 'x'), QString(42, QChar(0x5ca9)), QString(10000, '\\')}) {
    ChatResultStore store;
    QJsonObject native;
    for (const auto &field : {"status", "code", "contractVersion", "operationId", "commitReceiptRef", "execution"})
      native.insert(field, label);
    native.insert("commit", QJsonObject{{"state", "committed"}, {"receipt", label}});
    native.insert("rows", rows(50));
    const auto response = store.encode(raw(native), true, kMinimumEnvelopeBytes);
    QVERIFY2(compactJson(response).size() <= kMinimumEnvelopeBytes, compactJson(response).constData());
    QVERIFY(isResultEnvelope(response));
  }
  ChatResultStore store;
  const auto escaped = store.encode(raw(QJsonObject{{"status", QString(128, 's')},
      {"code", QString(128, 'c')}, {"contractVersion", QString(128, 'v')},
      {"commit", QJsonObject{{"state", QString(64, QChar(0))}}}, {"rows", rows(50)}}),
      true, kMinimumEnvelopeBytes);
  QVERIFY(compactJson(escaped).size() <= kMinimumEnvelopeBytes);
}

void TestChatResults::actualGatewayCommitReplayFactsSurviveCacheFailure() {
  ChatResultStore::Limits limits;
  limits.handles = 0;
  ChatResultStore store(limits);
  for (const int budget : {kMinimumEnvelopeBytes, 1024})
  for (const auto &code : {"computed", "replay", "receipt_persist_failed"}) {
    const QJsonObject native{{"code", code}, {"ok", false}, {"execution", "succeeded"},
      {"commitState", "committed"}, {"presentation", "not_requested"},
      {"operationId", "op-real"}, {"receipt", "set-receipt-real"}, {"rows", rows(100)}};
    const auto response = store.encode(raw(native), false, budget);
    QCOMPARE(response.value("status"), QJsonValue("committed"));
    QCOMPARE(response.value("code"), native.value("code"));
    QCOMPARE(response.value("execution"), native.value("execution"));
    QCOMPARE(response.value("commitState"), native.value("commitState"));
    QCOMPARE(response.value("presentation"), native.value("presentation"));
    QCOMPARE(response.value("receipt"), native.value("receipt"));
    QCOMPARE(response.value("operationId"), native.value("operationId"));
    QCOMPARE(response.value("transport").toObject().value("code"), QJsonValue("result_cache_full"));
    QVERIFY(compactJson(response).size() <= budget);
  }
}

void TestChatResults::wellBatchRecoveryFactsSurviveAllTransportFallbacks() {
  for(const bool fullCache:{false,true})for(const bool forceMinimal:{false,true}) {
    ChatResultStore::Limits limits;if(fullCache)limits.handles=0;
    ChatResultStore store(limits);
    const QJsonObject native{{"contract","paleo.well-facies.result.v2"},{"status","result_unavailable"},
      {"code","remote_succeeded_result_unavailable"},{"execution","partial"},{"executionScope","batch_summary"},
      {"inputOperationId","11111111-1111-1111-1111-111111111111"},
      {"childOperationId","22222222-2222-2222-2222-222222222222"},
      {"remoteOutcome","succeeded"},{"resultAvailability","unavailable"},
      {"receipt",forceMinimal?QJsonValue(QString(2000,'x')):QJsonValue()}, {"rows",rows(100)}};
    const auto envelope=store.encode(raw(native),false,kMinimumEnvelopeBytes);
    QVERIFY(compactJson(envelope).size()<=kMinimumEnvelopeBytes);QVERIFY(isResultEnvelope(envelope));
    for(const auto &field:{"inputOperationId","childOperationId","executionScope","remoteOutcome","resultAvailability"})
      QCOMPARE(envelope.value(QLatin1String(field)),native.value(QLatin1String(field)));
    QVERIFY(!envelope.contains("operationId"));QCOMPARE(envelope.value("executionScope").toString(),"batch_summary");
    QCOMPARE(envelope.value("transport").toObject().value("status").toString(),"incomplete");
    QVERIFY(!envelope.value("coverage").toObject().value("complete").toBool());
  }
}

QTEST_GUILESS_MAIN(TestChatResults)
#include "tst_chatresults.moc"
