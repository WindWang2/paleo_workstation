// PW-076：readBoundedFile 超限即失败、不截断；限额内原样返回。
#include "io/boundedread.h"

#include <QTemporaryDir>
#include <QtTest>

class TestBoundedRead : public QObject
{
  Q_OBJECT
private slots:
  void withinLimitReturnsBytes()
  {
    QTemporaryDir d;
    const QString p = d.filePath("a.txt");
    QFile f(p);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("hello");
    f.close();
    QByteArray out;
    QString err;
    QVERIFY2(paleo::io::readBoundedFile(p, 5, &out, &err), qPrintable(err));
    QCOMPARE(out, QByteArray("hello"));
  }
  void overLimitFailsWithoutTruncation()
  {
    QTemporaryDir d;
    const QString p = d.filePath("b.txt");
    QFile f(p);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("hello!");
    f.close();
    QByteArray out("untouched");
    QString err;
    QVERIFY(!paleo::io::readBoundedFile(p, 5, &out, &err));
    QCOMPARE(out, QByteArray("untouched"));
    QVERIFY(err.contains("too large"));
  }
  void missingFileFails()
  {
    QString err;
    QVERIFY(!paleo::io::readBoundedFile("/nonexistent/x", 10, nullptr, &err));
    QVERIFY(err.contains("cannot read"));
  }
};
QTEST_GUILESS_MAIN(TestBoundedRead)
#include "tst_boundedread.moc"
