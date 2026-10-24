#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>

#include <cstring>

#include "../src/io/segyreader.h"

// PW-070/071/125：字节序 / 无符号 ns / 样点格式 2·3·8 / 共用道 ns 判定。
// 合成 3×4 标准索引体（inline 偏移 188、xline 偏移 192、坐标 72/76）。
namespace
{
struct Spec
{
  bool le = false;
  int format = 5;
  int ns = 16;
  bool rev2Marker = false;
  int corruptTrace = -1; // 该道 ns 写 0xFFFF（显式损坏）
  int variableTrace = -1; // 该道 ns 写 ns-1（变道长）
};

void put16(uchar *p, quint16 v, bool le)
{
  if (le) qToLittleEndian<quint16>(v, p); else qToBigEndian<quint16>(v, p);
}
void put32(uchar *p, quint32 v, bool le)
{
  if (le) qToLittleEndian<quint32>(v, p); else qToBigEndian<quint32>(v, p);
}

int bytesPer(int f) { return f == 3 ? 2 : (f == 8 ? 1 : 4); }
float ampOf(int t, int k) { return static_cast<float>((t % 50) + k); }

QByteArray build(const Spec &s)
{
  QByteArray out(3600, '\0');
  uchar *bin = reinterpret_cast<uchar *>(out.data()) + 3200;
  put16(bin + 16, 2000, s.le);
  put16(bin + 20, static_cast<quint16>(s.ns), s.le);
  put16(bin + 24, static_cast<quint16>(s.format), s.le);
  if (s.rev2Marker)
    put32(bin + 96, 0x01020304u, s.le);
  int t = 0;
  for (int il = 0; il < 3; ++il)
    for (int xl = 0; xl < 4; ++xl, ++t)
    {
      const int ns = (t == s.variableTrace) ? s.ns - 1 : s.ns;
      QByteArray tr(240 + ns * bytesPer(s.format), '\0');
      uchar *h = reinterpret_cast<uchar *>(tr.data());
      put16(h + 114, t == s.corruptTrace ? 0xFFFF : static_cast<quint16>(ns), s.le);
      put16(h + 116, 2000, s.le);
      put32(h + 188, static_cast<quint32>(10 + il), s.le);
      put32(h + 192, static_cast<quint32>(100 + xl), s.le);
      put32(h + 72, static_cast<quint32>(1000 + 25 * xl), s.le);
      put32(h + 76, static_cast<quint32>(5000 + 25 * il), s.le);
      uchar *p = h + 240;
      for (int k = 0; k < ns; ++k)
      {
        const float a = ampOf(t, k);
        switch (s.format)
        {
        case 5: { quint32 b; std::memcpy(&b, &a, 4); put32(p + 4 * k, b, s.le); break; }
        case 2: put32(p + 4 * k, static_cast<quint32>(static_cast<qint32>(a)), s.le); break;
        case 3: put16(p + 2 * k, static_cast<quint16>(static_cast<qint16>(a)), s.le); break;
        case 8: p[k] = static_cast<uchar>(static_cast<qint8>(a)); break;
        }
      }
      out.append(tr);
    }
  return out;
}

QString writeTo(const QTemporaryDir &d, const QString &name, const QByteArray &b)
{
  const QString p = d.filePath(name);
  QFile f(p);
  if (!f.open(QIODevice::WriteOnly) || f.write(b) != b.size()) return QString();
  return p;
}
} // namespace

class TestSegyEndian : public QObject
{
  Q_OBJECT
private slots:
  void classifier()
  {
    using C = SegyReader::TraceNsClass;
    QCOMPARE(SegyReader::classifyTraceNs(0, 100).cls, C::UseBinary);
    QCOMPARE(SegyReader::classifyTraceNs(0, 100).ns, 100);
    QCOMPARE(SegyReader::classifyTraceNs(100, 100).cls, C::Normal);
    QCOMPARE(SegyReader::classifyTraceNs(90, 100).cls, C::Variable);
    QCOMPARE(SegyReader::classifyTraceNs(110, 100).cls, C::Variable); // 两路同语义
    QCOMPARE(SegyReader::classifyTraceNs(0xFFFF, 100).cls, C::Corrupt);
    QCOMPARE(SegyReader::classifyTraceNs(0xFFFF, 100).ns, 100);
    // 无符号：二进制头本身 ≥32768 的大道长文件不误判损坏
    QCOMPARE(SegyReader::classifyTraceNs(40000, 40000).cls, C::Normal);
    QCOMPARE(SegyReader::classifyTraceNs(40001, 40000).cls, C::Variable);
  }

  void formatBytes()
  {
    QCOMPARE(SegyReader::sampleBytesForFormat(1), 4);
    QCOMPARE(SegyReader::sampleBytesForFormat(2), 4);
    QCOMPARE(SegyReader::sampleBytesForFormat(3), 2);
    QCOMPARE(SegyReader::sampleBytesForFormat(5), 4);
    QCOMPARE(SegyReader::sampleBytesForFormat(8), 1);
    QCOMPARE(SegyReader::sampleBytesForFormat(4), 0);
  }

  void openAllVariants_data()
  {
    QTest::addColumn<bool>("le");
    QTest::addColumn<int>("format");
    QTest::addColumn<bool>("marker");
    for (bool le : {false, true})
      for (int f : {2, 3, 5, 8})
        for (bool m : {false, true})
          QTest::newRow(qPrintable(QStringLiteral("%1-f%2-%3").arg(le ? "le" : "be").arg(f).arg(m ? "rev2" : "plain")))
              << le << f << m;
  }
  void openAllVariants()
  {
    QFETCH(bool, le);
    QFETCH(int, format);
    QFETCH(bool, marker);
    QTemporaryDir d;
    Spec s; s.le = le; s.format = format; s.rev2Marker = marker;
    const QString p = writeTo(d, QStringLiteral("v.sgy"), build(s));
    SegyReader r;
    QString err;
    QVERIFY2(r.open(p, &err), qPrintable(err));
    QCOMPARE(r.littleEndian(), le);
    QCOMPARE(r.traceCount(), 12);
    QCOMPARE(r.samplesPerTrace(), 16);
    QCOMPARE(r.geometry().inlineMin, 10);
    QCOMPARE(r.geometry().xlineMax, 103);
    QVector<SegyTrace> line;
    QVERIFY2(r.readInline(11, &line, &err), qPrintable(err));
    QCOMPARE(line.size(), 4);
    for (int k = 0; k < 16; ++k)
      QCOMPARE(line.at(2).samples.at(k), ampOf(4 + 2, k));

    // 缓存路径：首次扫描 + 命中恢复都保持字节序/格式
    for (int pass = 0; pass < 2; ++pass)
    {
      SegyReader c;
      QVERIFY2(c.openCached(p, d.filePath(QStringLiteral("idx")), &err), qPrintable(err));
      QCOMPARE(c.littleEndian(), le);
      QVector<SegyTrace> l2;
      QVERIFY2(c.readInline(11, &l2, &err), qPrintable(err));
      QCOMPARE(l2.at(2).samples.at(5), ampOf(6, 5));
    }
  }

  void corruptTraceSkippedExplicitly()
  {
    QTemporaryDir d;
    Spec s; s.le = true; s.format = 3; s.corruptTrace = 5;
    const QString p = writeTo(d, QStringLiteral("c.sgy"), build(s));
    SegyReader r;
    QString err;
    QVERIFY2(r.open(p, &err), qPrintable(err));
    QCOMPARE(r.traceCount(), 11);
    QCOMPARE(r.badTraceOffsets().size(), 1);
  }

  void variableTraceSequentialOnly()
  {
    QTemporaryDir d;
    Spec s; s.format = 5; s.variableTrace = 11; // 末道变长
    const QString p = writeTo(d, QStringLiteral("var.sgy"), build(s));
    SegyReader r;
    QString err;
    QVERIFY2(r.open(p, &err), qPrintable(err));
    QCOMPARE(r.traceCount(), 12);
    QVERIFY(r.variableTraceLayout());
    QVERIFY(r.badTraceOffsets().isEmpty()); // 不再被并行判据当坏道吞掉
  }

  void unsupportedFormatRejected()
  {
    QTemporaryDir d;
    Spec s; s.format = 5;
    QByteArray b = build(s);
    qToBigEndian<quint16>(4, reinterpret_cast<uchar *>(b.data()) + 3224);
    const QString p = writeTo(d, QStringLiteral("f4.sgy"), b);
    SegyReader r;
    QString err;
    QVERIFY(!r.open(p, &err));
    QVERIFY(err.contains(QStringLiteral("Unsupported")));
  }
};

QTEST_GUILESS_MAIN(TestSegyEndian)
#include "tst_segy_endian.moc"
