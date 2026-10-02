#include <QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <limits>

#include "../src/io/segyreader.h"

// wave3/model-hardening — 合成最小 SEG-Y 夹具（testdata/segy/synthetic_4x5.sgy，
// 由 tools/make_segy_fixture.py 生成，13 KB）。夹具按本工区道头约定构造：
// inline 字（偏移 188）恒 0、CDP 在偏移 20、首道 field record（偏移 8）= inline
// 起点、角点坐标在偏移 72/76（Source X/Y）。真 966MB 体不进仓库——这些用例
// 证明夹具与 SegyReader 的索引/解码/角点/范围门契约互相咬合。
class TestSegyFixture : public QObject
{
  Q_OBJECT

private:
  QString fixturePath() const
  {
    return QDir(QStringLiteral(TESTDATA_DIR)).filePath(QStringLiteral("segy/synthetic_4x5.sgy"));
  }

  // 生成器同一振幅公式：f(i,j,k) = (i+1)*100 + j + k/100
  static float amp(int i, int j, int k)
  {
    return static_cast<float>((i + 1) * 100 + j) + static_cast<float>(k) / 100.0f;
  }

private slots:
  void initTestCase()
  {
    QVERIFY2(QFile::exists(fixturePath()), qPrintable(fixturePath()));
  }

  // 索引建立：20 道、64 样本/道、2000µs；inline 10..13、xline 100..104 全在线。
  void indexBuilding()
  {
    SegyReader reader;
    QString err;
    QVERIFY2(reader.open(fixturePath(), &err), qPrintable(err));
    QCOMPARE(reader.traceCount(), 20);
    QCOMPARE(reader.samplesPerTrace(), 64);
    QCOMPARE(reader.sampleIntervalUs(), 2000.0f);

    const SegyGeometry g = reader.geometry();
    QCOMPARE(g.inlineMin, 10);
    QCOMPARE(g.inlineMax, 13);
    QCOMPARE(g.xlineMin, 100);
    QCOMPARE(g.xlineMax, 104);
    QCOMPARE(g.startTimeMs, 100.0);

    const QVector<qint32> inlines = reader.inlineNumbers();
    QCOMPARE(inlines.size(), 4);
    QCOMPARE(inlines.front(), 10);
    QCOMPARE(inlines.back(), 13);
    const QVector<qint32> xlines = reader.crosslineNumbers();
    QCOMPARE(xlines.size(), 5);
    QCOMPARE(xlines.front(), 100);
    QCOMPARE(xlines.back(), 104);
  }

  // 单线解码：readInline 按道返回 5 道且样本逐点还原生成器公式；
  // readCrossline 对称返回 4 道。
  void singleLineDecode()
  {
    SegyReader reader;
    QString err;
    QVERIFY2(reader.open(fixturePath(), &err), qPrintable(err));

    // inline 12 = i=2
    QVector<SegyTrace> line;
    QVERIFY2(reader.readInline(12, &line, &err), qPrintable(err));
    QCOMPARE(line.size(), 5);
    for (int j = 0; j < 5; ++j)
    {
      QCOMPARE(line.at(j).lineNo, 12);
      QCOMPARE(line.at(j).xlineNo, 100 + j);
      QCOMPARE(line.at(j).samples.size(), 64);
      for (int k = 0; k < 64; ++k)
        QCOMPARE(line.at(j).samples.at(k), amp(2, j, k));
    }

    // crossline 103 = j=3
    QVector<SegyTrace> xline;
    QVERIFY2(reader.readCrossline(103, &xline, &err), qPrintable(err));
    QCOMPARE(xline.size(), 4);
    for (int i = 0; i < 4; ++i)
    {
      QCOMPARE(xline.at(i).xlineNo, 103);
      QCOMPARE(xline.at(i).samples.size(), 64);
      QCOMPARE(xline.at(i).samples.at(0), amp(i, 3, 0));
    }

    // 网外的线拒绝并报错
    QVERIFY(!reader.readInline(9, &line, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(!reader.readCrossline(105, &xline, &err));
  }

  // 角点定位（72/76 偏移约定）：四角取自 Source X/Y 构成的整齐网格的
  // 四个极端道，slot 顺序 (inlMin,xlMin)(inlMin,xlMax)(inlMax,xlMax)(inlMax,xlMin)。
  void cornerLocationFromSourceCoords()
  {
    SegyReader reader;
    QString err;
    QVERIFY2(reader.open(fixturePath(), &err), qPrintable(err));

    const SegyGeometry g = reader.geometry();
    // 网格：x = 1000 + j*25（j=0..4），y = 5000 + i*25（i=0..3）
    QCOMPARE(g.cornerX[0], 1000.0); QCOMPARE(g.cornerY[0], 5000.0); // (10,100)
    QCOMPARE(g.cornerX[1], 1100.0); QCOMPARE(g.cornerY[1], 5000.0); // (10,104)
    QCOMPARE(g.cornerX[2], 1100.0); QCOMPARE(g.cornerY[2], 5075.0); // (13,104)
    QCOMPARE(g.cornerX[3], 1000.0); QCOMPARE(g.cornerY[3], 5075.0); // (13,100)
  }

  // 范围门拒绝超网道：把第 2 条线里一道（偏移 72）的 X 打乱成小于线内
  // 前一道——道落到了测网结构之外，角点一致性门必须拒开整份文件并点名。
  void rangeGateRejectsOutOfNetworkTrace()
  {
    QFile src(fixturePath());
    QVERIFY(src.open(QIODevice::ReadOnly));
    QByteArray bytes = src.readAll();
    src.close();

    // 第 12 道（0 基 index 11；第二线 j=1）道头偏移 = 3600 + 11*496。
    // 该道 X=1025；写成 800（< 线内前一道 1000，破坏线内 x 单调）。
    const qint64 traceOffset = 3600 + 11LL * (240 + 64 * 4);
    uchar *xWord = reinterpret_cast<uchar *>(bytes.data()) + traceOffset + 72;
    qToBigEndian<qint32>(800, xWord);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString badPath = dir.filePath(QStringLiteral("out_of_network.sgy"));
    QFile dst(badPath);
    QVERIFY(dst.open(QIODevice::WriteOnly));
    QVERIFY(dst.write(bytes) == bytes.size());
    dst.close();

    SegyReader reader;
    QString err;
    QVERIFY(!reader.open(badPath, &err));
    QVERIFY2(err.contains(QStringLiteral("corner coordinate mismatch")),
             qPrintable(err));
  }

  // #83：同一 reader 先被取消、再成功重开——open() 必须完整复位，成功扫描
  // 的 snapshot 不得沿用上次取消留下的 complete=false。
  void reopenAfterCancelResetsScanState()
  {
    SegyReader reader;
    QString err;
    SegyOptions cancelNow;
    cancelNow.cancel = [] { return true; };
    QVERIFY(!reader.open(fixturePath(), &err, &cancelNow));
    QCOMPARE(err, QStringLiteral("cancelled"));

    err.clear();
    QVERIFY2(reader.open(fixturePath(), &err), qPrintable(err));
    QCOMPARE(reader.traceCount(), 20);
    SegyIndexStore::StoredIndex snap;
    QVERIFY(reader.snapshot(&snap));
    QVERIFY2(snap.complete, "successful re-open must snapshot as complete");
    QVERIFY(snap.badTraceOffsets.isEmpty());
  }

  // #83：首道 field record（inline 起点）取 qint32 极值——inline 编号
  // base + 道号/N 会越过 qint32（有符号溢出 UB）。必须如实拒开而非溢出。
  void extremeInlineBaseRejectedWithoutOverflow()
  {
    QFile src(fixturePath());
    QVERIFY(src.open(QIODevice::ReadOnly));
    QByteArray bytes = src.readAll();
    src.close();
    uchar *fieldRec = reinterpret_cast<uchar *>(bytes.data()) + 3600 + 8;
    qToBigEndian<qint32>(std::numeric_limits<qint32>::max() - 1, fieldRec);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString badPath = dir.filePath(QStringLiteral("extreme_inline.sgy"));
    QFile dst(badPath);
    QVERIFY(dst.open(QIODevice::WriteOnly));
    QVERIFY(dst.write(bytes) == bytes.size());
    dst.close();

    SegyReader reader;
    QString err;
    QVERIFY(!reader.open(badPath, &err));
    QVERIFY2(err.contains(QStringLiteral("overflows")), qPrintable(err));
  }

  // CDP 顺序门：第二条线某道的 CDP 不再重复首线序列 → 拒开并点名。
  void cdpOrderGateRejectsBrokenLine()
  {
    QFile src(fixturePath());
    QVERIFY(src.open(QIODevice::ReadOnly));
    QByteArray bytes = src.readAll();
    src.close();

    // 第 12 道（第二线 j=1）CDP 应为 101；改成 999。
    const qint64 traceOffset = 3600 + 11LL * (240 + 64 * 4);
    uchar *cdpWord = reinterpret_cast<uchar *>(bytes.data()) + traceOffset + 20;
    qToBigEndian<qint32>(999, cdpWord);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString badPath = dir.filePath(QStringLiteral("broken_cdp.sgy"));
    QFile dst(badPath);
    QVERIFY(dst.open(QIODevice::WriteOnly));
    QVERIFY(dst.write(bytes) == bytes.size());
    dst.close();

    SegyReader reader;
    QString err;
    QVERIFY(!reader.open(badPath, &err));
    QVERIFY2(err.contains(QStringLiteral("CDP order mismatch")), qPrintable(err));
  }
};

QTEST_MAIN(TestSegyFixture)
#include "tst_segy_fixture.moc"
