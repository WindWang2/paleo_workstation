#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QtEndian>

#include "../src/io/segyreader.h"

#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif

// plan §7 测线级 SEG-Y：打开只索引道头（内存不随体增长）、
// 单 inline/crossline 按需解码、survey 几何冻结、crossline 道头字节 193。
class TestSegyLines : public QObject
{
  Q_OBJECT

private slots:
  void readsSingleInline();
  void readsSingleCrossline();
  void geometryFrozenFromHeaders();
  void missingLineFails();
  void openMemoryDoesNotScaleWithVolume();
  void ordinalIndexingWhenLineWordConstant();
  void ordinalIndexCdpOrderMismatchFails();
  void ordinalIndexTraceCountMismatchFails();
  void variableTraceLengthsKeepOrdinalIndex();

private:
  // 复刻真工区结构的合成件：偏移 188/192 恒为 0，inline 走道号索引——
  // field record @8 = base+l、CDP @20 = cdpBase+p、坐标 @72/76(Source X/Y) = (p*20, l*40)、
  // 二进制头字节 13-14 = 每条 inline 道数（真文件 = 641）。
  static bool writeOrdinalSegy(const QString &path, int traces, int perLine,
                               qint32 frecBase, qint32 cdpBase,
                               int breakTrace = -1, qint32 breakCdp = 0)
  {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
      return false;
    f.write(QByteArray(3200, ' '));
    QByteArray bh(400, 0);
    qToBigEndian<qint16>(perLine, reinterpret_cast<uchar *>(bh.data()) + 12); // 字节 13-14
    qToBigEndian<qint16>(2000, reinterpret_cast<uchar *>(bh.data()) + 16);
    qToBigEndian<qint16>(64, reinterpret_cast<uchar *>(bh.data()) + 20);
    qToBigEndian<qint16>(5, reinterpret_cast<uchar *>(bh.data()) + 24); // IEEE
    f.write(bh);
    for (int i = 0; i < traces; ++i)
    {
      const int l = i / perLine, p = i % perLine;
      QByteArray th(240, 0);
      qToBigEndian<qint32>(i + 1, reinterpret_cast<uchar *>(th.data()) + 0);
      qToBigEndian<qint32>(frecBase + l, reinterpret_cast<uchar *>(th.data()) + 8);
      const qint32 cdp = (i == breakTrace) ? breakCdp : cdpBase + p;
      qToBigEndian<qint32>(cdp, reinterpret_cast<uchar *>(th.data()) + 20);
      qToBigEndian<qint16>(1, reinterpret_cast<uchar *>(th.data()) + 70); // 坐标比例因子
      qToBigEndian<qint32>(p * 20, reinterpret_cast<uchar *>(th.data()) + 72);
      qToBigEndian<qint32>(l * 40, reinterpret_cast<uchar *>(th.data()) + 76);
      qToBigEndian<qint16>(64, reinterpret_cast<uchar *>(th.data()) + 114);
      qToBigEndian<qint16>(2000, reinterpret_cast<uchar *>(th.data()) + 116);
      f.write(th);
      f.write(QByteArray(64 * 4, 0));
    }
    return true;
  }

  QString fixture() const
  {
    return QStringLiteral(PROJECT_FIXTURE_DIR) + QStringLiteral("/mini_seismic.sgy");
  }
  // 夹具生成时的样本编码：((inl-1000)*4 + (xl-2000))*10 + (s-ns/2)*0.25
  float expectedSample(int inl, int xl, int s, int ns) const
  {
    return ((inl - 1000) * 4 + (xl - 2000)) * 10.0f + (s - ns / 2) * 0.25f;
  }
  static qulonglong residentBytes()
  {
#if defined(Q_OS_UNIX)
    QFile f(QStringLiteral("/proc/self/statm"));
    if (!f.open(QIODevice::ReadOnly))
      return 0;
    const QStringList t = QString::fromUtf8(f.readAll()).split(QLatin1Char(' '));
    if (t.size() < 2)
      return 0;
    bool ok = false;
    const qulonglong pages = t.at(1).toULongLong(&ok);
    return ok ? pages * static_cast<qulonglong>(sysconf(_SC_PAGESIZE)) : 0;
#else
    return 0; // RSS assertion is Linux-specific; trace indexing still runs.
#endif
  }
};

void TestSegyLines::readsSingleInline()
{
  SegyReader r;
  QString err;
  QVERIFY2(r.open(fixture(), &err), qPrintable(err));
  QCOMPARE(r.traceCount(), 12);
  QCOMPARE(r.samplesPerTrace(), 64);
  QCOMPARE(r.sampleIntervalUs(), 2000.0f);

  QVector<SegyTrace> line;
  QVERIFY2(r.readInline(1001, &line, &err), qPrintable(err));
  QCOMPARE(line.size(), 4);
  // 按 crossline 升序
  for (int i = 1; i < line.size(); ++i)
    QVERIFY(line.at(i - 1).xlineNo < line.at(i).xlineNo);
  QCOMPARE(line.front().xlineNo, 2000);
  QCOMPARE(line.back().xlineNo, 2003);
  // 样本值（IBM 解码，宽松比较）
  const int ns = 64;
  for (int tr = 0; tr < line.size(); ++tr)
  {
    const int xl = 2000 + tr;
    QCOMPARE(line.at(tr).samples.size(), ns);
    for (int s = 0; s < ns; ++s)
    {
      const float want = expectedSample(1001, xl, s, ns);
      QVERIFY2(qAbs(line.at(tr).samples.at(s) - want) < qAbs(want) * 1e-5f + 1e-4f,
               qPrintable(QStringLiteral("s=%1 got=%2 want=%3")
                              .arg(s).arg(line.at(tr).samples.at(s)).arg(want)));
    }
  }
}

void TestSegyLines::readsSingleCrossline()
{
  SegyReader r;
  QString err;
  QVERIFY2(r.open(fixture(), &err), qPrintable(err));
  QVector<SegyTrace> line;
  QVERIFY2(r.readCrossline(2001, &line, &err), qPrintable(err));
  QCOMPARE(line.size(), 3);
  // 按 inline 升序
  for (int i = 1; i < line.size(); ++i)
    QVERIFY(line.at(i - 1).lineNo < line.at(i).lineNo);
  QCOMPARE(line.front().lineNo, 1000);
  QCOMPARE(line.back().lineNo, 1002);
}

void TestSegyLines::geometryFrozenFromHeaders()
{
  SegyReader r;
  QString err;
  QVERIFY2(r.open(fixture(), &err), qPrintable(err));
  const SegyGeometry g = r.geometry();
  QCOMPARE(g.inlineMin, 1000);
  QCOMPARE(g.inlineMax, 1002);
  QCOMPARE(g.xlineMin, 2000);
  QCOMPARE(g.xlineMax, 2003);
  QCOMPARE(g.startTimeMs, 0.0);
}

void TestSegyLines::missingLineFails()
{
  SegyReader r;
  QVERIFY(r.open(fixture()));
  QVector<SegyTrace> line;
  QString err;
  QVERIFY(!r.readInline(9999, &line, &err));
  QVERIFY(!err.isEmpty());
  QVERIFY(!r.readCrossline(9999, &line, &err));
}

void TestSegyLines::openMemoryDoesNotScaleWithVolume()
{
  // 合成体：2000 道 × 1024 样本（样本字节 8.2 MB）。索引式打开后
  // RSS 增长必须远小于样本总量（只允许 O(道头) 的索引开销）。
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = dir.filePath(QStringLiteral("big.sgy"));
  const int nInl = 250, nXl = 10, ns = 1024;

  QFile f(path);
  QVERIFY(f.open(QIODevice::WriteOnly));
  f.write(QByteArray(3200, ' '));
  QByteArray bh(400, 0);
  qToBigEndian<qint16>(ns, reinterpret_cast<uchar *>(bh.data()) + 20);
  qToBigEndian<qint16>(2000, reinterpret_cast<uchar *>(bh.data()) + 16);
  qToBigEndian<qint16>(5, reinterpret_cast<uchar *>(bh.data()) + 24); // IEEE，直接位序写
  f.write(bh);
  QByteArray sampleBytes(ns * 4, 0);
  for (int i = 0; i < nInl; ++i)
    for (int x = 0; x < nXl; ++x)
    {
      QByteArray th(240, 0);
      qToBigEndian<qint32>(i * nXl + x, reinterpret_cast<uchar *>(th.data()) + 0);
      qToBigEndian<qint32>(1000 + i, reinterpret_cast<uchar *>(th.data()) + 188);
      qToBigEndian<qint32>(5000 + x, reinterpret_cast<uchar *>(th.data()) + 192);
      qToBigEndian<qint16>(ns, reinterpret_cast<uchar *>(th.data()) + 114);
      f.write(th);
      f.write(sampleBytes);
    }
  f.close();

  const qint64 sampleTotal = static_cast<qint64>(nInl) * nXl * ns * 4;
  QVERIFY(sampleTotal > 8 * 1024 * 1024); // 夹具本身够大，断言才有意义

  const qulonglong rssBefore = residentBytes();
  {
    SegyReader r;
    QString err;
    QVERIFY2(r.open(path, &err), qPrintable(err));
    QCOMPARE(r.traceCount(), nInl * nXl);
    const qulonglong rssAfter = residentBytes();
    if (rssBefore > 0 && rssAfter > 0)
    {
      QVERIFY2(rssAfter >= rssBefore, "rss sanity");
      const qulonglong growth = rssAfter - rssBefore;
      QVERIFY2(growth < 3ULL * 1024 * 1024,
               qPrintable(QStringLiteral("open() grew RSS by %1 MB; sample payload is %2 MB")
                              .arg(growth / (1024 * 1024)).arg(sampleTotal / (1024 * 1024))));
    }
    // 单线解码规模符合预期（10 道 × 1024 样本）。
    QVector<SegyTrace> line;
    QVERIFY(r.readInline(1100, &line, &err));
    QCOMPARE(line.size(), nXl);
    QCOMPARE(line.front().samples.size(), ns);
  }
}

void TestSegyLines::ordinalIndexingWhenLineWordConstant()
{
  // 真工区 200P 的结构缩影：偏移 188 恒 0 → 按道号索引
  // inline = 1315 + 道号/4，crossline = 该道 CDP（plan §2）。
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = dir.filePath(QStringLiteral("ordinal.sgy"));
  QVERIFY(writeOrdinalSegy(path, 3 * 4, 4, 1315, 4165));

  SegyReader r;
  QString err;
  QVERIFY2(r.open(path, &err), qPrintable(err));
  QCOMPARE(r.traceCount(), 12);
  QCOMPARE(r.inlineNumbers(), QVector<qint32>({1315, 1316, 1317}));
  QCOMPARE(r.crosslineNumbers(), QVector<qint32>({4165, 4166, 4167, 4168}));

  QVector<SegyTrace> line;
  QVERIFY2(r.readInline(1316, &line, &err), qPrintable(err));
  QCOMPARE(line.size(), 4);
  for (int i = 0; i < line.size(); ++i)
  {
    QCOMPARE(line.at(i).lineNo, 1316);
    QCOMPARE(line.at(i).xlineNo, 4165 + i); // 按 crossline 升序
  }
  QVector<SegyTrace> xl;
  QVERIFY2(r.readCrossline(4166, &xl, &err), qPrintable(err));
  QCOMPARE(xl.size(), 3);
  for (int i = 0; i < xl.size(); ++i)
    QCOMPARE(xl.at(i).lineNo, 1315 + i); // 按 inline 升序

  const SegyGeometry g = r.geometry();
  QCOMPARE(g.inlineMin, 1315);
  QCOMPARE(g.inlineMax, 1317);
  QCOMPARE(g.xlineMin, 4165);
  QCOMPARE(g.xlineMax, 4168);
  // 四角 = 四条极端道的坐标：(inlMin,xlMin)(inlMin,xlMax)(inlMax,xlMax)(inlMax,xlMin)
  QCOMPARE(g.cornerX[0], 0.0);
  QCOMPARE(g.cornerY[0], 0.0);
  QCOMPARE(g.cornerX[1], 60.0);
  QCOMPARE(g.cornerY[1], 0.0);
  QCOMPARE(g.cornerX[2], 60.0);
  QCOMPARE(g.cornerY[2], 80.0);
  QCOMPARE(g.cornerX[3], 0.0);
  QCOMPARE(g.cornerY[3], 80.0);
}

void TestSegyLines::ordinalIndexCdpOrderMismatchFails()
{
  // CDP 顺序对不上（第 6 道应为 4167，读到 9999）→ 停止并报出两值。
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = dir.filePath(QStringLiteral("cdp_bad.sgy"));
  QVERIFY(writeOrdinalSegy(path, 3 * 4, 4, 1315, 4165, 6, 9999));

  SegyReader r;
  QString err;
  QVERIFY(!r.open(path, &err));
  QVERIFY2(err.contains(QStringLiteral("4167")) && err.contains(QStringLiteral("9999")),
           qPrintable(err));
}

void TestSegyLines::ordinalIndexTraceCountMismatchFails()
{
  // 道数对不上：CDP 每 3 道回落，总道数 5 不是整数倍 → 停止并报数。
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = dir.filePath(QStringLiteral("count_bad.sgy"));
  QVERIFY(writeOrdinalSegy(path, 5, 3, 1315, 4165));

  SegyReader r;
  QString err;
  QVERIFY(!r.open(path, &err));
  QVERIFY2(err.contains(QStringLiteral("5")) && err.contains(QStringLiteral("3")),
           qPrintable(err));
}

void TestSegyLines::variableTraceLengthsKeepOrdinalIndex()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QFile file(dir.filePath(QStringLiteral("variable.sgy")));
  QVERIFY(file.open(QIODevice::WriteOnly));
  file.write(QByteArray(3200, ' '));
  QByteArray binary(400, 0);
  qToBigEndian<qint16>(2, reinterpret_cast<uchar *>(binary.data() + 12));
  qToBigEndian<qint16>(2000, reinterpret_cast<uchar *>(binary.data() + 16));
  qToBigEndian<qint16>(4, reinterpret_cast<uchar *>(binary.data() + 20));
  qToBigEndian<qint16>(5, reinterpret_cast<uchar *>(binary.data() + 24));
  file.write(binary);
  for (int i = 0; i < 4; ++i)
  {
    QByteArray header(240, 0);
    qToBigEndian<qint32>(1315 + i / 2, reinterpret_cast<uchar *>(header.data() + 8));
    qToBigEndian<qint32>(4165 + i % 2, reinterpret_cast<uchar *>(header.data() + 20));
    qToBigEndian<qint32>((i % 2) * 20, reinterpret_cast<uchar *>(header.data() + 72));
    qToBigEndian<qint32>((i / 2) * 40, reinterpret_cast<uchar *>(header.data() + 76));
    qToBigEndian<qint16>(i % 2 ? 5 : 3, reinterpret_cast<uchar *>(header.data() + 114));
    qToBigEndian<qint16>(i % 2 ? 4000 : 2000, reinterpret_cast<uchar *>(header.data() + 116));
    file.write(header);
    file.write(QByteArray((i % 2 ? 5 : 3) * 4, 0));
  }
  file.close();

  SegyReader reader;
  QString error;
  QVERIFY2(reader.open(file.fileName(), &error), qPrintable(error));
  QCOMPARE(reader.inlineNumbers(), QVector<qint32>({1315, 1316}));
  QVector<SegyTrace> traces;
  QVERIFY(reader.readInline(1315, &traces, &error));
  QCOMPARE(traces.size(), 2);
  QCOMPARE(traces.at(0).samples.size(), 3);
  QCOMPARE(traces.at(1).samples.size(), 5);
  QCOMPARE(traces.at(1).sampleIntervalUs, 4000.0f);
}

QTEST_MAIN(TestSegyLines)
#include "tst_segy_lines.moc"
