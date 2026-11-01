// 层：测试壳
#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringEncoder>
#include <QTemporaryDir>
#include <cmath>

#include "domain/wellnumeric.h"
#include "domain/welltopsedit.h"
#include "io/wellfileparsers.h"
#include "io/welltable.h"
#include "io/timedeptool.h"
#include "io/ziparchive.h"

namespace
{
QByteArray fixture(const QString &name)
{
  QFile f(QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath(
      QStringLiteral("fixtures/io_robustness/") + name));
  if (!f.open(QIODevice::ReadOnly))
    return {};
  return f.readAll();
}

bool writeCsv(const QString &path, const QByteArray &csv)
{
  QDir().mkpath(QFileInfo(path).path());
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(csv) == csv.size();
}

// 最小 OOXML 多工作表夹具（tst_projectsetup 同款写入口径，sheet 顺序即页序）。
QString writeXlsxMulti(const QString &path, const QVector<QPair<QString, QVector<QStringList>>> &sheets)
{
  QByteArray sheetTags;
  QByteArray rels;
  QVector<QByteArray> parts;
  for (int s = 0; s < sheets.size(); ++s)
  {
    const auto &[sheet, rows] = sheets.at(s);
    QByteArray xml("<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData>");
    for (int i = 0; i < rows.size(); ++i)
    {
      xml += "<row r=\"" + QByteArray::number(i + 1) + "\">";
      for (int c = 0; c < rows[i].size(); ++c)
        xml += "<c r=\"" + QByteArray(1, char('A' + c)) + QByteArray::number(i + 1) +
               "\" t=\"inlineStr\"><is><t>" + rows[i][c].toHtmlEscaped().toUtf8() + "</t></is></c>";
      xml += "</row>";
    }
    xml += "</sheetData></worksheet>";
    parts.append(xml);
    sheetTags += QString("<sheet name=\"%1\" sheetId=\"%2\" r:id=\"rId%2\"/>")
                     .arg(sheet)
                     .arg(s + 1)
                     .toUtf8();
    rels += QString("<Relationship Id=\"rId%1\" Target=\"worksheets/sheet%1.xml\"/>")
               .arg(s + 1)
               .toUtf8();
  }
  const QByteArray workbook =
      QString("<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
              "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
              "<sheets>%1</sheets></workbook>")
          .arg(QString::fromUtf8(sheetTags))
          .toUtf8();
  const QByteArray relations =
      QString("<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/"
              "relationships\">%1</Relationships>")
          .arg(QString::fromUtf8(rels))
          .toUtf8();
  QVector<paleo::io::ZipWriteEntry> entries{{"xl/workbook.xml", workbook},
                                            {"xl/_rels/workbook.xml.rels", relations}};
  for (int s = 0; s < parts.size(); ++s)
    entries.append({QStringLiteral("xl/worksheets/sheet%1.xml").arg(s + 1), parts.at(s)});
  QDir().mkpath(QFileInfo(path).absolutePath());
  QString error;
  return paleo::io::zipWriteArchive(path, entries, {}, &error) ? path : QString();
}
}

class WellFileParserTests : public QObject
{
  Q_OBJECT
private slots:
  void sentinelVocabularyIsExact();
  void rejectsDepthlessTopsWithPhysicalLineAndColumn();
  void blankColumnsNeverShift();
  void lineEndingsKeepPhysicalRowNumbers();
  void goodRowsAreUnchanged();
  void timeDepthFilteringMatchesBothDirections();
  void missingTvdssKeepsUsableLookupDepths();
  void reportsOptionalMissingCellsWithoutRejectingGoodDepth();
  void depthAliases_data();
  void depthAliases();
  void acousticUnitAliases_data();
  void acousticUnitAliases();
  void densityUnitAliases_data();
  void densityUnitAliases();
  void streamingDeviceEntryMatchesWholeRead();
  void sheet1WellNameHeaderReadsOneTop();
  void baseMdHeaderIsNotStoredAsTopMd();
  void topDepthComesFromMdNotBaseMd();
  void stratTableWithoutSourceCrsKeepsImporting();
  void csvQuotedFieldsFollowRfc4180();
  // finding-58：分层表之后的纯井口页也要并入 heads（不再静默整表丢弃）
  void stratificationThenWellHeadSheetKeepsHeads();
  // issue-796 / finding-61：.dat 分层底界按井名+层名哈希匹配（O(N²) → 索引）
  void layerIntervalDatBasesMatchByKey();
  void diagnosticSummaryIsBoundedWithoutDiscardingReport();
};

void WellFileParserTests::diagnosticSummaryIsBoundedWithoutDiscardingReport() {
  WellParseReport report;
  for(int i=0;i<1000;++i)report.issues.append(QStringLiteral("bad row %1: ").arg(i)+QString(10000,QLatin1Char('x')));
  const auto summary=wellParseSummary(report);
  QVERIFY(summary.size()<9000);QVERIFY(summary.contains("984"));
  QCOMPARE(report.issues.size(),1000);QVERIFY(report.issues.last().size()>10000);
}

// PW-072：引号内逗号不切列，"" 还原为字面引号。
void WellFileParserTests::csvQuotedFieldsFollowRfc4180()
{
  WellParseReport report;
  const auto rows = parseWellHeadText("\"W,1 \"\"A\"\"\",100,200,30,1500\n", &report);
  QCOMPARE(rows.size(), 1);
  QCOMPARE(rows.at(0).name, QStringLiteral("W,1 \"A\""));
  QCOMPARE(rows.at(0).x, 100.0);
  QCOMPARE(rows.at(0).y, 200.0);
}

void WellFileParserTests::sentinelVocabularyIsExact()
{
  using namespace paleo::wellnumeric;
  QCOMPARE(kNullValues.size(), size_t(4));
  for (double v : {-99999.0, -999.25, -9999.0, -999.0})
  {
    QVERIFY(isNull(v));
    double out = 123;
    QCOMPARE(parse(QString::number(v, 'g', 17), &out), NumberKind::NullSentinel);
    QCOMPARE(out, 123.0);
  }
  for (double v : {-100000.0, -99999.1, -999.251, -998.9, -123.0, 0.0, 1.0})
    QVERIFY(isUsable(v));
  QCOMPARE(parse(QString(), nullptr), NumberKind::Empty);
  QCOMPARE(parse(QStringLiteral("broken"), nullptr), NumberKind::NonNumeric);
  QCOMPARE(parse(QStringLiteral("nan"), nullptr), NumberKind::NonFinite);
  QCOMPARE(parse(QStringLiteral("inf"), nullptr), NumberKind::NonFinite);
}

void WellFileParserTests::rejectsDepthlessTopsWithPhysicalLineAndColumn()
{
  const QByteArray bytes = fixture(QStringLiteral("tops_nulls.tsv"));
  QVERIFY(!bytes.isEmpty());
  WellParseReport report;
  const auto rows = parseWellTopsText(bytes, &report);
  QCOMPARE(rows.size(), 1);
  QCOMPARE(rows[0].topName, QStringLiteral("GOOD"));
  QCOMPARE(rows[0].md, 100.0);
  QCOMPARE(report.acceptedRows, 1);
  QCOMPARE(report.rejectedRows, 6);
  QCOMPARE(report.sentinelHits, 3);
  QCOMPARE(report.blankCells, 2);
  QCOMPARE(report.invalidCells, 1);
  QCOMPARE(report.issues.size(), 6);
  for (int i = 0; i < report.issues.size(); ++i)
  {
    QVERIFY(report.issues[i].contains(QStringLiteral("第 %1 行").arg(i + 3)));
    QVERIFY(report.issues[i].contains(QStringLiteral("MD")));
    QVERIFY(report.issues[i].contains(QStringLiteral("拒收")));
  }
}

void WellFileParserTests::blankColumnsNeverShift()
{
  for (const QByteArray &bytes : {QByteArray("W1\tT\t\t10\t20\t30\t40\t50\n"),
                                 QByteArray("W1,T,,10,20,30,40,50\n")})
  {
    WellParseReport report;
    QVERIFY(parseWellTopsText(bytes, &report).isEmpty());
    QCOMPARE(report.rejectedRows, 1);
    QCOMPARE(report.blankCells, 1);
    QVERIFY(report.issues[0].contains(QStringLiteral("MD")));
  }
  WellParseReport report;
  const auto rows = parseWellHeadText("W1\t\t20\t30\t40\n", &report);
  QVERIFY(rows.isEmpty());
  QVERIFY(report.issues[0].contains(QStringLiteral("X")));
}

void WellFileParserTests::lineEndingsKeepPhysicalRowNumbers()
{
  for (const QByteArray &separator : {QByteArray("\n"), QByteArray("\r\n"), QByteArray("\r")})
  {
    WellParseReport report;
    const auto rows = parseWellTopsText("# header" + separator + "W1 GOOD 100" + separator +
                                      "W1 BAD -999.25" + separator, &report);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(report.rejectedRows, 1);
    QVERIFY(report.issues[0].contains(QStringLiteral("第 3 行")));
  }
}

void WellFileParserTests::goodRowsAreUnchanged()
{
  const QByteArray bytes = "# comment\r\n\r\nW1 D61 100.125 -100000 20.5 -500 90.25 80.125\r\n";
  WellParseReport report;
  const auto rows = parseWellTopsText(bytes, &report);
  QCOMPARE(rows.size(), 1);
  const auto &r = rows[0];
  QCOMPARE(r.md, 100.125);
  QCOMPARE(r.x, -100000.0);
  QCOMPARE(r.y, 20.5);
  QCOMPARE(r.z, -500.0);
  QCOMPARE(r.tvd, 90.25);
  QCOMPARE(r.timeMs, 80.125);
  QVERIFY(r.hasMd && r.hasTvd && r.hasTime && r.hasX && r.hasY);
  QCOMPARE(report.rejectedRows, 0);
  QVERIFY(report.issues.isEmpty());
  const auto back = parseWellTopsText(writeWellTopsText(rows));
  QCOMPARE(back.size(), 1);
  QCOMPARE(back[0].md, r.md);
  QCOMPARE(back[0].x, r.x);
  QCOMPARE(back[0].tvd, r.tvd);
  QCOMPARE(back[0].timeMs, r.timeMs);
}

void WellFileParserTests::timeDepthFilteringMatchesBothDirections()
{
  WellParseReport report;
  const auto td = parseTimeDepthText(fixture(QStringLiteral("td_nulls.tsv")), &report);
  QCOMPARE(td.rows.size(), 2);
  QCOMPARE(report.rejectedRows, 3);
  QCOMPARE(report.sentinelHits, 2);
  QCOMPARE(report.blankCells, 1);
  QCOMPARE(TimeDepthTool::interpolateTimeMs(td, 150, true).timeMs, 150.0);
  QCOMPARE(TimeDepthTool::interpolateDepthAtTimeMs(td, 150, true).depth, 150.0);
  TimeDepthTable manual = td;
  for (double v : paleo::wellnumeric::kNullValues)
  {
    TdRow bad;
    bad.hasMd = bad.hasTvd = true;
    bad.md = bad.tvd = v;
    bad.timeMs = 150;
    manual.rows.insert(1, bad);
    bad.md = bad.tvd = 150;
    bad.timeMs = v;
    manual.rows.insert(1, bad);
  }
  const auto forward = TimeDepthTool::interpolateTimeMs(manual, 150, true);
  const auto inverse = TimeDepthTool::interpolateDepthAtTimeMs(manual, 150, true);
  QVERIFY(forward.ok());
  QVERIFY(inverse.ok());
  QCOMPARE(forward.timeMs, 150.0);
  QCOMPARE(inverse.depth, 150.0);
  QCOMPARE(forward.ignoredRows, 8);
  QCOMPARE(inverse.ignoredRows, 8);
}

void WellFileParserTests::missingTvdssKeepsUsableLookupDepths()
{
  WellParseReport report;
  const auto td = parseTimeDepthText(
      "100\t-999.25\t100\t100\n200\t\t200\t200\n300\t-99999\t-99999\t-99999\n", &report);
  QCOMPARE(td.rows.size(), 2);
  QCOMPARE(report.acceptedRows, 2);
  QCOMPARE(report.rejectedRows, 1);
  QCOMPARE(report.sentinelHits, 4);
  QCOMPARE(report.blankCells, 1);
  QVERIFY(std::isnan(td.rows[0].tvdss));
  QVERIFY(std::isnan(td.rows[1].tvdss));
  QVERIFY(td.rows[0].hasTvd && td.rows[0].hasMd);
  QCOMPARE(td.rows[0].tvd, 100.0);
  QCOMPARE(td.rows[0].md, 100.0);
  QVERIFY(report.issues.join(QString()).contains(QStringLiteral("没有可用深度")));
  for (bool useMd : {false, true})
  {
    const auto forward = TimeDepthTool::interpolateTimeMs(td, 150, useMd);
    const auto inverse = TimeDepthTool::interpolateDepthAtTimeMs(td, 150, useMd);
    QVERIFY(forward.ok() && inverse.ok());
    QCOMPARE(forward.timeMs, 150.0);
    QCOMPARE(inverse.depth, 150.0);
  }
}

void WellFileParserTests::reportsOptionalMissingCellsWithoutRejectingGoodDepth()
{
  WellParseReport report;
  const auto rows = parseWellTopsText("W1\tD61\t100\t-999.25\t\t-99999\t-9999\tbad\n", &report);
  QCOMPARE(rows.size(), 1);
  QVERIFY(rows[0].hasMd);
  QVERIFY(!rows[0].hasX && !rows[0].hasY && !rows[0].hasTvd && !rows[0].hasTime);
  QCOMPARE(report.sentinelHits, 3);
  QCOMPARE(report.blankCells, 1);
  QCOMPARE(report.invalidCells, 1);
  QCOMPARE(report.rejectedRows, 0);
  QCOMPARE(report.issues.size(), 5);
  QVERIFY(std::isnan(rows[0].z));
  const auto back = parseWellTopsText(writeWellTopsText(rows), &report);
  QCOMPARE(back.size(), 1);
  QVERIFY(std::isnan(back[0].z));
  QVERIFY(WellTopsEdit::sameValues(rows[0], back[0]));
  auto changed = back[0];
  changed.z = 0.0;
  QVERIFY(WellTopsEdit::sameValues(rows[0], changed)); // 无坐标组，Z 不参与。
  const auto shortRow = parseWellTopsText("W1 D62 100\n", &report);
  const auto shortBack = parseWellTopsText(writeWellTopsText(shortRow), &report);
  QVERIFY(WellTopsEdit::sameValues(shortRow[0], shortBack[0]));
  const auto coordinates = parseWellTopsText("W1 D63 100 10 20 -999.25\n", &report);
  const auto coordinatesBack = parseWellTopsText(writeWellTopsText(coordinates), &report);
  QVERIFY(WellTopsEdit::sameValues(coordinates[0], coordinatesBack[0]));
  changed = coordinatesBack[0];
  changed.z = 0.0;
  QVERIFY(!WellTopsEdit::sameValues(coordinates[0], changed)); // 真实 0 与缺失 Z 有别。
}

void WellFileParserTests::depthAliases_data()
{
  QTest::addColumn<QString>("unit");
  QTest::addColumn<double>("scale");
  for (const char *u : {"M", "METER", "METERS", "METRE", "METRES"})
  {
    QTest::newRow(u) << QString::fromLatin1(u) << 1.0;
    const QByteArray lower = QByteArray(u).toLower();
    QTest::newRow(lower.constData()) << QString::fromLatin1(lower) << 1.0;
  }
  for (const char *u : {"FT", "F", "FOOT", "FEET"})
  {
    QTest::newRow(u) << QString::fromLatin1(u) << 0.3048;
    const QByteArray lower = QByteArray(u).toLower();
    QTest::newRow(lower.constData()) << QString::fromLatin1(lower) << 0.3048;
  }
  // PW-029：LAS 头常见尾点写法与其它两个转换器口径统一。
  QTest::newRow("M.") << QStringLiteral("M.") << 1.0;
  QTest::newRow("FT.") << QStringLiteral("FT.") << 0.3048;
  QTest::newRow("dot-only") << QStringLiteral(".") << -1.0;
  for (const char *u : {"", "UNKNOWN", "MM", "METER/SEC", "FATHOM", "FOOTS"})
    QTest::newRow(*u ? u : "empty") << QString::fromLatin1(u) << -1.0;
}

void WellFileParserTests::depthAliases()
{
  QFETCH(QString, unit);
  QFETCH(double, scale);
  const auto parsed = paleo::wellnumeric::depthScaleToMetres(unit);
  QCOMPARE(parsed.has_value(), scale > 0);
  if (parsed)
  {
    QCOMPARE(*parsed, scale);
    QCOMPARE(*paleo::wellnumeric::depthScaleToMetres(QStringLiteral(" ") + unit + QStringLiteral(" ")), scale);
  }
}

void WellFileParserTests::acousticUnitAliases_data()
{
  QTest::addColumn<QString>("unit");
  QTest::addColumn<double>("scale");
  for (const char *u : {"US/M", "USEC/M", "MICROSEC/M", "US/METER"})
    QTest::newRow(u) << QString::fromLatin1(u) << 1.0;
  for (const char *u : {"US/FT", "USEC/FT", "MICROSEC/FT", "US/FOOT"})
    QTest::newRow(u) << QString::fromLatin1(u) << 1.0 / 0.3048;
  // 空单位 = 遗留数据无元数据，按 µs/m 假设（乘 1）；未知非空单位拒绝。
  QTest::newRow("empty") << QString() << 1.0;
  for (const char *u : {"MS/M", "S/M", "FT", "US"})
    QTest::newRow(u) << QString::fromLatin1(u) << -1.0;
}

void WellFileParserTests::acousticUnitAliases()
{
  QFETCH(QString, unit);
  QFETCH(double, scale);
  const auto parsed = paleo::wellnumeric::acousticScaleToUsPerM(unit);
  QCOMPARE(parsed.has_value(), scale > 0);
  if (parsed)
    QCOMPARE(*parsed, scale);
  // 大小写与前后空白不敏感（LAS 头字段常态）。
  if (!unit.isEmpty() && parsed)
    QCOMPARE(*paleo::wellnumeric::acousticScaleToUsPerM(
                 QStringLiteral(" ") + unit.toLower() + QStringLiteral(" ")),
             scale);
}

void WellFileParserTests::densityUnitAliases_data()
{
  QTest::addColumn<QString>("unit");
  QTest::addColumn<double>("scale");
  for (const char *u : {"G/CC", "G/CM3", "G/C3", "GCM3", "GM/CC"})
    QTest::newRow(u) << QString::fromLatin1(u) << 1.0;
  for (const char *u : {"KG/M3", "KG/M^3", "KGM3"})
    QTest::newRow(u) << QString::fromLatin1(u) << 0.001;
  QTest::newRow("empty") << QString() << 1.0;
  for (const char *u : {"KG", "CC", "G", "N/M3"})
    QTest::newRow(u) << QString::fromLatin1(u) << -1.0;
}

void WellFileParserTests::densityUnitAliases()
{
  QFETCH(QString, unit);
  QFETCH(double, scale);
  const auto parsed = paleo::wellnumeric::densityScaleToGPerCc(unit);
  QCOMPARE(parsed.has_value(), scale > 0);
  if (parsed)
    QCOMPARE(*parsed, scale);
}

// #561：流式逐行入口与整读版同核同口径——四型 ×（LF/CRLF/孤 CR、UTF-8
// BOM、GB18030、列名头）对拍，外加 runningHash 与「readAll 后整段哈希」等价。
void WellFileParserTests::streamingDeviceEntryMatchesWholeRead()
{
  QStringEncoder gbEnc(QStringLiteral("GB18030"));
  const QList<QByteArray> topsCases = {
      QByteArray("W1 T1 100\nW1 T2 -999.25\n"),                     // LF
      QByteArray("# h\r\nW1 T1 100\r\nW1 T2 x\r\n"),            // CRLF + 拒收行
      QByteArray("W1 T1 100\rW1 T2 -999.25\r"),                     // 孤 CR（旧 Mac）
      QByteArray("\xEF\xBB\xBF# bom\nW1 T1 100\nW1 T2 -999.25\n"),  // UTF-8 BOM
      gbEnc.encode(QStringLiteral("井1 顶1 100\n井2 顶2 -999.25\n")), // GB18030 井名
      QByteArray("#wellName topName DepthMD\r\nW1 T1 100\r\nW1 T2 bad\r\n"), // 列名头
  };
  for (const QByteArray &bytes : topsCases)
  {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("tops.dat"));
    {
      QFile f(path);
      QVERIFY(f.open(QIODevice::WriteOnly));
      QCOMPARE(f.write(bytes), qint64(bytes.size()));
    }
    QFile dev(path);
    QVERIFY(dev.open(QIODevice::ReadOnly));
    QCryptographicHash running(QCryptographicHash::Sha256);
    QString ioErr;
    WellParseReport devReport;
    const auto streamed = parseWellTopsDevice(&dev, &running, &devReport, &ioErr);
    QVERIFY2(streamed.has_value(), qPrintable(ioErr));
    QCOMPARE(QString::fromLatin1(running.result().toHex()),
             QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
    WellParseReport textReport;
    const auto whole = parseWellTopsText(bytes, &textReport);
    QCOMPARE(streamed->size(), whole.size());
    for (int i = 0; i < whole.size(); ++i)
    {
      QCOMPARE((*streamed)[i].wellName, whole[i].wellName);
      QCOMPARE((*streamed)[i].topName, whole[i].topName);
      QCOMPARE((*streamed)[i].md, whole[i].md);
    }
    QCOMPARE(devReport.acceptedRows, textReport.acceptedRows);
    QCOMPARE(devReport.rejectedRows, textReport.rejectedRows);
    QCOMPARE(devReport.sentinelHits, textReport.sentinelHits);
    QCOMPARE(devReport.blankCells, textReport.blankCells);
    QCOMPARE(devReport.invalidCells, textReport.invalidCells);
    QCOMPARE(devReport.issues.join(QLatin1Char(';')),
             textReport.issues.join(QLatin1Char(';')));
  }

  // 井口 / 时深 / 井斜：同一流式骨架（行界机共用），各取一组混合行界对拍。
  {
    const QByteArray bytes = "\xEF\xBB\xBFW1 10 20 1 100\r\nW2 bad 30 2 200\n";
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("heads.dat"));
    {
      QFile f(path);
      QVERIFY(f.open(QIODevice::WriteOnly));
      QCOMPARE(f.write(bytes), qint64(bytes.size()));
    }
    QFile dev(path);
    QVERIFY(dev.open(QIODevice::ReadOnly));
    QString ioErr;
    WellParseReport devReport, textReport;
    const auto streamed = parseWellHeadDevice(&dev, nullptr, &devReport, &ioErr);
    QVERIFY2(streamed.has_value(), qPrintable(ioErr));
    const auto whole = parseWellHeadText(bytes, &textReport);
    QCOMPARE(streamed->size(), whole.size());
    for (int i = 0; i < whole.size(); ++i)
    {
      QCOMPARE((*streamed)[i].name, whole[i].name);
      QCOMPARE((*streamed)[i].x, whole[i].x);
      QCOMPARE((*streamed)[i].td, whole[i].td);
    }
    QCOMPARE(devReport.rejectedRows, textReport.rejectedRows);
    QCOMPARE(devReport.issues.join(QLatin1Char(';')),
             textReport.issues.join(QLatin1Char(';')));
  }
  {
    const QByteArray bytes = "# Well : W9\r100 -999.25 100 100\n200\t\t200 200\r";
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("td.dat"));
    {
      QFile f(path);
      QVERIFY(f.open(QIODevice::WriteOnly));
      QCOMPARE(f.write(bytes), qint64(bytes.size()));
    }
    QFile dev(path);
    QVERIFY(dev.open(QIODevice::ReadOnly));
    QString ioErr;
    WellParseReport devReport, textReport;
    const auto streamed = parseTimeDepthDevice(&dev, nullptr, &devReport, &ioErr);
    QVERIFY2(streamed.has_value(), qPrintable(ioErr));
    const auto whole = parseTimeDepthText(bytes, &textReport);
    QCOMPARE(streamed->wellName, whole.wellName);
    QCOMPARE(streamed->rows.size(), whole.rows.size());
    QCOMPARE(devReport.acceptedRows, textReport.acceptedRows);
    QCOMPARE(devReport.issues.join(QLatin1Char(';')),
             textReport.issues.join(QLatin1Char(';')));
  }
  {
    const QByteArray bytes = "# Well : W9\n100 10 45\r\nbad 20 90\n";
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("dev.dat"));
    {
      QFile f(path);
      QVERIFY(f.open(QIODevice::WriteOnly));
      QCOMPARE(f.write(bytes), qint64(bytes.size()));
    }
    QFile dev(path);
    QVERIFY(dev.open(QIODevice::ReadOnly));
    QString ioErr;
    WellParseReport devReport, textReport;
    const auto streamed = parseDeviationDevice(&dev, nullptr, &devReport, &ioErr);
    QVERIFY2(streamed.has_value(), qPrintable(ioErr));
    const auto whole = parseDeviationText(bytes, &textReport);
    QCOMPARE(streamed->wellName, whole.wellName);
    QCOMPARE(streamed->stations.size(), whole.stations.size());
    QCOMPARE(devReport.rejectedRows, textReport.rejectedRows);
    QCOMPARE(devReport.issues.join(QLatin1Char(';')),
             textReport.issues.join(QLatin1Char(';')));
  }
}

// #671：Sheet1 上 WELL_NAME / TOP_NAME / HORIZON_NAME 仍对上井名或层位。
void WellFileParserTests::sheet1WellNameHeaderReadsOneTop()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = dir.filePath(QStringLiteral("Sheet1.csv"));
  QVERIFY(writeCsv(path, "WELL_NAME,TOP,MD\nA1,D61,100\n"));
  const auto table = paleo::io::readWellTable(path);
  QVERIFY2(table.error.isEmpty(), qPrintable(table.error));
  QCOMPARE(table.tops.size(), 1);
  QCOMPARE(table.tops[0].wellName, QStringLiteral("A1"));
  QCOMPARE(table.tops[0].topName, QStringLiteral("D61"));
  QVERIFY(table.tops[0].hasMd);
  QCOMPARE(table.tops[0].md, 100.0);

  const QString topName = dir.filePath(QStringLiteral("top/Sheet1.csv"));
  QVERIFY(writeCsv(topName, "WELL_NAME,TOP_NAME,MD\nA1,D62,110\n"));
  const auto namedTop = paleo::io::readWellTable(topName);
  QCOMPARE(namedTop.tops.size(), 1);
  QCOMPARE(namedTop.tops[0].wellName, QStringLiteral("A1"));
  QCOMPARE(namedTop.tops[0].topName, QStringLiteral("D62"));
  QCOMPARE(namedTop.tops[0].md, 110.0);

  const QString horizon = dir.filePath(QStringLiteral("horizon/Sheet1.csv"));
  QVERIFY(writeCsv(horizon, "WELL_NAME,HORIZON_NAME,MD\nA1,D63,120\n"));
  const auto namedHorizon = paleo::io::readWellTable(horizon);
  QCOMPARE(namedHorizon.tops.size(), 1);
  QCOMPARE(namedHorizon.tops[0].wellName, QStringLiteral("A1"));
  QCOMPARE(namedHorizon.tops[0].topName, QStringLiteral("D63"));
  QCOMPARE(namedHorizon.tops[0].md, 120.0);

  // TOP_NAME 在前时不得被井名别名 NAME 抢走。
  const QString ordered = dir.filePath(QStringLiteral("order/Sheet1.csv"));
  QVERIFY(writeCsv(ordered, "TOP_NAME,WELL_NAME,MD\nD61,A1,100\n"));
  const auto swapped = paleo::io::readWellTable(ordered);
  QCOMPARE(swapped.tops.size(), 1);
  QCOMPARE(swapped.tops[0].wellName, QStringLiteral("A1"));
  QCOMPARE(swapped.tops[0].topName, QStringLiteral("D61"));
  QCOMPARE(swapped.tops[0].md, 100.0);
}

// #671：注释头里的底深列不得写成顶深 MD。
void WellFileParserTests::baseMdHeaderIsNotStoredAsTopMd()
{
  for (const char *header : {"# wellName topName BASE_MD\n", "# wellName topName BOTTOM_MD\n"})
  {
    const auto tops = parseWellTopsText(QByteArray(header) + "A1 D61 1500\n");
    for (const auto &top : tops)
      QVERIFY(!top.hasMd);
    QVERIFY(tops.isEmpty());
  }
}

// #671：同时有 MD 与 BASE_MD 时，顶深只取 MD 列。仅有 BASE_MD 时不占顶深。
void WellFileParserTests::topDepthComesFromMdNotBaseMd()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = dir.filePath(QStringLiteral("layers.csv"));
  QVERIFY(writeCsv(path, "WELL,TOP,MD,BASE_MD\nA1,D61,100,80\n"));
  const auto table = paleo::io::readWellTable(path);
  QVERIFY2(table.error.isEmpty(), qPrintable(table.error));
  QCOMPARE(table.tops.size(), 1);
  QCOMPARE(table.tops[0].wellName, QStringLiteral("A1"));
  QCOMPARE(table.tops[0].topName, QStringLiteral("D61"));
  QVERIFY(table.tops[0].hasMd);
  QCOMPARE(table.tops[0].md, 100.0);

  const QString baseOnly = dir.filePath(QStringLiteral("base.csv"));
  QVERIFY(writeCsv(baseOnly, "WELL,TOP,BASE_MD\nA1,D61,80\n"));
  QVERIFY(paleo::io::readWellTable(baseOnly).tops.isEmpty());

  const auto named = parseWellTopsText(QByteArrayLiteral("# WELL TOP MD BASE_MD\nA1 D61 100 80\n"));
  QCOMPARE(named.size(), 1);
  QVERIFY(named[0].hasMd);
  QCOMPARE(named[0].md, 100.0);
  QCOMPARE(named[0].wellName, QStringLiteral("A1"));
  QCOMPARE(named[0].topName, QStringLiteral("D61"));

  const auto reversed = parseWellTopsText(QByteArrayLiteral("# WELL TOP BASE_MD MD\nA1 D61 80 100\n"));
  QCOMPARE(reversed.size(), 1);
  QVERIFY(reversed[0].hasMd);
  QCOMPARE(reversed[0].md, 100.0);
}

// finding-58：分层表（无坐标列）之后的纯井口页，井口坐标此前被静默整表
// 丢弃——井口表与分层表同库时井位全失且无任何提示。
void WellFileParserTests::stratificationThenWellHeadSheetKeepsHeads()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = dir.filePath(QStringLiteral("mixed.xlsx"));
  QVERIFY(!writeXlsxMulti(path,
                          {{QStringLiteral("分层"),
                            {{QStringLiteral("井号"), QStringLiteral("层位"), QStringLiteral("顶深")},
                             {QStringLiteral("A1"), QStringLiteral("D61"), QStringLiteral("100")},
                             {QStringLiteral("A1"), QStringLiteral("D62"), QStringLiteral("120")}}},
                           {QStringLiteral("井口"),
                            {{QStringLiteral("井号"), QStringLiteral("X"), QStringLiteral("Y")},
                             {QStringLiteral("A1"), QStringLiteral("500000"), QStringLiteral("4400000")}}}})
              .isEmpty());

  const auto table = paleo::io::readWellTable(path);
  QVERIFY2(table.error.isEmpty(), qPrintable(table.error));
  // 层位+深度优先：整表仍判为分层（不被后页翻回井口）。
  QCOMPARE(table.type, QStringLiteral("well_stratification"));
  QCOMPARE(table.tops.size(), 2);
  QCOMPARE(table.tops.at(0).topName, QStringLiteral("D61"));
  // 井口页的坐标必须保留——旧实现在 parseSheet 里被整页丢弃。
  QCOMPARE(table.heads.size(), 1);
  QCOMPARE(table.heads.first().name, QStringLiteral("A1"));
  QCOMPARE(table.heads.first().x, 500000.0);
  QCOMPARE(table.heads.first().y, 4400000.0);

  // 底界仍只取底界列（分层页无底界列 → 空）。
  const auto bases = paleo::io::readWellTableBases(path);
  QVERIFY(bases.isEmpty());
}

// issue-796 / finding-61：.dat 的底界注释按「井名+层名」哈希索引匹配
// （此前逐条 find_if 是 O(底界×顶界)）；同时钉 64 MB 有界读入口仍能正常
// 解析常规 .dat（闸只挡大小，不改口径）。
void WellFileParserTests::layerIntervalDatBasesMatchByKey()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = dir.filePath(QStringLiteral("layers.dat"));
  const QByteArray dat =
      "# WELL TOP MD\n"
      "A1 D61 100\n"
      "A1 D62 120\n"
      "B2 D61 80\n"
      "# PaleoLayerInterval: {\"well\":\"A1\",\"layer\":\"D62\",\"topMd\":120,\"baseMd\":140}\n"
      "# PaleoLayerInterval: {\"well\":\"B2\",\"layer\":\"D61\",\"topMd\":80,\"baseMd\":95}\n"
      "# PaleoLayerInterval: {\"well\":\"B2\",\"layer\":\"D62\",\"topMd\":999,\"baseMd\":1000}\n"
      "# PaleoLayerInterval: {\"well\":\"A1\",\"layer\":\"D61\",\"topMd\":100,\"baseMd\":115}\n";
  QVERIFY(writeCsv(path, dat));

  const auto bases = paleo::io::readWellTableBases(path);
  QCOMPARE(bases.size(), 3); // 第 3 条（B2/D62）无对应顶界 → 不匹配
  QCOMPARE(bases.at(0).wellName, QStringLiteral("A1"));
  QCOMPARE(bases.at(0).topName, QStringLiteral("D62"));
  QVERIFY(bases.at(0).hasMd);
  QCOMPARE(bases.at(0).md, 140.0);
  QCOMPARE(bases.at(1).wellName, QStringLiteral("B2"));
  QCOMPARE(bases.at(1).topName, QStringLiteral("D61"));
  QCOMPARE(bases.at(1).md, 95.0);
  QCOMPARE(bases.at(2).wellName, QStringLiteral("A1"));
  QCOMPARE(bases.at(2).topName, QStringLiteral("D61"));
  QCOMPARE(bases.at(2).md, 115.0);

  // 同文件写入侧（writeWellLayerTable 的哈希索引）round-trip：底界随顶界
  // 一起序列化，读回仍按井名+层名对得上。
  const auto tops = parseWellTopsText(dat);
  QCOMPARE(tops.size(), 3);
  const auto written = paleo::io::writeWellLayerTable(tops, bases);
  const QString out = dir.filePath(QStringLiteral("roundtrip.dat"));
  QVERIFY(writeCsv(out, written));
  const auto reparsed = paleo::io::readWellTableBases(out);
  QCOMPARE(reparsed.size(), bases.size());
  for (int i = 0; i < bases.size(); ++i)
  {
    QCOMPARE(reparsed.at(i).wellName, bases.at(i).wellName);
    QCOMPARE(reparsed.at(i).topName, bases.at(i).topName);
    QCOMPARE(reparsed.at(i).md, bases.at(i).md);
  }
}

QTEST_GUILESS_MAIN(WellFileParserTests)
#include "tst_wellfileparsers.moc"

// 分层表带井口坐标列但源 CRS 未知时不得整表拒绝（paleo_comp_data 分层总表
// 被「井点坐标系未知，不能转换到工程坐标系」拦死的回归）：井位以井口表为
// 主，分层表头坐标是附带信息；井口表自身的空 CRS 硬闸（#664）不变。
void WellFileParserTests::stratTableWithoutSourceCrsKeepsImporting()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = dir.filePath(QStringLiteral("分层数据.csv"));
  QVERIFY(writeCsv(path, "WELL_NAME,X,Y,TOP,MD\nA1,500000,4400000,D61,100\n"));
  QVariantMap mapping;
  mapping.insert(QStringLiteral("projectCrs"), QStringLiteral("EPSG:32649"));
  const auto table = paleo::io::readWellTable(path, mapping);
  QVERIFY2(table.error.isEmpty(), qPrintable(table.error));
  QCOMPARE(table.type, QStringLiteral("well_stratification"));
  QCOMPARE(table.tops.size(), 1);
  QCOMPARE(table.tops[0].wellName, QStringLiteral("A1"));
  QCOMPARE(table.tops[0].md, 100.0);

  // 井口表没有源 CRS 时仍按 #664 拒绝（坐标是主数据）。
  const QString headPath = dir.filePath(QStringLiteral("井口.csv"));
  QVERIFY(writeCsv(headPath, "WELL_NAME,X,Y\nA1,500000,4400000\n"));
  const auto head = paleo::io::readWellTable(headPath, mapping);
  QVERIFY(!head.error.isEmpty());
  QVERIFY(head.error.contains(QStringLiteral("井点坐标系未知")));
}
