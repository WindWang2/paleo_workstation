// 层：测试壳
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cmath>

#include "domain/wellnumeric.h"
#include "domain/welltopsedit.h"
#include "io/wellfileparsers.h"
#include "io/timedeptool.h"

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
  void reportsOptionalMissingCellsWithoutRejectingGoodDepth();
  void depthAliases_data();
  void depthAliases();
};

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

QTEST_GUILESS_MAIN(WellFileParserTests)
#include "tst_wellfileparsers.moc"
