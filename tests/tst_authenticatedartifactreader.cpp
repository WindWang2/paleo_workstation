// 层：测试壳
#include <QtTest>
#include "services/authenticatedartifactreader.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QSemaphore>
#include <QTemporaryDir>
#include <array>
#include <atomic>
#include <thread>

using paleo::artifacts::AuthenticatedArtifactReader;
namespace {
using Reader = AuthenticatedArtifactReader;
constexpr qint64 Chunk = 64 * 1024;

QByteArray content(qsizetype size) {
  QByteArray bytes(size, Qt::Uninitialized);
  for (qsizetype i = 0; i < size; ++i)
    bytes[i] = char((i * 31 + (i / 4096) * 17) % 251);
  return bytes;
}

bool writeFile(const QString &path, const QByteArray &bytes) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}

Reader::Identity identity(const QString &path, const QByteArray &bytes,
                          const QString &scope = QStringLiteral("document:epoch:operation:port:caller")) {
  Reader::Identity result;
  result.scopeKey = scope;
  result.path = path;
  result.sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
  result.byteSize = bytes.size();
  return result;
}

bool replaceByteKeepingTime(const QString &path, qint64 offset, char value, const QDateTime &modified) {
  QFile file(path);
  return file.open(QIODevice::ReadWrite) && file.seek(offset) && file.write(&value, 1) == 1 &&
      file.flush() && file.setFileTime(modified, QFileDevice::FileModificationTime);
}
}

class TestAuthenticatedArtifactReader : public QObject {
  Q_OBJECT
private slots:
  void sequentialPagesHashTheWholeArtifactOnlyOnce();
  void boundedPagesAndEmptyArtifact();
  void alteredFilesNeverReturnUnauthenticatedBytes_data();
  void alteredFilesNeverReturnUnauthenticatedBytes();
  void damagedColdScanDoesNotPoisonTheIndex();
  void eofAuthenticatesTheLastChunk();
  void eachRequestedRangeRemainsBoundToTheFrozenArtifact();
  void cancelledColdScanCannotBeReused();
  void invalidationStopsAnActiveScanAndFutureReads();
  void concurrentColdPagesShareOneFullScan();
  void callerAndIdentityChangesDoNotReuseAnotherIndex();
  void leastRecentlyUsedIndexesStayBounded();
};

void TestAuthenticatedArtifactReader::sequentialPagesHashTheWholeArtifactOnlyOnce() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const auto bytes = content(32 * 1024 * 1024);
  const auto path = dir.filePath("large.bin");
  QVERIFY(writeFile(path, bytes));
  const auto input = identity(path, bytes);
  Reader reader;
  QByteArray received;
  received.reserve(bytes.size());
  constexpr int PageBytes = 24 * 1024;
  qint64 pages = 0;
  for (qint64 offset = 0; offset < bytes.size(); offset += PageBytes) {
    const auto page = reader.read(input, offset, PageBytes);
    QCOMPARE(page.code, QStringLiteral("ok"));
    QCOMPARE(page.bytes, bytes.mid(offset, PageBytes));
    received.append(page.bytes);
    ++pages;
  }
  QCOMPARE(received, bytes);
  const auto stats = reader.statistics();
  qInfo().nospace() << "artifact_read_io fileBytes=" << bytes.size() << " pages=" << pages
                    << " fullScans=" << stats.fullScans << " fullHashBytes=" << stats.fullHashBytes
                    << " chunkHashBytes=" << stats.chunkHashBytes << " readBytes=" << stats.readBytes;
  QCOMPARE(stats.fullScans, quint64(1));
  QCOMPARE(stats.fullHashBytes, quint64(bytes.size()));
  const auto authenticationBound = quint64(bytes.size()) + quint64(pages) * (PageBytes + 2 * (Chunk - 1));
  QVERIFY(stats.readBytes <= authenticationBound);
  QVERIFY(stats.chunkHashBytes <= authenticationBound);
  QVERIFY(stats.readBytes > quint64(bytes.size()));
  QCOMPARE(stats.chunkHashBytes, stats.readBytes);
  QVERIFY(stats.readBytes < quint64(bytes.size()) * quint64(pages) / 128);
  QCOMPARE(stats.cachedEntries, quint64(1));
  QCOMPARE(stats.cachedDigestBytes, quint64(bytes.size() / Chunk * 32));
}

void TestAuthenticatedArtifactReader::boundedPagesAndEmptyArtifact() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const auto bytes = content(3 * Chunk + 17);
  const auto path = dir.filePath("boundaries.bin");
  QVERIFY(writeFile(path, bytes));
  const auto input = identity(path, bytes);
  Reader reader;
  const std::array<qint64, 5> offsets{0, Chunk - 1, Chunk + 11, bytes.size() - 3, bytes.size()};
  for (const auto offset : offsets) {
    const auto page = reader.read(input, offset, 19);
    QCOMPARE(page.code, QStringLiteral("ok"));
    QCOMPARE(page.bytes, bytes.mid(offset, 19));
  }
  QCOMPARE(reader.statistics().fullScans, quint64(1));
  QVERIFY(reader.read(input, bytes.size() + 1, 1).code != QLatin1String("ok"));
  QVERIFY(reader.read(input, -1, 1).code != QLatin1String("ok"));
  QVERIFY(reader.read(input, 0, 0).code != QLatin1String("ok"));
  QVERIFY(reader.read(input, 0, 24 * 1024 + 1).code != QLatin1String("ok"));

  const auto emptyPath = dir.filePath("empty.bin");
  QVERIFY(writeFile(emptyPath, {}));
  const auto empty = identity(emptyPath, {});
  const auto page = reader.read(empty, 0, 17);
  QCOMPARE(page.code, QStringLiteral("ok"));
  QVERIFY(page.bytes.isEmpty());
  QCOMPARE(reader.read(empty, 0, 1).code, QStringLiteral("ok"));
  auto wrong = empty;
  wrong.sha256 = QString(64, 'f');
  const auto rejected = reader.read(wrong, 0, 1);
  QVERIFY(rejected.code != QLatin1String("ok"));
  QVERIFY(rejected.bytes.isEmpty());
}

void TestAuthenticatedArtifactReader::alteredFilesNeverReturnUnauthenticatedBytes_data() {
  QTest::addColumn<QString>("attack");
  QTest::newRow("same-size-restored-mtime") << QStringLiteral("overwrite");
  QTest::newRow("replacement-restored-mtime") << QStringLiteral("replace");
  QTest::newRow("deleted") << QStringLiteral("delete");
  QTest::newRow("truncated") << QStringLiteral("truncate");
  QTest::newRow("appended") << QStringLiteral("append");
}

void TestAuthenticatedArtifactReader::alteredFilesNeverReturnUnauthenticatedBytes() {
  QFETCH(QString, attack);
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const auto bytes = content(3 * Chunk + 29);
  const auto path = dir.filePath("result.bin");
  QVERIFY(writeFile(path, bytes));
  const auto input = identity(path, bytes);
  const auto modified = QFileInfo(path).lastModified();
  Reader reader;
  QCOMPARE(reader.read(input, 0, 16).code, QStringLiteral("ok"));
  const qint64 changedAt = Chunk + 7;
  if (attack == QLatin1String("overwrite")) {
    QVERIFY(replaceByteKeepingTime(path, changedAt, char(bytes.at(changedAt) ^ 0x5a), modified));
    QCOMPARE(QFileInfo(path).lastModified(), modified);
    QCOMPARE(QFileInfo(path).size(), input.byteSize);
  } else if (attack == QLatin1String("replace")) {
    QVERIFY(QFile::rename(path, path + ".old"));
    auto altered = bytes;
    altered[changedAt] = char(altered.at(changedAt) ^ 0x5a);
    QVERIFY(writeFile(path, altered));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadWrite));
    QVERIFY(file.setFileTime(modified, QFileDevice::FileModificationTime));
    file.close();
    QCOMPARE(QFileInfo(path).lastModified(), modified);
    QCOMPARE(QFileInfo(path).size(), input.byteSize);
  } else if (attack == QLatin1String("delete")) {
    QVERIFY(QFile::remove(path));
  } else {
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadWrite));
    QVERIFY(file.resize(bytes.size() + (attack == QLatin1String("append") ? 1 : -1)));
  }
  const auto page = reader.read(input, changedAt, 13);
  QVERIFY2(page.code != QLatin1String("ok"), qPrintable(attack));
  QVERIFY(page.bytes.isEmpty());
}

void TestAuthenticatedArtifactReader::damagedColdScanDoesNotPoisonTheIndex() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const auto bytes = content(3 * Chunk + 3);
  const auto path = dir.filePath("untrusted.bin");
  const auto input = identity(path, bytes);
  auto corrupted = bytes;
  corrupted[2 * Chunk + 1] = char(corrupted.at(2 * Chunk + 1) ^ 0x3c);
  QVERIFY(writeFile(path, corrupted));
  Reader reader;
  const auto page = reader.read(input, 0, 1);
  QVERIFY(page.code != QLatin1String("ok"));
  QVERIFY(page.bytes.isEmpty());
  QCOMPARE(reader.statistics().cachedEntries, quint64(0));
  QVERIFY(writeFile(path, bytes));
  const auto repaired = reader.read(input, 0, 1);
  QCOMPARE(repaired.code, QStringLiteral("ok"));
  QCOMPARE(repaired.bytes, bytes.left(1));
  QCOMPARE(reader.statistics().cachedEntries, quint64(1));
}

void TestAuthenticatedArtifactReader::cancelledColdScanCannotBeReused() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const auto bytes = content(8 * 1024 * 1024);
  const auto path = dir.filePath("cancel.bin");
  QVERIFY(writeFile(path, bytes));
  const auto input = identity(path, bytes);
  Reader reader;
  const auto cancelled = reader.read(input, 0, 16, [&] {
    return reader.statistics().fullHashBytes >= quint64(2 * Chunk);
  });
  QCOMPARE(cancelled.code, QStringLiteral("cancelled"));
  QVERIFY(cancelled.bytes.isEmpty());
  const auto partial = reader.statistics();
  QVERIFY(partial.fullHashBytes > 0);
  QVERIFY(partial.fullHashBytes < quint64(bytes.size()));
  QCOMPARE(partial.cachedEntries, quint64(0));
  const auto valid = reader.read(input, 0, 16);
  QCOMPARE(valid.code, QStringLiteral("ok"));
  QCOMPARE(valid.bytes, bytes.left(16));
  QCOMPARE(reader.statistics().fullHashBytes - partial.fullHashBytes, quint64(bytes.size()));
}

void TestAuthenticatedArtifactReader::invalidationStopsAnActiveScanAndFutureReads() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const auto bytes = content(8 * 1024 * 1024);
  const auto path = dir.filePath("invalidate.bin");
  QVERIFY(writeFile(path, bytes));
  const auto input = identity(path, bytes);
  Reader reader;
  const auto cancelled = reader.read(input, 0, 16, [&] {
    if (reader.statistics().fullHashBytes >= quint64(2 * Chunk))
      reader.invalidate();
    return false;
  });
  QCOMPARE(cancelled.code, QStringLiteral("cancelled"));
  QVERIFY(cancelled.bytes.isEmpty());
  QCOMPARE(reader.statistics().cachedEntries, quint64(0));
  const auto next = reader.read(input, 0, 16);
  QCOMPARE(next.code, QStringLiteral("cancelled"));
  QVERIFY(next.bytes.isEmpty());
}

void TestAuthenticatedArtifactReader::eofAuthenticatesTheLastChunk() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const auto bytes = content(2 * Chunk + 31);
  const auto path = dir.filePath("eof.bin");
  QVERIFY(writeFile(path, bytes));
  const auto input = identity(path, bytes);
  Reader reader;
  QCOMPARE(reader.read(input, 0, 1).code, QStringLiteral("ok"));
  QVERIFY(replaceByteKeepingTime(path, bytes.size() - 1,
      char(bytes.back() ^ 0x27), QFileInfo(path).lastModified()));
  const auto eof = reader.read(input, bytes.size(), 1);
  QVERIFY(eof.code != QLatin1String("ok"));
  QVERIFY(eof.bytes.isEmpty());
}

void TestAuthenticatedArtifactReader::eachRequestedRangeRemainsBoundToTheFrozenArtifact() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const auto bytes = content(3 * Chunk);
  const auto path = dir.filePath("range.bin");
  QVERIFY(writeFile(path, bytes));
  const auto input = identity(path, bytes);
  Reader reader;
  QCOMPARE(reader.read(input, 0, 1).code, QStringLiteral("ok"));
  const qint64 changedAt = 2 * Chunk + 7;
  QVERIFY(replaceByteKeepingTime(path, changedAt, char(bytes.at(changedAt) ^ 0x38), QFileInfo(path).lastModified()));
  // A warm page authenticates its returned range; it is not a fresh whole-file health scan.
  const auto intact = reader.read(input, 0, 13);
  QCOMPARE(intact.code, QStringLiteral("ok"));
  QCOMPARE(intact.bytes, bytes.left(13));
  const auto damaged = reader.read(input, changedAt, 13);
  QVERIFY(damaged.code != QLatin1String("ok"));
  QVERIFY(damaged.bytes.isEmpty());
}

void TestAuthenticatedArtifactReader::concurrentColdPagesShareOneFullScan() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const auto bytes = content(8 * 1024 * 1024);
  const auto path = dir.filePath("concurrent.bin");
  QVERIFY(writeFile(path, bytes));
  const auto input = identity(path, bytes);
  Reader reader;
  QSemaphore ready, go;
  std::array<Reader::Page, 4> pages;
  std::array<std::thread, 4> threads;
  for (std::size_t i = 0; i < threads.size(); ++i) {
    threads[i] = std::thread([&, i] {
      ready.release();
      go.acquire();
      pages[i] = reader.read(input, qint64(i) * (Chunk - 5), 19);
    });
  }
  ready.acquire(int(threads.size()));
  go.release(int(threads.size()));
  for (auto &thread : threads)
    thread.join();
  for (std::size_t i = 0; i < pages.size(); ++i) {
    QCOMPARE(pages[i].code, QStringLiteral("ok"));
    QCOMPARE(pages[i].bytes, bytes.mid(qint64(i) * (Chunk - 5), 19));
  }
  QCOMPARE(reader.statistics().fullScans, quint64(1));
  QCOMPARE(reader.statistics().fullHashBytes, quint64(bytes.size()));
}

void TestAuthenticatedArtifactReader::callerAndIdentityChangesDoNotReuseAnotherIndex() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const auto bytes = content(Chunk + 9);
  const auto path = dir.filePath("scoped.bin");
  const auto otherPath = dir.filePath("other.bin");
  QVERIFY(writeFile(path, bytes));
  QVERIFY(writeFile(otherPath, bytes));
  auto input = identity(path, bytes, "caller-a:doc:epoch1:operation:port");
  Reader reader;
  QCOMPARE(reader.read(input, 0, 4).code, QStringLiteral("ok"));
  input.scopeKey = "caller-b:doc:epoch1:operation:port";
  QCOMPARE(reader.read(input, 0, 4).code, QStringLiteral("ok"));
  QCOMPARE(reader.statistics().fullScans, quint64(2));
  input.scopeKey = "caller-b:doc:epoch2:operation:port";
  QCOMPARE(reader.read(input, 0, 4).code, QStringLiteral("ok"));
  QCOMPARE(reader.statistics().fullScans, quint64(3));
  input.path = otherPath;
  QCOMPARE(reader.read(input, 0, 4).code, QStringLiteral("ok"));
  QCOMPARE(reader.statistics().fullScans, quint64(4));
  input.sha256 = QString(64, 'f');
  const auto wrong = reader.read(input, 0, 4);
  QVERIFY(wrong.code != QLatin1String("ok"));
  QVERIFY(wrong.bytes.isEmpty());
}

void TestAuthenticatedArtifactReader::leastRecentlyUsedIndexesStayBounded() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const auto bytes = content(Chunk + 13);
  const auto path = dir.filePath("lru.bin");
  QVERIFY(writeFile(path, bytes));
  Reader reader;
  std::array<Reader::Identity, 9> inputs;
  for (std::size_t i = 0; i < inputs.size(); ++i)
    inputs[i] = identity(path, bytes, QStringLiteral("scope-%1").arg(i));
  for (int i = 0; i < 8; ++i)
    QCOMPARE(reader.read(inputs[i], 0, 1).code, QStringLiteral("ok"));
  QCOMPARE(reader.statistics().cachedEntries, quint64(8));
  QVERIFY(reader.statistics().cachedDigestBytes <= quint64(4 * 1024 * 1024));
  QCOMPARE(reader.read(inputs[0], 0, 1).code, QStringLiteral("ok"));
  QCOMPARE(reader.statistics().fullScans, quint64(8));
  QCOMPARE(reader.read(inputs[8], 0, 1).code, QStringLiteral("ok"));
  QCOMPARE(reader.statistics().cachedEntries, quint64(8));
  QCOMPARE(reader.statistics().fullScans, quint64(9));
  QCOMPARE(reader.read(inputs[0], 0, 1).code, QStringLiteral("ok"));
  QCOMPARE(reader.statistics().fullScans, quint64(9));
  QCOMPARE(reader.read(inputs[1], 0, 1).code, QStringLiteral("ok"));
  QCOMPARE(reader.statistics().fullScans, quint64(10));
  QCOMPARE(reader.statistics().cachedEntries, quint64(8));
}

QTEST_GUILESS_MAIN(TestAuthenticatedArtifactReader)
#include "tst_authenticatedartifactreader.moc"
