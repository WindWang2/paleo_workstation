// setDiskFile(lazy) 与在途 sha256Hex 重叠时，不得用旧路径覆盖新工程的 sha.json。
#include <QtTest>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>

#include "io/pathcanon.h"
#include "io/shacache.h"

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

class TestShaCache : public QObject
{
  Q_OBJECT

private slots:
  void setDiskFileDuringHashDoesNotClobberNewShaJson()
  {
#if !defined(Q_OS_UNIX)
    QSKIP("POSIX fifo is required to overlap sha256Hex and setDiskFile");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString fifoPath = dir.filePath(QStringLiteral("inflight.fifo"));
    const QString oldSha = dir.filePath(QStringLiteral("old/sha.json"));
    const QString newSha = dir.filePath(QStringLiteral("new/sha.json"));
    const QByteArray fifoName = QFile::encodeName(fifoPath);
    QCOMPARE(::mkfifo(fifoName.constData(), 0600), 0);

    ShaCache &cache = ShaCache::shared();
    cache.setDiskFile(QString());
    cache.invalidate();
    cache.setDiskFile(oldSha, false);

    QString hashed;
    QString herr;
    QThread *worker = QThread::create([&] {
      hashed = cache.sha256Hex(fifoPath, &herr);
    });
    worker->start();

    int wfd = -1;
    for (int i = 0; i < 5000 && wfd < 0; ++i)
    {
      wfd = ::open(fifoName.constData(), O_WRONLY | O_NONBLOCK);
      if (wfd >= 0)
        break;
      if (errno != ENXIO && errno != EINTR)
        break;
      if (worker->isFinished())
        break;
      QThread::msleep(1);
    }
    if (wfd < 0)
    {
      const int kick = ::open(fifoName.constData(), O_WRONLY | O_NONBLOCK);
      if (kick >= 0)
        ::close(kick);
      worker->wait(2000);
      delete worker;
      QFAIL("hasher never blocked inside hashFile");
    }

    // 写端已与 hashFile 的读端会合：代际已采样，read 仍堵在无数据的 fifo 上。
    cache.setDiskFile(newSha, true);
    const QByteArray payload("project-a-bytes");
    const ssize_t wrote = ::write(wfd, payload.constData(), size_t(payload.size()));
    ::close(wfd);
    QVERIFY2(worker->wait(10000), "sha256Hex did not finish");
    delete worker;

    QCOMPARE(wrote, ssize_t(payload.size()));
    QCryptographicHash expected(QCryptographicHash::Sha256);
    expected.addData(payload);
    QCOMPARE(hashed, QString::fromLatin1(expected.result().toHex()));
    QVERIFY(herr.isEmpty());
    // 在途哈希的内存指纹还在；新 sha.json 不应被这次结果创建。
    QCOMPARE(cache.memoryEntries(), 1);
    QVERIFY(!QFile::exists(newSha));

    const QString fileB = dir.filePath(QStringLiteral("b.bin"));
    const QByteArray payloadB("project-b-bytes");
    {
      QFile f(fileB);
      QVERIFY(f.open(QIODevice::WriteOnly));
      QCOMPARE(f.write(payloadB), qint64(payloadB.size()));
    }
    QString herrB;
    const QString shaB = cache.sha256Hex(fileB, &herrB);
    QVERIFY2(!shaB.isEmpty(), qPrintable(herrB));

    QFile out(newSha);
    QVERIFY2(out.open(QIODevice::ReadOnly), qPrintable(out.errorString()));
    const QJsonDocument doc = QJsonDocument::fromJson(out.readAll());
    QVERIFY(doc.isObject());
    const QJsonObject obj = doc.object();
    const QString canonA = PathCanon::canonicalize(fifoPath);
    const QString canonB = PathCanon::canonicalize(fileB);
    QCOMPARE(obj.size(), 1);
    QVERIFY(obj.contains(canonB));
    QVERIFY(!obj.contains(canonA));
    QVERIFY(!obj.contains(fifoPath));
    const QStringList parts = obj.value(canonB).toString().split(QLatin1Char('|'));
    QCOMPARE(parts.size(), 3);
    QCOMPARE(parts.at(2), shaB);
#endif
  }
};

QTEST_GUILESS_MAIN(TestShaCache)
#include "tst_shacache.moc"
