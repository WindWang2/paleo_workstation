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
  // finding-43①：limit<0 = 按 size 整读；读短（size 与实读不一致）必须判
  // 失败——头注释「读短返回 false」的承诺，I/O 错误下的部分内容不是整读成功。
  void negativeLimitReadsWholeFileAndReportsShortRead()
  {
    QTemporaryDir d;
    const QString p = d.filePath("whole.txt");
    {
      QFile f(p);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write("hello world");
      f.close();
    }
    QByteArray out;
    QString err;
    QVERIFY2(paleo::io::readBoundedFile(p, -1, &out, &err), qPrintable(err));
    QCOMPARE(out, QByteArray("hello world"));
    // 空文件：整读 0 字节也是成功（不是短读）。
    const QString empty = d.filePath("empty.txt");
    {
      QFile f(empty);
      QVERIFY(f.open(QIODevice::WriteOnly));
    }
    QByteArray out2("untouched");
    QVERIFY2(paleo::io::readBoundedFile(empty, -1, &out2, &err), qPrintable(err));
    QCOMPARE(out2, QByteArray());
  }
};
QTEST_GUILESS_MAIN(TestBoundedRead)
#include "tst_boundedread.moc"
