// 层：数据
#include "outsourceworkbook.h"

#include "ziparchive.h"

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QXmlStreamReader>

#include <cmath>

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

  const auto finishRow = [&]() {
    if ( !inRow )
      return;
    if ( !rowHasError )
    {
      if ( sheet.headers.isEmpty() )
      {
        if ( rowHasAnyText( row ) )
          sheet.headers = normalizeHeaderRow( row );
      }
      else
      {
        sheet.rows.append( row );
      }
    }
    row.clear();
    rowHasError = false;
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
          else if ( value > kMaxColumns )
          {
            result.issues.append( QStringLiteral( "工作表「%1」第 %2 行：ss:Index=%3 超过列上限 %4，整行跳过" )
                                      .arg( sheet.name )
                                      .arg( rowNumber )
                                      .arg( value )
                                      .arg( kMaxColumns ) );
            rowHasError = true;
          }
          else
          {
            nextColumn = value;
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
        if ( span > 0 && column + span > kMaxColumns )
        {
          result.issues.append( QStringLiteral( "工作表「%1」第 %2 行：合并跨度 %3 超过列上限 %4，按不合并处理" )
                                    .arg( sheet.name )
                                    .arg( rowNumber )
                                    .arg( span )
                                    .arg( kMaxColumns ) );
          span = 0;
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
          padTo( &row, column - 1 );
          row.append( text );
          for ( int i = 0; i < span; ++i )
            row.append( QString() );
          nextColumn = column + span + 1;
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
int columnIndexFromRef( const QString &ref )
{
  int index = 0;
  int letters = 0;
  for ( const QChar &character : ref )
  {
    if ( !character.isLetter() )
      break;
    index = index * 26 + ( character.toUpper().unicode() - 'A' + 1 );
    ++letters;
  }
  if ( letters == 0 || letters > 3 )
    return -1;
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
  if ( !hasSharedStrings )
  {
    result.issues.append( QStringLiteral( "%1：读不到 xl/sharedStrings.xml（%2），字符串单元格会缺值" )
                              .arg( fileName, sharedError ) );
  }
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
    const auto finishRow = [&]() {
      if ( !inRow )
        return;
      if ( sheet.headers.isEmpty() )
      {
        if ( rowHasAnyText( row ) )
          sheet.headers = normalizeHeaderRow( row );
      }
      else
      {
        sheet.rows.append( row );
      }
      row.clear();
      inRow = false;
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
        }
        else if ( inRow && name == QLatin1String( "c" ) )
        {
          const QString ref = reader.attributes().value( QStringLiteral( "r" ) ).toString();
          const QString type = reader.attributes().value( QStringLiteral( "t" ) ).toString();
          int column = nextColumn;
          if ( !ref.isEmpty() )
          {
            const int parsed = columnIndexFromRef( ref );
            if ( parsed < 0 )
            {
              result.issues.append( QStringLiteral( "%1：工作表「%2」第 %3 行单元格引用 %4 非法，该格跳过" )
                                        .arg( fileName, sheet.name )
                                        .arg( rowNumber )
                                        .arg( ref ) );
              continue;
            }
            if ( parsed > kMaxColumns )
            {
              result.issues.append( QStringLiteral( "%1：工作表「%2」第 %3 行列号 %4 超过列上限 %5，该格跳过" )
                                        .arg( fileName, sheet.name )
                                        .arg( rowNumber )
                                        .arg( parsed )
                                        .arg( kMaxColumns ) );
              continue;
            }
            column = parsed;
          }
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
          }
          padTo( &row, column - 1 );
          row.append( cellText( value ) );
          nextColumn = column + 1;
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
  if ( !ok || !std::isfinite( parsed ) )
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
      const int displayRow = i + 2; // 表头占第 1 行
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
      matchingRows.append( i + 2 ); // 表头占第 1 行
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
      rowText << QString::number( row );
    result.error = QStringLiteral( "%1：工作表「%2」层号 %3 出现 %4 次（第 %5 行），无法确定唯一层段行" )
                       .arg( fileName, sheetName, intervalName )
                       .arg( matchingRows.size() )
                       .arg( rowText.join( QStringLiteral( "、" ) ) );
    return result;
  }
  result.headers = found->headers;
  result.values = found->rows.at( matchingRows.first() - 2 );
  result.rowNumber = matchingRows.first();
  result.ok = true;
  return result;
}

bool intervalRowNumber( const IntervalRow &row, const QString &field, double *value, QString *error )
{
  const int column = headerIndex( row.headers, field );
  if ( column < 0 )
  {
    if ( error )
      *error = QStringLiteral( "工作表「%1」第 %2 行（表头为第 1 行）表头没有「%3」列" ).arg( row.sheetName ).arg( row.rowNumber ).arg( field );
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

} // namespace paleo::io
