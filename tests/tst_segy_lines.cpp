#include <QtTest>
#include <QTemporaryDir>
#include <QFile>

#include "../src/io/segyreader.h"

#include <sys/stat.h>
#include <unistd.h>

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

private:
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
    QFile f(QStringLiteral("/proc/self/statm"));
    if (!f.open(QIODevice::ReadOnly))
      return 0;
    const QStringList t = QString::fromUtf8(f.readAll()).split(QLatin1Char(' '));
    if (t.size() < 2)
      return 0;
    bool ok = false;
    const qulonglong pages = t.at(1).toULongLong(&ok);
    return ok ? pages * static_cast<qulonglong>(sysconf(_SC_PAGESIZE)) : 0;
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
    QVERIFY2(rssAfter >= rssBefore, "rss sanity");
    const qulonglong growth = rssAfter - rssBefore;
    QVERIFY2(growth < 3ULL * 1024 * 1024,
             qPrintable(QStringLiteral("open() grew RSS by %1 MB; sample payload is %2 MB")
                            .arg(growth / (1024 * 1024)).arg(sampleTotal / (1024 * 1024))));
    // 单线解码规模符合预期（10 道 × 1024 样本）。
    QVector<SegyTrace> line;
    QVERIFY(r.readInline(1100, &line, &err));
    QCOMPARE(line.size(), nXl);
    QCOMPARE(line.front().samples.size(), ns);
  }
}

QTEST_MAIN(TestSegyLines)
#include "tst_segy_lines.moc"
