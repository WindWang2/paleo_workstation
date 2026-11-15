// 层：测试壳
#include "services/piprotocol.h"
#include <QJsonDocument>
#include <QtEndian>
#include <QtTest>

using namespace paleo::pi;
class TestPiProtocol:public QObject {
  Q_OBJECT
private slots:
  void fragmentedFramePreservesExactIdentity() {
    const QJsonObject expected{{"generation","18446744073709551615"},{"sessionId","session_1"},{"sequence","1"}};
    const auto frame=encodeFrame(expected);QByteArray pending;QJsonObject result{{"old",true}};QString error;bool complete=false;
    for(qsizetype i=0;i<frame.size();++i) {
      pending+=frame[i];QVERIFY(takeFrame(pending,result,complete,error));QVERIFY(error.isEmpty());
      if(i+1<frame.size()){QVERIFY(!complete);QVERIFY(result.isEmpty());}
    }
    QVERIFY(complete);QCOMPARE(result,expected);QVERIFY(pending.isEmpty());
  }
  void countersAreCanonicalStrings_data() {
    QTest::addColumn<QString>("value");QTest::addColumn<bool>("valid");
    QTest::newRow("zero")<<QString("0")<<true;
    QTest::newRow("above-js-safe-integer")<<QString("9007199254740993")<<true;
    QTest::newRow("uint64-maximum")<<QString("18446744073709551615")<<true;
    for(const auto &text:QStringList{"","01","-1","+1","1.0","18446744073709551616"," 1"})
      QTest::newRow(qPrintable("invalid-"+text))<<text<<false;
  }
  void countersAreCanonicalStrings(){QFETCH(QString,value);QFETCH(bool,valid);QCOMPARE(validCounter(value),valid);}
  void malformedBodiesNeverDeliverPartialObjects_data() {
    QTest::addColumn<QByteArray>("body");
    QTest::newRow("not-object")<<QByteArray("[]");
    QTest::newRow("padding")<<QByteArray(" {} ");
    QTest::newRow("duplicate-key")<<QByteArray("{\"a\":1,\"a\":2}");
    QTest::newRow("unordered")<<QByteArray("{\"b\":1,\"a\":2}");
    QTest::newRow("utf8")<<QByteArray::fromHex("7b22c0223a317d");
    QTest::newRow("bom")<<(QByteArray::fromHex("efbbbf")+"{}");
  }
  void malformedBodiesNeverDeliverPartialObjects() {
    QFETCH(QByteArray,body);QByteArray frame(4,Qt::Uninitialized);qToLittleEndian<quint32>(quint32(body.size()),frame.data());frame+=body;
    QJsonObject result{{"old",true}};bool complete=true;QString error;
    QVERIFY(!takeFrame(frame,result,complete,error));QVERIFY(!error.isEmpty());QVERIFY(result.isEmpty());QVERIFY(!complete);QVERIFY(frame.isEmpty());
  }
  void declaredByteLimitPrecedesPayload() {
    for(quint32 size:{quint32(0),quint32(MaxFrameBytes+1),quint32(0xffffffffU)}) {
      QByteArray frame(4,Qt::Uninitialized);qToLittleEndian<quint32>(size,frame.data());
      QJsonObject result;bool complete=false;QString error;QVERIFY(!takeFrame(frame,result,complete,error));QCOMPARE(error,QString("pi_frame_budget"));
    }
    QVERIFY(encodeFrame(QJsonObject{{"oversized",QString(MaxFrameBytes,'x')}}).isEmpty());
  }
  void sessionsDoNotBecomePaths() {
    QVERIFY(validSession("session-42_A"));QVERIFY(!validSession("../project"));QVERIFY(!validSession("caller:scope"));
    QVERIFY(!validSession(QString(129,'x')));QVERIFY(!validSession(QString()));
  }
};
QTEST_GUILESS_MAIN(TestPiProtocol)
#include "tst_piprotocol.moc"
