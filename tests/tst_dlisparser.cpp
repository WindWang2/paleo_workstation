// 层：测试壳
#include <QtTest>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <cstring>
#include <limits>

#include "io/dlisparser.h"
#include "io/welllogread.h"

#include "welllogfixturewriters.h"

using namespace dlisfix;

// 方向 44 Oracle 1/4：夹具生成源即证据——本文件内联 RP66 v1 写入器，
// 逐字节生成 DLIS 夹具，断言 DlisParser 读回的井名/全部曲线/全帧数据/
// 单位/深度基准与生成源一致；截断/半帧/白名单子结构各有专项夹具。


class TestDlisParser : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void readsFullCatalogFramesUnitsAndBasis();
    void headerScanMatchesFullParse();
    void truncatedFileReportsTruncatedIssue();
    void halfFrameReportsError();
    void overlappingGarbageTailWarnsButDecodes();
    void nonIncreasingFrameNumberWarns();
    void multiFrameTypeWhitelisted();
    void arrayChannelWhitelistedAndSlotsSkipped();
    void encryptedLogicalRecordSkipped();
    void noFormatRecordSkipped();
    void secondLogicalFileStopsWithWarning();
    void multipleLrsPackedInOneVisibleRecord();
    void channelEflrSpanningVisibleRecords();
    void packedSegmentTrailerPadChecksumDecoded();
    void orphanPredecessorSegmentFails();
    void danglingSuccessorAtEofFails();
    void emptyFileFails();

  private:
    QTemporaryDir m_tempDir;
    QString write(const QString &name, const QByteArray &bytes);
    QByteArray mainFixture();
};

void TestDlisParser::initTestCase()
{
  QVERIFY(m_tempDir.isValid());
}

QString TestDlisParser::write(const QString &name, const QByteArray &bytes)
{
  const QString path = m_tempDir.filePath(name);
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return QString();
  f.write(bytes);
  return path;
}

QByteArray TestDlisParser::mainFixture()
{
  QVector<ChannelDef> channels = {
    { QStringLiteral("DEPT"), QStringLiteral("m"), 2 /*FSINGL*/, {}, QStringLiteral("LN-DEPT") },
    { QStringLiteral("GR"), QStringLiteral("GAPI"), 7 /*FDOUBL*/, {}, QString() },
    { QStringLiteral("RHOB"), QStringLiteral("g/cm3"), 5 /*ISINGL*/, {}, QString() },
    { QStringLiteral("NPHI"), QStringLiteral("v/v"), 1 /*FSHORT*/, {}, QString() },
    { QStringLiteral("VAXCH"), QStringLiteral("VAX"), 6 /*VSINGL*/, {}, QString() },
  };
  QVector<FrameDef> frames = { { QStringLiteral("1000"),
                                 { QStringLiteral("DEPT"), QStringLiteral("GR"),
                                   QStringLiteral("RHOB"), QStringLiteral("NPHI"),
                                   QStringLiteral("VAXCH") },
                                 QStringLiteral("BOREHOLE-DEPTH"),
                                 QStringLiteral("INCREASING") } };
  QByteArray b = sul();
  b += buildFileHeaderEflr(QStringLiteral("SMOKE"));
  b += buildOriginEflr(QStringLiteral("HZ28-6-1"));
  b += buildLongNameEflr(QStringLiteral("LN-DEPT"), QStringLiteral("Measured Depth"));
  b += buildChannelEflr(channels);
  b += buildFrameEflr(frames);
  // 三帧：DEPT/GR/RHOB/NPHI/VAXCH —— 各表示码编码与解析互逆断言
  for (int i = 0; i < 3; ++i)
  {
    QByteArray slotBytes;
    slotBytes += fsinglBytes(float(1000.0 + 0.5 * i));
    slotBytes += fdoublBytes(i == 1 ? -12.25 : 87.5 + i);
    slotBytes += ibmBytes(i == 2 ? 2400.0 : 2.5);
    slotBytes += fshortBytes(i == 1 ? -0.75 : 0.75);
    slotBytes += vaxBytes(0.375 * (i + 1));
    b += buildFdata(QStringLiteral("1000"), quint64(i + 1), slotBytes);
  }
  return b;
}

void TestDlisParser::readsFullCatalogFramesUnitsAndBasis()
{
  const QString path = write(QStringLiteral("main.dlis"), mainFixture());
  QVERIFY(!path.isEmpty());

  QVERIFY(DlisParser::sniff(path));
  QCOMPARE(WellLogRead::detect(path), WellLogFormat::Dlis);

  LasHeaderInfo header;
  QList<LasCurve> curves;
  QList<LasIssue> issues;
  QString error;
  QVERIFY2(DlisParser::parse(path, header, curves, &error, &issues),
           qPrintable(error));
  QCOMPARE(header.wellName, QStringLiteral("HZ28-6-1"));
  QCOMPARE(header.indexBasis, QStringLiteral("MD"));
  QVERIFY(header.sawAscii);
  QCOMPARE(header.curveNames,
           QStringList({ QStringLiteral("DEPT"), QStringLiteral("GR"),
                         QStringLiteral("RHOB"), QStringLiteral("NPHI"),
                         QStringLiteral("VAXCH") }));
  QCOMPARE(curves.size(), 5);
  QCOMPARE(curves.at(0).unit, QStringLiteral("m"));
  QCOMPARE(curves.at(1).unit, QStringLiteral("GAPI"));
  QCOMPARE(curves.at(2).unit, QStringLiteral("g/cm3"));
  QCOMPARE(curves.at(3).unit, QStringLiteral("v/v"));
  QCOMPARE(curves.at(4).unit, QStringLiteral("VAX"));
  // descr：LONG-NAME 引用解析到描述文本
  QCOMPARE(curves.at(0).descr, QStringLiteral("Measured Depth"));

  QCOMPARE(curves.at(0).values.size(), 3);
  for (int i = 0; i < 3; ++i)
  {
    QCOMPARE(curves.at(0).values.at(i), double(1000.0 + 0.5 * i));
    QCOMPARE(curves.at(1).values.at(i), i == 1 ? -12.25 : 87.5 + i);
    QCOMPARE(curves.at(2).values.at(i), i == 2 ? 2400.0 : 2.5);
    QCOMPARE(curves.at(3).values.at(i), i == 1 ? -0.75 : 0.75);
    QCOMPARE(curves.at(4).values.at(i), 0.375 * (i + 1));
  }
  QVERIFY(issues.isEmpty()); // 主夹具无告警——干净读全
}

void TestDlisParser::headerScanMatchesFullParse()
{
  const QString path = write(QStringLiteral("hdr.dlis"), mainFixture());
  LasHeaderInfo full, hdrOnly;
  QString e1, e2;
  QList<LasCurve> curves;
  QVERIFY(DlisParser::parse(path, full, curves, &e1));
  QVERIFY(DlisParser::parseHeader(path, hdrOnly, &e2));
  QCOMPARE(hdrOnly.curveNames, full.curveNames);
  QCOMPARE(hdrOnly.wellName, full.wellName);
  QCOMPARE(hdrOnly.indexBasis, full.indexBasis);
}

void TestDlisParser::truncatedFileReportsTruncatedIssue()
{
  const QByteArray whole = mainFixture();
  // 掐在最后一个 FDATA 中段：段体不完整
  QByteArray cut = whole.left(whole.size() - 8);
  const QString path = write(QStringLiteral("trunc.dlis"), cut);
  LasHeaderInfo header;
  QList<LasCurve> curves;
  QList<LasIssue> issues;
  QString error;
  QVERIFY(!DlisParser::parse(path, header, curves, &error, &issues));
  QVERIFY2(!error.isEmpty(), "截断必须给因");
  bool sawTruncated = false;
  for (const LasIssue &i : issues)
    if (i.category == LasIssue::Category::Truncated)
      sawTruncated = true;
  QVERIFY2(sawTruncated, "截断必须落 Truncated 类目 issue");
}

void TestDlisParser::halfFrameReportsError()
{
  // 帧声明 5 通道，但最后一帧槽位只有一半——畸形帧边界专项
  QVector<ChannelDef> channels = {
    { QStringLiteral("DEPT"), QStringLiteral("m"), 2, {}, QString() },
    { QStringLiteral("GR"), QStringLiteral("GAPI"), 2, {}, QString() },
  };
  QVector<FrameDef> frames = { { QStringLiteral("2000"),
                                 { QStringLiteral("DEPT"), QStringLiteral("GR") },
                                 QString(), QString() } };
  QByteArray b = sul();
  b += buildFileHeaderEflr(QStringLiteral("HALF"));
  b += buildOriginEflr(QStringLiteral("W-HALF"));
  b += buildChannelEflr(channels);
  b += buildFrameEflr(frames);
  QByteArray good;
  good += fsinglBytes(1500.0f);
  good += fsinglBytes(90.0f);
  b += buildFdata(QStringLiteral("2000"), 1, good);
  b += buildFdata(QStringLiteral("2000"), 2, fsinglBytes(1500.5f)); // 半帧
  const QString path = write(QStringLiteral("half.dlis"), b);

  LasHeaderInfo header;
  QList<LasCurve> curves;
  QList<LasIssue> issues;
  QString error;
  QVERIFY(!DlisParser::parse(path, header, curves, &error, &issues));
  QVERIFY2(error.contains(QStringLiteral("截断")), qPrintable(error));
}

void TestDlisParser::overlappingGarbageTailWarnsButDecodes()
{
  // 帧尾多出残余字节（重叠帧边界）：全部通道按布局解出 + Warning 如实记
  QVector<ChannelDef> channels = {
    { QStringLiteral("DEPT"), QStringLiteral("m"), 2, {}, QString() },
    { QStringLiteral("GR"), QStringLiteral("GAPI"), 2, {}, QString() },
  };
  QVector<FrameDef> frames = { { QStringLiteral("3000"),
                                 { QStringLiteral("DEPT"), QStringLiteral("GR") },
                                 QString(), QString() } };
  QByteArray slotBytes;
  slotBytes += fsinglBytes(2000.0f);
  slotBytes += fsinglBytes(55.0f);
  slotBytes += QByteArray(3, '\x5A'); // 残余
  QByteArray b = sul();
  b += buildFileHeaderEflr(QStringLiteral("JUNK"));
  b += buildOriginEflr(QStringLiteral("W-JUNK"));
  b += buildChannelEflr(channels);
  b += buildFrameEflr(frames);
  b += buildFdata(QStringLiteral("3000"), 1, slotBytes);
  const QString path = write(QStringLiteral("junk.dlis"), b);

  LasHeaderInfo header;
  QList<LasCurve> curves;
  QList<LasIssue> issues;
  QString error;
  QVERIFY2(DlisParser::parse(path, header, curves, &error, &issues),
           qPrintable(error));
  QCOMPARE(curves.at(0).values, QVector<double>{ 2000.0 });
  QCOMPARE(curves.at(1).values, QVector<double>{ 55.0 });
  bool sawLeftover = false;
  for (const LasIssue &i : issues)
    if (i.severity == LasIssue::Severity::Warning &&
        i.message.contains(QStringLiteral("残余")))
      sawLeftover = true;
  QVERIFY2(sawLeftover, "残余字节必须有 Warning");
}

void TestDlisParser::nonIncreasingFrameNumberWarns()
{
  QVector<ChannelDef> channels = {
    { QStringLiteral("DEPT"), QStringLiteral("m"), 2, {}, QString() },
  };
  QVector<FrameDef> frames = { { QStringLiteral("4000"),
                                 { QStringLiteral("DEPT") },
                                 QString(), QString() } };
  QByteArray b = sul();
  b += buildFileHeaderEflr(QStringLiteral("FRNO"));
  b += buildOriginEflr(QStringLiteral("W-FRNO"));
  b += buildChannelEflr(channels);
  b += buildFrameEflr(frames);
  b += buildFdata(QStringLiteral("4000"), 5, fsinglBytes(10.0f));
  b += buildFdata(QStringLiteral("4000"), 5, fsinglBytes(10.5f)); // 帧号重复
  const QString path = write(QStringLiteral("frno.dlis"), b);

  LasHeaderInfo header;
  QList<LasCurve> curves;
  QList<LasIssue> issues;
  QString error;
  QVERIFY2(DlisParser::parse(path, header, curves, &error, &issues),
           qPrintable(error));
  QCOMPARE(curves.at(0).values, QVector<double>({ 10.0, 10.5 }));
  bool saw = false;
  for (const LasIssue &i : issues)
    if (i.message.contains(QStringLiteral("帧号")))
      saw = true;
  QVERIFY2(saw, "帧号未递增必须告警");
}

void TestDlisParser::multiFrameTypeWhitelisted()
{
  QVector<ChannelDef> channels = {
    { QStringLiteral("DEPT"), QStringLiteral("m"), 2, {}, QString() },
    { QStringLiteral("TDEP"), QStringLiteral("s"), 2, {}, QString() },
    { QStringLiteral("SONIC"), QStringLiteral("us/ft"), 2, {}, QString() },
  };
  QVector<FrameDef> frames = {
    { QStringLiteral("5000"), { QStringLiteral("DEPT") }, QString(), QString() },
    { QStringLiteral("5001"), { QStringLiteral("TDEP"), QStringLiteral("SONIC") },
      QString(), QString() },
  };
  QByteArray b = sul();
  b += buildFileHeaderEflr(QStringLiteral("MULTI"));
  b += buildOriginEflr(QStringLiteral("W-MULTI"));
  b += buildChannelEflr(channels);
  b += buildFrameEflr(frames);
  b += buildFdata(QStringLiteral("5000"), 1, fsinglBytes(3000.0f));
  QByteArray tSlots;
  tSlots += fsinglBytes(1.0f);
  tSlots += fsinglBytes(80.0f);
  b += buildFdata(QStringLiteral("5001"), 1, tSlots); // 第二帧类型：白名单
  b += buildFdata(QStringLiteral("5000"), 2, fsinglBytes(3000.5f));
  const QString path = write(QStringLiteral("multi.dlis"), b);

  LasHeaderInfo header;
  QList<LasCurve> curves;
  QList<LasIssue> issues;
  QString error;
  QVERIFY2(DlisParser::parse(path, header, curves, &error, &issues),
           qPrintable(error));
  // 主帧 = 首个有 FDATA 的帧类型；第二帧类型的通道不进表（白名单口径）
  QCOMPARE(header.curveNames, QStringList({ QStringLiteral("DEPT") }));
  QCOMPARE(curves.at(0).values, QVector<double>({ 3000.0, 3000.5 }));
  // 白名单证据：目录不含 TDEP/SONIC 且无静默（issue 无第二帧类型告警是
  // 契约行为——目录冻结在首帧；见 ledger 白名单 4）
}

void TestDlisParser::arrayChannelWhitelistedAndSlotsSkipped()
{
  QVector<ChannelDef> channels = {
    { QStringLiteral("DEPT"), QStringLiteral("m"), 2, {}, QString() },
    { QStringLiteral("PAD"), QStringLiteral("cnt"), 2, { 2 }, QString() }, // 2 元素数组
    { QStringLiteral("GR"), QStringLiteral("GAPI"), 2, {}, QString() },
  };
  QVector<FrameDef> frames = { { QStringLiteral("6000"),
                                 { QStringLiteral("DEPT"), QStringLiteral("PAD"),
                                   QStringLiteral("GR") },
                                 QString(), QString() } };
  QByteArray b = sul();
  b += buildFileHeaderEflr(QStringLiteral("ARR"));
  b += buildOriginEflr(QStringLiteral("W-ARR"));
  b += buildChannelEflr(channels);
  b += buildFrameEflr(frames);
  QByteArray slotBytes;
  slotBytes += fsinglBytes(4000.0f);
  slotBytes += fsinglBytes(1.0f);
  slotBytes += fsinglBytes(2.0f); // PAD 两元素（跳过）
  slotBytes += fsinglBytes(61.0f); // GR 在数组道之后——槽位跳过不错位
  b += buildFdata(QStringLiteral("6000"), 1, slotBytes);
  const QString path = write(QStringLiteral("array.dlis"), b);

  LasHeaderInfo header;
  QList<LasCurve> curves;
  QList<LasIssue> issues;
  QString error;
  QVERIFY2(DlisParser::parse(path, header, curves, &error, &issues),
           qPrintable(error));
  QCOMPARE(header.curveNames,
           QStringList({ QStringLiteral("DEPT"), QStringLiteral("GR") }));
  QCOMPARE(curves.at(1).values, QVector<double>{ 61.0 });
  bool saw = false;
  for (const LasIssue &i : issues)
    if (i.message.contains(QStringLiteral("PAD")))
      saw = true;
  QVERIFY2(saw, "数组通道白名单必须逐条记因");
}

void TestDlisParser::encryptedLogicalRecordSkipped()
{
  QVector<ChannelDef> channels = {
    { QStringLiteral("DEPT"), QStringLiteral("m"), 2, {}, QString() },
  };
  QVector<FrameDef> frames = { { QStringLiteral("7000"),
                                 { QStringLiteral("DEPT") },
                                 QString(), QString() } };
  QByteArray b = sul();
  b += buildFileHeaderEflr(QStringLiteral("ENC"));
  b += buildOriginEflr(QStringLiteral("W-ENC"));
  b += buildChannelEflr(channels);
  b += buildFrameEflr(frames);
  b += buildFdata(QStringLiteral("7000"), 1, fsinglBytes(5000.0f));
  // 加密 EFLR：attrs 带加密位
  QByteArray body = setComponent(QStringLiteral("COMMENT"), QString());
  QByteArray lrs;
  lrs.append(u16be(4 + quint64(body.size())));
  lrs.append(char(0x80 | 0x10));
  lrs.append(char(6));
  lrs.append(body);
  QByteArray vr;
  vr.append(u16be(4 + quint64(lrs.size())));
  vr.append(char(0xFF));
  vr.append(char(0x01));
  vr.append(lrs);
  b += vr;
  const QString path = write(QStringLiteral("enc.dlis"), b);

  LasHeaderInfo header;
  QList<LasCurve> curves;
  QList<LasIssue> issues;
  QString error;
  QVERIFY2(DlisParser::parse(path, header, curves, &error, &issues),
           qPrintable(error));
  QCOMPARE(curves.at(0).values, QVector<double>{ 5000.0 }); // 数据不受影响
  bool saw = false;
  for (const LasIssue &i : issues)
    if (i.message.contains(QStringLiteral("加密")))
      saw = true;
  QVERIFY2(saw, "加密记录跳过必须记白名单告警");
}

void TestDlisParser::noFormatRecordSkipped()
{
  QVector<ChannelDef> channels = {
    { QStringLiteral("DEPT"), QStringLiteral("m"), 2, {}, QString() },
  };
  QVector<FrameDef> frames = { { QStringLiteral("8000"),
                                 { QStringLiteral("DEPT") },
                                 QString(), QString() } };
  QByteArray b = sul();
  b += buildFileHeaderEflr(QStringLiteral("NF"));
  b += buildOriginEflr(QStringLiteral("W-NF"));
  b += buildChannelEflr(channels);
  b += buildFrameEflr(frames);
  b += buildFdata(QStringLiteral("8000"), 1, fsinglBytes(6000.0f));
  b += wrapLogicalRecord(1, false, obnameBytes(0, 0, QStringLiteral("blob")));
  const QString path = write(QStringLiteral("nf.dlis"), b);

  LasHeaderInfo header;
  QList<LasCurve> curves;
  QList<LasIssue> issues;
  QString error;
  QVERIFY2(DlisParser::parse(path, header, curves, &error, &issues),
           qPrintable(error));
  QCOMPARE(curves.at(0).values, QVector<double>{ 6000.0 });
  bool saw = false;
  for (const LasIssue &i : issues)
    if (i.message.contains(QStringLiteral("NO-FORMAT")))
      saw = true;
  QVERIFY2(saw, "NO-FORMAT 白名单必须记因");
}

void TestDlisParser::secondLogicalFileStopsWithWarning()
{
  QVector<ChannelDef> channels = {
    { QStringLiteral("DEPT"), QStringLiteral("m"), 2, {}, QString() },
  };
  QVector<FrameDef> frames = { { QStringLiteral("9000"),
                                 { QStringLiteral("DEPT") },
                                 QString(), QString() } };
  QByteArray b = sul();
  b += buildFileHeaderEflr(QStringLiteral("LF1"));
  b += buildOriginEflr(QStringLiteral("W-LF"));
  b += buildChannelEflr(channels);
  b += buildFrameEflr(frames);
  b += buildFdata(QStringLiteral("9000"), 1, fsinglBytes(7000.0f));
  b += buildFileHeaderEflr(QStringLiteral("LF2")); // 第二逻辑文件
  b += buildFdata(QStringLiteral("9000"), 2, fsinglBytes(7001.0f));
  const QString path = write(QStringLiteral("twolf.dlis"), b);

  LasHeaderInfo header;
  QList<LasCurve> curves;
  QList<LasIssue> issues;
  QString error;
  QVERIFY2(DlisParser::parse(path, header, curves, &error, &issues),
           qPrintable(error));
  QCOMPARE(curves.at(0).values, QVector<double>{ 7000.0 }); // 只读第一个 LF
  bool saw = false;
  for (const LasIssue &i : issues)
    if (i.message.contains(QStringLiteral("第二个文件头")))
      saw = true;
  QVERIFY2(saw, "多逻辑文件白名单必须记因");
}

void TestDlisParser::multipleLrsPackedInOneVisibleRecord()
{
  // 生产写出器把 VR 填满：多个 FDATA LR 打包进一个可见记录——全部帧都要读到
  QVector<ChannelDef> channels = {
    { QStringLiteral("DEPT"), QStringLiteral("m"), 2, {}, QString() },
    { QStringLiteral("GR"), QStringLiteral("GAPI"), 2, {}, QString() },
  };
  QVector<FrameDef> frames = { { QStringLiteral("1000"),
                                 { QStringLiteral("DEPT"), QStringLiteral("GR") },
                                 QString(), QString() } };
  QByteArray b = sul();
  b += buildFileHeaderEflr(QStringLiteral("PACK"));
  b += buildOriginEflr(QStringLiteral("W-PACK"));
  b += buildChannelEflr(channels);
  b += buildFrameEflr(frames);
  QList<QByteArray> segs;
  for (int i = 0; i < 10; ++i)
  {
    QByteArray slotBytes;
    slotBytes += fsinglBytes(float(900.0 + 0.5 * i));
    slotBytes += fsinglBytes(float(40.0 + i));
    segs.append(logicalRecordSegment(0, 0x00,
                                     fdataBody(QStringLiteral("1000"),
                                               quint64(i + 1), slotBytes)));
  }
  b += visibleRecord(segs); // 10 个 FDATA LR 共用一个 VR
  const QString path = write(QStringLiteral("pack.dlis"), b);

  LasHeaderInfo header;
  QList<LasCurve> curves;
  QList<LasIssue> issues;
  QString error;
  QVERIFY2(DlisParser::parse(path, header, curves, &error, &issues),
           qPrintable(error));
  QCOMPARE(curves.at(0).values.size(), 10);
  QCOMPARE(curves.at(1).values.size(), 10);
  for (int i = 0; i < 10; ++i)
  {
    QCOMPARE(curves.at(0).values.at(i), double(900.0 + 0.5 * i));
    QCOMPARE(curves.at(1).values.at(i), double(40.0 + i));
  }
  for (const LasIssue &i : issues) // 打包不得再触发「可见记录残余」式静默丢弃
    QVERIFY2(!i.message.contains(QStringLiteral("可见记录")),
             qPrintable(i.message));
}

void TestDlisParser::channelEflrSpanningVisibleRecords()
{
  // 一个 LR 跨多个 VR 分段（前驱/后继位闭合）：CHANNEL EFLR 拆 3 段跨 3 个 VR
  QVector<ChannelDef> channels = {
    { QStringLiteral("DEPT"), QStringLiteral("m"), 2, {}, QString() },
    { QStringLiteral("GR"), QStringLiteral("GAPI"), 2, {}, QString() },
    { QStringLiteral("RHOB"), QStringLiteral("g/cm3"), 2, {}, QString() },
  };
  QVector<FrameDef> frames = { { QStringLiteral("2000"),
                                 { QStringLiteral("DEPT"), QStringLiteral("GR"),
                                   QStringLiteral("RHOB") },
                                 QString(), QString() } };
  QByteArray b = sul();
  b += buildFileHeaderEflr(QStringLiteral("SPAN"));
  b += buildOriginEflr(QStringLiteral("W-SPAN"));
  const QByteArray body = channelEflrBody(channels);
  QVERIFY(body.size() > 48); // 夹具前提：体足够大，能拆出 3 段
  for (const QByteArray &vr : splitLogicalRecordAcrossVrs(3, true, body, 24))
    b += vr;
  b += buildFrameEflr(frames);
  QByteArray slotBytes;
  slotBytes += fsinglBytes(1500.0f);
  slotBytes += fsinglBytes(77.0f);
  slotBytes += fsinglBytes(2.5f);
  b += buildFdata(QStringLiteral("2000"), 1, slotBytes);
  const QString path = write(QStringLiteral("span.dlis"), b);

  LasHeaderInfo header;
  QList<LasCurve> curves;
  QList<LasIssue> issues;
  QString error;
  QVERIFY2(DlisParser::parse(path, header, curves, &error, &issues),
           qPrintable(error));
  // 跨 VR 的 EFLR 必须完整重组：目录与单 VR 写法一致
  QCOMPARE(header.curveNames,
           QStringList({ QStringLiteral("DEPT"), QStringLiteral("GR"),
                         QStringLiteral("RHOB") }));
  QCOMPARE(curves.size(), 3);
  QCOMPARE(curves.at(0).values, QVector<double>{ 1500.0 });
  QCOMPARE(curves.at(1).values, QVector<double>{ 77.0 });
  QCOMPARE(curves.at(2).values, QVector<double>{ 2.5 });
}

void TestDlisParser::packedSegmentTrailerPadChecksumDecoded()
{
  // 打包 VR 内带 pad/checksum/trailing-length 尾的段：trailer 自尾剥后正常解码
  QVector<ChannelDef> channels = {
    { QStringLiteral("DEPT"), QStringLiteral("m"), 2, {}, QString() },
  };
  QVector<FrameDef> frames = { { QStringLiteral("3000"),
                                 { QStringLiteral("DEPT") },
                                 QString(), QString() } };
  QByteArray b = sul();
  b += buildFileHeaderEflr(QStringLiteral("TRLR"));
  b += buildOriginEflr(QStringLiteral("W-TRLR"));
  b += buildChannelEflr(channels);
  b += buildFrameEflr(frames);
  QList<QByteArray> segs;
  segs.append(logicalRecordSegmentTrailer(
      0, 0x01 | 0x02 | 0x04, // pad + trailing-length + checksum
      fdataBody(QStringLiteral("3000"), 1, fsinglBytes(1234.0f)),
      /*padCount=*/2, /*withTrailLen=*/true, /*withChecksum=*/true));
  segs.append(logicalRecordSegment(
      0, 0x00, fdataBody(QStringLiteral("3000"), 2, fsinglBytes(1234.5f))));
  b += visibleRecord(segs);
  const QString path = write(QStringLiteral("trlr.dlis"), b);

  LasHeaderInfo header;
  QList<LasCurve> curves;
  QList<LasIssue> issues;
  QString error;
  QVERIFY2(DlisParser::parse(path, header, curves, &error, &issues),
           qPrintable(error));
  QCOMPARE(curves.at(0).values, QVector<double>({ 1234.0, 1234.5 }));
}

void TestDlisParser::orphanPredecessorSegmentFails()
{
  // 段头带前驱位却无待续 LR——分段状态与字节流不一致，必须如实报错
  QVector<ChannelDef> channels = {
    { QStringLiteral("DEPT"), QStringLiteral("m"), 2, {}, QString() },
  };
  QVector<FrameDef> frames = { { QStringLiteral("4000"),
                                 { QStringLiteral("DEPT") },
                                 QString(), QString() } };
  QByteArray b = sul();
  b += buildFileHeaderEflr(QStringLiteral("ORPH"));
  b += buildOriginEflr(QStringLiteral("W-ORPH"));
  b += buildChannelEflr(channels);
  b += buildFrameEflr(frames);
  b += visibleRecord({ logicalRecordSegment(
      0, 0x40 /*前驱*/, fdataBody(QStringLiteral("4000"), 1, fsinglBytes(1.0f))) });
  const QString path = write(QStringLiteral("orph.dlis"), b);

  LasHeaderInfo header;
  QList<LasCurve> curves;
  QString error;
  QVERIFY(!DlisParser::parse(path, header, curves, &error));
  QVERIFY2(error.contains(QStringLiteral("前驱")), qPrintable(error));
}

void TestDlisParser::danglingSuccessorAtEofFails()
{
  // 末段带后继位但文件就此结束：半截 LR 不得当完整数据交出
  QVector<ChannelDef> channels = {
    { QStringLiteral("DEPT"), QStringLiteral("m"), 2, {}, QString() },
  };
  QVector<FrameDef> frames = { { QStringLiteral("5000"),
                                 { QStringLiteral("DEPT") },
                                 QString(), QString() } };
  QByteArray b = sul();
  b += buildFileHeaderEflr(QStringLiteral("DANG"));
  b += buildOriginEflr(QStringLiteral("W-DANG"));
  b += buildChannelEflr(channels);
  b += buildFrameEflr(frames);
  b += visibleRecord({ logicalRecordSegment(
      0, 0x20 /*后继*/, fdataBody(QStringLiteral("5000"), 1, fsinglBytes(2.0f))) });
  const QString path = write(QStringLiteral("dang.dlis"), b);

  LasHeaderInfo header;
  QList<LasCurve> curves;
  QList<LasIssue> issues;
  QString error;
  QVERIFY(!DlisParser::parse(path, header, curves, &error, &issues));
  QVERIFY2(error.contains(QStringLiteral("悬置")), qPrintable(error));
  bool sawTruncated = false;
  for (const LasIssue &i : issues)
    if (i.category == LasIssue::Category::Truncated)
      sawTruncated = true;
  QVERIFY2(sawTruncated, "未闭合延续必须落 Truncated 类目 issue");
}

void TestDlisParser::emptyFileFails()
{
  const QString path = write(QStringLiteral("empty.dlis"), QByteArray());
  LasHeaderInfo header;
  QList<LasCurve> curves;
  QString error;
  QVERIFY(!DlisParser::parse(path, header, curves, &error));
  QVERIFY(!error.isEmpty());
  QVERIFY(!DlisParser::sniff(path));
}

QTEST_MAIN(TestDlisParser)
#include "tst_dlisparser.moc"
