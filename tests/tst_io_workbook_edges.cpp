// 层：测试壳
#include <QtTest>
#include <QFileInfo>
#include <QDir>
#include "io/cuttingsdoc.h"
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
  // 方向 69 第二解释源：岩屑录井纯表解析（合成 WorkbookSheet 直调）。
  void cuttingsSheetStandardHeaders();
  void cuttingsSheetDialectHeaders();
  void cuttingsSheetMissingColumnIsHonest();
  void cuttingsSheetBadRowsSkippedIntoIssues();
  void cuttingsSheetDescriptionColumnOptional();
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

namespace
{
// 合成 WorkbookSheet（cuttings 纯表解析直调用）：物理行号从表头行顺延。
paleo::io::WorkbookSheet cuttingsSheet(const QStringList &headers,
                                       const QVector<QStringList> &rows,
                                       int headerRowNumber = 1)
{
  paleo::io::WorkbookSheet sheet;
  sheet.name = QStringLiteral("录井");
  sheet.headers = headers;
  sheet.headerRowNumber = headerRowNumber;
  for (int i = 0; i < rows.size(); ++i) {
    sheet.rowNumbers.append(headerRowNumber + 1 + i);
    sheet.rows.append(rows[i]);
  }
  return sheet;
}
} // namespace

void IoWorkbookEdgeTests::cuttingsSheetStandardHeaders()
{
  const auto table = paleo::io::parseCuttingsSheet(cuttingsSheet(
      {QStringLiteral("顶深"), QStringLiteral("底深"), QStringLiteral("岩性"),
       QStringLiteral("描述")},
      {{QStringLiteral("150"), QStringLiteral("200"), QStringLiteral("泥岩"),
        QStringLiteral("深灰色")},
       {QStringLiteral("100"), QStringLiteral("150"), QStringLiteral("细砂岩"),
        QStringLiteral("褐灰色")}}));
  QVERIFY2(table.ok, qPrintable(table.error));
  QCOMPARE(table.sheetName, QStringLiteral("录井"));
  QCOMPARE(table.intervals.size(), 2);
  // 按 topMd 升序（与输入顺序无关）。
  QCOMPARE(table.intervals[0].topMd, 100.0);
  QCOMPARE(table.intervals[0].baseMd, 150.0);
  QCOMPARE(table.intervals[0].litho, QStringLiteral("细砂岩"));
  QCOMPARE(table.intervals[0].description, QStringLiteral("褐灰色"));
  QCOMPARE(table.intervals[0].rowNumber, 3); // 表头行 1 → 第 2 条数据行物理行号 3
  QCOMPARE(table.intervals[1].topMd, 150.0);
  QCOMPARE(table.intervals[1].litho, QStringLiteral("泥岩"));
  QVERIFY(table.issues.isEmpty());
}

void IoWorkbookEdgeTests::cuttingsSheetDialectHeaders()
{
  // 方言表头 A：括号单位（半角/全角）+ 空白 + 定名词面。
  const auto a = paleo::io::parseCuttingsSheet(cuttingsSheet(
      {QStringLiteral(" 顶深(m) "), QStringLiteral("底深（米）"),
       QStringLiteral("岩性定名")},
      {{QStringLiteral("100"), QStringLiteral("150"),
        QStringLiteral("细砂岩")}}));
  QVERIFY2(a.ok, qPrintable(a.error));
  QCOMPARE(a.intervals.size(), 1);
  QCOMPARE(a.intervals[0].topMd, 100.0);
  QCOMPARE(a.intervals[0].baseMd, 150.0);
  QCOMPARE(a.intervals[0].litho, QStringLiteral("细砂岩"));
  QVERIFY(a.intervals[0].description.isEmpty()); // 无描述列 → 空
  // 方言表头 B：英文词面大小写不敏感（TOP/BOT/Lithology），bot 入底深方言。
  const auto b = paleo::io::parseCuttingsSheet(cuttingsSheet(
      {QStringLiteral("TOP"), QStringLiteral("BOT"), QStringLiteral("Lithology")},
      {{QStringLiteral("100"), QStringLiteral("150"),
        QStringLiteral("细砂岩")}}));
  QVERIFY2(b.ok, qPrintable(b.error));
  QCOMPARE(b.intervals.size(), 1);
  QCOMPARE(b.intervals[0].topMd, 100.0);
  QCOMPARE(b.intervals[0].baseMd, 150.0);
  QCOMPARE(b.intervals[0].litho, QStringLiteral("细砂岩"));
}

void IoWorkbookEdgeTests::cuttingsSheetMissingColumnIsHonest()
{
  const auto table = paleo::io::parseCuttingsSheet(cuttingsSheet(
      {QStringLiteral("顶深"), QStringLiteral("岩性")},
      {{QStringLiteral("100"), QStringLiteral("细砂岩")}}));
  QVERIFY(!table.ok);
  QVERIFY2(table.error.contains(QStringLiteral("缺少必需列表头")),
           qPrintable(table.error));
  QVERIFY2(table.error.contains(QStringLiteral("底深")), qPrintable(table.error));
  QCOMPARE(table.error.count(QStringLiteral("、")), 0); // 只缺底深一列
  QVERIFY(table.intervals.isEmpty());
}

void IoWorkbookEdgeTests::cuttingsSheetBadRowsSkippedIntoIssues()
{
  const auto table = paleo::io::parseCuttingsSheet(cuttingsSheet(
      {QStringLiteral("顶深"), QStringLiteral("底深"), QStringLiteral("岩性")},
      // 逆序、非数值、空词面各一；表头行号 1 → 数据行物理行号 2..5。
      {{QStringLiteral("300"), QStringLiteral("280"), QStringLiteral("逆序段")},
       {QStringLiteral("abc"), QStringLiteral("150"), QStringLiteral("坏数值")},
       {QStringLiteral("100"), QStringLiteral("150"), QStringLiteral("  ")},
       {QStringLiteral("100"), QStringLiteral("150"), QStringLiteral("细砂岩")}}));
  QVERIFY2(table.ok, qPrintable(table.error));
  QCOMPARE(table.intervals.size(), 1);
  QCOMPARE(table.intervals[0].litho, QStringLiteral("细砂岩"));
  QCOMPARE(table.issues.size(), 3);
  QVERIFY2(table.issues[0].contains(QStringLiteral("第 2 行")),
           qPrintable(table.issues.join(QStringLiteral("\n"))));
  QVERIFY2(table.issues[0].contains(QStringLiteral("底深 280 不大于顶深 300")),
           qPrintable(table.issues[0]));
  QVERIFY2(table.issues[1].contains(QStringLiteral("顶深不是数值")),
           qPrintable(table.issues[1]));
  QVERIFY2(table.issues[1].contains(QStringLiteral("第 3 行")),
           qPrintable(table.issues[1]));
  QVERIFY2(table.issues[2].contains(QStringLiteral("岩性词面为空")),
           qPrintable(table.issues[2]));
}

void IoWorkbookEdgeTests::cuttingsSheetDescriptionColumnOptional()
{
  // 无描述列：照常解析，description 为空。
  const auto noDesc = paleo::io::parseCuttingsSheet(cuttingsSheet(
      {QStringLiteral("顶深"), QStringLiteral("底深"), QStringLiteral("岩性")},
      {{QStringLiteral("100"), QStringLiteral("150"), QStringLiteral("细砂岩")}}));
  QVERIFY2(noDesc.ok, qPrintable(noDesc.error));
  QCOMPARE(noDesc.intervals.size(), 1);
  QVERIFY(noDesc.intervals[0].description.isEmpty());
  // 描述列以「备注」方言出现：照常进 description。
  const auto withRemark = paleo::io::parseCuttingsSheet(cuttingsSheet(
      {QStringLiteral("顶深"), QStringLiteral("底深"), QStringLiteral("岩性"),
       QStringLiteral("备注")},
      {{QStringLiteral("100"), QStringLiteral("150"), QStringLiteral("细砂岩"),
        QStringLiteral("见油斑")}}));
  QVERIFY2(withRemark.ok, qPrintable(withRemark.error));
  QCOMPARE(withRemark.intervals[0].description, QStringLiteral("见油斑"));
}
QTEST_GUILESS_MAIN(IoWorkbookEdgeTests)
#include "tst_io_workbook_edges.moc"
