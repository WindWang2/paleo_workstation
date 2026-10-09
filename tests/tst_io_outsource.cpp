// 层：数据（测试壳位于 tests/，被测对象为数据层读取面）
#include <QtTest/QtTest>

#include "domain/projectclassifier.h"
#include "io/outsourceworkbook.h"

#include <QDir>
#include <QFileInfo>

using namespace paleo::io;

namespace
{

QString fixturePath( const QString &name )
{
  const QString fromSource = QFileInfo( QString::fromUtf8( __FILE__ ) )
                                 .dir()
                                 .filePath( QStringLiteral( "fixtures/outsource/" ) + name );
  if ( QFileInfo::exists( fromSource ) )
    return fromSource;
  return QStringLiteral( "tests/fixtures/outsource/" ) + name;
}

int issueCountContaining( const QStringList &issues, const QString &needle )
{
  int count = 0;
  for ( const QString &issue : issues )
  {
    if ( issue.contains( needle ) )
      ++count;
  }
  return count;
}

} // namespace

class OutsourceWorkbookTests : public QObject
{
  Q_OBJECT
  private slots:
    void readsSpreadsheetMl();
    void reportsSpreadsheetMlBadRows();
    void readsXlsxSheets();
    void readsCoordinateTable();
    void readsIntervalRowAndValues();
    void rejectsForeignXml();
    void scansDirectoryHonestly();
    void canonicalizesWellNamesAndNumbers();
    void registersFormatsInClassifierVocabulary();
};

// Oracle 4：SpreadsheetML 2003 逐表逐行读全（含 ss:Index 空洞补齐）。
void OutsourceWorkbookTests::readsSpreadsheetMl()
{
  const WorkbookReadResult result = readWorkbook( fixturePath( QStringLiteral( "wg1_well.xml" ) ) );
  QVERIFY2( result.ok, qPrintable( result.error ) );
  QCOMPARE( result.format, QStringLiteral( "spreadsheetml" ) );
  QCOMPARE( result.sheets.size(), 2 );

  const WorkbookSheet &curves = result.sheets.at( 0 );
  QCOMPARE( curves.name, QStringLiteral( "测井曲线" ) );
  QCOMPARE( curves.headers, QStringList( { QStringLiteral( "深度" ), QStringLiteral( "GR" ),
                                           QStringLiteral( "孔隙度" ) } ) );
  QCOMPARE( curves.rows.size(), 2 );
  QCOMPARE( curves.rows.at( 0 ).at( 0 ), QStringLiteral( "1000" ) );
  // 嵌套富文本 <Data>0.<B>12</B></Data> 必须拼成 0.12（否则 reader 进 error 态、
  // 整表剩余行静默消失）。
  QCOMPARE( curves.rows.at( 0 ).at( 2 ), QStringLiteral( "0.12" ) );
  QCOMPARE( curves.rows.at( 1 ).at( 2 ), QStringLiteral( "0.15" ) );

  const WorkbookSheet &intervals = result.sheets.at( 1 );
  QCOMPARE( intervals.name, QStringLiteral( "地层单位道" ) );
  QCOMPARE( intervals.headers.size(), 4 );
  QCOMPARE( intervals.rows.size(), 4 );
  QCOMPARE( intervals.rows.at( 0 ), QStringList( { QStringLiteral( "T1" ), QStringLiteral( "1000" ),
                                                    QStringLiteral( "1050" ), QStringLiteral( "50" ) } ) );
  // ss:Index="4"：2/3 列补空串
  QCOMPARE( intervals.rows.at( 1 ).size(), 4 );
  QCOMPARE( intervals.rows.at( 1 ).at( 0 ), QStringLiteral( "T2" ) );
  QCOMPARE( intervals.rows.at( 1 ).at( 1 ), QString() );
  QCOMPARE( intervals.rows.at( 1 ).at( 2 ), QString() );
  QCOMPARE( intervals.rows.at( 1 ).at( 3 ), QStringLiteral( "60" ) );
  QCOMPARE( intervals.rows.at( 2 ).at( 1 ), QStringLiteral( "1200" ) );
}

// Oracle 4（坏行零静默）：ss:Index 非整数 → 整行跳过并列因。
void OutsourceWorkbookTests::reportsSpreadsheetMlBadRows()
{
  const WorkbookReadResult result = readWorkbook( fixturePath( QStringLiteral( "wg1_well.xml" ) ) );
  QVERIFY( result.ok );
  QCOMPARE( issueCountContaining( result.issues, QStringLiteral( "整行跳过" ) ), 1 );
  QVERIFY( result.issues.first().contains( QStringLiteral( "ss:Index=abc" ) ) );
  QVERIFY( result.issues.first().contains( QStringLiteral( "地层单位道" ) ) );
  // 被跳过的那一行（T4）不得出现在结果里，也不得悄悄换行号。
  for ( const WorkbookSheet &sheet : result.sheets )
  {
    for ( const QStringList &row : sheet.rows )
      QVERIFY( !row.contains( QStringLiteral( "T4" ) ) );
  }
}

// Oracle 4：OOXML 逐表读取（工作表目标、共享字符串、多表）。
void OutsourceWorkbookTests::readsXlsxSheets()
{
  const WorkbookReadResult result = readWorkbook( fixturePath( QStringLiteral( "coordinates.xlsx" ) ) );
  QVERIFY2( result.ok, qPrintable( result.error ) );
  QCOMPARE( result.format, QStringLiteral( "xlsx" ) );
  QCOMPARE( result.sheets.size(), 2 );
  QCOMPARE( result.sheets.at( 0 ).name, QStringLiteral( "坐标" ) );
  QCOMPARE( result.sheets.at( 1 ).name, QStringLiteral( "说明" ) );
  QCOMPARE( result.sheets.at( 0 ).headers, QStringList( { QStringLiteral( "井号" ), QStringLiteral( "X" ),
                                                           QStringLiteral( "Y" ), QStringLiteral( "备注" ) } ) );
  QCOMPARE( result.sheets.at( 0 ).rows.size(), 6 );
  QCOMPARE( result.sheets.at( 0 ).rows.at( 0 ).at( 0 ), QStringLiteral( "A1" ) );
  QCOMPARE( result.sheets.at( 0 ).rows.at( 1 ).at( 1 ), QStringLiteral( "1,200.75" ) );
  QVERIFY( result.issues.isEmpty() );
}

// Oracle 4（坏行逐条列因）：空井号 / 非数值坐标 / 重复井号，一条都不静默。
void OutsourceWorkbookTests::readsCoordinateTable()
{
  const WellCoordinateTable table = readWellCoordinateTable( fixturePath( QStringLiteral( "coordinates.xlsx" ) ) );
  QVERIFY2( table.ok, qPrintable( table.error ) );
  QCOMPARE( table.sheetName, QStringLiteral( "坐标" ) );
  QCOMPARE( table.coordinates.size(), 3 );
  QCOMPARE( table.coordinates.at( 0 ).wellName, QStringLiteral( "A1" ) );
  QCOMPARE( table.coordinates.at( 0 ).x, 100.5 );
  QCOMPARE( table.coordinates.at( 0 ).y, 200.25 );
  QCOMPARE( table.coordinates.at( 1 ).wellName, QStringLiteral( "A2" ) );
  QCOMPARE( table.coordinates.at( 1 ).x, 1200.75 ); // 千分位逗号口径
  QCOMPARE( table.coordinates.at( 2 ).wellName, QStringLiteral( "A4" ) );

  QCOMPARE( issueCountContaining( table.issues, QStringLiteral( "井号为空" ) ), 1 );
  QCOMPARE( issueCountContaining( table.issues, QStringLiteral( "X/Y 非数值" ) ), 1 );
  QCOMPARE( issueCountContaining( table.issues, QStringLiteral( "重复" ) ), 1 );
  // 逐条带行号（表头为第 1 行 → 坏行分别是第 4/5/6 行）。
  QVERIFY( table.issues.at( 0 ).contains( QStringLiteral( "第 4 行" ) ) );
  QVERIFY( table.issues.at( 1 ).contains( QStringLiteral( "第 5 行" ) ) );
  QVERIFY( table.issues.at( 2 ).contains( QStringLiteral( "第 6 行" ) ) );
}

// 层段行 + 数值字段：找不到层号、空值、非数值都如实报因。
void OutsourceWorkbookTests::readsIntervalRowAndValues()
{
  const QString path = fixturePath( QStringLiteral( "wg1_well.xml" ) );
  // 唯一层号 T2（第 3 行）：行号口径 = 工作表内第 N 行，表头为第 1 行。
  const IntervalRow row = readIntervalRow( path, QStringLiteral( "地层单位道" ), QStringLiteral( "T2" ) );
  QVERIFY2( row.ok, qPrintable( row.error ) );
  QCOMPARE( row.rowNumber, 3 );
  double thickness = 0;
  QString error;
  QVERIFY( intervalRowNumber( row, QStringLiteral( "厚度" ), &thickness, &error ) );
  QCOMPARE( thickness, 60.0 );

  const IntervalRow empty = row; // 同一行：顶深/底深是 ss:Index 空洞
  error.clear();
  QVERIFY( !intervalRowNumber( empty, QStringLiteral( "顶深" ), nullptr, &error ) );
  QVERIFY2( error.contains( QStringLiteral( "为空" ) ), qPrintable( error ) );

  const IntervalRow bad = readIntervalRow( path, QStringLiteral( "地层单位道" ), QStringLiteral( "T3" ) );
  QVERIFY( bad.ok );
  error.clear();
  QVERIFY( !intervalRowNumber( bad, QStringLiteral( "底深" ), nullptr, &error ) );
  QVERIFY2( error.contains( QStringLiteral( "abc" ) ), qPrintable( error ) );
  error.clear();
  QVERIFY( !intervalRowNumber( bad, QStringLiteral( "厚度" ), nullptr, &error ) );
  QVERIFY2( error.contains( QStringLiteral( "\"-\"" ) ), qPrintable( error ) );
  error.clear();
  QVERIFY( !intervalRowNumber( bad, QStringLiteral( "不存在字段" ), nullptr, &error ) );
  QVERIFY( error.contains( QStringLiteral( "没有" ) ) );

  const IntervalRow ambiguous = readIntervalRow( path, QStringLiteral( "地层单位道" ), QStringLiteral( "T1" ) );
  QVERIFY( !ambiguous.ok );
  QVERIFY2( ambiguous.error.contains( QStringLiteral( "多义" ) ) ||
                ambiguous.error.contains( QStringLiteral( "无法确定唯一层段行" ) ),
            qPrintable( ambiguous.error ) );
  QVERIFY( ambiguous.error.contains( QStringLiteral( "2 次" ) ) );

  const IntervalRow missing = readIntervalRow( path, QStringLiteral( "地层单位道" ), QStringLiteral( "T9" ) );
  QVERIFY( !missing.ok );
  QVERIFY2( missing.error.contains( QStringLiteral( "没有层号 T9" ) ), qPrintable( missing.error ) );
}

// 非工作簿 XML 不得被当成数据（分类器把它当参考资料的同一判据）。
void OutsourceWorkbookTests::rejectsForeignXml()
{
  const WorkbookReadResult result = readWorkbook( fixturePath( QStringLiteral( "not_a_workbook.xml" ) ) );
  QVERIFY( !result.ok );
  QVERIFY2( result.error.contains( QStringLiteral( "Worksheet" ) ), qPrintable( result.error ) );

  const WellCoordinateTable table = readWellCoordinateTable( fixturePath( QStringLiteral( "wg1_well.xml" ) ) );
  QVERIFY( !table.ok );
  QVERIFY2( table.error.contains( QStringLiteral( "井号" ) ), qPrintable( table.error ) );
}

// 批量扫描：单文件失败不中断，逐文件列因。
void OutsourceWorkbookTests::scansDirectoryHonestly()
{
  const QString dir = QFileInfo( fixturePath( QStringLiteral( "not_a_workbook.xml" ) ) ).absolutePath();
  const WorkbookScanResult scan = scanWorkbookDirectory( dir );
  // 不硬编码整个共享夹具目录的计数（别的方向新增夹具不该让本测试变红）：
  // 只断言「列出的每个文件都有结论」+「三个已知夹具都在且结论正确」。
  QCOMPARE( scan.entries.size(), scan.filesSeen );
  QVERIFY( scan.filesSeen >= 3 );
  QVERIFY( scan.filesFailed >= 1 );
  QHash<QString, const WorkbookScanEntry *> byName;
  for ( const WorkbookScanEntry &entry : scan.entries )
  {
    byName.insert( QFileInfo( entry.path ).fileName(), &entry );
    if ( entry.ok )
    {
      QVERIFY( entry.error.isEmpty() );
      QVERIFY( entry.sheetCount > 0 );
      QVERIFY( entry.rowCount > 0 );
    }
    else
    {
      QVERIFY( !entry.error.isEmpty() );
    }
  }
  QVERIFY( byName.contains( QStringLiteral( "coordinates.xlsx" ) ) );
  QVERIFY( byName.value( QStringLiteral( "coordinates.xlsx" ) )->ok );
  QVERIFY( byName.contains( QStringLiteral( "wg1_well.xml" ) ) );
  QVERIFY( byName.value( QStringLiteral( "wg1_well.xml" ) )->ok );
  QVERIFY( byName.contains( QStringLiteral( "not_a_workbook.xml" ) ) );
  QVERIFY( !byName.value( QStringLiteral( "not_a_workbook.xml" ) )->ok );
}

void OutsourceWorkbookTests::canonicalizesWellNamesAndNumbers()
{
  QCOMPARE( canonicalWellName( QStringLiteral( "hz28-6-1" ) ), QStringLiteral( "HZ28-6-1" ) );
  QCOMPARE( canonicalWellName( QStringLiteral( " A1井 " ) ), QStringLiteral( "A1" ) );
  QCOMPARE( canonicalWellName( QStringLiteral( "a1_2" ) ), QStringLiteral( "A1-2" ) );
  QCOMPARE( canonicalWellName( QString() ), QString() );

  double value = 0;
  QVERIFY( parseNumericCell( QStringLiteral( "1,200.75" ), &value ) );
  QCOMPARE( value, 1200.75 );
  QVERIFY( parseNumericCell( QStringLiteral( "12%" ), &value ) );
  QCOMPARE( value, 12.0 );
  QVERIFY( !parseNumericCell( QStringLiteral( "abc" ), &value ) );
  QVERIFY( !parseNumericCell( QString(), &value ) );
  QVERIFY( !parseNumericCell( QStringLiteral( "-" ), &value ) );
  QVERIFY( !parseNumericCell( QStringLiteral( "inf" ), &value ) );
}

// 新格式进 manifest 词表登记（不旁路分类器）；.xml 内容嗅探沿用既有判据。
void OutsourceWorkbookTests::registersFormatsInClassifierVocabulary()
{
  const QStringList vocab = projectClassifierTypes();
  QVERIFY( vocab.contains( QStringLiteral( "single_factor_package" ) ) );
  QVERIFY( vocab.contains( QStringLiteral( "outsource_workbook" ) ) );

  const ProjectClassification package = classifyProjectPath( QStringLiteral( "/外委/demo.sfpkg" ) );
  QCOMPARE( package.type, QStringLiteral( "single_factor_package" ) );
  QCOMPARE( package.format, QStringLiteral( "sfpkg" ) );
  QCOMPARE( package.role, QStringLiteral( "input" ) );

  const ProjectClassification workbook =
      classifyProjectPath( QStringLiteral( "/外委/坐标统计.xlsx" ) );
  QCOMPARE( workbook.type, QStringLiteral( "outsource_workbook" ) );
  QCOMPARE( workbook.role, QStringLiteral( "input" ) );

  // 旧二进制 .xls 同档归工作簿：原生读面不覆盖，预览经 LibreOffice 转
  // DERIVED .xlsx 再渲染（转换编排、非原生解析能力）。
  QCOMPARE( classifyProjectPath( QStringLiteral( "/外委/旧表.xls" ) ).type,
            QStringLiteral( "outsource_workbook" ) );
  QCOMPARE( classifyProjectPath( QStringLiteral( "/外委/旧表.xls" ) ).format,
            QStringLiteral( "xls" ) );

  // SpreadsheetML 的 .xml 仍按内容嗅探判为测井类（既有行为与既有断言一致），
  // 本方向不改判；批量读取走 scanWorkbookDirectory 独立面。
  const QByteArray spreadsheetXml =
      "<Workbook xmlns='urn:schemas-microsoft-com:office:spreadsheet' "
      "xmlns:ss='urn:schemas-microsoft-com:office:spreadsheet'>"
      "<Worksheet ss:Name='测井曲线'><Table><Row/></Table></Worksheet></Workbook>";
  QCOMPARE( classifyProjectImport( QStringLiteral( "/参考资料/柱状图.xml" ), spreadsheetXml ).type,
            QStringLiteral( "well_log" ) );
}

QTEST_MAIN( OutsourceWorkbookTests )
#include "tst_io_outsource.moc"