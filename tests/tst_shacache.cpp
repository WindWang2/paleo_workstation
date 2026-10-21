// setDiskFile(lazy) 与在途 sha256Hex 重叠时，不得用旧路径覆盖新工程的 sha.json。
#include <QtTest>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>

#include "io/pathcanon.h"
#include "io/shacache.h"

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

class TestShaCache : public QObject
{
  Q_OBJECT

private slots:
  // #561：血缘复验加强档——指纹带 inode，磁盘条目升 4 段；POSIX 下「同
  // mtime+size 换文件」必击穿缓存（默认口径如实保留 git 式弱点以作对照）。
  void inodeFingerprintOption()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("lineage.bin"));
    const QString shaJson = dir.filePath(QStringLiteral("sha.json"));
    const QDateTime stamp(QDate(2026, 10, 18), QTime(12, 0, 0));

    auto writeFileAt = [&](const QString &target, const QByteArray &payload) {
      QVERIFY(!QFile::exists(target) || QFile::remove(target));
      {
        QFile f(target);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QCOMPARE(f.write(payload), qint64(payload.size()));
        f.close(); // setFileTime 需要重开句柄
        QVERIFY(f.open(QIODevice::ReadWrite));
        QVERIFY(f.setFileTime(stamp, QFileDevice::FileModificationTime));
      }
    };

    ShaCache &cache = ShaCache::shared();
    cache.setDiskFile(QString());
    cache.invalidate();
    // A-01 节流与本用例语义（逐 miss 后的盘上指纹形状）解耦：恢复「每 miss
    // 落盘」口径（setDiskFile 换代后首 miss 本就立即落盘）。
    cache.setDiskSaveThrottle(0, 1);
    cache.setDiskFile(shaJson, false);

    // 默认口径首算：磁盘 3 段条目。
    writeFileAt(file, QByteArray("v1"));
    QString herr;
    const QString shaV1 = cache.sha256Hex(file, &herr);
    QVERIFY2(!shaV1.isEmpty(), qPrintable(herr));
    {
      QFile j(shaJson);
      QVERIFY(j.open(QIODevice::ReadOnly));
      const QStringList parts = QJsonDocument::fromJson(j.readAll())
                                    .object()
                                    .value(PathCanon::canonicalize(file))
                                    .toString()
                                    .split(QLatin1Char('|'));
      QCOMPARE(parts.size(), 3);
      QCOMPARE(parts.constLast(), shaV1);
    }

    // 强口径：3 段旧条目证不了 inode → 重算（自愈覆写为 4 段），sha 不变。
    const QString shaStrong = cache.sha256Hex(
        file, &herr, ShaCache::Fingerprint::MtimeSizeInode);
    QCOMPARE(shaStrong, shaV1);
    {
      QFile j(shaJson);
      QVERIFY(j.open(QIODevice::ReadOnly));
      const QStringList parts = QJsonDocument::fromJson(j.readAll())
                                    .object()
                                    .value(PathCanon::canonicalize(file))
                                    .toString()
                                    .split(QLatin1Char('|'));
      QCOMPARE(parts.size(), 4);
      // 第 3 段 = inode 数目字（POSIX 为真实 inode，Windows 恒 0——平台口径
      // 见 shacache.h 头注；这里只验「是数字」，值面由下方 POSIX 段覆盖）。
      bool inoOk = false;
      parts.at(2).toULongLong(&inoOk);
      QVERIFY(inoOk);
      QCOMPARE(parts.constLast(), shaV1);
    }

#ifdef Q_OS_UNIX
    // 敌意场景（POSIX）：同路径换文件、还原 mtime+size——默认口径仍命中旧值
    //（git 式启发式的已知弱点，口径如实保留）；强口径必须重算出新 sha。
    // 换文件走「旁路写 + rename 顶替」：v2 的 inode 在旧文件存活期间分配，
    // 必与旧 inode 不同（删后原地重建会被文件系统复用 inode，演示不可复现）。
    const QString fileV2 = dir.filePath(QStringLiteral("lineage.v2"));
    writeFileAt(fileV2, QByteArray("v2"));
    QVERIFY(QFile::remove(file));
    QCOMPARE(::rename(QFile::encodeName(fileV2).constData(),
                      QFile::encodeName(file).constData()),
             0);
    QCOMPARE(cache.sha256Hex(file, &herr), shaV1); // 弱点对照：缓存投毒成立
    const QString shaV2 = cache.sha256Hex(
        file, &herr, ShaCache::Fingerprint::MtimeSizeInode);
    QCryptographicHash expected(QCryptographicHash::Sha256);
    expected.addData(QByteArray("v2"));
    QCOMPARE(shaV2, QString::fromLatin1(expected.result().toHex()));
    QVERIFY(shaV2 != shaV1);
#else
    // Windows 无 inode（恒 0）：强口径退化回默认口径——平台口径见
    // segyindexstore::Identity::inode 与 shacache.h 头注。
    qDebug("inode 判等仅 POSIX；Windows 恒 0，强口径退化为 mtime+size。");
#endif
  }

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
    cache.setDiskSaveThrottle(0, 1); // A-01：与节流时序解耦
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
  // A-01（full-audit-20261021）：miss 路径磁盘层节流——宽阈值下后续 miss
  // 不再每次全表落盘，flushDisk()/invalidate 强制刷后全量可见。修复前每个
  // miss 都在持锁临界区内全表序列化 + 同步写盘（批量导入写放大）。
  void diskWritesThrottledOnMiss()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString shaJson = dir.filePath(QStringLiteral("sha.json"));
    ShaCache &cache = ShaCache::shared();
    cache.setDiskFile(QString());
    cache.invalidate();
    // 测试口径：1 小时 / 64 条——除换代后首 miss 外全部走惰性脏记。
    cache.setDiskSaveThrottle(3600 * 1000, 64);
    cache.setDiskFile(shaJson, false);

    QStringList shas;
    for (int i = 0; i < 8; ++i)
    {
      const QString f = dir.filePath(QStringLiteral("throttle-%1.bin").arg(i));
      {
        QFile file(f);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(QByteArray::fromRawData("payload", 7) + QByteArray::number(i)) > 0);
      }
      QString err;
      const QString sha = cache.sha256Hex(f, &err);
      QVERIFY2(!sha.isEmpty(), qPrintable(err));
      shas << sha;
    }
    {
      // 换代后首 miss 立即落盘（1 条）；其余 7 条在节流窗内不落盘——
      // 修复前这里会是 8 条。
      QFile j(shaJson);
      QVERIFY(j.open(QIODevice::ReadOnly));
      const QJsonObject obj = QJsonDocument::fromJson(j.readAll()).object();
      QCOMPARE(obj.size(), 1);
      QVERIFY(obj.contains(PathCanon::canonicalize(dir.filePath(QStringLiteral("throttle-0.bin")))));
    }
    cache.flushDisk(); // 强制刷（invalidate 是清表语义，不适用此处）
    {
      QFile j(shaJson);
      QVERIFY(j.open(QIODevice::ReadOnly));
      const QJsonObject obj = QJsonDocument::fromJson(j.readAll()).object();
      QCOMPARE(obj.size(), 8);
      QCOMPARE(obj.value(PathCanon::canonicalize(dir.filePath(QStringLiteral("throttle-7.bin"))))
                   .toString()
                   .section(QLatin1Char('|'), -1),
               shas.last());
    }
    // 命中路径零写副作用：内存命中不产生新落盘（flushDisk 无脏可刷）。
    const qint64 before = QFileInfo(shaJson).lastModified().toMSecsSinceEpoch();
    QString err;
    QCOMPARE(cache.sha256Hex(dir.filePath(QStringLiteral("throttle-3.bin")), &err), shas.at(3));
    cache.flushDisk();
    QCOMPARE(QFileInfo(shaJson).lastModified().toMSecsSinceEpoch(), before);
    // 恢复默认节流口径，不污染后续用例。
    cache.setDiskSaveThrottle(1000, 64);
  }
};

QTEST_GUILESS_MAIN(TestShaCache)
#include "tst_shacache.moc"
