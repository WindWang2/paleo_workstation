// 层：测试壳
// H7-W1/W3/W4/W5/W6：临时工具结果的最终 envelope 预算、连续分页与过期生命周期。
//
// oracle 口径：每一页都必须能作为最终 paleo.tool-result.v1 envelope 原样发送
// （resultEnvelopeBytes ≤ 该调用有效配额），从真实页重组出的值与原不可变值
// 完全相等；同引用多页不得增长句柄/引用。
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include "services/toolresultprotocol.h"
#include "workflow/chatresultstore.h"
#include "workflow/toolreplybudget.h"

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
                             {"label", QString(30, QChar(0x5ca8)) + "\"\\"}});
  return values;
}
int codepointsOf(const QString &text) {
  int count = 0;
  for (qsizetype i = 0; i < text.size(); ++i) {
    if (text[i].isHighSurrogate() && i + 1 < text.size() && text[i + 1].isLowSurrogate()) ++i;
    ++count;
  }
  return count;
}
bool fits(const QJsonObject &page, int quota) {
  return resultEnvelopeBytes(page, true, quota) <= quota;
}
} // namespace

class TestH7ResultBudget : public QObject {
  Q_OBJECT
private slots:
  void finalEnvelopeFitsQuotaAndReassembles();
  void requestCeilingDoesNotBecomeCursorIdentity();
  void nestedLargeMemberUsesSameReference();
  void unicodePiecesFitQuotaAndReassemble();
  void emptyContainersAndScalarsAreComplete();
  void minimumQuotaFailureKeepsOriginalReference();
  void expiredReadReleasesValueImmediately();
  void replyQuotaContractStaysBounded();
};

void TestH7ResultBudget::finalEnvelopeFitsQuotaAndReassembles() {
  ChatResultStore store;
  const auto values = rows(120);
  const auto reference = resultRef(store.encode(raw(values), true, 1024));
  QVERIFY(!reference.isEmpty());
  // 同一请求（max_bytes=65536）下配额由 8192 收缩到 1024 再回升 4096；
  // cursor 只按 ref/selector/mode/offset 记账，不因宿主配额变化而失效。
  const QVector<int> quotas{8192, 1024, 4096, 2048, 1280};
  QJsonObject request{{"result_ref", reference}, {"page_size", 7}, {"max_bytes", 65536}};
  QJsonArray assembled;
  int pages = 0;
  for (; pages < 400; ++pages) {
    const int quota = quotas.at(pages % quotas.size());
    const auto page = store.read(request, quota);
    QCOMPARE(page.value("code").toString(), QStringLiteral("ok"));
    QVERIFY2(fits(page, quota), qPrintable(QStringLiteral("page %1 exceeds %2: %3")
        .arg(pages).arg(quota).arg(QString::fromUtf8(compactJson(page)))));
    QVERIFY(compactJson(page).size() <= quota);
    QCOMPARE(page.value("offset").toInt(), assembled.size());
    for (const auto &entry : page.value("entries").toArray()) {
      QVERIFY(entry.toObject().contains("value"));
      assembled.append(entry.toObject().value("value"));
    }
    if (!page.value("hasMore").toBool()) break;
    QVERIFY(!page.value("nextCursor").toString().isEmpty());
    request.insert("cursor", page.value("nextCursor"));
  }
  QCOMPARE(assembled, values);
  QVERIFY(pages > 1);
  // 读页不签发新引用：同一不可变值仍只占一个句柄。
  QCOMPARE(store.retainedResults(), 1);
}

void TestH7ResultBudget::requestCeilingDoesNotBecomeCursorIdentity() {
  ChatResultStore store;
  const auto values = rows(40);
  const auto reference = resultRef(store.encode(raw(values), true, 1024));
  QVERIFY(!reference.isEmpty());
  QJsonObject request{{"result_ref", reference}, {"page_size", 5}, {"max_bytes", 65536}};
  const auto first = store.read(request, 4096);
  QCOMPARE(first.value("code").toString(), QStringLiteral("ok"));
  const auto cursor = first.value("nextCursor").toString();
  QVERIFY(!cursor.isEmpty());
  // 原请求 max_bytes 保持不变；宿主配额缩小/回升都不影响 cursor 身份。
  request.insert("cursor", cursor);
  const auto smaller = store.read(request, 1024);
  QCOMPARE(smaller.value("code").toString(), QStringLiteral("ok"));
  QCOMPARE(store.lastReadEnvelopeBytes(), 1024);
  QCOMPARE(smaller.value("offset").toInt(), first.value("returnedCount").toInt());
  QVERIFY(fits(smaller, 1024));
  const auto larger = store.read(request, 8192);
  QCOMPARE(store.lastReadEnvelopeBytes(), 8192);
  QCOMPARE(larger.value("offset").toInt(), first.value("returnedCount").toInt());
  QVERIFY(fits(larger, 8192));
  // 宿主额度大于模型请求时不得放宽请求上限：生效值仍是请求值。
  QJsonObject narrow{{"result_ref", reference}, {"page_size", 3}, {"max_bytes", 1024}};
  QCOMPARE(store.read(narrow, 8192).value("code").toString(), QStringLiteral("ok"));
  QCOMPARE(store.lastReadEnvelopeBytes(), 1024);
  // 篡改 cursor 仍按原签名规则拒绝。
  auto forged = cursor;
  forged[forged.size() - 1] = forged.back() == 'a' ? 'b' : 'a';
  QCOMPARE(store.read({{"result_ref", reference}, {"cursor", forged}}, 8192).value("code").toString(),
           QStringLiteral("invalid_result_cursor"));
}

void TestH7ResultBudget::nestedLargeMemberUsesSameReference() {
  ChatResultStore store;
  const QJsonObject value{{"big", rows(90)}, {"a/b~c", QStringLiteral("tiny")}};
  const auto reference = resultRef(store.encode(raw(value), true, 1024));
  QVERIFY(!reference.isEmpty());
  QJsonObject request{{"result_ref", reference}};
  QString selector;
  QStringList inlineValues;
  for (int pages = 0; pages < 10 && selector.isEmpty(); ++pages) {
    const auto page = store.read(request, 2048);
    QCOMPARE(page.value("code").toString(), QStringLiteral("ok"));
    QVERIFY(fits(page, 2048));
    for (const auto &entry : page.value("entries").toArray()) {
      const auto item = entry.toObject();
      if (item.contains("child")) {
        selector = item.value("child").toObject().value("selector").toString();
        continue;
      }
      inlineValues << item.value("value").toString();
    }
    if (selector.isEmpty()) {
      QVERIFY(page.value("hasMore").toBool());
      request.insert("cursor", page.value("nextCursor"));
    }
  }
  QCOMPARE(selector, QStringLiteral("/big"));
  QCOMPARE(inlineValues, QStringList{QStringLiteral("tiny")});
  QCOMPARE(store.retainedResults(), 1); // 子 selector 不新建引用
  QJsonObject childRequest{{"result_ref", reference}, {"selector", selector}, {"page_size", 9}};
  QJsonArray assembled;
  for (int pages = 0; pages < 100; ++pages) {
    const auto page = store.read(childRequest, 1024);
    QCOMPARE(page.value("code").toString(), QStringLiteral("ok"));
    QVERIFY(fits(page, 1024));
    for (const auto &entry : page.value("entries").toArray()) {
      QVERIFY(entry.toObject().contains("value"));
      assembled.append(entry.toObject().value("value"));
    }
    if (!page.value("hasMore").toBool()) break;
    childRequest.insert("cursor", page.value("nextCursor"));
  }
  QCOMPARE(assembled, rows(90));
  QCOMPARE(store.retainedResults(), 1);
}

void TestH7ResultBudget::unicodePiecesFitQuotaAndReassemble() {
  ChatResultStore store;
  const QString text = QStringLiteral("岩😀\"\\\n").repeated(4000);
  const auto reference = resultRef(store.encode(raw(QJsonObject{{"text", text}}), true, 1024));
  QVERIFY(!reference.isEmpty());
  const QVector<int> quotas{1024, 5120, 2048, 1280};
  QJsonObject request{{"result_ref", reference}, {"selector", "/text"}, {"mode", "text_parts"}, {"max_bytes", 65536}};
  QString assembled;
  int expectedOffset = 0;
  for (int pages = 0; pages < 2000; ++pages) {
    const int quota = quotas.at(pages % quotas.size());
    const auto page = store.read(request, quota);
    QCOMPARE(page.value("code").toString(), QStringLiteral("ok"));
    QVERIFY2(fits(page, quota), qPrintable(QString::fromUtf8(compactJson(page))));
    QVERIFY(page.value("valueFragment").toBool());
    QCOMPARE(page.value("offsetCodepoints").toInt(), expectedOffset);
    const auto fragment = page.value("text").toString();
    QVERIFY(fragment.isEmpty() || !fragment.back().isHighSurrogate());
    QCOMPARE(page.value("returnedCodepoints").toInt(), codepointsOf(fragment));
    assembled += fragment;
    expectedOffset += page.value("returnedCodepoints").toInt();
    if (!page.value("hasMore").toBool()) break;
    request.insert("cursor", page.value("nextCursor"));
  }
  QCOMPARE(assembled, text);
  for (qsizetype i = 0; i < assembled.size(); ++i)
    QVERIFY(!assembled.at(i).isLowSurrogate() || (i > 0 && assembled.at(i - 1).isHighSurrogate()));
  // 最小 768 字节装不下「引用 + 长串元信息 + 游标」时给稳定错误，不自动
  // 抬高预算；同一引用在更大配额下仍可从头读取（引用生命周期不变）。
  const auto tooSmall = store.read({{"result_ref", reference}, {"selector", "/text"},
                                    {"mode", "text_parts"}, {"max_bytes", 768}}, 768);
  QCOMPARE(tooSmall.value("code").toString(), QStringLiteral("value_exceeds_page_budget"));
  QCOMPARE(store.retainedResults(), 1);
  // 被拒绝的读取不留下“成功页预算”，最终化不会误用。
  QCOMPARE(store.lastReadEnvelopeBytes(), 0);
  // 宿主配额小于协议下限时只向下收紧，不抬高成 768 生成超限页。
  const auto belowFloor = store.read({{"result_ref", reference}}, 500);
  QCOMPARE(belowFloor.value("code").toString(), QStringLiteral("value_exceeds_page_budget"));
  QCOMPARE(store.lastReadEnvelopeBytes(), 0);
  QCOMPARE(store.read({{"result_ref", reference}, {"selector", 12}}).value("code").toString(),
           QStringLiteral("invalid_result_selector"));
  QCOMPARE(store.lastReadEnvelopeBytes(), 0);
  const auto firstAt1024 = store.read({{"result_ref", reference}, {"selector", "/text"},
                                      {"mode", "text_parts"}, {"max_bytes", 1024}}, 1024);
  QCOMPARE(firstAt1024.value("code").toString(), QStringLiteral("ok"));
  QVERIFY(fits(firstAt1024, 1024));
}

void TestH7ResultBudget::emptyContainersAndScalarsAreComplete() {
  ChatResultStore store;
  const QJsonObject value{{"empty", QJsonArray{}}, {"obj", QJsonObject{}},
                          {"n", 3}, {"s", QStringLiteral("短")}, {"pad", rows(40)}};
  const auto reference = resultRef(store.encode(raw(value), true, 1024));
  QVERIFY(!reference.isEmpty());
  for (const auto &selector : {QStringLiteral("/empty"), QStringLiteral("/obj")}) {
    const auto page = store.read({{"result_ref", reference}, {"selector", selector}}, 1024);
    QCOMPARE(page.value("code").toString(), QStringLiteral("ok"));
    QVERIFY(page.value("entries").toArray().isEmpty());
    QVERIFY(!page.value("hasMore").toBool());
    QCOMPARE(page.value("totalCount").toInt(), 0);
    QVERIFY(page.value("coverage").toObject().value("complete").toBool());
    QVERIFY(fits(page, 1024));
  }
  const auto scalar = store.read({{"result_ref", reference}, {"selector", "/n"}}, 1024);
  QCOMPARE(scalar.value("code").toString(), QStringLiteral("ok"));
  QCOMPARE(scalar.value("value").toInt(), 3);
  QVERIFY(!scalar.value("hasMore").toBool());
  QVERIFY(fits(scalar, 1024));
}

void TestH7ResultBudget::minimumQuotaFailureKeepsOriginalReference() {
  ChatResultStore store;
  // 单条 entry 的 key 很长：完整 entry 装不下 768，child selector 也装不下。
  const QString key(520, QChar(0x5ca9));
  const QJsonObject value{{key, QString(120, QChar(0x5ca8))}};
  const auto reference = resultRef(store.encode(raw(value), true, 1024));
  QVERIFY(!reference.isEmpty());
  const auto failed = store.read({{"result_ref", reference}}, kMinimumEnvelopeBytes);
  QCOMPARE(failed.value("code").toString(), QStringLiteral("value_exceeds_page_budget"));
  QCOMPARE(store.retainedResults(), 1);
  // 不自动抬高预算：同一个引用用更大配额可前进，且不新建引用。
  const auto page = store.read({{"result_ref", reference}}, 4096);
  QCOMPARE(page.value("code").toString(), QStringLiteral("ok"));
  QVERIFY(fits(page, 4096));
  QCOMPARE(store.retainedResults(), 1);
}

void TestH7ResultBudget::expiredReadReleasesValueImmediately() {
  qint64 now = 1000;
  ChatResultStore::Limits limits;
  limits.ttlMs = 10;
  ChatResultStore store(limits, [&now] { return now; });
  store.setScope(QStringLiteral("chat"), QStringLiteral("doc"), 1);
  const auto reference = resultRef(store.encode(raw(rows(40)), true, 1024));
  QVERIFY(!reference.isEmpty());
  QVERIFY(store.retainedBytes() > 0);
  QCOMPARE(store.retainedResults(), 1);
  now += 11;
  QCOMPARE(store.read({{"result_ref", reference}}).value("code").toString(),
           QStringLiteral("result_expired"));
  // 读路径即维护点：到期值与其字节当场释放，不必等下一次 put。
  QCOMPARE(store.retainedResults(), 0);
  QCOMPARE(store.retainedBytes(), 0);
  // 有界 tombstone 保持 expired 与 unknown 的区分。
  QCOMPARE(store.read({{"result_ref", reference}}).value("code").toString(),
           QStringLiteral("result_expired"));
}

void TestH7ResultBudget::replyQuotaContractStaysBounded() {
  using paleo::chat::replyQuotaBytes;
  QCOMPARE(replyQuotaBytes(1, 0, 6000, 384), kResultPageBytes);
  QCOMPARE(replyQuotaBytes(2, 0, 6000, 384), kResultPageBytes);
  QCOMPARE(replyQuotaBytes(1, 2048, 6000, 384), 7904);
  QCOMPARE(replyQuotaBytes(2, 2048, 6000, 384), 7136);
  QCOMPARE(replyQuotaBytes(1, 6000, 6000, 384), kMinimumEnvelopeBytes);
  QCOMPARE(replyQuotaBytes(1, 99999, 6000, 384), kMinimumEnvelopeBytes);
}

QTEST_GUILESS_MAIN(TestH7ResultBudget)
#include "tst_h7_resultbudget.moc"
