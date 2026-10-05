// 层：数据
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// 方向41：外委资料批量读取（上游 Drawing/drawing/single_factor/workflow.py
// @27fdb998a32d7a7f50d7e6ef0d2ebb3a5d06378f 的扫描口径）。
//
// 两种工作簿容器：
//   - SpreadsheetML 2003（.xml，urn:schemas-microsoft-com:office:spreadsheet）
//   - OOXML（.xlsx，ZIP：xl/workbook.xml + xl/_rels/workbook.xml.rels +
//     xl/worksheets/sheetN.xml；sharedStrings.xml 仅共享字符串单元格需要）
// 单元格语义与上游一致：首行为表头，其后为数据行；ss:Index/ss:MergeAcross 与
// OOXML 的 r="B3" 列引用都按 1 基列号补齐空洞。
//
// 诚实面：文件级失败 → ok=false + error（缺 workbook.xml、根元素不是工作簿、
// XML 结构损坏、目标工作表缺失）；行/单元格级坏数据 → issues 逐条列因（行号 +
// 原因），照常返回其余行，不静默丢行、不假装读全。
// 层：数据
namespace paleo::io
{

struct WorkbookSheet
{
  QString name;
  QStringList headers;         // 首个非空行的单元格（原样，尾部空列已去掉）
  int headerRowNumber = 1;     // 首个非空表头的物理行号
  QVector<int> rowNumbers;     // 与 rows 平行；不按缺行分配空洞
  QVector<QStringList> rows;   // 表头之后的行（保留原始列数，含空行位置）
};

struct WorkbookReadResult
{
  bool ok = false;
  QString error;
  QString path;
  QString format; // "spreadsheetml" | "xlsx"
  QVector<WorkbookSheet> sheets;
  QStringList issues;
};

// 按扩展名分派（.xml → SpreadsheetML，.xlsx → OOXML）。不抛异常。
WorkbookReadResult readWorkbook( const QString &path );

struct WorkbookScanEntry
{
  QString path;
  bool ok = false;
  QString format;
  QString error;
  int sheetCount = 0;
  int rowCount = 0;
  QStringList issues;
};

struct WorkbookScanResult
{
  QVector<WorkbookScanEntry> entries;
  int filesSeen = 0;
  int filesFailed = 0;
};

// 扫描目录（可递归）里的 .xml/.xlsx 普通文件，逐个读。单文件失败不中断其余文件。
WorkbookScanResult scanWorkbookDirectory( const QString &dirPath, bool recursive = true );

// 坐标表（上游「坐标统计.xlsx」：井号 / X / Y）。找不到同时含这三列的
// 工作表时 ok=false 并说明；坏行列因，不静默。
struct WellCoordinate
{
  QString wellName;
  double x = 0;
  double y = 0;
};

struct WellCoordinateTable
{
  bool ok = false;
  QString error;
  QString sheetName;
  QVector<WellCoordinate> coordinates;
  QStringList issues;
};

WellCoordinateTable readWellCoordinateTable( const QString &path );

// 上游 _extract_interval_row_from_xml / _row_to_mapping：在指定工作表里按
// 「层号」列取一行。表头缺失、找不到层号、同一层号出现多次都如实报因。
struct IntervalRow
{
  bool ok = false;
  QString error;
  QString sheetName;
  int rowNumber = 0; // 工作簿内物理行号
  QStringList headers;
  QStringList values;
  QStringList issues;
};

IntervalRow readIntervalRow( const QString &path, const QString &sheetName, const QString &intervalName );

// 从层段行取数值字段（厚度/顶深/底深/顶TVD/底TVD 等）：空值与「-」等占位
// 一律报因并返回 false，不静默当 0。
bool intervalRowNumber( const IntervalRow &row, const QString &field, double *value, QString *error );

// 上游 _canonicalize_well_name：大写、下划线转连字符，取 井号样式 子串；
// 取不到时去掉「井」字返回大写原文。空输入返回空串。
QString canonicalWellName( const QString &raw );

// 上游 _parse_numeric：去空白与千分位逗号、去尾部百分号，非有限返回 false。
bool parseNumericCell( const QString &raw, double *value );

} // namespace paleo::io
