// 层：测试壳
#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include <QDir>
#include <cmath>
#include "io/welllogread.h"
#include "io/welllogtext.h"
#include "io/welllogtext_internal.h"
#include "io/wellcompositexml.h"

namespace
{
QString xmlSheet(const QString &name, const QList<QStringList> &rows)
{
  QString xml = QStringLiteral("<Worksheet ss:Name=\"%1\"><Table>").arg(name.toHtmlEscaped());
  for (const auto &row : rows)
  {
    xml += QStringLiteral("<Row>");
    for (const auto &value : row)
      xml += QStringLiteral("<Cell><Data ss:Type=\"String\">%1</Data></Cell>").arg(value.toHtmlEscaped());
    xml += QStringLiteral("</Row>");
  }
  return xml + QStringLiteral("</Table></Worksheet>");
}

QByteArray workbook(const QString &sheets)
{
  return (QStringLiteral("<?xml version=\"1.0\"?><Workbook xmlns=\"urn:schemas-microsoft-com:office:spreadsheet\" xmlns:ss=\"urn:schemas-microsoft-com:office:spreadsheet\">") +
          sheets + QStringLiteral("</Workbook>")).toUtf8();
}

bool write(const QString &path, const QByteArray &content)
{
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}
}

class TestWellLogText : public QObject
{
  Q_OBJECT
private slots:
  void legacyUnitDeclarationsDoNotSelectCompositeMode_data()
  {
    QTest::addColumn<QString>("axis");
    QTest::addColumn<QString>("unit");
    QTest::addColumn<QString>("basis");
    QTest::newRow("explicit-metres") << QStringLiteral("深度[m]") << QStringLiteral("m") << QString();
    QTest::newRow("explicit-feet") << QStringLiteral("深度[ft]") << QStringLiteral("ft") << QString();
    QTest::newRow("parenthesized-metres") << QStringLiteral("深度(m)") << QStringLiteral("m") << QString();
    QTest::newRow("explicit-md-feet") << QStringLiteral("MD[ft]") << QStringLiteral("ft") << QStringLiteral("MD");
  }
  void legacyUnitDeclarationsDoNotSelectCompositeMode()
  {
    QFETCH(QString, axis); QFETCH(QString, unit); QFETCH(QString, basis);
    QTemporaryDir dir;
    const auto path = dir.filePath("legacy.xml");
    QVERIFY(write(path, workbook(xmlSheet(QStringLiteral("测井曲线"),
        {{"井号", axis, "TVD", "TVDSS", "GR"}, {"A1", "1000", "990", "", "10"}}))));
    LasHeaderInfo header; QList<LasCurve> curves; QString error;
    QVERIFY2(WellLogRead::parse(path, header, curves, &error), qPrintable(error));
    QCOMPARE(header.indexBasis, basis);
    QCOMPARE(curves.first().unit, unit);
    QVERIFY(curves.at(1).unit.isEmpty());
    QCOMPARE(curves.first().values, QVector<double>({1000}));
  }

  void ordinaryTextWithSameColumnsKeepsUnknownDepthMetadata()
  {
    QTemporaryDir dir; const auto path = dir.filePath("ordinary.csv");
    QVERIFY(write(path, QStringLiteral("井号,深度,TVD,TVDSS,GR\nA1,1000,990,,10\n").toUtf8()));
    LasHeaderInfo header; QList<LasCurve> curves; QString error;
    QVERIFY2(WellLogRead::parse(path, header, curves, &error), qPrintable(error));
    QVERIFY(header.indexBasis.isEmpty()); QVERIFY(curves.first().unit.isEmpty());
  }

  void compositeVerticalColumnsKeepSamplingSources()
  {
    QTemporaryDir dir;
    const auto path = dir.filePath("sampling.xml");
    const auto content = xmlSheet(QStringLiteral("测井曲线"),
        {{"井号", "深度", "TVD", "TVDSS", "GR"}, {"A1", "100", "99", "", "10"}, {"A1", "101", "100", "", "-9999"}}) +
        xmlSheet(QStringLiteral("离散曲线"), {{"井号", "深度", "TVD", "TVDSS", "POR"}, {"A1", "100.5", "99.4", "", "5"}});
    QVERIFY(write(path, workbook(content)));
    const auto doc = WellLogRead::parseDoc(path);
    QVERIFY2(doc.ok, qPrintable(doc.error));
    QCOMPARE(doc.curveNames, QStringList({"深度", "TVD", "TVDSS", "GR", "TVD [离散曲线]", "TVDSS [离散曲线]", "POR"}));
    QCOMPARE(doc.curves.first().values, QVector<double>({100, 100.5, 101}));
    QCOMPARE(doc.curves.first().unit, QStringLiteral("m"));
    LasHeaderInfo header;
    QVERIFY(WellLogRead::parseHeader(path, header));
    QCOMPARE(header.indexBasis, QStringLiteral("MD"));
    QCOMPARE(doc.curves[4].values[1], 99.4);
    QVERIFY(std::isnan(doc.curves[1].values[1]));
    QVERIFY(std::isnan(doc.curves[3].values[2]));
  }

  void emptyDiscreteSheetDoesNotInvalidateContinuousLog()
  {
    QTemporaryDir dir;
    const auto path = dir.filePath("empty-discrete.xml");
    QVERIFY(write(path, workbook(xmlSheet(QStringLiteral("测井曲线"), {{"井号", "深度", "GR"}, {"A1", "100", "2"}}) +
        xmlSheet(QStringLiteral("离散曲线"), {{"井号", "深度", "TOC"}}))));
    const auto doc = WellLogRead::parseDoc(path);
    QVERIFY2(doc.ok, qPrintable(doc.error));
    QCOMPARE(doc.curveNames, QStringList({"深度", "GR"}));
  }

  void spreadsheetLargerThanLegacyByteLimit()
  {
    QTemporaryDir dir;
    const auto path = dir.filePath("large.xml");
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray content = workbook(xmlSheet(QStringLiteral("测井曲线"), {{"井号", "MD[m]", "GR"}, {"A1", "100", "2"}}));
    const int insert = content.indexOf('>', content.indexOf("<Workbook")) + 1;
    file.write(content.first(insert)); file.write("<!--");
    const QByteArray chunk(1024*1024, ' ');
    for (int i = 0; i < 65; ++i) QCOMPARE(file.write(chunk), chunk.size());
    file.write("-->"); file.write(content.mid(insert)); file.close();
    const auto doc = WellLogRead::parseDoc(path);
    QVERIFY2(doc.ok, qPrintable(doc.error));
    QCOMPARE(doc.curves[1].values, QVector<double>({2}));
  }

  void hxnSourceDocumentsWhenExplicitlyProvided()
  {
    const auto root = qEnvironmentVariable("PALEO_HXN_DATA_DIR");
    if (root.isEmpty()) QSKIP("Set PALEO_HXN_DATA_DIR to run private source acceptance");
    const auto files = QDir(root + QStringLiteral("/测井数据")).entryInfoList({"*.xml"}, QDir::Files, QDir::Name);
    QVERIFY(!files.isEmpty());
    for (const auto &file : files) {
      const auto doc = WellLogRead::parseDoc(file.absoluteFilePath());
      QVERIFY2(doc.ok, qPrintable(file.fileName() + ": " + doc.error));
      QVERIFY(doc.curves.size() >= 2);
      qInfo().noquote() << file.fileName() << doc.curves.size() << doc.curves.first().values.size();
      WellComposite::ComprehensiveWellData composite;
      QString error;
      QVERIFY2(WellComposite::parseComprehensiveWellXml(file.absoluteFilePath(), composite, &error), qPrintable(file.fileName() + ": " + error));
      QVERIFY(!composite.wellName.isEmpty());
      QVERIFY(!composite.continuousCurves.isEmpty());
    }
  }
  void csvPreservesMetadataUnitsAndMissing()
  {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("curve.las"));
    QVERIFY(write(path, "\xef\xbb\xbf# Well: A1\r\nMD[ft],\"GR[API]\",RHOB[g/cm3]\r\n1000.125,10,2.3\r\n1000.25,,NA\r\n1000.375,-999.25,-9999\r\n"));
    QCOMPARE(WellLogRead::detect(path), WellLogFormat::Text);
    LasHeaderInfo header;
    QString error;
    QVERIFY2(WellLogRead::parseHeader(path, header, &error), qPrintable(error));
    QCOMPARE(header.wellName, QStringLiteral("A1"));
    QCOMPARE(header.indexBasis, QStringLiteral("MD"));
    QString well;
    QVERIFY(WellLogRead::readWellInfo(path, well));
    QCOMPARE(well, QStringLiteral("A1"));
    const auto doc = WellLogRead::parseDoc(path);
    QVERIFY2(doc.ok, qPrintable(doc.error));
    QCOMPARE(doc.curveNames, QStringList({"MD", "GR", "RHOB"}));
    QCOMPARE(doc.curves.at(0).unit, QStringLiteral("ft"));
    QCOMPARE(doc.curves.at(0).values.at(0), 1000.125);
    QCOMPARE(doc.curves.at(1).unit, QStringLiteral("API"));
    QCOMPARE(doc.curves.at(2).unit, QStringLiteral("g/cm3"));
    QVERIFY(std::isnan(doc.curves.at(1).values.at(1)));
    QVERIFY(std::isnan(doc.curves.at(2).values.at(1)));
    QVERIFY(std::isnan(doc.curves.at(2).values.at(2)));
  }

  void references_data()
  {
    QTest::addColumn<QByteArray>("content");
    QTest::addColumn<QString>("basis");
    QTest::addColumn<QString>("unit");
    QTest::newRow("time-tsv") << QByteArray("TIME[ms]\tDT[us/ft]\n1\t20\n2\t21\n") << QStringLiteral("TIME") << QStringLiteral("ms");
    QTest::newRow("tvd-txt") << QByteArray("TVD(m) GR(API)\n100 20\n101 21\n") << QStringLiteral("TVD") << QStringLiteral("m");
    QTest::newRow("tvdss") << QByteArray("TVDSS[m],GR[API]\n-200,1\n-199,2\n") << QStringLiteral("TVDSS") << QStringLiteral("m");
    QTest::newRow("depth-unknown") << QByteArray("DEPTH,GR\n1,2\n2,3\n") << QString() << QString();
  }
  void references()
  {
    QFETCH(QByteArray, content);
    QFETCH(QString, basis);
    QFETCH(QString, unit);
    QTemporaryDir dir;
    const auto path = dir.filePath(QStringLiteral("curve.opaque"));
    QVERIFY(write(path, content));
    LasHeaderInfo header;
    QVERIFY(WellLogRead::parseHeader(path, header));
    QCOMPARE(header.indexBasis, basis);
    const auto doc = WellLogRead::parseDoc(path);
    QVERIFY(doc.ok);
    QCOMPARE(doc.curves.first().unit, unit);
  }

  void malformedText_data()
  {
    QTest::addColumn<QByteArray>("content");
    QTest::addColumn<int>("line");
    QTest::newRow("bad-depth") << QByteArray("# Well: A1\nMD[m],GR[API]\n100,1\nbad,2\n") << 4;
    QTest::newRow("bad-value") << QByteArray("MD[m],GR[API]\n100,oops\n") << 2;
    QTest::newRow("duplicate-depth") << QByteArray("MD[m],GR[API]\n100,1\n100,2\n") << 3;
    QTest::newRow("descending-depth") << QByteArray("MD[m],GR[API]\n100,1\n99,2\n") << 3;
    QTest::newRow("missing-index") << QByteArray("X,GR\n1,2\n") << 1;
    QTest::newRow("infinite-index") << QByteArray("MD,GR\ninf,2\n") << 2;
    QTest::newRow("infinite-value") << QByteArray("MD,GR\n1,inf\n") << 2;
    QTest::newRow("short-row") << QByteArray("MD,GR\n1\n") << 2;
    QTest::newRow("blank-index") << QByteArray("MD,GR\n,2\n") << 2;
    QTest::newRow("quote") << QByteArray("MD,GR\n1,\"2\n") << 2;
    QTest::newRow("metadata-conflict") << QByteArray("# Well: A1\n# Well: B1\nMD,GR\n1,2\n") << 2;
    QTest::newRow("duplicate-curve") << QByteArray("MD,GR,gr\n1,2,3\n") << 1;
  }
  void malformedText()
  {
    QFETCH(QByteArray, content);
    QFETCH(int, line);
    QTemporaryDir dir;
    const auto path = dir.filePath(QStringLiteral("bad.csv"));
    QVERIFY(write(path, content));
    QList<LasIssue> issues;
    const auto doc = WellLogRead::parseDoc(path, &issues);
    QVERIFY(!doc.ok);
    QVERIFY(doc.curves.isEmpty());
    QVERIFY(doc.error.contains(path));
    QVERIFY(!issues.isEmpty());
    QCOMPARE(issues.last().line, line);
    QCOMPARE(issues.last().severity, LasIssue::Severity::Error);
  }

  void xmlExactUnionAndUnits()
  {
    QTemporaryDir dir;
    const auto path = dir.filePath(QStringLiteral("well.csv"));
    const QString sheets = xmlSheet(QStringLiteral("测井曲线-A1"),
        {{"井号", "MD[m]", "GR[API]"}, {"A1", "1000.00000001", "10"}, {"A1", "1001", "20"}}) +
        xmlSheet(QStringLiteral("离散曲线-A1"),
        {{"井号", "MD[m]", "POR[%]"}, {"A1", "1000.5", "5"}, {"A1", "1001", ""}});
    QVERIFY(write(path, workbook(sheets)));
    QCOMPARE(WellLogRead::detect(path), WellLogFormat::SpreadsheetXml);
    LasHeaderInfo header;
    QVERIFY(WellLogRead::parseHeader(path, header));
    QCOMPARE(header.wellName, QStringLiteral("A1"));
    QCOMPARE(header.indexBasis, QStringLiteral("MD"));
    const auto doc = WellLogRead::parseDoc(path);
    QVERIFY2(doc.ok, qPrintable(doc.error));
    QCOMPARE(doc.curveNames, QStringList({"MD", "GR", "POR"}));
    QCOMPARE(doc.curves.first().values, QVector<double>({1000.00000001, 1000.5, 1001}));
    QCOMPARE(doc.curves.at(1).values.at(0), 10.0);
    QVERIFY(std::isnan(doc.curves.at(1).values.at(1)));
    QCOMPARE(doc.curves.at(2).values.at(1), 5.0);
    QVERIFY(std::isnan(doc.curves.at(2).values.at(0)));
    QCOMPARE(doc.curves.at(2).unit, QStringLiteral("%"));
  }

  void existingRepositoryCompositeFixture()
  {
    const QString path = QFINDTESTDATA("fixtures/outsource/composite_sheet.xml");
    QVERIFY(!path.isEmpty());
    LasHeaderInfo header;
    QList<LasCurve> curves;
    QString error;
    QVERIFY2(WellLogRead::parse(path, header, curves, &error), qPrintable(error));
    QCOMPARE(header.wellName, QStringLiteral("A1"));
    QCOMPARE(header.curveNames, QStringList({QStringLiteral("深度"), QStringLiteral("GR")}));
    // The repository's original fixture declares neither an axis basis nor units.
    // Retain its values without inventing metres, MD or an inferred GR unit.
    QVERIFY(header.indexBasis.isEmpty());
    QCOMPARE(curves.first().values, QVector<double>({1000, 1010, 1020}));
    QCOMPARE(curves.at(1).values, QVector<double>({45, 50, 55}));
    QVERIFY(curves.first().unit.isEmpty());
    QVERIFY(curves.at(1).unit.isEmpty());
  }

  void xmlCurveSheetBeyondProbe()
  {
    QTemporaryDir dir;
    const auto path = dir.filePath(QStringLiteral("late.txt"));
    const QString sheets = xmlSheet(QStringLiteral("说明"),
        {{"说明"}, {QString(70000, QLatin1Char('x'))}}) +
        xmlSheet(QStringLiteral("测井曲线-A1"),
        {{"井号", "深度(m)", "GR(API)"}, {"A1", "1000.25", "20"}});
    const auto bytes = workbook(sheets);
    QVERIFY(!bytes.left(64 * 1024).contains(QStringLiteral("测井曲线").toUtf8()));
    QVERIFY(WellLogText::sniffSpreadsheetXml(bytes.left(64 * 1024)));
    QVERIFY(write(path, bytes));
    QCOMPARE(WellLogRead::detect(path), WellLogFormat::SpreadsheetXml);
    LasHeaderInfo header;
    QList<LasCurve> curves;
    QString error;
    QVERIFY2(WellLogRead::parse(path, header, curves, &error), qPrintable(error));
    QCOMPARE(header.wellName, QStringLiteral("A1"));
    QCOMPARE(header.indexBasis, QString());
    QCOMPARE(curves.at(0).unit, QStringLiteral("m"));
    QCOMPARE(curves.at(1).unit, QStringLiteral("API"));
    QCOMPARE(curves.at(0).values.first(), 1000.25);
  }

  void xmlFailures_data()
  {
    QTest::addColumn<QByteArray>("content");
    const auto base = xmlSheet(QStringLiteral("测井曲线-A1"), {{"井号", "MD[m]", "GR[API]"}, {"A1", "1", "2"}});
    QTest::newRow("bad-value") << workbook(xmlSheet(QStringLiteral("测井曲线-A1"), {{"井号", "MD[m]", "GR[API]"}, {"A1", "1", "bad"}}));
    QTest::newRow("mixed-wells") << workbook(xmlSheet(QStringLiteral("测井曲线-A1"), {{"井号", "MD[m]", "GR[API]"}, {"A1", "1", "2"}, {"B1", "2", "3"}}));
    QTest::newRow("different-units") << workbook(base + xmlSheet(QStringLiteral("离散曲线"), {{"MD[ft]", "POR[%]"}, {"1", "2"}}));
    QTest::newRow("different-reference") << workbook(base + xmlSheet(QStringLiteral("离散曲线"), {{"TIME[ms]", "POR[%]"}, {"1", "2"}}));
    QTest::newRow("duplicate-name") << workbook(base + xmlSheet(QStringLiteral("离散曲线"), {{"MD[m]", "GR[API]"}, {"1", "2"}}));
    QTest::newRow("unknown-xml") << QByteArray("<logs><log><depth>1</depth></log></logs>");
    QTest::newRow("truncated") << workbook(base).chopped(12);
    QTest::newRow("unknown-sheet") << workbook(xmlSheet(QStringLiteral("Anything"), {{"MD", "GR"}, {"1", "2"}}));
    QTest::newRow("bad-cell-index") << workbook(QStringLiteral("<Worksheet ss:Name=\"测井曲线\"><Table><Row><Cell ss:Index=\"0\"><Data>MD</Data></Cell></Row></Table></Worksheet>"));
  }

  void explicitSheetWellIdentity_data()
  {
    QTest::addColumn<QString>("sheets");
    QTest::addColumn<bool>("ok");
    QTest::addColumn<QString>("well");
    const QList<QStringList> gr = {{"MD[m]", "GR[API]"}, {"1", "2"}};
    const QList<QStringList> por = {{"MD[m]", "POR[%]"}, {"1", "3"}};
    QTest::newRow("explicit-suffix") << xmlSheet(QStringLiteral("测井曲线-A1井"), gr) << true << QStringLiteral("A1");
    QTest::newRow("discrete-suffix") << xmlSheet(QStringLiteral("离散曲线-A1井"), por) << true << QStringLiteral("A1");
    QTest::newRow("one-terminal-character") << xmlSheet(QStringLiteral("测井曲线-A井井"), gr) << true << QStringLiteral("A井");
    QTest::newRow("no-suffix") << xmlSheet(QStringLiteral("测井曲线"), gr) << true << QString();
    QTest::newRow("unmarked-suffix") << xmlSheet(QStringLiteral("测井曲线-A1"), gr) << true << QString();
    QTest::newRow("same-sheet-identities") << (xmlSheet(QStringLiteral("测井曲线-A1井"), gr) + xmlSheet(QStringLiteral("离散曲线-A1井"), por)) << true << QStringLiteral("A1");
    QTest::newRow("different-sheet-identities") << (xmlSheet(QStringLiteral("测井曲线-A1井"), gr) + xmlSheet(QStringLiteral("离散曲线-B1井"), por)) << false << QString();
    QTest::newRow("suffix-vs-column") << xmlSheet(QStringLiteral("测井曲线-A1井"), {{"井号", "MD[m]", "GR[API]"}, {"B1", "1", "2"}}) << false << QString();
    QTest::newRow("prior-column-vs-later-suffix") << (xmlSheet(QStringLiteral("测井曲线"), {{"井号", "MD[m]", "GR[API]"}, {"B1", "1", "2"}}) + xmlSheet(QStringLiteral("离散曲线-A1井"), por)) << false << QString();
  }

  void explicitSheetWellIdentity()
  {
    QFETCH(QString, sheets);
    QFETCH(bool, ok);
    QFETCH(QString, well);
    QTemporaryDir dir;
    const auto path = dir.filePath(QStringLiteral("neutral-name.xml"));
    QVERIFY(write(path, workbook(sheets)));
    LasHeaderInfo header;
    QList<LasCurve> curves;
    QString error;
    QCOMPARE(WellLogRead::parse(path, header, curves, &error), ok);
    QCOMPARE(header.wellName, well);
    if (!ok)
    {
      QVERIFY(!error.isEmpty());
      QVERIFY(curves.isEmpty());
    }
  }

  void unifiedParseClearsFailures()
  {
    QTemporaryDir dir;
    const auto path = dir.filePath(QStringLiteral("bad.csv"));
    QVERIFY(write(path, "# Well: A1\nMD[m],GR[API]\n1,bad\n"));
    LasHeaderInfo header;
    header.wellName = QStringLiteral("stale");
    QList<LasCurve> curves = {{QStringLiteral("stale"), {}, {}, {1.0}}};
    QString error;
    QVERIFY(!WellLogRead::parse(path, header, curves, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(header.wellName.isEmpty());
    QVERIFY(curves.isEmpty());
  }
  void xmlFailures()
  {
    QFETCH(QByteArray, content);
    QTemporaryDir dir;
    const auto path = dir.filePath(QStringLiteral("bad.xml"));
    QVERIFY(write(path, content));
    const auto doc = WellLogRead::parseDoc(path);
    QVERIFY(!doc.ok);
    QVERIFY(!doc.error.isEmpty());
    QVERIFY(doc.curves.isEmpty());
  }

  void unknownAndSizeBudget()
  {
    QTemporaryDir dir;
    const auto unknown = dir.filePath(QStringLiteral("unknown.blob"));
    QVERIFY(write(unknown, "arbitrary text"));
    QCOMPARE(WellLogRead::detect(unknown), WellLogFormat::Unknown);
    QVERIFY(!WellLogRead::parseDoc(unknown).ok);
    const auto oversized = dir.filePath(QStringLiteral("large.csv"));
    QFile file(oversized);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.resize(64LL * 1024 * 1024 + 1));
    file.close();
    QList<LasIssue> issues;
    QVERIFY(!WellLogRead::parseDoc(oversized, &issues).ok);
    QCOMPARE(issues.last().category, LasIssue::Category::Oversize);
  }

  void sparseUnionBudget()
  {
    // Each table individually fits. Only the exact union's dense output would
    // exceed the budget, so this guards the Cartesian memory amplification.
    QVector<WellLogText::Detail::Table> tables;
    for (int sheet = 0; sheet < 2; ++sheet)
    {
      WellLogText::Detail::Table table;
      table.headers.append(QStringLiteral("MD[m]"));
      for (int c = 0; c < 100; ++c)
        table.headers.append(QStringLiteral("C%1_%2[API]").arg(sheet).arg(c));
      QStringList row;
      for (int c = 0; c < 101; ++c) row.append(QStringLiteral("1"));
      for (int r = 0; r < 11000; ++r)
      {
        row[0] = QString::number(r * 2 + sheet);
        table.rows.append(row);
      }
      tables.append(table);
    }
    LasHeaderInfo header;
    QList<LasCurve> curves;
    QList<LasIssue> issues;
    QString error;
    QVERIFY(!WellLogText::Detail::convert(QStringLiteral("union.xml"), tables, header, curves, &error, &issues));
    QVERIFY(curves.isEmpty());
    QCOMPARE(issues.last().category, LasIssue::Category::Oversize);
  }

  void gb18030WellNameAndChineseIndexDecode()
  {
    // A-02：井表生态同口径——GB18030 中文井名/表头的文本曲线必须可读回。
    // GBK 字节：某=C4B3、井=BEAE（表头用 MD——「深度」有意不声明基准）。
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("gb.csv"));
    const QByteArray gbBytes = QByteArray("# Well: ")
        + QByteArray::fromHex("c4b3") + QByteArray("1") + QByteArray::fromHex("beae")
        + QByteArray("\n")
        + QByteArray("MD[m],")
        + QByteArray::fromHex("beae") // 井：曲线名带中文，GB 解码全链生效
        + QByteArray("-GR[API]\n1000,20\n1001,30\n");
    QVERIFY(write(path, gbBytes));
    QCOMPARE(WellLogRead::detect(path), WellLogFormat::Text);
    LasHeaderInfo header;
    QString error;
    QVERIFY2(WellLogRead::parseHeader(path, header, &error), qPrintable(error));
    QCOMPARE(header.wellName, QStringLiteral("某1井"));
    QCOMPARE(header.indexBasis, QStringLiteral("MD"));
    const auto doc = WellLogRead::parseDoc(path);
    QVERIFY2(doc.ok, qPrintable(doc.error));
    QCOMPARE(doc.curves.at(0).values.size(), 2);
    QCOMPARE(doc.curves.at(0).values.at(0), 1000.0);
    QCOMPARE(doc.curves.at(0).unit, QStringLiteral("m"));
  }

  void jingshenIndexAndSemicolonDelimiterRoundTrip()
  {
    // A-03：分类器提议过的词表读取器必须读得回——井深表头、分号分隔。
    QTemporaryDir dir;
    const QString jingshen = dir.filePath(QStringLiteral("js.csv"));
    QVERIFY(write(jingshen, "井深[m],GR[API]\n100,5\n101,6\n"));
    QCOMPARE(WellLogRead::detect(jingshen), WellLogFormat::Text);
    const auto js = WellLogRead::parseDoc(jingshen);
    QVERIFY2(js.ok, qPrintable(js.error));
    QCOMPARE(js.curveNames, QStringList({"井深", "GR"}));
    QCOMPARE(js.curves.at(0).values.at(0), 100.0);

    const QString semi = dir.filePath(QStringLiteral("semi.csv"));
    QVERIFY(write(semi, "DEPTH[m];GR[API]\n100;5\n101;6\n"));
    QCOMPARE(WellLogRead::detect(semi), WellLogFormat::Text);
    const auto sd = WellLogRead::parseDoc(semi);
    QVERIFY2(sd.ok, qPrintable(sd.error));
    QCOMPARE(sd.curveNames, QStringList({"DEPTH", "GR"}));
    QCOMPARE(sd.curves.at(1).values.at(1), 6.0);
  }

  void wideWhitespaceLineFailsFast()
  {
    // A-06：空白分隔超宽单行在 256 列预算处早退——修前先物化整行 QStringList，
    // 错误落在第 2 行「列数与表头不一致」；修后在表头行即「列数超过上限」。
    QByteArray header = QByteArrayLiteral("MD");
    for (int i = 0; i < 300; ++i)
      header += QByteArray(" C") + QByteArray::number(i);
    header += QByteArray("\n100 1 2 3\n");
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("wide.txt"));
    QVERIFY(write(path, header));
    QList<LasIssue> issues;
    const auto doc = WellLogRead::parseDoc(path, &issues);
    QVERIFY(!doc.ok);
    QVERIFY2(doc.error.contains(QStringLiteral("列数超过上限")),
             qPrintable(doc.error));
  }
};
QTEST_GUILESS_MAIN(TestWellLogText)
#include "tst_welllogtext.moc"
