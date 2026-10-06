// 层：测试壳
#include <QtTest>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <cstring>
#include <limits>

#include "io/dlisparser.h"
#include "io/lisparser.h"

#include "welllogfixturewriters.h"

using namespace lisfix;
#include "io/welllogread.h"

// 方向 44 Oracle 1/4：夹具生成源即证据——本文件内联 LIS79 写入器（PR/LR/
// DFSR/信息记录/数据帧，TIF 磁带标头链按 dlisio 公开夹具核实的小端布局），
// 断言 LisParser 读回的井名/全部曲线/全帧数据/单位/深度基准与生成源一致。


class TestLisParser : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void readsFullCatalogFramesUnitsRaw();
    void readsFullCatalogFramesUnitsTif();
    void sniffDistinguishesFormats();
    void multiPrStitchingWithTrailersAndPads();
    void absentValueBecomesNaN();
    void mode1DownStepsBySpacing();
    void mode1UpDecreases();
    void truncatedFileFailsWithReason();
    void halfFrameFails();
    void secondLogsetWhitelisted();
    void stringReprcLogsetWhitelisted();
    void tvdProcessIndicatorSetsBasis();
    void headerScanMatchesFullParse();
    void emptyFileFails();

  private:
    QTemporaryDir m_tempDir;
    QString write(const QString &name, const QByteArray &bytes);
};

void TestLisParser::initTestCase()
{
  QVERIFY(m_tempDir.isValid());
}

QString TestLisParser::write(const QString &name, const QByteArray &bytes)
{
  const QString path = m_tempDir.filePath(name);
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return QString();
  f.write(bytes);
  return path;
}

void TestLisParser::readsFullCatalogFramesUnitsRaw()
{
  const QString path = write(QStringLiteral("main.lis"), buildMain().raw);
  QVERIFY(!path.isEmpty());
  QVERIFY(LisParser::sniff(path));
  QCOMPARE(WellLogRead::detect(path), WellLogFormat::Lis);

  LasHeaderInfo header;
  QList<LasCurve> curves;
  QList<LasIssue> issues;
  QString error;
  QVERIFY2(LisParser::parse(path, header, curves, &error, &issues),
           qPrintable(error));
  QCOMPARE(header.wellName, QStringLiteral("HZ28-6-2"));
  QCOMPARE(header.curveNames,
           QStringList({ QStringLiteral("DEPT"), QStringLiteral("GR") }));
  QCOMPARE(header.indexBasis, QStringLiteral("MD"));
  QVERIFY(header.sawAscii);
  QCOMPARE(curves.at(0).unit, QStringLiteral("M"));
  QCOMPARE(curves.at(1).unit, QStringLiteral("GAPI"));
  QCOMPARE(curves.at(0).values, QVector<double>({ 1500.0, 1500.25, 1500.5 }));
  QCOMPARE(curves.at(1).values.at(0), 90.0);
  QCOMPARE(curves.at(1).values.at(1), -35.0);
  QVERIFY(std::isnan(curves.at(1).values.at(2))); // -999 → 缺席 → NaN
  QVERIFY(issues.isEmpty());
}

void TestLisParser::readsFullCatalogFramesUnitsTif()
{
  const QString path = write(QStringLiteral("main-tif.lis"), buildMain().tif);
  QVERIFY(LisParser::sniff(path));
  QCOMPARE(WellLogRead::detect(path), WellLogFormat::Lis);

  LasHeaderInfo header;
  QList<LasCurve> curves;
  QString error;
  QVERIFY2(LisParser::parse(path, header, curves, &error), qPrintable(error));
  QCOMPARE(header.wellName, QStringLiteral("HZ28-6-2"));
  QCOMPARE(curves.at(0).values, QVector<double>({ 1500.0, 1500.25, 1500.5 }));
  QCOMPARE(curves.at(1).values.at(0), 90.0);
}

void TestLisParser::sniffDistinguishesFormats()
{
  QVERIFY(!LisParser::sniff(write(QStringLiteral("garbage.lis"),
                                  QByteArray("hello world, definitely not LIS!!"))));
  QVERIFY(!DlisParser::sniff(
      write(QStringLiteral("garbage2.lis"), QByteArray(64, '\x01'))));
  // DLIS 主夹具不该判成 LIS，反之亦然
  QVERIFY(!LisParser::sniff(m_tempDir.filePath(QStringLiteral("nonexist.lis"))));
}

void TestLisParser::multiPrStitchingWithTrailersAndPads()
{
  // LR 拆 3 个 PR（带校验和/文件号尾），PR 间加 0x20 杂数
  MainFixture fx = buildMain();
  QByteArray frames;
  const double depths[3] = { 2200.0, 2200.25, 2200.5 };
  const qint16 grs[3] = { 11, 22, 33 };
  for (int i = 0; i < 3; ++i)
  {
    frames += f32Bytes(depths[i]);
    frames += u16be(quint16(grs[i]));
  }
  QByteArray b;
  b += physicalRecord(128, fileHeaderPayload(QStringLiteral("SPLIT")));
  b += physicalRecord(34, wellsitePayload(QStringLiteral("W-SPLIT")));
  LisDfsr d;
  LisSpec dept;
  dept.mnemonic = QStringLiteral("DEPT");
  dept.reprc = 68;
  dept.units = QStringLiteral("M");
  LisSpec gr;
  gr.mnemonic = QStringLiteral("GR");
  gr.reprc = 79;
  gr.units = QStringLiteral("GAPI");
  d.specs = { dept, gr };
  b += physicalRecord(64, dfsrPayload(d));
  b += splitPhysicalRecord(0, frames, 3); // 6 字节帧 ×3 拆 3 段
  b += QByteArray(4, '\x20');             // 杂数
  const QString path = write(QStringLiteral("split.lis"), b);

  LasHeaderInfo header;
  QList<LasCurve> curves;
  QString error;
  QList<LasIssue> issues;
  QVERIFY2(LisParser::parse(path, header, curves, &error, &issues),
           qPrintable(error));
  QCOMPARE(header.wellName, QStringLiteral("W-SPLIT"));
  QCOMPARE(curves.at(0).values, QVector<double>({ 2200.0, 2200.25, 2200.5 }));
  QCOMPARE(curves.at(1).values, QVector<double>({ 11.0, 22.0, 33.0 }));
}

void TestLisParser::absentValueBecomesNaN()
{
  // 主夹具第三帧 GR = -999（条目 12 缺席值）已在 readsFullCatalogFramesUnitsRaw
  // 断言 NaN——本用例钉「缺席≠普通值」：-999 不出现在值里
  const QString path = write(QStringLiteral("abs.lis"), buildMain().raw);
  LasHeaderInfo header;
  QList<LasCurve> curves;
  QString error;
  QVERIFY(LisParser::parse(path, header, curves, &error));
  for (double v : curves.at(1).values)
    QVERIFY(v != -999.0);
}

void TestLisParser::mode1DownStepsBySpacing()
{
  LisDfsr d;
  d.depthMode = 1;
  d.depthReprc = 68;
  d.depthUnits = QStringLiteral("M");
  d.spacing = 1.0;
  d.direction = 255; // DOWN：递增
  LisSpec gr;
  gr.mnemonic = QStringLiteral("GR");
  gr.reprc = 79;
  gr.units = QStringLiteral("GAPI");
  d.specs = { gr }; // 模式 1：无索引 spec
  QByteArray b;
  b += physicalRecord(128, fileHeaderPayload(QStringLiteral("M1D")));
  b += physicalRecord(34, wellsitePayload(QStringLiteral("W-M1D")));
  b += physicalRecord(64, dfsrPayload(d));
  QByteArray rec1;
  rec1 += f32Bytes(3000.0);
  rec1 += u16be(101);
  rec1 += u16be(102);
  b += physicalRecord(0, rec1);
  QByteArray rec2;
  rec2 += f32Bytes(3002.0);
  rec2 += u16be(103);
  b += physicalRecord(0, rec2);
  const QString path = write(QStringLiteral("m1d.lis"), b);

  LasHeaderInfo header;
  QList<LasCurve> curves;
  QString error;
  QVERIFY2(LisParser::parse(path, header, curves, &error), qPrintable(error));
  QCOMPARE(header.curveNames,
           QStringList({ QStringLiteral("DEPT"), QStringLiteral("GR") }));
  QCOMPARE(curves.at(0).values,
           QVector<double>({ 3000.0, 3001.0, 3002.0 }));
  QCOMPARE(curves.at(1).values,
           QVector<double>({ 101.0, 102.0, 103.0 }));
  QCOMPARE(curves.at(0).unit, QStringLiteral("M"));
}

void TestLisParser::mode1UpDecreases()
{
  LisDfsr d;
  d.depthMode = 1;
  d.depthReprc = 68;
  d.spacing = 0.5;
  d.direction = 1; // UP：递减
  LisSpec gr;
  gr.mnemonic = QStringLiteral("GR");
  gr.reprc = 79;
  d.specs = { gr };
  QByteArray b;
  b += physicalRecord(128, fileHeaderPayload(QStringLiteral("M1U")));
  b += physicalRecord(34, wellsitePayload(QStringLiteral("W-M1U")));
  b += physicalRecord(64, dfsrPayload(d));
  QByteArray rec;
  rec += f32Bytes(4000.0);
  rec += u16be(7);
  rec += u16be(8);
  rec += u16be(9);
  b += physicalRecord(0, rec);
  const QString path = write(QStringLiteral("m1u.lis"), b);

  LasHeaderInfo header;
  QList<LasCurve> curves;
  QString error;
  QVERIFY2(LisParser::parse(path, header, curves, &error), qPrintable(error));
  QCOMPARE(curves.at(0).values,
           QVector<double>({ 4000.0, 3999.5, 3999.0 }));
}

void TestLisParser::truncatedFileFailsWithReason()
{
  const QByteArray whole = buildMain().raw;
  const QString path = write(QStringLiteral("trunc.lis"), whole.left(whole.size() - 5));
  LasHeaderInfo header;
  QList<LasCurve> curves;
  QList<LasIssue> issues;
  QString error;
  QVERIFY(!LisParser::parse(path, header, curves, &error, &issues));
  QVERIFY2(!error.isEmpty(), "截断必须给因");
  bool saw = false;
  for (const LasIssue &i : issues)
    if (i.category == LasIssue::Category::Truncated)
      saw = true;
  QVERIFY2(saw, "截断必须落 Truncated 类目 issue");
}

void TestLisParser::halfFrameFails()
{
  // 数据记录尾带 3 字节 < 帧宽 6——半帧边界专项
  MainFixture fx = buildMain();
  QByteArray frames;
  frames += f32Bytes(1500.0);
  frames += u16be(90);
  frames += QByteArray(3, '\x11'); // 半帧
  QByteArray b;
  b += physicalRecord(128, fileHeaderPayload(QStringLiteral("HALF")));
  b += physicalRecord(34, wellsitePayload(QStringLiteral("W-HALF")));
  LisDfsr d;
  LisSpec dept;
  dept.mnemonic = QStringLiteral("DEPT");
  dept.reprc = 68;
  dept.units = QStringLiteral("M");
  LisSpec gr;
  gr.mnemonic = QStringLiteral("GR");
  gr.reprc = 79;
  d.specs = { dept, gr };
  b += physicalRecord(64, dfsrPayload(d));
  b += physicalRecord(0, frames);
  const QString path = write(QStringLiteral("half.lis"), b);

  LasHeaderInfo header;
  QList<LasCurve> curves;
  QList<LasIssue> issues;
  QString error;
  QVERIFY(!LisParser::parse(path, header, curves, &error, &issues));
  QVERIFY2(error.contains(QStringLiteral("半帧")), qPrintable(error));
}

void TestLisParser::secondLogsetWhitelisted()
{
  LisDfsr d1;
  LisSpec dept;
  dept.mnemonic = QStringLiteral("DEPT");
  dept.reprc = 68;
  dept.units = QStringLiteral("M");
  LisSpec gr;
  gr.mnemonic = QStringLiteral("GR");
  gr.reprc = 79;
  d1.specs = { dept, gr };
  LisDfsr d2;
  LisSpec sonic;
  sonic.mnemonic = QStringLiteral("DT");
  sonic.reprc = 68;
  sonic.units = QStringLiteral("US/F");
  d2.specs = { sonic };
  QByteArray b;
  b += physicalRecord(128, fileHeaderPayload(QStringLiteral("2LS")));
  b += physicalRecord(34, wellsitePayload(QStringLiteral("W-2LS")));
  b += physicalRecord(64, dfsrPayload(d1));
  QByteArray f1;
  f1 += f32Bytes(100.0);
  f1 += u16be(1);
  b += physicalRecord(0, f1);
  b += physicalRecord(64, dfsrPayload(d2)); // 第二 logset
  QByteArray f2;
  f2 += f32Bytes(200.0);
  b += physicalRecord(0, f2);
  const QString path = write(QStringLiteral("twols.lis"), b);

  LasHeaderInfo header;
  QList<LasCurve> curves;
  QList<LasIssue> issues;
  QString error;
  QVERIFY2(LisParser::parse(path, header, curves, &error, &issues),
           qPrintable(error));
  QCOMPARE(header.curveNames,
           QStringList({ QStringLiteral("DEPT"), QStringLiteral("GR") }));
  QCOMPARE(curves.at(0).values, QVector<double>{ 100.0 });
  bool saw = false;
  for (const LasIssue &i : issues)
    if (i.message.contains(QStringLiteral("logset 2")))
      saw = true;
  QVERIFY2(saw, "第二 logset 白名单必须记因");
}

void TestLisParser::stringReprcLogsetWhitelisted()
{
  LisDfsr d;
  LisSpec dept;
  dept.mnemonic = QStringLiteral("DEPT");
  dept.reprc = 68;
  dept.units = QStringLiteral("M");
  LisSpec txt;
  txt.mnemonic = QStringLiteral("MSG");
  txt.reprc = 65; // 字符串道
  txt.reserved = 4;
  d.specs = { dept, txt };
  QByteArray b;
  b += physicalRecord(128, fileHeaderPayload(QStringLiteral("STR")));
  b += physicalRecord(34, wellsitePayload(QStringLiteral("W-STR")));
  b += physicalRecord(64, dfsrPayload(d));
  QByteArray f;
  f += f32Bytes(50.0);
  f += field(QStringLiteral("ABCD"), 4);
  b += physicalRecord(0, f);
  const QString path = write(QStringLiteral("str.lis"), b);

  LasHeaderInfo header;
  QList<LasCurve> curves;
  QList<LasIssue> issues;
  QString error;
  QVERIFY(!LisParser::parse(path, header, curves, &error, &issues));
  QVERIFY2(error.contains(QStringLiteral("白名单")), qPrintable(error));
}

void TestLisParser::tvdProcessIndicatorSetsBasis()
{
  LisDfsr d;
  LisSpec dept;
  dept.mnemonic = QStringLiteral("DEPT");
  dept.reprc = 68;
  dept.units = QStringLiteral("M");
  dept.tvd = true; // subtype-1 TVD 位（fixture 写 subtype 0 尾区首位——
                   // 解析器按掩码首位读，与 dlisio 布局一致）
  d.specs = { dept };
  QByteArray b;
  b += physicalRecord(128, fileHeaderPayload(QStringLiteral("TVD")));
  b += physicalRecord(34, wellsitePayload(QStringLiteral("W-TVD")));
  b += physicalRecord(64, dfsrPayload(d));
  QByteArray f;
  f += f32Bytes(77.0);
  b += physicalRecord(0, f);
  const QString path = write(QStringLiteral("tvd.lis"), b);

  LasHeaderInfo header;
  QList<LasCurve> curves;
  QString error;
  QVERIFY2(LisParser::parse(path, header, curves, &error), qPrintable(error));
  QCOMPARE(header.indexBasis, QStringLiteral("TVD"));
}

void TestLisParser::headerScanMatchesFullParse()
{
  const QString path = write(QStringLiteral("hdr.lis"), buildMain().raw);
  LasHeaderInfo full, hdrOnly;
  QList<LasCurve> curves;
  QString e1, e2;
  QVERIFY(LisParser::parse(path, full, curves, &e1));
  QVERIFY(LisParser::parseHeader(path, hdrOnly, &e2));
  QCOMPARE(hdrOnly.curveNames, full.curveNames);
  QCOMPARE(hdrOnly.wellName, full.wellName);
  QCOMPARE(hdrOnly.indexBasis, full.indexBasis);
}

void TestLisParser::emptyFileFails()
{
  const QString path = write(QStringLiteral("empty.lis"), QByteArray());
  QVERIFY(!path.isEmpty());
  LasHeaderInfo header;
  QList<LasCurve> curves;
  QString error;
  QVERIFY(!LisParser::parse(path, header, curves, &error));
  QVERIFY(!LisParser::sniff(path));
}

QTEST_MAIN(TestLisParser)
#include "tst_lisparser.moc"
