// 层：测试壳
#include <QtTest>
#include <QFileInfo>
#include <QDir>
#include "io/outsourceworkbook.h"

namespace
{
QString fixture(const QString &name)
{
  return QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath(
      QStringLiteral("fixtures/io_robustness/") + name);
}
}
class IoWorkbookEdgeTests : public QObject
{
  Q_OBJECT
private slots:
  void workbookEdgesAreHonest();
  void malformedCellReferencesAreRejected();
  void unorderedCellsRetainTheirColumns();
  void physicalRowNumbersArePreserved();
  void workbookAdditionalEdges();
};

void IoWorkbookEdgeTests::workbookEdgesAreHonest()
{
  const auto result = paleo::io::readWellCoordinateTable(fixture(QStringLiteral("coordinates_edges.xlsx")));
  QVERIFY2(result.ok, qPrintable(result.error));
  QCOMPARE(result.coordinates.size(), 1);
  QCOMPARE(result.coordinates[0].x, 123.5);
  QCOMPARE(result.coordinates[0].y, -45.25);
  QCOMPARE(result.issues.size(), 3); // inline strings 不依赖 sharedStrings.xml
  QVERIFY(result.issues[0].contains(QStringLiteral("第 3 行")));
  QVERIFY(result.issues[1].contains(QStringLiteral("第 4 行")));
  QVERIFY(result.issues[2].contains(QStringLiteral("重复")));
}
void IoWorkbookEdgeTests::malformedCellReferencesAreRejected()
{
  const auto result = paleo::io::readWellCoordinateTable(fixture(QStringLiteral("malformed_refs.xlsx")));
  QVERIFY(result.ok);
  QVERIFY(result.coordinates.isEmpty());
  QVERIFY(result.issues.join(QString()).contains(QStringLiteral("B2junk")));
  QVERIFY(result.issues.join(QString()).contains(QStringLiteral("C0")));
}
void IoWorkbookEdgeTests::unorderedCellsRetainTheirColumns()
{
  const auto result = paleo::io::readWellCoordinateTable(fixture(QStringLiteral("unordered_cells.xlsx")));
  QVERIFY2(result.ok, qPrintable(result.error));
  QCOMPARE(result.coordinates.size(), 1);
  QCOMPARE(result.coordinates[0].wellName, QStringLiteral("W1"));
  QCOMPARE(result.coordinates[0].x, 123.0);
  QCOMPARE(result.coordinates[0].y, 456.0);
  QVERIFY(result.issues.isEmpty());
}
void IoWorkbookEdgeTests::physicalRowNumbersArePreserved()
{
  const auto result = paleo::io::readWellCoordinateTable(fixture(QStringLiteral("physical_rows.xlsx")));
  QVERIFY(result.ok);
  QCOMPARE(result.issues.size(), 1);
  QVERIFY(result.issues[0].contains(QStringLiteral("第 100 行")));
}

void IoWorkbookEdgeTests::workbookAdditionalEdges()
{
  auto result = paleo::io::readWellCoordinateTable(fixture(QStringLiteral("duplicate_cells.xlsx")));
  QCOMPARE(result.coordinates.size(), 1);
  QCOMPARE(result.coordinates[0].x, 123.0);
  QCOMPARE(result.issues.size(), 1);
  QVERIFY(result.issues[0].contains(QStringLiteral("重复")));
  auto workbook = paleo::io::readWorkbook(fixture(QStringLiteral("formula_error.xlsx")));
  QVERIFY(workbook.ok);
  QVERIFY(workbook.sheets[0].rows[0][1].isEmpty());
  QVERIFY(workbook.issues.join(QString()).contains(QStringLiteral("#DIV/0!")));
  result = paleo::io::readWellCoordinateTable(fixture(QStringLiteral("shared_strings_missing.xlsx")));
  QVERIFY(result.coordinates.isEmpty());
  QVERIFY(result.issues.join(QString()).contains(QStringLiteral("sharedStrings.xml")));
  result = paleo::io::readWellCoordinateTable(fixture(QStringLiteral("implicit_row.xlsx")));
  QCOMPARE(result.coordinates.size(), 1);
  QCOMPARE(result.coordinates[0].x, 123.0);
  QVERIFY(result.issues.isEmpty());
  result = paleo::io::readWellCoordinateTable(fixture(QStringLiteral("duplicate_cells.xml")));
  QVERIFY(result.ok);
  QCOMPARE(result.coordinates.size(), 1);
  QCOMPARE(result.coordinates[0].x, 123.0);
  QCOMPARE(result.coordinates[0].y, 456.0);
  QCOMPARE(result.issues.size(), 1);
  QVERIFY(result.issues[0].contains(QStringLiteral("重复")));
  result = paleo::io::readWellCoordinateTable(fixture(QStringLiteral("oversize_merge.xml")));
  QVERIFY(result.ok);
  QCOMPARE(result.coordinates.size(), 1);
  QCOMPARE(result.coordinates[0].x, 123.0);
  QCOMPARE(result.coordinates[0].y, 456.0);
  QCOMPARE(result.issues.size(), 1);
  QVERIFY(result.issues[0].contains(QStringLiteral("2147483647")));
  const auto interval = paleo::io::readIntervalRow(fixture(QStringLiteral("physical_intervals.xml")),
      QStringLiteral("层段"), QStringLiteral("T1"));
  QVERIFY2(interval.ok, qPrintable(interval.error));
  QCOMPARE(interval.rowNumber, 100);
  double thickness = 0;
  QVERIFY(paleo::io::intervalRowNumber(interval, QStringLiteral("厚度"), &thickness, nullptr));
  QCOMPARE(thickness, 123.0);
}
QTEST_GUILESS_MAIN(IoWorkbookEdgeTests)
#include "tst_io_workbook_edges.moc"
