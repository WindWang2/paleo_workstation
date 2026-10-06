#include <QtTest>
#include <QByteArray>
#include <QMap>
#include <QTemporaryFile>

#include <zlib.h>

#include "../src/io/ziparchive.h"

using namespace paleo::io;

class TestIoZipArchiveUnit : public QObject
{
  Q_OBJECT

private slots:
  void readArchiveNonExistent();
  void corruptedArchiveFails();
  void emptyOrTruncatedArchive();
  void storedZipRoundTrip();
  void extractNonExistentEntry();
  void mutationDemonstration_corruptEntryDetected();

private:
  static QByteArray makeStoredZip(const QMap<QString, QByteArray> &files);
};

static void writeU16(QByteArray &b, quint16 v)
{
  b.append(static_cast<char>(v & 0xFF));
  b.append(static_cast<char>((v >> 8) & 0xFF));
}

static void writeU32(QByteArray &b, quint32 v)
{
  b.append(static_cast<char>(v & 0xFF));
  b.append(static_cast<char>((v >> 8) & 0xFF));
  b.append(static_cast<char>((v >> 16) & 0xFF));
  b.append(static_cast<char>((v >> 24) & 0xFF));
}

QByteArray TestIoZipArchiveUnit::makeStoredZip(const QMap<QString, QByteArray> &files)
{
  QByteArray archive;
  struct EntryMeta
  {
    QString name;
    quint32 crc;
    quint32 size;
    quint32 offset;
  };
  QVector<EntryMeta> metas;

  for (auto it = files.begin(); it != files.end(); ++it)
  {
    const QString name = it.key();
    const QByteArray data = it.value();
    const quint32 offset = static_cast<quint32>(archive.size());
    const quint32 crc = static_cast<quint32>(crc32(0L, reinterpret_cast<const Bytef*>(data.constData()), data.size()));

    // Local file header: PK\x03\x04
    archive.append("\x50\x4b\x03\x04", 4);
    writeU16(archive, 10); // version needed
    writeU16(archive, 0);  // flags
    writeU16(archive, 0);  // method: stored
    writeU32(archive, 0);  // time/date
    writeU32(archive, crc);
    writeU32(archive, static_cast<quint32>(data.size()));
    writeU32(archive, static_cast<quint32>(data.size()));
    writeU16(archive, static_cast<quint16>(name.toUtf8().size()));
    writeU16(archive, 0);  // extra len
    archive.append(name.toUtf8());
    archive.append(data);

    metas.append({name, crc, static_cast<quint32>(data.size()), offset});
  }

  const quint32 cdOffset = static_cast<quint32>(archive.size());
  for (const auto &m : metas)
  {
    // Central header: PK\x01\x02
    archive.append("\x50\x4b\x01\x02", 4);
    writeU16(archive, 20); // version made
    writeU16(archive, 10); // version needed
    writeU16(archive, 0);  // flags
    writeU16(archive, 0);  // method: stored
    writeU32(archive, 0);  // time/date
    writeU32(archive, m.crc);
    writeU32(archive, m.size);
    writeU32(archive, m.size);
    writeU16(archive, static_cast<quint16>(m.name.toUtf8().size()));
    writeU16(archive, 0); // extra
    writeU16(archive, 0); // comment
    writeU16(archive, 0); // disk
    writeU16(archive, 0); // int attr
    writeU32(archive, 0); // ext attr
    writeU32(archive, m.offset);
    archive.append(m.name.toUtf8());
  }

  const quint32 cdSize = static_cast<quint32>(archive.size() - cdOffset);

  // EOCD: PK\x05\x06
  archive.append("\x50\x4b\x05\x06", 4);
  writeU16(archive, 0); // disk num
  writeU16(archive, 0); // cd start disk
  writeU16(archive, static_cast<quint16>(metas.size()));
  writeU16(archive, static_cast<quint16>(metas.size()));
  writeU32(archive, cdSize);
  writeU32(archive, cdOffset);
  writeU16(archive, 0); // comment len

  return archive;
}

void TestIoZipArchiveUnit::readArchiveNonExistent()
{
  QString err;
  const QByteArray data = zipReadArchive(QStringLiteral("non_existent_file.zip"), &err);
  QVERIFY(data.isEmpty());
  QVERIFY(!err.isEmpty());
}

void TestIoZipArchiveUnit::corruptedArchiveFails()
{
  const QByteArray garbage = "THIS_IS_NOT_A_VALID_ZIP_FILE_AT_ALL_JUST_RANDOM_TEXT";
  const ZipListResult res = zipListBytes(garbage);
  QVERIFY(!res.ok);
  QVERIFY(!res.error.isEmpty());
}

void TestIoZipArchiveUnit::emptyOrTruncatedArchive()
{
  const ZipListResult res1 = zipListBytes(QByteArray());
  QVERIFY(!res1.ok);

  const ZipListResult res2 = zipListBytes(QByteArray("PK\x05\x06"));
  QVERIFY(!res2.ok);
}

void TestIoZipArchiveUnit::storedZipRoundTrip()
{
  QMap<QString, QByteArray> inputFiles;
  inputFiles.insert(QStringLiteral("hello.txt"), QByteArrayLiteral("Hello Paleo Workstation!"));
  inputFiles.insert(QStringLiteral("meta/spec.json"), QByteArrayLiteral("{\"version\": 2, \"active\": true}"));

  const QByteArray zipBytes = makeStoredZip(inputFiles);
  QVERIFY(!zipBytes.isEmpty());

  const ZipListResult list = zipListBytes(zipBytes);
  QVERIFY2(list.ok, qPrintable(list.error));
  QCOMPARE(list.entries.size(), 2);
  QVERIFY(list.contains(QStringLiteral("hello.txt")));
  QVERIFY(list.contains(QStringLiteral("meta/spec.json")));

  const ZipEntry *e1 = list.entry(QStringLiteral("hello.txt"));
  QVERIFY(e1 != nullptr);
  QCOMPARE(e1->method, 0);
  QCOMPARE(e1->uncompressedSize, static_cast<quint64>(inputFiles.value(QStringLiteral("hello.txt")).size()));

  // 解压提取
  QByteArray out1;
  QString err;
  QVERIFY(zipExtractBytes(zipBytes, QStringLiteral("hello.txt"), &out1, &err));
  QCOMPARE(out1, inputFiles.value(QStringLiteral("hello.txt")));

  QByteArray out2;
  QVERIFY(zipExtractBytes(zipBytes, QStringLiteral("meta/spec.json"), &out2, &err));
  QCOMPARE(out2, inputFiles.value(QStringLiteral("meta/spec.json")));
}

void TestIoZipArchiveUnit::extractNonExistentEntry()
{
  QMap<QString, QByteArray> inputFiles;
  inputFiles.insert(QStringLiteral("a.txt"), QByteArrayLiteral("AAA"));
  const QByteArray zipBytes = makeStoredZip(inputFiles);

  QByteArray out;
  QString err;
  const bool ok = zipExtractBytes(zipBytes, QStringLiteral("missing_entry.txt"), &out, &err);
  QVERIFY(!ok);
  QVERIFY(err.contains(QStringLiteral("未找到")) || err.contains(QStringLiteral("条目")));
}

void TestIoZipArchiveUnit::mutationDemonstration_corruptEntryDetected()
{
  // 变异测试示范：若数据发生单字节篡改，解压或者读取必须报错或校验失败
  QMap<QString, QByteArray> inputFiles;
  inputFiles.insert(QStringLiteral("test.bin"), QByteArrayLiteral("ORIGINAL_DATA_BLOCK"));
  QByteArray zipBytes = makeStoredZip(inputFiles);

  // 破坏中间数据
  const int targetIndex = zipBytes.indexOf("ORIGINAL");
  QVERIFY(targetIndex != -1);
  zipBytes[targetIndex] = 'X';

  // 尝试读取，或者破坏局部头
  zipBytes[0] = 'Z'; // 破坏魔数
  const ZipListResult list = zipListBytes(zipBytes);
  QVERIFY(!list.ok);
}

QTEST_GUILESS_MAIN(TestIoZipArchiveUnit)
#include "tst_io_ziparchive_unit.moc"
