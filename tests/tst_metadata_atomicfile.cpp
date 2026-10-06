#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QFileInfo>

#include "../src/metadata/atomicfile.h"

class TestMetadataAtomicFile : public QObject
{
  Q_OBJECT

private slots:
  void normalWriteNewFile();
  void replaceExistingFile();
  void binaryAndCrlfRoundTrip();
  void faultInjection_beforeWrite();
  void faultInjection_duringWrite();
  void faultInjection_simulateDiskFull();
  void faultInjection_beforeReplace();
  void errorOnInvalidPath();
  void transaction_cleanLifecycle();
  void transaction_rollbackCleansUp();
  void transaction_crashSimulationAndRecovery();
  void mutationDemonstration_corruptDefense();
};

void TestMetadataAtomicFile::normalWriteNewFile()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString target = QDir(tmp.path()).filePath(QStringLiteral("new_doc.json"));
  QVERIFY(!QFile::exists(target));

  const QByteArray payload = "{\"status\": \"ok\", \"count\": 42}\n";
  QString err;
  const bool ok = paleoWriteFileAtomic(target, payload, &err);
  QVERIFY2(ok, qPrintable(err));
  QVERIFY(QFile::exists(target));

  QFile f(target);
  QVERIFY(f.open(QIODevice::ReadOnly));
  QCOMPARE(f.readAll(), payload);

  // 确认目录下只有目标文件，无残留临时文件
  const QStringList entries = QDir(tmp.path()).entryList(QDir::Files | QDir::Hidden);
  QCOMPARE(entries, QStringList{QStringLiteral("new_doc.json")});
}

void TestMetadataAtomicFile::replaceExistingFile()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString target = QDir(tmp.path()).filePath(QStringLiteral("versioned.dat"));

  const QByteArray originalData = "ORIGINAL_PRE_EXISTING_DATA_V1";
  QVERIFY(paleoWriteFileAtomic(target, originalData));

  // 用新数据原子替换
  const QByteArray updatedData = "UPDATED_DATA_V2_LONG_CONTENT_REPLACED_ATOMICALLY";
  QString err;
  const bool ok = paleoWriteFileAtomic(target, updatedData, &err);
  QVERIFY2(ok, qPrintable(err));

  QFile f(target);
  QVERIFY(f.open(QIODevice::ReadOnly));
  QCOMPARE(f.readAll(), updatedData);
}

void TestMetadataAtomicFile::binaryAndCrlfRoundTrip()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString target = QDir(tmp.path()).filePath(QStringLiteral("binary_roundtrip.bin"));

  // 构造包含 NULL 字节、高位字节、CRLF 与 LF 换行符的二进制载荷
  QByteArray binaryPayload;
  binaryPayload.append("HEAD\r\n");
  binaryPayload.append('\0');
  binaryPayload.append("\xDE\xAD\xBE\xEF", 4);
  binaryPayload.append("\r\nMID_LINE\n");
  for (int i = 0; i < 256; ++i)
  {
    binaryPayload.append(static_cast<char>(i));
  }
  binaryPayload.append("\r\nTAIL\0\0END", 11);

  QString err;
  const bool ok = paleoWriteFileAtomic(target, binaryPayload, &err);
  QVERIFY2(ok, qPrintable(err));

  QFile f(target);
  QVERIFY(f.open(QIODevice::ReadOnly));
  const QByteArray readBack = f.readAll();
  QCOMPARE(readBack.size(), binaryPayload.size());
  QCOMPARE(readBack, binaryPayload);
}

void TestMetadataAtomicFile::faultInjection_beforeWrite()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString target = QDir(tmp.path()).filePath(QStringLiteral("intact_before.txt"));
  const QByteArray initialData = "CRITICAL_ORIGINAL_PROJECT_METADATA";
  QVERIFY(paleoWriteFileAtomic(target, initialData));

  QString err;
  const bool ok = paleoWriteFileAtomic(target, "NEW_BAD_DATA", &err,
                                       PaleoAtomicFaultPoint::BeforeWrite);
  QVERIFY(!ok);
  QVERIFY(err.contains(QStringLiteral("故障注入")));

  // 核心不变量：原文件必须 100% 完好无损
  QFile f(target);
  QVERIFY(f.open(QIODevice::ReadOnly));
  QCOMPARE(f.readAll(), initialData);
}

void TestMetadataAtomicFile::faultInjection_duringWrite()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString target = QDir(tmp.path()).filePath(QStringLiteral("intact_during.txt"));
  const QByteArray initialData = "PRECIOUS_WELL_SECTION_DATA_DO_NOT_CORRUPT";
  QVERIFY(paleoWriteFileAtomic(target, initialData));

  QString err;
  const bool ok = paleoWriteFileAtomic(target, "INCOMPLETE_CORRUPTED_WRITE_PAYLOAD", &err,
                                       PaleoAtomicFaultPoint::DuringWrite);
  QVERIFY(!ok);
  QVERIFY(err.contains(QStringLiteral("故障注入")));

  // 核心不变量：原文件内容完好，且无临时脏文件残留
  QFile f(target);
  QVERIFY(f.open(QIODevice::ReadOnly));
  QCOMPARE(f.readAll(), initialData);

  const QStringList files = QDir(tmp.path()).entryList(QDir::Files | QDir::Hidden);
  QCOMPARE(files, QStringList{QStringLiteral("intact_during.txt")});
}

void TestMetadataAtomicFile::faultInjection_simulateDiskFull()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString target = QDir(tmp.path()).filePath(QStringLiteral("intact_disk_full.gpkg"));
  const QByteArray initialData = "GEOPACKAGE_DATABASE_IMAGE_BEFORE_DISK_FULL";
  QVERIFY(paleoWriteFileAtomic(target, initialData));

  QString err;
  const bool ok = paleoWriteFileAtomic(target, "HUGE_UNFINISHED_PAYLOAD", &err,
                                       PaleoAtomicFaultPoint::SimulateDiskFull);
  QVERIFY(!ok);
  QVERIFY(err.contains(QStringLiteral("ENOSPC")) || err.contains(QStringLiteral("空间耗尽")));

  // 原文件无损
  QFile f(target);
  QVERIFY(f.open(QIODevice::ReadOnly));
  QCOMPARE(f.readAll(), initialData);
}

void TestMetadataAtomicFile::faultInjection_beforeReplace()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString target = QDir(tmp.path()).filePath(QStringLiteral("intact_before_replace.sqlite"));
  const QByteArray initialData = "SQLITE_HEADER_ORIGINAL_FILE_VERSION";
  QVERIFY(paleoWriteFileAtomic(target, initialData));

  QString err;
  const bool ok = paleoWriteFileAtomic(target, "DATA_WRITTEN_BUT_CRASHED_BEFORE_REPLACE", &err,
                                       PaleoAtomicFaultPoint::BeforeReplace);
  QVERIFY(!ok);

  // 原文件无损
  QFile f(target);
  QVERIFY(f.open(QIODevice::ReadOnly));
  QCOMPARE(f.readAll(), initialData);

  // 临时文件被安全清理
  const QStringList files = QDir(tmp.path()).entryList(QDir::Files | QDir::Hidden);
  QCOMPARE(files, QStringList{QStringLiteral("intact_before_replace.sqlite")});
}

void TestMetadataAtomicFile::errorOnInvalidPath()
{
  QString err;
  const bool ok = paleoWriteFileAtomic(QString(), "DATA", &err);
  QVERIFY(!ok);
  QVERIFY(!err.isEmpty());

  // 无法写入的受限或非法路径
#ifdef Q_OS_WIN
  const QString invalidTarget = QStringLiteral("Z:/non_existent_drive_9999/test.dat");
#else
  const QString invalidTarget = QStringLiteral("/proc/non_existent_sysfs/test.dat");
#endif
  const bool ok2 = paleoWriteFileAtomic(invalidTarget, "DATA", &err);
  QVERIFY(!ok2);
  QVERIFY(!err.isEmpty());
}

void TestMetadataAtomicFile::transaction_cleanLifecycle()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString target = QDir(tmp.path()).filePath(QStringLiteral("tx_target.json"));
  QVERIFY(!PaleoAtomicTransaction::hasDirtyMarker(target));

  PaleoAtomicTransaction tx(target);
  QString err;
  QVERIFY(tx.begin(&err));
  QVERIFY(PaleoAtomicTransaction::hasDirtyMarker(target));
  QVERIFY(QFile::exists(tx.runningMarkerPath()));

  const QByteArray content = "{\"tx\": \"committed_successfully\"}\n";
  QVERIFY(tx.writeStaged(content, &err));
  QVERIFY(QFile::exists(tx.stagePath()));

  QVERIFY(tx.commit(&err));
  // commit 成功后：目标文件在位，.running 脏标记清除，staged 暂存已被重命名消费
  QVERIFY(!PaleoAtomicTransaction::hasDirtyMarker(target));
  QVERIFY(!QFile::exists(tx.stagePath()));
  QVERIFY(QFile::exists(target));

  QFile f(target);
  QVERIFY(f.open(QIODevice::ReadOnly));
  QCOMPARE(f.readAll(), content);
}

void TestMetadataAtomicFile::transaction_rollbackCleansUp()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString target = QDir(tmp.path()).filePath(QStringLiteral("tx_rollback.json"));
  const QByteArray original = "INITIAL_STATE_ROLLBACK";
  QVERIFY(paleoWriteFileAtomic(target, original));

  {
    PaleoAtomicTransaction tx(target);
    QVERIFY(tx.begin());
    QVERIFY(tx.writeStaged("STAGED_UNWANTED_DATA"));
    tx.rollback();
  }

  // rollback 后：脏标记清除，原文件内容无变化
  QVERIFY(!PaleoAtomicTransaction::hasDirtyMarker(target));
  QFile f(target);
  QVERIFY(f.open(QIODevice::ReadOnly));
  QCOMPARE(f.readAll(), original);
}

void TestMetadataAtomicFile::transaction_crashSimulationAndRecovery()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString target = QDir(tmp.path()).filePath(QStringLiteral("tx_crash.json"));
  const QByteArray original = "SAFE_STATE_BEFORE_CRASH";
  QVERIFY(paleoWriteFileAtomic(target, original));

  {
    PaleoAtomicTransaction tx(target);
    QVERIFY(tx.begin());
    QVERIFY(tx.writeStaged("PARTIAL_DATA_INTERRUPTED_BY_HARD_KILL"));
    tx.simulateCrash(); // 模拟中途 SIGKILL，未执行 commit/rollback
  }

  // 模拟崩溃后：.running 脏标记残留，原文件完全无损！
  QVERIFY(PaleoAtomicTransaction::hasDirtyMarker(target));
  QFile f(target);
  QVERIFY(f.open(QIODevice::ReadOnly));
  QCOMPARE(f.readAll(), original);

  // 执行启动恢复逻辑（类似 crashreport recovery 探测）
  QVERIFY(PaleoAtomicTransaction::recover(target));
  QVERIFY(!PaleoAtomicTransaction::hasDirtyMarker(target));

  // 恢复后原文件依旧 100% 完整可用
  QFile f2(target);
  QVERIFY(f2.open(QIODevice::ReadOnly));
  QCOMPARE(f2.readAll(), original);
}

void TestMetadataAtomicFile::mutationDemonstration_corruptDefense()
{
  // 变异示范断言：验证当文件写入失败时，原文件绝不被清空或截断
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString target = QDir(tmp.path()).filePath(QStringLiteral("mutation_guard.txt"));
  const QByteArray original = "MUTATION_SHIELD_ORIGINAL";
  QVERIFY(paleoWriteFileAtomic(target, original));

  // 如果有人在 paleoWriteFileAtomic 错误分支中误执行了 QFile::remove(destination)
  // 或覆盖了目标文件，此断言将立即变红抓住 bug：
  QFile verifyFile(target);
  QVERIFY(verifyFile.open(QIODevice::ReadOnly));
  const QByteArray snapshot = verifyFile.readAll();
  verifyFile.close();

  // 执行带故障的写操作
  paleoWriteFileAtomic(target, "GARBAGE", nullptr, PaleoAtomicFaultPoint::DuringWrite);

  QFile checkFile(target);
  QVERIFY(checkFile.open(QIODevice::ReadOnly));
  QCOMPARE(checkFile.readAll(), snapshot);
}

QTEST_MAIN(TestMetadataAtomicFile)
#include "tst_metadata_atomicfile.moc"
