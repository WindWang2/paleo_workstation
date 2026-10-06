// 层：数据
#include "outsourceworkbook.h"

#include "ziparchive.h"
#include "domain/wellnumeric.h"

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QXmlStreamReader>

#include <algorithm>
#include <cmath>
#include <limits>

// 层：数据
namespace paleo::io
{
namespace
{

const QString kSsNs = QStringLiteral( "urn:schemas-microsoft-com:office:spreadsheet" );
const QString kMainNs = QStringLiteral( "http://schemas.openxmlformats.org/spreadsheetml/2006/main" );
const QString kRelNs = QStringLiteral( "http://schemas.openxmlformats.org/package/2006/relationships" );
const QString kOfficeRelNs =
    QStringLiteral( "http://schemas.openxmlformats.org/officeDocument/2006/relationships" );

// 去掉尾部空列（上游 _normalize_header_row）。
QStringList normalizeHeaderRow( QStringList values )
{
  for ( QString &value : values )
    value = value.trimmed();
  while ( !values.isEmpty() && values.last().isEmpty() )
    values.removeLast();
  return values;
}

// 列号上限：畸形 ss:Index / 列引用（如 200000000）会让补空列的分配失控。
// 16384 列（XFD）已远超任何外委表的实际宽度。
constexpr int kMaxColumns = 16384;
constexpr int kMaxRows = 1048576;

void padTo( QStringList *values, int size )
{
  while ( values->size() < size )
    values->append( QString() );
}

// 读取元素文本并**包含子元素**：SpreadsheetML/Excel 的富文本与批注会在
// Data/si 里嵌 Font/r/t 等子节点，Qt 默认的 ErrorOnUnexpectedElement 会让
// reader 进入 error 态，外层循环随之退出——整表剩余行会静默消失。
QString readCellText( QXmlStreamReader &reader )
{
  return reader.readElementText( QXmlStreamReader::IncludeChildElements );
}

// 表头行判定：至少有一个非空白单元格（显式循环，不依赖正则重载的行为差异）。
bool rowHasAnyText( const QStringList &values )
{
  for ( const QString &value : values )
  {
    if ( !value.trimmed().isEmpty() )
      return true;
  }
  return false;
}

QString cellText( const QString &raw )
{
  return raw.trimmed();
}

// ---- SpreadsheetML 2003 ----

WorkbookReadResult readSpreadsheetMl( const QString &path, const QByteArray &bytes )
{
  WorkbookReadResult result;
  result.path = path;
  result.format = QStringLiteral( "spreadsheetml" );

  QXmlStreamReader reader( bytes );
  WorkbookSheet sheet;
  bool inSheet = false;
  bool inRow = false;
  int rowNumber = 0;
  int nextColumn = 0;
  QStringList row;
  bool rowHasError = false;
  QSet<int> usedColumns;

  const auto finishRow = [&]() {
    if ( !inRow )
      return;
    if ( !rowHasError )
    {
      if ( sheet.headers.isEmpty() )
      {
        if ( rowHasAnyText( row ) )
        {
          sheet.headers = normalizeHeaderRow( row );
          sheet.headerRowNumber = rowNumber;
        }
      }
      else
      {
        sheet.rows.append( row );
        sheet.rowNumbers.append( rowNumber );
      }
    }
    row.clear();
    rowHasError = false;
    usedColumns.clear();
    inRow = false;
  };
  const auto finishSheet = [&]() {
    finishRow();
    if ( inSheet )
    {
      if ( sheet.headers.isEmpty() )
        result.issues.append( QStringLiteral( "%1：工作表「%2」没有表头行" )
                                  .arg( QFileInfo( path ).fileName(), sheet.name ) );
      result.sheets.append( sheet );
    }
    sheet = WorkbookSheet();
    inSheet = false;
  };

  while ( !reader.atEnd() )
  {
    const QXmlStreamReader::TokenType token = reader.readNext();
    if ( token == QXmlStreamReader::StartElement )
    {
      const QString name = reader.name().toString();
      if ( name == QLatin1String( "Worksheet" ) )
      {
        finishSheet();
        inSheet = true;
        rowNumber = 0; // 行号按工作表重置：issues 的「第 N 行」永远是本表内行号
        sheet.name = reader.attributes().value( kSsNs, QStringLiteral( "Name" ) ).toString();
        if ( sheet.name.isEmpty() )
          sheet.name = QStringLiteral( "Sheet%1" ).arg( result.sheets.size() + 1 );
      }
      else if ( inSheet && name == QLatin1String( "Row" ) )
      {
        finishRow();
        inRow = true;
        ++rowNumber;
        nextColumn = 1;
        const QStringView index = reader.attributes().value( kSsNs, QStringLiteral( "Index" ) );
        if ( !index.isEmpty() )
        {
          bool ok = false;
          const int value = index.toInt( &ok );
          if ( !ok || value < 1 )
          {
            result.issues.append( QStringLiteral( "工作表「%1」第 %2 行：ss:Index=%3 不是正整数，整行跳过" )
                                      .arg( sheet.name )
                                      .arg( rowNumber )
                                      .arg( index.toString() ) );
            rowHasError = true;
          }
          else if ( value > kMaxRows )
          {
            result.issues.append( QStringLiteral( "工作表「%1」第 %2 行：ss:Index=%3 超过行上限 %4，整行跳过" )
                                      .arg( sheet.name )
                                      .arg( rowNumber )
                                      .arg( value )
                                      .arg( kMaxRows ) );
            rowHasError = true;
          }
          else
          {
            rowNumber = value;
          }
        }
      }
      else if ( inSheet && inRow && name == QLatin1String( "Cell" ) )
      {
        int column = nextColumn;
        bool cellOk = true;
        const QStringView index = reader.attributes().value( kSsNs, QStringLiteral( "Index" ) );
        if ( !index.isEmpty() )
        {
          bool ok = false;
          const int value = index.toInt( &ok );
          if ( !ok || value < 1 )
          {
            result.issues.append( QStringLiteral( "工作表「%1」第 %2 行：ss:Index=%3 不是正整数，该单元格跳过" )
                                      .arg( sheet.name )
                                      .arg( rowNumber )
                                      .arg( index.toString() ) );
            cellOk = false;
          }
          else if ( value > kMaxColumns )
          {
            result.issues.append( QStringLiteral( "工作表「%1」第 %2 行：ss:Index=%3 超过列上限 %4，该单元格跳过" )
                                      .arg( sheet.name )
                                      .arg( rowNumber )
                                      .arg( value )
                                      .arg( kMaxColumns ) );
            cellOk = false;
          }
          else
          {
            column = value;
          }
        }
        int span = 0;
        const QStringView merge = reader.attributes().value( kSsNs, QStringLiteral( "MergeAcross" ) );
        if ( !merge.isEmpty() )
        {
          bool ok = false;
          const int value = merge.toInt( &ok );
          if ( !ok || value < 0 )
          {
            result.issues.append( QStringLiteral( "工作表「%1」第 %2 行：ss:MergeAcross=%3 不是非负整数，按 0 处理" )
                                      .arg( sheet.name )
                                      .arg( rowNumber )
                                      .arg( merge.toString() ) );
          }
          else
          {
            span = value;
          }
        }
        if ( column < 1 || column > kMaxColumns || usedColumns.contains( column ) )
        {
          result.issues.append( QCoreApplication::translate( "OutsourceWorkbook", "工作表「%1」第 %2 行列 %3 重复或越界，该格跳过" )
                                    .arg( sheet.name ).arg( rowNumber ).arg( column ) );
          cellOk = false;
        }
        if ( span > 0 && span > kMaxColumns - column )
        {
          result.issues.append( QStringLiteral( "工作表「%1」第 %2 行：合并跨度 %3 超过列上限 %4，按不合并处理" )
                                    .arg( sheet.name )
                                    .arg( rowNumber )
                                    .arg( span )
                                    .arg( kMaxColumns ) );
          span = 0;
        }
        if ( cellOk )
          for ( int merged = column; merged <= column + span; ++merged )
            if ( usedColumns.contains( merged ) )
            {
              result.issues.append( QCoreApplication::translate( "OutsourceWorkbook", "工作表「%1」第 %2 行合并范围覆盖已有列 %3，该格跳过" )
                                        .arg( sheet.name ).arg( rowNumber ).arg( merged ) );
              cellOk = false;
              break;
            }
        QString text;
        while ( !reader.atEnd() )
        {
          const QXmlStreamReader::TokenType inner = reader.readNext();
          if ( inner == QXmlStreamReader::StartElement && reader.name() == QLatin1String( "Data" ) )
          {
            text = cellText( readCellText( reader ) );
            break;
          }
          if ( inner == QXmlStreamReader::EndElement && reader.name() == QLatin1String( "Cell" ) )
            break;
        }
        if ( cellOk )
        {
          padTo( &row, column + span );
          row[column - 1] = text;
          for ( int merged = column; merged <= column + span; ++merged )
            usedColumns.insert( merged );
          nextColumn = std::max( nextColumn, column + span + 1 );
        }
      }
    }
    else if ( token == QXmlStreamReader::EndElement )
    {
      const QString name = reader.name().toString();
      if ( name == QLatin1String( "Row" ) )
        finishRow();
      else if ( name == QLatin1String( "Worksheet" ) )
        finishSheet();
    }
  }
  finishSheet();
  if ( reader.hasError() )
  {
    result.error = QStringLiteral( "XML 解析错误（行 %1）：%2" )
                       .arg( reader.lineNumber() )
                       .arg( reader.errorString() );
    return result;
  }
  if ( result.sheets.isEmpty() )
  {
    result.error = QStringLiteral( "没有找到 Worksheet 工作簿节点（不是 SpreadsheetML 工作簿）" );
    return result;
  }
  result.ok = true;
  return result;
}

// ---- OOXML（.xlsx）----

QHash<QString, QString> readRelationships( const QByteArray &bytes )
{
  QHash<QString, QString> map;
  QXmlStreamReader reader( bytes );
  while ( !reader.atEnd() )
  {
    if ( reader.readNext() == QXmlStreamReader::StartElement &&
         reader.name() == QLatin1String( "Relationship" ) &&
         ( reader.namespaceUri() == kRelNs || reader.namespaceUri().isEmpty() ) )
    {
      const QString id = reader.attributes().value( QStringLiteral( "Id" ) ).toString();
      QString target = reader.attributes().value( QStringLiteral( "Target" ) ).toString();
      if ( target.startsWith( QLatin1Char( '/' ) ) )
        target = target.mid( 1 );
      else if ( !target.startsWith( QLatin1String( "xl/" ) ) )
        target = QStringLiteral( "xl/" ) + target;
      if ( !id.isEmpty() && !target.isEmpty() )
        map.insert( id, target );
    }
  }
  return map;
}

QVector<QString> readSharedStrings( const QByteArray &bytes )
{
  QVector<QString> values;
  QXmlStreamReader reader( bytes );
  QString current;
  bool inItem = false;
  while ( !reader.atEnd() )
  {
    const QXmlStreamReader::TokenType token = reader.readNext();
    if ( token == QXmlStreamReader::StartElement )
    {
      const QString name = reader.name().toString();
      if ( name == QLatin1String( "si" ) )
      {
        inItem = true;
        current.clear();
      }
      else if ( inItem && name == QLatin1String( "t" ) )
      {
        current += readCellText( reader ); // 富文本 <r><t> 串接
      }
    }
    else if ( token == QXmlStreamReader::EndElement && reader.name() == QLatin1String( "si" ) )
    {
      values.append( current );
      inItem = false;
    }
  }
  return values;
}

// "BC12" → 列号（1 基）；引用非法返回 -1。
int columnIndexFromRef( const QString &ref, int *rowNumber )
{
  static const QRegularExpression pattern( QStringLiteral( "^([A-Za-z]{1,3})([1-9][0-9]{0,6})$" ) );
  const auto match = pattern.match( ref );
  if ( !match.hasMatch() )
    return -1;
  bool ok = false;
  const int row = match.captured( 2 ).toInt( &ok );
  if ( !ok || row > kMaxRows )
    return -1;
  int index = 0;
  for ( const QChar &character : match.captured( 1 ).toUpper() )
    index = index * 26 + character.unicode() - 'A' + 1;
  if ( rowNumber )
    *rowNumber = row;
  return index;
}

// 同一个内存 ZIP 读面解 OOXML 包（不落临时文件、不依赖 QMimeDatabase）。
bool readZipEntry( const QByteArray &archive, const QString &entry, QByteArray *bytes, QString *error )
{
  return zipExtractBytes( archive, entry, bytes, error );
}

WorkbookReadResult readXlsx( const QString &path )
{
  WorkbookReadResult result;
  result.path = path;
  result.format = QStringLiteral( "xlsx" );
  const QString fileName = QFileInfo( path ).fileName();

  QString archiveError;
  const QByteArray archive = zipReadArchive( path, &archiveError );
  if ( archive.isEmpty() )
  {
    result.error = QStringLiteral( "%1 不是 OOXML 工作簿：%2" )
                       .arg( fileName, archiveError.isEmpty() ? QStringLiteral( "无法读取包" ) : archiveError );
    return result;
  }
  QByteArray workbookBytes;
  QString entryError;
  if ( !readZipEntry( archive, QStringLiteral( "xl/workbook.xml" ), &workbookBytes, &entryError ) )
  {
    result.error = QStringLiteral( "%1 不是 OOXML 工作簿：%2" ).arg( fileName, entryError );
    return result;
  }
  QByteArray relBytes;
  QString relError;
  const bool hasRelationships =
      readZipEntry( archive, QStringLiteral( "xl/_rels/workbook.xml.rels" ), &relBytes, &relError );
  const QHash<QString, QString> relationships =
      hasRelationships ? readRelationships( relBytes ) : QHash<QString, QString>();
  if ( !hasRelationships )
  {
    // 根因单独列一条：否则后面只剩「每张表 r:id 没有目标」这种症状级文案。
    result.issues.append(
        QStringLiteral( "%1：读不到 xl/_rels/workbook.xml.rels（%2），无法定位工作表" ).arg( fileName, relError ) );
  }
  QByteArray sharedBytes;
  QString sharedError;
  const bool hasSharedStrings =
      readZipEntry( archive, QStringLiteral( "xl/sharedStrings.xml" ), &sharedBytes, &sharedError );
  const QVector<QString> sharedStrings = hasSharedStrings ? readSharedStrings( sharedBytes ) : QVector<QString>();

  QVector<QPair<QString, QString>> sheetTargets;
  {
    QXmlStreamReader reader( workbookBytes );
    while ( !reader.atEnd() )
    {
      if ( reader.readNext() == QXmlStreamReader::StartElement &&
           reader.name() == QLatin1String( "sheet" ) &&
           ( reader.namespaceUri() == kMainNs || reader.namespaceUri().isEmpty() ) )
      {
        const QString name = reader.attributes().value( QStringLiteral( "name" ) ).toString();
        const QString relId = reader.attributes().value( kOfficeRelNs, QStringLiteral( "id" ) ).toString();
        if ( relId.isEmpty() )
        {
          result.issues.append(
              QStringLiteral( "%1：工作表「%2」没有 r:id，无法定位 sheet XML" ).arg( fileName, name ) );
          continue;
        }
        const QString target = relationships.value( relId );
        if ( target.isEmpty() )
        {
          result.issues.append(
              QStringLiteral( "%1：工作表「%2」的 r:id=%3 在 rels 里没有对应目标" ).arg( fileName, name, relId ) );
          continue;
        }
        sheetTargets.append( { name.isEmpty() ? target : name, target } );
      }
    }
    if ( reader.hasError() )
    {
      result.error = QStringLiteral( "xl/workbook.xml 解析错误（行 %1）：%2" )
                         .arg( reader.lineNumber() )
                         .arg( reader.errorString() );
      return result;
    }
  }
  if ( sheetTargets.isEmpty() )
  {
    result.error = QStringLiteral( "%1：xl/workbook.xml 里没有工作表" ).arg( fileName );
    return result;
  }

  for ( const QPair<QString, QString> &item : sheetTargets )
  {
    QByteArray sheetBytes;
    QString sheetError;
    if ( !readZipEntry( archive, item.second, &sheetBytes, &sheetError ) )
    {
      result.issues.append( QStringLiteral( "%1：工作表「%2」%3" ).arg( fileName, item.first, sheetError ) );
      continue;
    }
    WorkbookSheet sheet;
    sheet.name = item.first;
    QXmlStreamReader reader( sheetBytes );
    QStringList row;
    bool inRow = false;
    int rowNumber = 0;
    int nextColumn = 1;
    bool rowHasError = false;
    bool rowIsExplicit = false;
    QSet<int> usedColumns;
    const auto finishRow = [&]() {
      if ( !inRow )
        return;
      if ( !rowHasError && sheet.headers.isEmpty() )
      {
        if ( rowHasAnyText( row ) )
        {
          sheet.headers = normalizeHeaderRow( row );
          sheet.headerRowNumber = rowNumber;
        }
      }
      else if ( !rowHasError )
      {
        sheet.rows.append( row );
        sheet.rowNumbers.append( rowNumber );
      }
      row.clear();
      inRow = false;
      rowHasError = false;
      usedColumns.clear();
    };
    while ( !reader.atEnd() )
    {
      const QXmlStreamReader::TokenType token = reader.readNext();
      if ( token == QXmlStreamReader::StartElement )
      {
        const QString name = reader.name().toString();
        if ( name == QLatin1String( "row" ) )
        {
          finishRow();
          inRow = true;
          ++rowNumber;
          nextColumn = 1;
          const QString rawRow = reader.attributes().value( QStringLiteral( "r" ) ).toString();
          rowIsExplicit = !rawRow.isEmpty();
          if ( rowIsExplicit )
          {
            bool ok = false;
            const int physicalRow = rawRow.toInt( &ok );
            if ( !ok || physicalRow < 1 || physicalRow > kMaxRows )
            {
              result.issues.append( QCoreApplication::translate( "OutsourceWorkbook", "%1：工作表「%2」行号 %3 非法，整行跳过" )
                                        .arg( fileName, sheet.name, rawRow ) );
              rowHasError = true;
            }
            else
              rowNumber = physicalRow;
          }
        }
        else if ( inRow && name == QLatin1String( "c" ) )
        {
          const QString ref = reader.attributes().value( QStringLiteral( "r" ) ).toString();
          const QString type = reader.attributes().value( QStringLiteral( "t" ) ).toString();
          int column = nextColumn;
          if ( !ref.isEmpty() )
          {
            int cellRow = 0;
            const int parsed = columnIndexFromRef( ref, &cellRow );
            if ( parsed > 0 && !rowIsExplicit && usedColumns.isEmpty() ) rowNumber = cellRow;
            if ( parsed < 0 || cellRow != rowNumber )
            {
              result.issues.append( QStringLiteral( "%1：工作表「%2」第 %3 行单元格引用 %4 非法，该格跳过" )
                                        .arg( fileName, sheet.name )
                                        .arg( rowNumber )
                                        .arg( ref ) );
              reader.skipCurrentElement();
              continue;
            }
            if ( parsed > kMaxColumns )
            {
              result.issues.append( QStringLiteral( "%1：工作表「%2」第 %3 行列号 %4 超过列上限 %5，该格跳过" )
                                        .arg( fileName, sheet.name )
                                        .arg( rowNumber )
                                        .arg( parsed )
                                        .arg( kMaxColumns ) );
              reader.skipCurrentElement();
              continue;
            }
            column = parsed;
          }
          if ( column > kMaxColumns || usedColumns.contains( column ) )
          {
            result.issues.append( QCoreApplication::translate( "OutsourceWorkbook", "%1：工作表「%2」第 %3 行列 %4 重复或越界，该格跳过" )
                                      .arg( fileName, sheet.name ).arg( rowNumber ).arg( column ) );
            reader.skipCurrentElement();
            continue;
          }
          usedColumns.insert( column );
          QString value;
          QString errorText;
          bool isError = false;
          while ( !reader.atEnd() )
          {
            const QXmlStreamReader::TokenType inner = reader.readNext();
            if ( inner == QXmlStreamReader::StartElement )
            {
              const QString innerName = reader.name().toString();
              // IncludeChildElements：富文本/公式子节点不会把 reader 打成 error
              // 态（否则外层循环退出，整表剩余行静默消失）。
              if ( innerName == QLatin1String( "v" ) )
                value = readCellText( reader );
              else if ( innerName == QLatin1String( "t" ) )
                value += readCellText( reader );
            }
            else if ( inner == QXmlStreamReader::EndElement && reader.name() == QLatin1String( "c" ) )
            {
              break;
            }
          }
          if ( type == QLatin1String( "s" ) )
          {
            if ( !hasSharedStrings )
              result.issues.append( QCoreApplication::translate( "OutsourceWorkbook", "%1：工作表「%2」第 %3 行需要共享字符串表但读不到 xl/sharedStrings.xml（%4），该格置空" )
                                        .arg( fileName, sheet.name ).arg( rowNumber ).arg( sharedError ) );
            bool ok = false;
            const int index = value.toInt( &ok );
            if ( !ok || index < 0 || index >= sharedStrings.size() )
            {
              result.issues.append( QStringLiteral( "%1：工作表「%2」第 %3 行共享字符串索引 %4 越界" )
                                        .arg( fileName, sheet.name )
                                        .arg( rowNumber )
                                        .arg( value ) );
              value.clear();
            }
            else
            {
              value = sharedStrings.at( index );
            }
          }
          else if ( type == QLatin1String( "e" ) )
          {
            isError = true;
            errorText = value;
          }
          if ( isError )
          {
            result.issues.append( QStringLiteral( "%1：工作表「%2」第 %3 行单元格公式错误值 %4，该格置空" )
                                      .arg( fileName, sheet.name )
                                      .arg( rowNumber )
                                      .arg( errorText ) );
            value.clear();
          }
          padTo( &row, column );
          row[column - 1] = cellText( value );
          nextColumn = std::max( nextColumn, column + 1 );
        }
      }
      else if ( token == QXmlStreamReader::EndElement && reader.name() == QLatin1String( "row" ) )
      {
        finishRow();
      }
    }
    finishRow();
    if ( reader.hasError() )
    {
      result.issues.append( QStringLiteral( "%1：工作表「%2」XML 解析错误（行 %3）：%4" )
                                .arg( fileName, sheet.name )
                                .arg( reader.lineNumber() )
                                .arg( reader.errorString() ) );
      continue;
    }
    if ( sheet.headers.isEmpty() )
      result.issues.append(
          QStringLiteral( "%1：工作表「%2」没有表头行" ).arg( fileName, sheet.name ) );
    result.sheets.append( sheet );
  }
  if ( result.sheets.isEmpty() )
  {
    result.error = QStringLiteral( "%1：没有任何工作表被成功读取" ).arg( fileName );
    return result;
  }
  result.ok = true;
  return result;
}

// 表头关键字命中（上游 _find_header_index：X/Y 用前缀匹配，其余用包含匹配）。
int headerIndex( const QStringList &headers, const QString &keyword )
{
  const QString target = keyword.toUpper();
  for ( int index = 0; index < headers.size(); ++index )
  {
    const QString cleaned = headers.at( index ).toUpper();
    if ( ( keyword == QLatin1String( "X" ) || keyword == QLatin1String( "Y" ) ) &&
         cleaned.startsWith( keyword ) )
      return index;
    if ( cleaned.contains( target ) )
      return index;
  }
  return -1;
}

QString valueAt( const QStringList &values, int index )
{
  if ( index < 0 || index >= values.size() )
    return QString();
  return values.at( index ).trimmed();
}

} // namespace

QString canonicalWellName( const QString &raw )
{
  QString text = raw.trimmed().toUpper().replace( QLatin1Char( '_' ), QLatin1Char( '-' ) );
  if ( text.isEmpty() )
    return QString();
  static const QRegularExpression pattern( QStringLiteral( "([A-Z]{1,4}\\d{1,2}(?:-[0-9A-Z]+){1,4})" ) );
  const QRegularExpressionMatch match = pattern.match( text );
  if ( match.hasMatch() )
    return match.captured( 1 );
  return text.remove( QStringLiteral( "井" ) );
}

bool parseNumericCell( const QString &raw, double *value )
{
  QString text = raw.trimmed();
  if ( text.isEmpty() )
    return false;
  text.remove( QLatin1Char( ',' ) );
  if ( text.endsWith( QLatin1Char( '%' ) ) )
    text.chop( 1 );
  bool ok = false;
  const double parsed = text.toDouble( &ok );
  if ( !ok || !paleo::wellnumeric::isUsable( parsed ) )
    return false;
  if ( value )
    *value = parsed;
  return true;
}

WorkbookReadResult readWorkbook( const QString &path )
{
  WorkbookReadResult result;
  result.path = path;
  const QFileInfo info( path );
  if ( !info.exists() || !info.isFile() )
  {
    result.error = QStringLiteral( "文件不存在：%1" ).arg( path );
    return result;
  }
  const QString suffix = info.suffix().toLower();
  if ( suffix == QLatin1String( "xml" ) )
  {
    QFile file( path );
    if ( !file.open( QIODevice::ReadOnly ) )
    {
      result.error = QStringLiteral( "无法打开文件：%1" ).arg( path );
      return result;
    }
    return readSpreadsheetMl( path, file.readAll() );
  }
  if ( suffix == QLatin1String( "xlsx" ) )
    return readXlsx( path ); // 非 ZIP/OOXML 由读面如实报因（不靠扩展名或 mime 猜测）
  result.error = QStringLiteral( "不支持的工作簿扩展名：.%1（只读 .xml/.xlsx）" ).arg( suffix );
  return result;
}

WorkbookScanResult scanWorkbookDirectory( const QString &dirPath, bool recursive )
{
  WorkbookScanResult result;
  const QDir root( dirPath );
  if ( !root.exists() )
    return result;
  QDirIterator::IteratorFlags flags = QDirIterator::NoIteratorFlags;
  if ( recursive )
    flags |= QDirIterator::Subdirectories;
  QDirIterator iterator( dirPath, QDir::Files | QDir::NoSymLinks, flags );
  while ( iterator.hasNext() )
  {
    const QString path = iterator.next();
    const QString suffix = QFileInfo( path ).suffix().toLower();
    if ( suffix != QLatin1String( "xml" ) && suffix != QLatin1String( "xlsx" ) )
      continue;
    ++result.filesSeen;
    const WorkbookReadResult read = readWorkbook( path );
    WorkbookScanEntry entry;
    entry.path = path;
    entry.ok = read.ok;
    entry.format = read.format;
    entry.error = read.error;
    entry.sheetCount = read.sheets.size();
    for ( const WorkbookSheet &sheet : read.sheets )
      entry.rowCount += sheet.rows.size();
    entry.issues = read.issues;
    if ( !read.ok )
      ++result.filesFailed;
    result.entries.append( entry );
  }
  return result;
}

WellCoordinateTable readWellCoordinateTable( const QString &path )
{
  WellCoordinateTable table;
  const WorkbookReadResult workbook = readWorkbook( path );
  table.issues = workbook.issues;
  if ( !workbook.ok )
  {
    table.error = workbook.error;
    return table;
  }
  const QString fileName = QFileInfo( path ).fileName();
  for ( const WorkbookSheet &sheet : workbook.sheets )
  {
    const int wellColumn = headerIndex( sheet.headers, QStringLiteral( "井号" ) );
    const int xColumn = headerIndex( sheet.headers, QStringLiteral( "X" ) );
    const int yColumn = headerIndex( sheet.headers, QStringLiteral( "Y" ) );
    if ( wellColumn < 0 || xColumn < 0 || yColumn < 0 )
      continue;
    table.sheetName = sheet.name;
    QHash<QString, int> firstRowOfWell;
    for ( int i = 0; i < sheet.rows.size(); ++i )
    {
      const QStringList &row = sheet.rows.at( i );
      const int displayRow = sheet.rowNumbers.value( i, i + sheet.headerRowNumber + 1 );
      const QString rawName = valueAt( row, wellColumn );
      double x = 0;
      double y = 0;
      const bool hasX = parseNumericCell( valueAt( row, xColumn ), &x );
      const bool hasY = parseNumericCell( valueAt( row, yColumn ), &y );
      if ( rawName.isEmpty() && valueAt( row, xColumn ).isEmpty() && valueAt( row, yColumn ).isEmpty() )
        continue; // 全空行：上游也是直接跳过，不记为坏行
      if ( rawName.isEmpty() )
      {
        table.issues.append( QStringLiteral( "%1：工作表「%2」第 %3 行井号为空，跳过该行" )
                                 .arg( fileName, sheet.name )
                                 .arg( displayRow ) );
        continue;
      }
      const QString well = canonicalWellName( rawName );
      if ( !hasX || !hasY )
      {
        table.issues.append( QStringLiteral( "%1：工作表「%2」第 %3 行井 %4 的 X/Y 非数值（X=\"%5\", Y=\"%6\"），跳过该行" )
                                 .arg( fileName, sheet.name )
                                 .arg( displayRow )
                                 .arg( well, valueAt( row, xColumn ), valueAt( row, yColumn ) ) );
        continue;
      }
      if ( firstRowOfWell.contains( well ) )
      {
        table.issues.append( QStringLiteral( "%1：工作表「%2」第 %3 行井 %4 重复（首次出现在第 %5 行），保留首个" )
                                 .arg( fileName, sheet.name )
                                 .arg( displayRow )
                                 .arg( well )
                                 .arg( firstRowOfWell.value( well ) ) );
        continue;
      }
      firstRowOfWell.insert( well, displayRow );
      table.coordinates.append( WellCoordinate{ well, x, y } );
    }
    table.ok = true;
    return table;
  }
  table.error = QStringLiteral( "%1：没有同时含「井号」「X」「Y」三列的工作表" ).arg( fileName );
  return table;
}

IntervalRow readIntervalRow( const QString &path, const QString &sheetName, const QString &intervalName )
{
  IntervalRow result;
  result.sheetName = sheetName;
  const WorkbookReadResult workbook = readWorkbook( path );
  result.issues = workbook.issues;
  if ( !workbook.ok )
  {
    result.error = workbook.error;
    return result;
  }
  const QString fileName = QFileInfo( path ).fileName();
  const WorkbookSheet *found = nullptr;
  for ( const WorkbookSheet &sheet : workbook.sheets )
  {
    if ( sheet.name == sheetName )
    {
      found = &sheet;
      break;
    }
  }
  if ( !found )
  {
    result.error = QStringLiteral( "%1：没有工作表「%2」" ).arg( fileName, sheetName );
    return result;
  }
  const int intervalColumn = headerIndex( found->headers, QStringLiteral( "层号" ) );
  if ( intervalColumn < 0 )
  {
    result.error = QStringLiteral( "%1：工作表「%2」表头没有「层号」列" ).arg( fileName, sheetName );
    return result;
  }
  // 扫完整张表再判：层号重复是**多义**（调用方指名要的那一行可能正是后面那次），
  // 不能悄悄取首次出现——如实报歧义并给出全部候选行号。
  QVector<int> matchingRows;
  for ( int i = 0; i < found->rows.size(); ++i )
  {
    if ( valueAt( found->rows.at( i ), intervalColumn ) == intervalName )
      matchingRows.append( i ); // 存索引；报告时转换为物理行号
  }
  if ( matchingRows.isEmpty() )
  {
    result.error = QStringLiteral( "%1：工作表「%2」没有层号 %3" ).arg( fileName, sheetName, intervalName );
    return result;
  }
  if ( matchingRows.size() > 1 )
  {
    QStringList rowText;
    for ( int row : matchingRows )
      rowText << QString::number( found->rowNumbers.value( row, row + found->headerRowNumber + 1 ) );
    result.error = QStringLiteral( "%1：工作表「%2」层号 %3 出现 %4 次（第 %5 行），无法确定唯一层段行" )
                       .arg( fileName, sheetName, intervalName )
                       .arg( matchingRows.size() )
                       .arg( rowText.join( QStringLiteral( "、" ) ) );
    return result;
  }
  result.headers = found->headers;
  result.values = found->rows.at( matchingRows.first() );
  result.rowNumber = found->rowNumbers.value( matchingRows.first(), matchingRows.first() + found->headerRowNumber + 1 );
  result.ok = true;
  return result;
}

bool intervalRowNumber( const IntervalRow &row, const QString &field, double *value, QString *error )
{
  const int column = headerIndex( row.headers, field );
  if ( column < 0 )
  {
    if ( error )
      *error = QStringLiteral( "工作表「%1」第 %2 行的表头没有「%3」列" ).arg( row.sheetName ).arg( row.rowNumber ).arg( field );
    return false;
  }
  const QString raw = valueAt( row.values, column );
  if ( raw.isEmpty() )
  {
    if ( error )
      *error = QStringLiteral( "工作表「%1」第 %2 行字段「%3」为空" ).arg( row.sheetName ).arg( row.rowNumber ).arg( field );
    return false;
  }
  if ( !parseNumericCell( raw, value ) )
  {
    if ( error )
      *error = QStringLiteral( "工作表「%1」第 %2 行字段「%3」不是数值：\"%4\"" )
                   .arg( row.sheetName )
                   .arg( row.rowNumber )
                   .arg( field, raw );
    return false;
  }
  return true;
}

// ---- 方向67：曲线统计与因素候选发现 ----

namespace
{

// 深度列口径：表头含「深度」的首列（顶深/底深/深度都算），不进曲线统计。
int depthColumnIndex( const QStringList &headers )
{
  for ( int i = 0; i < headers.size(); ++i )
  {
    if ( headers.at( i ).contains( QStringLiteral( "深度" ) ) )
      return i;
  }
  return -1;
}

} // namespace

CurveSheetStats curveSheetStatistics( const WorkbookSheet &sheet, int minCurveRows )
{
  CurveSheetStats stats;
  stats.sheetName = sheet.name;
  stats.minCurveRows = std::max( 1, minCurveRows );
  if ( sheet.headers.isEmpty() )
  {
    stats.error = QStringLiteral( "工作表「%1」没有表头" ).arg( sheet.name );
    return stats;
  }
  const int depthColumn = depthColumnIndex( sheet.headers );
  if ( depthColumn >= 0 )
  {
    stats.depthColumn = sheet.headers.at( depthColumn );
  }
  else
  {
    stats.issues.append( QStringLiteral( "表头不含「深度」列，深度区间不可用" ) );
  }

  // 行对齐的深度序列（无深度列/空或非数值行 → NaN），供各列深度区间配对。
  std::vector<double> depthByRow;
  if ( depthColumn >= 0 )
  {
    depthByRow.reserve( static_cast<std::size_t>( sheet.rows.size() ) );
    for ( const QStringList &row : sheet.rows )
    {
      const QString raw = depthColumn < row.size() ? row.at( depthColumn ) : QString();
      double value = 0;
      depthByRow.push_back( !raw.trimmed().isEmpty() && parseNumericCell( raw, &value )
                                ? value
                                : std::numeric_limits<double>::quiet_NaN() );
    }
  }

  for ( int column = 0; column < sheet.headers.size(); ++column )
  {
    if ( column == depthColumn )
      continue;
    int badCells = 0;
    std::vector<double> values;
    std::vector<std::size_t> rowIndexes; // values[k] 来自 sheet.rows 的下标（深度配对用）
    for ( std::size_t r = 0; r < sheet.rows.size(); ++r )
    {
      const QString raw =
          column < sheet.rows[r].size() ? sheet.rows[r].at( column ) : QString();
      if ( raw.trimmed().isEmpty() )
        continue;
      double value = 0;
      if ( parseNumericCell( raw, &value ) )
      {
        values.push_back( value );
        rowIndexes.push_back( r );
      }
      else
      {
        ++badCells;
      }
    }
    if ( static_cast<int>( values.size() ) < stats.minCurveRows )
    {
      if ( !values.empty() || badCells > 0 )
        stats.issues.append( QStringLiteral( "列「%1」数值行数 %2 不足 %3，不进曲线统计（坏单元格 %4 个）" )
                                 .arg( sheet.headers.at( column ) )
                                 .arg( values.size() )
                                 .arg( stats.minCurveRows )
                                 .arg( badCells ) );
      continue;
    }
    CurveColumnStats columnStats;
    columnStats.name = sheet.headers.at( column );
    columnStats.rowCount = static_cast<int>( values.size() );
    columnStats.badCells = badCells;
    std::vector<double> sorted = values;
    std::sort( sorted.begin(), sorted.end() );
    columnStats.min = sorted.front();
    columnStats.max = sorted.back();
    columnStats.median = sorted.size() % 2 == 1
                             ? sorted[sorted.size() / 2]
                             : 0.5 * ( sorted[sorted.size() / 2 - 1] + sorted[sorted.size() / 2] );
    double sum = 0;
    for ( double value : values )
      sum += value;
    columnStats.mean = sum / static_cast<double>( values.size() );
    if ( depthColumn >= 0 )
    {
      double depthMin = std::numeric_limits<double>::max();
      double depthMax = std::numeric_limits<double>::lowest();
      bool anyDepth = false;
      for ( std::size_t k = 0; k < rowIndexes.size(); ++k )
      {
        const double depth = depthByRow[rowIndexes[k]];
        if ( !std::isfinite( depth ) )
          continue;
        depthMin = std::min( depthMin, depth );
        depthMax = std::max( depthMax, depth );
        anyDepth = true;
      }
      columnStats.hasDepth = anyDepth;
      columnStats.depthMin = depthMin;
      columnStats.depthMax = depthMax;
    }
    stats.columns.append( columnStats );
  }
  if ( stats.columns.isEmpty() )
    stats.issues.append( QStringLiteral( "工作表「%1」没有可统计的曲线列" ).arg( sheet.name ) );
  stats.ok = true;
  return stats;
}

FactorDiscovery discoverFactorCandidates( const WorkbookSheet &sheet, double minAbsCorrelation,
                                          int minPairedRows, int minCurveRows )
{
  FactorDiscovery discovery;
  if ( !( minAbsCorrelation >= 0 && minAbsCorrelation <= 1 ) )
  {
    discovery.error = QStringLiteral( "minAbsCorrelation 必须在 [0,1]" );
    return discovery;
  }
  const int minRows = std::max( 1, minCurveRows );
  if ( sheet.headers.isEmpty() )
  {
    discovery.error = QStringLiteral( "工作表「%1」没有表头" ).arg( sheet.name );
    return discovery;
  }
  const int depthColumn = depthColumnIndex( sheet.headers );

  // 行对齐数值序列（NaN = 空单元格/非数值）；曲线列 = 有限值行数 ≥ minRows 且
  // 不是深度列（与 curveSheetStatistics 的曲线列判定同口径）。
  std::vector<int> curveColumns;
  std::vector<std::vector<double>> seriesByColumn( static_cast<std::size_t>( sheet.headers.size() ) );
  for ( int column = 0; column < sheet.headers.size(); ++column )
  {
    if ( column == depthColumn )
      continue;
    std::vector<double> &series = seriesByColumn[static_cast<std::size_t>( column )];
    series.assign( static_cast<std::size_t>( sheet.rows.size() ),
                   std::numeric_limits<double>::quiet_NaN() );
    int finite = 0;
    for ( std::size_t r = 0; r < sheet.rows.size(); ++r )
    {
      const QString raw = column < sheet.rows[r].size() ? sheet.rows[r].at( column ) : QString();
      double value = 0;
      if ( !raw.trimmed().isEmpty() && parseNumericCell( raw, &value ) )
      {
        series[r] = value;
        ++finite;
      }
    }
    if ( finite >= minRows )
      curveColumns.push_back( column );
  }
  if ( curveColumns.size() < 2 )
  {
    discovery.ok = true;
    discovery.notes.append( QStringLiteral( "曲线列不足两列，无候选可评估" ) );
    return discovery;
  }

  // Pearson 两遍法（先均值，后方差/协方差）；常数列相关无定义，如实跳过计数。
  const auto pearson = []( const std::vector<double> &a, const std::vector<double> &b, int *paired,
                           bool *degenerate ) -> double {
    double sumA = 0, sumB = 0;
    int n = 0;
    for ( std::size_t r = 0; r < a.size(); ++r )
    {
      if ( std::isfinite( a[r] ) && std::isfinite( b[r] ) )
      {
        sumA += a[r];
        sumB += b[r];
        ++n;
      }
    }
    *paired = n;
    if ( n < 3 )
    {
      *degenerate = true;
      return 0;
    }
    const double meanA = sumA / n;
    const double meanB = sumB / n;
    double cov = 0, varA = 0, varB = 0;
    for ( std::size_t r = 0; r < a.size(); ++r )
    {
      if ( !( std::isfinite( a[r] ) && std::isfinite( b[r] ) ) )
        continue;
      cov += ( a[r] - meanA ) * ( b[r] - meanB );
      varA += ( a[r] - meanA ) * ( a[r] - meanA );
      varB += ( b[r] - meanB ) * ( b[r] - meanB );
    }
    if ( varA <= 0 || varB <= 0 )
    {
      *degenerate = true;
      return 0;
    }
    *degenerate = false;
    return std::clamp( cov / std::sqrt( varA * varB ), -1.0, 1.0 );
  };

  for ( std::size_t i = 0; i + 1 < curveColumns.size(); ++i )
  {
    for ( std::size_t j = i + 1; j < curveColumns.size(); ++j )
    {
      const int columnA = curveColumns[i];
      const int columnB = curveColumns[j];
      ++discovery.pairsConsidered;
      int paired = 0;
      bool degenerate = false;
      const double r = pearson( seriesByColumn[static_cast<std::size_t>( columnA )],
                                seriesByColumn[static_cast<std::size_t>( columnB )], &paired,
                                &degenerate );
      if ( degenerate )
      {
        ++discovery.pairsSkippedDegenerate;
        continue;
      }
      if ( paired < minPairedRows )
      {
        ++discovery.pairsSkippedSparse;
        continue;
      }
      if ( std::fabs( r ) < minAbsCorrelation )
        continue;
      FactorCandidate candidate;
      const QString nameA = sheet.headers.at( columnA );
      const QString nameB = sheet.headers.at( columnB );
      if ( nameA <= nameB )
      {
        candidate.columnA = nameA;
        candidate.columnB = nameB;
      }
      else
      {
        candidate.columnA = nameB;
        candidate.columnB = nameA;
      }
      candidate.correlation = r;
      candidate.pairedRows = paired;
      discovery.candidates.append( candidate );
    }
  }
  // |r| 降序；同 |r| 按列名字典序（稳定可复现）。
  std::sort( discovery.candidates.begin(), discovery.candidates.end(),
             []( const FactorCandidate &a, const FactorCandidate &b ) {
               if ( std::fabs( a.correlation ) != std::fabs( b.correlation ) )
                 return std::fabs( a.correlation ) > std::fabs( b.correlation );
               if ( a.columnA != b.columnA )
                 return a.columnA < b.columnA;
               return a.columnB < b.columnB;
             } );
  discovery.ok = true;
  return discovery;
}

} // namespace paleo::io
