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
  void crcMismatchRejectedAfterExtract(); // #477
  void zip64DirectoryOffsetWraparoundRejected(); // A-10（full-audit-20261020）
  void storedEntrySizeMismatchRejected();

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

static void writeU64(QByteArray &b, quint64 v)
{
  for (quint32 shift = 0; shift < 64; shift += 8)
    b.append(static_cast<char>((v >> shift) & 0xFF));
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

  // 破坏局部头魔数——listing 只读中央目录，须走 extract 才触达本地头校验
  zipBytes[0] = 'Z';
  QByteArray out;
  QString err;
  QVERIFY(!zipExtractBytes(zipBytes, QStringLiteral("test.bin"), &out, &err));
  QVERIFY(!err.isEmpty());
}

// #477：解压后必须比对中央目录 CRC32——此前只校验长度/zlib 状态，字节
// 被篡改但恰好 inflate 成功的条目会静默交出坏数据。
void TestIoZipArchiveUnit::crcMismatchRejectedAfterExtract()
{
  QMap<QString, QByteArray> inputFiles;
  inputFiles.insert(QStringLiteral("payload.bin"), QByteArrayLiteral("INTEGRITY_CHECK_BLOCK_0123456789"));
  QByteArray zipBytes = makeStoredZip(inputFiles);

  // 只翻转一个数据位（本地头/中央目录结构完好，长度不变）。
  const int dataIndex = zipBytes.indexOf("INTEGRITY_CHECK_BLOCK");
  QVERIFY(dataIndex != -1);
  zipBytes[dataIndex] = 'X'; // XNTEGRITY...

  QByteArray out;
  QString err;
  QVERIFY(!zipExtractBytes(zipBytes, QStringLiteral("payload.bin"), &out, &err));
  QVERIFY2(err.contains(QStringLiteral("CRC32")), qPrintable(err));

  // 未篡改的包照常通过（CRC 不误伤）。
  QByteArray clean = makeStoredZip(inputFiles);
  QByteArray cleanOut;
  QString cleanErr;
  QVERIFY(zipExtractBytes(clean, QStringLiteral("payload.bin"), &cleanOut, &cleanErr));
  QCOMPARE(cleanOut, inputFiles.value(QStringLiteral("payload.bin")));
}

QTEST_GUILESS_MAIN(TestIoZipArchiveUnit)
#include "tst_io_ziparchive_unit.moc"

// A-10（full-audit-20261020）：EOCD64 的 directoryOffset 取近 2^64 值、
// directorySize 取可回绕组合时，加法判界 (offset + size > len) 穿闸，
// 随后 qint64 换负下标读缓冲区之前的内存。修复后按减法判界拒绝。
void TestIoZipArchiveUnit::zip64DirectoryOffsetWraparoundRejected()
{
  QByteArray archive;
  archive.append("local-data-padding-not-relevant", 30);
  const int zip64Offset = archive.size();
  // EOCD64（56 字节）
  archive.append("\x50\x4b\x06\x06", 4); // 签名
  writeU64(archive, 44);  // 记录长 - 12
  writeU16(archive, 45);  // version made by
  writeU16(archive, 45);  // version needed
  writeU32(archive, 0);   // disk
  writeU32(archive, 0);   // cd disk
  writeU64(archive, 1);   // 本盘条目数
  writeU64(archive, 1);   // 总条目数
  writeU64(archive, 200); // directory size
  writeU64(archive, 0xFFFFFFFFFFFFFF9CULL); // directory offset = 2^64 - 100
  // locator（20 字节）
  archive.append("\x50\x4b\x06\x07", 4);
  writeU32(archive, 0);
  writeU64(archive, static_cast<quint64>(zip64Offset));
  writeU32(archive, 1);
  // EOCD（22 字节）：count 哨兵触发 ZIP64 解析
  archive.append("\x50\x4b\x05\x06", 4);
  writeU16(archive, 0);
  writeU16(archive, 0);
  writeU16(archive, 0xFFFF); // 本盘条目数 = 哨兵
  writeU16(archive, 0xFFFF); // 总条目数 = 哨兵
  writeU32(archive, 0xFFFFFFFF);
  writeU32(archive, 0xFFFFFFFF);
  writeU16(archive, 0);

  const ZipListResult listing = zipListBytes(archive);
  QVERIFY(!listing.ok);
  QVERIFY2(listing.error.contains(QStringLiteral("偏移越界")), qPrintable(listing.error));
}

// stored 条目 compressedSize != uncompressedSize 的声明矛盾必须拒绝：
// 即便 CRC 与实际字节恰好一致，也不得交出与声明不符长度的数据。
void TestIoZipArchiveUnit::storedEntrySizeMismatchRejected()
{
  QMap<QString, QByteArray> inputFiles;
  inputFiles.insert(QStringLiteral("m.txt"), QByteArrayLiteral("MISMATCH"));
  QByteArray zipBytes = makeStoredZip(inputFiles);

  // 中央目录条目 +24 处是 uncompressedSize：由 8 改成 9（compressedSize 保持 8）。
  const int eocd = zipBytes.lastIndexOf("PK\x05\x06");
  QVERIFY(eocd > 0);
  const quint32 cdOffset = static_cast<quint32>(static_cast<unsigned char>(zipBytes.at(eocd + 16))) |
                           (static_cast<quint32>(static_cast<unsigned char>(zipBytes.at(eocd + 17))) << 8) |
                           (static_cast<quint32>(static_cast<unsigned char>(zipBytes.at(eocd + 18))) << 16) |
                           (static_cast<quint32>(static_cast<unsigned char>(zipBytes.at(eocd + 19))) << 24);
  zipBytes[static_cast<int>(cdOffset) + 24] = '\x09';

  QByteArray out;
  QString err;
  QVERIFY(!zipExtractBytes(zipBytes, QStringLiteral("m.txt"), &out, &err));
  QVERIFY2(err.contains(QStringLiteral("stored 长度与声明不符")), qPrintable(err));
}
