// tst_keychainslot — PW-122：共用钥匙串实现的契约（无系统钥匙串依赖）。
#include <QtTest>
#include "../src/ai/keychainslot.h"

class TestKeychainSlot : public QObject {
  Q_OBJECT
private slots:
  void nullContextCallsBackWithError() {
    KeychainSlot s{QStringLiteral("svc"), QStringLiteral("acct"), "PALEO_TST_SLOT_OFF"};
    int calls = 0;
    s.read(nullptr, [&](bool ok, const QByteArray &, const QString &e) {
      ++calls; QVERIFY(!ok); QVERIFY(!e.isEmpty()); });
    s.write(nullptr, "k", [&](bool ok, const QString &e) { ++calls; QVERIFY(!ok); QVERIFY(!e.isEmpty()); });
    s.clear(nullptr, [&](bool ok, const QString &e) { ++calls; QVERIFY(!ok); QVERIFY(!e.isEmpty()); });
    QCOMPARE(calls, 3);
  }
  void disabledSlotReportsUnavailable() {
    qputenv("PALEO_TST_SLOT_OFF", "1");
    KeychainSlot s{QStringLiteral("svc"), QStringLiteral("acct"), "PALEO_TST_SLOT_OFF"};
    QVERIFY(!s.available());
    QObject ctx;
    int calls = 0;
    s.read(&ctx, [&](bool ok, const QByteArray &, const QString &e) {
      ++calls; QVERIFY(!ok); QCOMPARE(e, KeychainSlot::unavailableError()); });
    s.write(&ctx, "k", [&](bool ok, const QString &) { ++calls; QVERIFY(!ok); });
    QCOMPARE(calls, 2);
  }
};
QTEST_GUILESS_MAIN(TestKeychainSlot)
#include "tst_keychainslot.moc"
