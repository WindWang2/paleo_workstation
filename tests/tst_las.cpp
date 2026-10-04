#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <cmath>

#include "io/lasparser.h"

class TestLas : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void testMoreThan64CurvesDepthRange();
    void testMoreThan64CurvesRangeAndFull();
    void testUtf8BomSeekZero();
    void testNullValueHandling();
    void testExtremeCurveCount150();
    void testUtf8BomExactByteAlignment();
    void testEmptyLinesTrailingWhitespaceVariableColumns();
    void testDepthRangeAcrossChunkBoundaries_data();
    void testDepthRangeAcrossChunkBoundaries();
    void testWhitespaceOnlyAndCtrlZDoNotCreatePhantomRows();

  private:
    QTemporaryDir m_tempDir;
    QString createSyntheticLas(const QString &fileName, int numCurves, int numRows, bool withBom = false);
};

void TestLas::initTestCase()
{
  QVERIFY(m_tempDir.isValid());
}

QString TestLas::createSyntheticLas(const QString &fileName, int numCurves, int numRows, bool withBom)
{
  const QString filePath = m_tempDir.filePath(fileName);
  QFile file(filePath);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return QString();

  if (withBom)
  {
    const char bom[] = "\xEF\xBB\xBF";
    file.write(bom, 3);
  }

  QTextStream out(&file);
  out << "~Version Information\n";
  out << " VERS.                  2.0:   CWLS log ASCII Standard -VERSION 2.0\n";
  out << " WRAP.                   NO:   One line per depth step\n";
  out << "~Well Information Block\n";
  out << " STRT.M        1000.0000:\n";
  out << " STOP.M        " << QString::number(1000.0 + (numRows - 1)) << ":\n";
  out << " STEP.M          1.0000:\n";
  out << " NULL.        -999.2500:\n";
  out << " WELL.         TEST_WELL:\n";
  out << "~Curve Information Block\n";
  out << " DEPT.M                  :   DEPTH INDEX\n";
  for (int c = 1; c < numCurves; ++c)
  {
    out << QString(" C%1.                 :   CURVE %1\n").arg(c);
  }
  out << "~Parameter Information Block\n";
  out << "~Other Information\n";
  out << "~A\n";

  for (int r = 0; r < numRows; ++r)
  {
    const double depth = 1000.0 + r;
    out << QString::number(depth, 'f', 2);
    for (int c = 1; c < numCurves; ++c)
    {
      const double val = c * 100.0 + r;
      out << " " << QString::number(val, 'f', 2);
    }
    out << "\n";
  }

  file.close();
  return filePath;
}

void TestLas::testMoreThan64CurvesDepthRange()
{
  // BIZ-01 / P1-01: Verify parseDepthRange correctly parses files with > 64 curves
  // (e.g. 70 curves). Previously failed due to stack array rowVals[64].
  const int nCurves = 70;
  const int nRows = 10;
  const QString path = createSyntheticLas("curves70.las", nCurves, nRows);
  QVERIFY(!path.isEmpty());

  QStringList curveNames;
  QList<LasCurve> curves;
  QString error;

  bool ok = LasParser::parseDepthRange(path, 1000.0, 1004.0, curveNames, curves, &error);
  QVERIFY2(ok, qPrintable(error));
  QCOMPARE(curveNames.size(), nCurves);
  QCOMPARE(curves.size(), nCurves);

  // Check row count for depth window [1000.0, 1004.0] -> 5 rows
  for (int c = 0; c < nCurves; ++c)
  {
    QCOMPARE(curves[c].values.size(), 5);
  }

  // Verify DEPT (curve 0)
  for (int r = 0; r < 5; ++r)
  {
    QVERIFY(!std::isnan(curves[0].values[r]));
    QCOMPARE(curves[0].values[r], 1000.0 + r);
  }

  // Verify curve 63 (index 63)
  for (int r = 0; r < 5; ++r)
  {
    QVERIFY(!std::isnan(curves[63].values[r]));
    QCOMPARE(curves[63].values[r], 63 * 100.0 + r);
  }

  // Verify curves beyond 64: curve 64 (index 64) and curve 69 (index 69)
  for (int r = 0; r < 5; ++r)
  {
    QVERIFY2(!std::isnan(curves[64].values[r]), "Curve 64 value must not be NaN");
    QCOMPARE(curves[64].values[r], 64 * 100.0 + r);

    QVERIFY2(!std::isnan(curves[69].values[r]), "Curve 69 value must not be NaN");
    QCOMPARE(curves[69].values[r], 69 * 100.0 + r);
  }
}

void TestLas::testMoreThan64CurvesRangeAndFull()
{
  const int nCurves = 72;
  const int nRows = 8;
  const QString path = createSyntheticLas("curves72.las", nCurves, nRows);
  QVERIFY(!path.isEmpty());

  // Test parseRange
  QStringList curveNames;
  QList<LasCurve> curves;
  QString error;
  bool ok = LasParser::parseRange(path, 0, 8, curveNames, curves, &error);
  QVERIFY2(ok, qPrintable(error));
  QCOMPARE(curveNames.size(), nCurves);
  QCOMPARE(curves.size(), nCurves);

  // Check curve 70
  QCOMPARE(curves[70].values.size(), 8);
  for (int r = 0; r < 8; ++r)
  {
    QVERIFY(!std::isnan(curves[70].values[r]));
    QCOMPARE(curves[70].values[r], 70 * 100.0 + r);
  }

  // Test parseDoc
  LasDoc doc = LasParser::parseDoc(path);
  QVERIFY(doc.ok);
  QCOMPARE(doc.curveNames.size(), nCurves);
  QCOMPARE(doc.curves.size(), nCurves);
  QCOMPARE(doc.curves[71].values.size(), 8);
  QCOMPARE(doc.curves[71].values[0], 71 * 100.0);
}

void TestLas::testUtf8BomSeekZero()
{
  // BIZ-02 / P1-02: Verify 3-byte UTF-8 BOM is properly handled with seek(0)
  // in parseRange and parseDepthRange.
  const int nCurves = 10;
  const int nRows = 6;
  const QString path = createSyntheticLas("bom_test.las", nCurves, nRows, true /* withBom */);
  QVERIFY(!path.isEmpty());

  // Verify file has BOM
  {
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray head = f.read(3);
    QCOMPARE(head, QByteArray("\xEF\xBB\xBF"));
  }

  // 1. parseRange with BOM
  QStringList curveNames;
  QList<LasCurve> curves;
  QString error;
  bool ok = LasParser::parseRange(path, 0, 4, curveNames, curves, &error);
  QVERIFY2(ok, qPrintable(error));
  QCOMPARE(curveNames.size(), nCurves);
  QCOMPARE(curves.size(), nCurves);
  QCOMPARE(curves[0].values.size(), 4);
  QCOMPARE(curves[0].values[0], 1000.0);
  QCOMPARE(curves[0].values[3], 1003.0);
  QCOMPARE(curves[1].values[0], 100.0);

  // 2. parseDepthRange with BOM
  curveNames.clear();
  curves.clear();
  ok = LasParser::parseDepthRange(path, 1001.0, 1003.0, curveNames, curves, &error);
  QVERIFY2(ok, qPrintable(error));
  QCOMPARE(curveNames.size(), nCurves);
  QCOMPARE(curves.size(), nCurves);
  QCOMPARE(curves[0].values.size(), 3);
  QCOMPARE(curves[0].values[0], 1001.0);
  QCOMPARE(curves[0].values[2], 1003.0);
  QCOMPARE(curves[5].values[0], 500.0 + 1);

  // 3. Sequential repeated calls to verify seek(0) properly resets file pointer every time
  for (int iter = 0; iter < 3; ++iter)
  {
    curveNames.clear();
    curves.clear();
    ok = LasParser::parseDepthRange(path, 1000.0, 1002.0, curveNames, curves, &error);
    QVERIFY2(ok, qPrintable(error));
    QCOMPARE(curves[0].values.size(), 3);
    QCOMPARE(curves[0].values[0], 1000.0);

    curveNames.clear();
    curves.clear();
    ok = LasParser::parseRange(path, 1, 3, curveNames, curves, &error);
    QVERIFY2(ok, qPrintable(error));
    QCOMPARE(curves[0].values.size(), 2);
    QCOMPARE(curves[0].values[0], 1001.0);
  }
}

void TestLas::testNullValueHandling()
{
  const QString filePath = m_tempDir.filePath("null_test.las");
  QFile file(filePath);
  QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  QTextStream out(&file);
  out << "~Version Information\n";
  out << " VERS.                  2.0:\n";
  out << " WRAP.                   NO:\n";
  out << "~Well Information Block\n";
  out << " NULL.        -999.2500:\n";
  out << " WELL.         NULL_WELL:\n";
  out << "~Curve Information Block\n";
  out << " DEPT.M                  :   DEPTH\n";
  out << " GR.GAPI                 :   GAMMA\n";
  out << "~A\n";
  out << " 100.00 45.0\n";
  out << " 101.00 -999.25\n";
  out << " 102.00 55.0\n";
  file.close();

  QStringList curveNames;
  QList<LasCurve> curves;
  QString error;
  bool ok = LasParser::parseDepthRange(filePath, 100.0, 102.0, curveNames, curves, &error);
  QVERIFY2(ok, qPrintable(error));
  QCOMPARE(curves.size(), 2);
  QCOMPARE(curves[1].values.size(), 3);
  QCOMPARE(curves[1].values[0], 45.0);
  QVERIFY(std::isnan(curves[1].values[1]));
  QCOMPARE(curves[1].values[2], 55.0);
}

void TestLas::testExtremeCurveCount150()
{
  // Adversarial stress test: extreme curve count (150 curves > 64/100).
  const int nCurves = 150;
  const int nRows = 25;
  const QString path = createSyntheticLas("curves150.las", nCurves, nRows);
  QVERIFY(!path.isEmpty());

  // 1. parseDepthRange window [1000.0, 1010.0] -> 11 rows
  QStringList curveNames;
  QList<LasCurve> curves;
  QString error;
  bool ok = LasParser::parseDepthRange(path, 1000.0, 1010.0, curveNames, curves, &error);
  QVERIFY2(ok, qPrintable(error));
  QCOMPARE(curveNames.size(), nCurves);
  QCOMPARE(curves.size(), nCurves);
  for (int c = 0; c < nCurves; ++c)
  {
    QCOMPARE(curves[c].values.size(), 11);
  }
  // Check curve 0 (DEPT)
  for (int r = 0; r < 11; ++r)
  {
    QCOMPARE(curves[0].values[r], 1000.0 + r);
  }
  // Check boundary and high curves (63, 64, 100, 149)
  for (int r = 0; r < 11; ++r)
  {
    QCOMPARE(curves[63].values[r], 63 * 100.0 + r);
    QCOMPARE(curves[64].values[r], 64 * 100.0 + r);
    QCOMPARE(curves[100].values[r], 100 * 100.0 + r);
    QCOMPARE(curves[149].values[r], 149 * 100.0 + r);
  }

  // 2. parseRange (full 25 rows)
  curveNames.clear();
  curves.clear();
  ok = LasParser::parseRange(path, 0, 25, curveNames, curves, &error);
  QVERIFY2(ok, qPrintable(error));
  QCOMPARE(curveNames.size(), nCurves);
  QCOMPARE(curves.size(), nCurves);
  QCOMPARE(curves[149].values.size(), 25);
  QCOMPARE(curves[149].values[24], 149 * 100.0 + 24);

  // 3. parseDoc
  LasDoc doc = LasParser::parseDoc(path);
  QVERIFY(doc.ok);
  QCOMPARE(doc.curveNames.size(), nCurves);
  QCOMPARE(doc.curves.size(), nCurves);
  QCOMPARE(doc.curves[149].values.size(), 25);
  QCOMPARE(doc.curves[149].values[0], 149 * 100.0);

  // 4. legacy parse()
  curveNames.clear();
  curves.clear();
  ok = LasParser::parse(path, curveNames, curves, &error);
  QVERIFY2(ok, qPrintable(error));
  QCOMPARE(curves.size(), nCurves);
  QCOMPARE(curves[149].values.size(), 25);
  QCOMPARE(curves[149].values[24], 149 * 100.0 + 24);
}

void TestLas::testUtf8BomExactByteAlignment()
{
  // Adversarial stress test: Ensure that with a 3-byte UTF-8 BOM,
  // data in ~A is read from the exact starting byte without 3-byte drift.
  const QString path = m_tempDir.filePath("bom_drift_check.las");
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  const char bom[] = "\xEF\xBB\xBF";
  file.write(bom, 3);
  QTextStream out(&file);
  out << "~Version Information\n";
  out << " VERS.                  2.0:\n";
  out << " WRAP.                   NO:\n";
  out << "~Well Information Block\n";
  out << " NULL.        -999.2500:\n";
  out << " WELL.         BOM_TEST:\n";
  out << "~Curve Information Block\n";
  out << " DEPT.M                  :   DEPTH\n";
  out << " GR.GAPI                 :   GAMMA\n";
  out << "~A\n";
  // The first row starts immediately after ~A\n.
  // We use distinct multi-digit values:
  // If there were a 3-byte drift forward, "1234.50" would become "4.50" or fail.
  out << "1234.50 88.25\n";
  out << "1235.50 89.25\n";
  file.close();

  // Test parseDepthRange
  QStringList names;
  QList<LasCurve> curves;
  QString error;
  bool ok = LasParser::parseDepthRange(path, 1234.50, 1235.50, names, curves, &error);
  QVERIFY2(ok, qPrintable(error));
  QCOMPARE(curves.size(), 2);
  QCOMPARE(curves[0].values.size(), 2);
  QCOMPARE(curves[0].values[0], 1234.50);
  QCOMPARE(curves[1].values[0], 88.25);
  QCOMPARE(curves[0].values[1], 1235.50);
  QCOMPARE(curves[1].values[1], 89.25);

  // Test parseRange
  names.clear();
  curves.clear();
  ok = LasParser::parseRange(path, 0, 2, names, curves, &error);
  QVERIFY2(ok, qPrintable(error));
  QCOMPARE(curves.size(), 2);
  QCOMPARE(curves[0].values.size(), 2);
  QCOMPARE(curves[0].values[0], 1234.50);
  QCOMPARE(curves[1].values[0], 88.25);

  // Test parseDoc
  LasDoc doc = LasParser::parseDoc(path);
  QVERIFY(doc.ok);
  QCOMPARE(doc.curves.size(), 2);
  QCOMPARE(doc.curves[0].values.size(), 2);
  QCOMPARE(doc.curves[0].values[0], 1234.50);
  QCOMPARE(doc.curves[1].values[0], 88.25);
}

void TestLas::testEmptyLinesTrailingWhitespaceVariableColumns()
{
  // Adversarial stress test: empty lines, whitespace-only lines, trailing whitespace,
  // tabs, variable columns (truncated columns, extra columns).
  const QString path = m_tempDir.filePath("malformed_rows.las");
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  QTextStream out(&file);
  out << "~Version Information\r\n";
  out << " VERS.                  2.0:\r\n";
  out << " WRAP.                   NO:\r\n";
  out << "~Well Information Block\r\n";
  out << " NULL.        -999.2500:\r\n";
  out << " WELL.         DIRTY_DATA:\r\n";
  out << "~Curve Information Block\r\n";
  out << " DEPT.M                  :   DEPTH\r\n";
  out << " GR.GAPI                 :   GAMMA\r\n";
  out << " NPHI.V/V                :   POROSITY\r\n";
  out << " RHOB.G/C3               :   DENSITY\r\n";
  out << "~A\r\n";
  // Row 0: Trailing spaces and tabs
  out << "1000.00\t45.00  0.1500  2.4500   \t  \r\n";
  // Row 1: Completely empty line
  out << "\r\n";
  // Row 2: Whitespace only line
  out << "   \t   \r\n";
  // Row 3: Truncated columns (only 2 columns given, curves 2 and 3 missing)
  out << "1001.00  50.00 \r\n";
  // Row 4: Extra columns (6 columns given, curves have only 4)
  out << "1002.00  55.00  0.2500  2.5500  999.00  888.00\r\n";
  // Row 5: Row with NULL value in column 1
  out << "1003.00  -999.2500  0.3000  2.6000\r\n";
  // Row 6: Empty line at end of file
  out << "\r\n";
  file.close();

  // Test parseDepthRange across the entire depth interval [1000.0, 1003.0]
  QStringList names;
  QList<LasCurve> curves;
  QString error;
  QList<LasIssue> issues;
  bool ok = LasParser::parseDepthRange(path, 1000.0, 1003.0, names, curves, &error, &issues);
  QVERIFY2(ok, qPrintable(error));
  QCOMPARE(names.size(), 4);
  QCOMPARE(curves.size(), 4);

  // We expect 4 valid depth rows (1000, 1001, 1002, 1003).
  // Empty lines and whitespace-only lines without depth should not produce rows in parseDepthRange.
  QCOMPARE(curves[0].values.size(), 4);
  QCOMPARE(curves[0].values[0], 1000.0);
  QCOMPARE(curves[0].values[1], 1001.0);
  QCOMPARE(curves[0].values[2], 1002.0);
  QCOMPARE(curves[0].values[3], 1003.0);

  // Row 0 (depth 1000.0): all 4 columns present
  QCOMPARE(curves[1].values[0], 45.0);
  QCOMPARE(curves[2].values[0], 0.15);
  QCOMPARE(curves[3].values[0], 2.45);

  // Row 1 (depth 1001.0): truncated (columns 2 and 3 must be NaN)
  QCOMPARE(curves[1].values[1], 50.0);
  QVERIFY(std::isnan(curves[2].values[1]));
  QVERIFY(std::isnan(curves[3].values[1]));

  // Row 2 (depth 1002.0): extra columns ignored, 4 columns intact
  QCOMPARE(curves[1].values[2], 55.0);
  QCOMPARE(curves[2].values[2], 0.25);
  QCOMPARE(curves[3].values[2], 2.55);

  // Row 3 (depth 1003.0): column 1 was NULL (-999.25), mapped to NaN
  QVERIFY(std::isnan(curves[1].values[3]));
  QCOMPARE(curves[2].values[3], 0.30);
  QCOMPARE(curves[3].values[3], 2.60);

  // Test parseDoc
  issues.clear();
  LasDoc doc = LasParser::parseDoc(path, &issues);
  QVERIFY(doc.ok);
  QCOMPARE(doc.curves.size(), 4);
  // Verify Truncated issue was reported
  bool sawTruncatedIssue = false;
  for (const LasIssue &issue : issues)
  {
    if (issue.category == LasIssue::Category::Truncated)
      sawTruncatedIssue = true;
  }
  QVERIFY2(sawTruncatedIssue, "Should report LasIssue::Category::Truncated for malformed rows");
  // #167：纯空白行不得变成深度为 NaN 的幽灵行。
  QCOMPARE(doc.curves[0].values.size(), 4);
  for (double d : doc.curves[0].values)
    QVERIFY(std::isfinite(d));
}

// #167：纯空白行 + DOS 尾部 Ctrl-Z（0x1A）——parseDoc 与 parse 都只出 3 行，
// 深度无 NaN（旧代码 parseDoc 出 5 行 "100 100.5 nan 101 nan"）。
void TestLas::testWhitespaceOnlyAndCtrlZDoNotCreatePhantomRows()
{
  const QString path = m_tempDir.filePath("ws_ctrlz.las");
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  file.write("~V\nVERS. 2.0 :\nWRAP. NO :\n~W\nNULL. -999.25 :\nWELL. W1 :\n"
             "~C\nDEPT.M :\nGR.API :\n~A\n100 10\n100.5 20\n   \t \n101 30\n\x1a");
  file.close();

  QList<LasIssue> issues;
  const LasDoc doc = LasParser::parseDoc(path, &issues);
  QVERIFY(doc.ok);
  QCOMPARE(doc.curves.size(), 2);
  QCOMPARE(doc.curves[0].values, (QVector<double>{100.0, 100.5, 101.0}));
  QCOMPARE(doc.curves[1].values, (QVector<double>{10.0, 20.0, 30.0}));
  for (const LasIssue &issue : issues)
    QVERIFY2(issue.category != LasIssue::Category::Truncated, qPrintable(issue.message));

  QStringList names;
  QList<LasCurve> curves;
  QString error;
  QVERIFY2(LasParser::parse(path, names, curves, &error), qPrintable(error));
  QCOMPARE(curves.size(), 2);
  QCOMPARE(curves[0].values, (QVector<double>{100.0, 100.5, 101.0}));
}

void TestLas::testDepthRangeAcrossChunkBoundaries_data()
{
  // #78：> 8MB（跨 2 个以上 4MB 块）。不同行宽让块边界落在不同 token 上：
  // pad=1 时旧实现子区间读出 0 行；pad=5 时全区间静默截断到 292365 行。
  QTest::addColumn<int>("pad");
  QTest::newRow("pad1") << 1;
  QTest::newRow("pad5") << 5;
}

void TestLas::testDepthRangeAcrossChunkBoundaries()
{
  QFETCH(int, pad);
  const int rows = 450000;
  const QString path = m_tempDir.filePath(QStringLiteral("chunk_boundary_%1.las").arg(pad));
  {
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("~Version\nVERS. 2.0 :\nWRAP. NO :\n~Well\nNULL. -999.25 :\nWELL. W1 :\n"
            "~Curve\nDEPT.M :\nGR.API :\nRT.OHMM :\n~A\n");
    const QByteArray spaces(pad, ' ');
    QByteArray buf;
    for (int i = 0; i < rows; ++i)
    {
      buf += QByteArray::number(1000.0 + i * 0.1, 'f', 1) + ' ' +
             QByteArray::number(50.0 + (i % 7), 'f', 4) + spaces + ' ' +
             QByteArray::number(10.0 + (i % 3), 'f', 4) + '\n';
      if (buf.size() > (1 << 20))
      {
        f.write(buf);
        buf.clear();
      }
    }
    f.write(buf);
    QVERIFY(f.size() > 8 * 1024 * 1024);
  }

  QString error;
  QStringList fullNames;
  QList<LasCurve> full;
  QVERIFY2(LasParser::parse(path, fullNames, full, &error), qPrintable(error));
  QCOMPARE(full.size(), 3);
  QCOMPARE(full[0].values.size(), qsizetype(rows));

  QStringList names;
  QList<LasCurve> ranged;
  QVERIFY2(LasParser::parseDepthRange(path, 0.0, 1e9, names, ranged, &error), qPrintable(error));
  QCOMPARE(ranged.size(), 3);
  QCOMPARE(ranged[0].values.size(), full[0].values.size());
  for (int c = 0; c < 3; ++c)
    for (qsizetype r = 0; r < full[c].values.size(); ++r)
      if (ranged[c].values[r] != full[c].values[r])
        QFAIL(qPrintable(QStringLiteral("row %1 curve %2: %3 != %4")
                             .arg(r).arg(c)
                             .arg(ranged[c].values[r]).arg(full[c].values[r])));

  // 子区间（单调深度 → 越界即停路径）：行数与按深度筛 parse() 一致。
  const double lo = 1000.0 + rows * 0.05, hi = 1000.0 + rows * 0.08;
  qsizetype expected = 0;
  for (double d : full[0].values)
    if (d >= lo && d <= hi)
      ++expected;
  QList<LasCurve> sub;
  QVERIFY2(LasParser::parseDepthRange(path, lo, hi, names, sub, &error), qPrintable(error));
  QCOMPARE(sub[0].values.size(), expected);
}

QTEST_MAIN(TestLas)
#include "tst_las.moc"
