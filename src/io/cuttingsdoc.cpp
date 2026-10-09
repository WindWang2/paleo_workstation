// 层：数据
#include "cuttingsdoc.h"

#include "encodingdetect.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

// 层：数据
namespace paleo::io
{
namespace
{

// 表头归一：去空白、去尾部括号单位（全角/半角，如 顶深(m) / 顶深（米）），
// 供方言大小写不敏感匹配。
QString normalizeCuttingsHeader( const QString &raw )
{
  QString text = raw.trimmed();
  text.remove( QRegularExpression( QStringLiteral( "[(（][^)）]*[)）]\\s*$" ) ) );
  return text.trimmed();
}

// 方言词表 → 列角色。命中即定列（表头从左到右首个命中者），不猜列序之外
// 的顺序信息；同一角色在多个列出现时取首列。
enum ColumnRole
{
  TopColumn = 1,
  BaseColumn = 2,
  LithoColumn = 3,
  DescriptionColumn = 4,
  ColorColumn = 5
};

QHash<QString, int> cuttingsDialect()
{
  static const QHash<QString, int> dialect = [] {
    QHash<QString, int> d;
    const auto add = [&d]( int role, const QStringList &words ) {
      for ( const QString &w : words )
        d.insert( w.toLower(), role );
    };
    add( TopColumn, { QStringLiteral( "顶深" ), QStringLiteral( "顶界深度" ),
                      QStringLiteral( "顶界" ), QStringLiteral( "top" ) } );
    add( BaseColumn, { QStringLiteral( "底深" ), QStringLiteral( "底界深度" ),
                       QStringLiteral( "底界" ), QStringLiteral( "base" ),
                       QStringLiteral( "bot" ) } );
    add( LithoColumn, { QStringLiteral( "岩性" ), QStringLiteral( "定名" ),
                        QStringLiteral( "岩性定名" ), QStringLiteral( "岩石定名" ),
                        QStringLiteral( "岩性名称" ),
                        QStringLiteral( "岩性段" ), QStringLiteral( "litho" ),
                        QStringLiteral( "lithology" ) } );
    add( DescriptionColumn, { QStringLiteral( "描述" ), QStringLiteral( "岩性描述" ),
                              QStringLiteral( "备注" ) } );
    add( ColorColumn, { QStringLiteral( "颜色" ), QStringLiteral( "色" ),
                        QStringLiteral( "color" ) } );
    return d;
  }();
  return dialect;
}

QString roleDisplayName( int role )
{
  switch ( role )
  {
    case TopColumn:
      return QStringLiteral( "顶深" );
    case BaseColumn:
      return QStringLiteral( "底深" );
    default:
      return QStringLiteral( "岩性" );
  }
}

} // namespace

CuttingsTable parseCuttingsSheet( const WorkbookSheet &sheet )
{
  CuttingsTable table;
  table.sheetName = sheet.name;

  const QHash<QString, int> dialect = cuttingsDialect();
  int topColumn = -1, baseColumn = -1, lithoColumn = -1, descriptionColumn = -1, colorColumn = -1;
  for ( int c = 0; c < sheet.headers.size(); ++c )
  {
    const auto it = dialect.constFind( normalizeCuttingsHeader( sheet.headers.at( c ) ).toLower() );
    if ( it == dialect.constEnd() )
      continue;
    switch ( it.value() )
    {
      case TopColumn:
        if ( topColumn < 0 )
          topColumn = c;
        break;
      case BaseColumn:
        if ( baseColumn < 0 )
          baseColumn = c;
        break;
      case LithoColumn:
        if ( lithoColumn < 0 )
          lithoColumn = c;
        break;
      case ColorColumn:
        if ( colorColumn < 0 )
          colorColumn = c;
        break;
      default:
        if ( descriptionColumn < 0 )
          descriptionColumn = c;
        break;
    }
  }
  // 必需列缺失如实点名（不猜列序）；描述列可选。
  QStringList missing;
  if ( topColumn < 0 )
    missing << roleDisplayName( TopColumn );
  if ( baseColumn < 0 )
    missing << roleDisplayName( BaseColumn );
  if ( lithoColumn < 0 )
    missing << roleDisplayName( LithoColumn );
  if ( !missing.isEmpty() )
  {
    table.error = QStringLiteral( "工作表「%1」缺少必需列表头：%2" )
                      .arg( sheet.name, missing.join( QLatin1String( "、" ) ) );
    return table;
  }

  for ( int r = 0; r < sheet.rows.size(); ++r )
  {
    const QStringList &row = sheet.rows.at( r );
    const auto cell = [&row]( int column ) {
      return column >= 0 && column < row.size() ? row.at( column ).trimmed() : QString();
    };
    const QString rawTop = cell( topColumn );
    const QString rawBase = cell( baseColumn );
    const QString rawLitho = cell( lithoColumn );
    const QString rawColor = cell( colorColumn );
    if ( rawTop.isEmpty() && rawBase.isEmpty() && rawLitho.isEmpty() )
      continue; // 全空行：无信息可判，直接跳过（与上游口径一致）
    const int rowNumber =
        r < sheet.rowNumbers.size() ? sheet.rowNumbers.at( r ) : sheet.headerRowNumber + 1 + r;
    const auto skip = [&table, &sheet]( int line, const QString &reason ) {
      table.issues.append( QStringLiteral( "工作表「%1」第 %2 行%3，跳过该行" )
                               .arg( sheet.name )
                               .arg( line )
                               .arg( reason ) );
    };
    double topMd = 0, baseMd = 0;
    if ( !parseNumericCell( rawTop, &topMd ) )
    {
      skip( rowNumber, QStringLiteral( "顶深不是数值：\"%1\"" ).arg( rawTop ) );
      continue;
    }
    if ( !parseNumericCell( rawBase, &baseMd ) )
    {
      skip( rowNumber, QStringLiteral( "底深不是数值：\"%1\"" ).arg( rawBase ) );
      continue;
    }
    if ( !( baseMd > topMd ) )
    {
      skip( rowNumber,
            QStringLiteral( "底深 %1 不大于顶深 %2" )
                .arg( QString::number( baseMd ), QString::number( topMd ) ) );
      continue;
    }
    if ( rawLitho.isEmpty() )
    {
      skip( rowNumber, QStringLiteral( "岩性词面为空" ) );
      continue;
    }
    CuttingsInterval interval;
    interval.topMd = topMd;
    interval.baseMd = baseMd;
    interval.litho = ( !rawColor.isEmpty() && !rawLitho.startsWith( rawColor ) )
                         ? rawColor + rawLitho
                         : rawLitho;
    interval.description = cell( descriptionColumn );
    interval.rowNumber = rowNumber;
    table.intervals.append( interval );
  }
  std::stable_sort( table.intervals.begin(), table.intervals.end(),
                    []( const CuttingsInterval &a, const CuttingsInterval &b ) {
                      return a.topMd < b.topMd;
                    } );
  table.ok = true;
  return table;
}

WorkbookSheet parseTextCuttings( const QString &content, const QString &name,
                                 QStringList *issues )
{
  WorkbookSheet sheet;
  sheet.name = name;

  if ( content.trimmed().isEmpty() )
  {
    if ( issues )
      issues->append( QStringLiteral( "文件为空，无表头行" ) );
    return sheet;
  }

  // 1. 探测分隔符：从首个非空行探测候选（\t, ;, ,），按计数与优先级（\t > ; > ,）
  QChar delimiter = QLatin1Char( ',' );
  int headerStartPos = 0;
  QString firstLine;
  bool inQ = false;
  for ( int i = 0; i < content.size(); ++i )
  {
    const QChar ch = content.at( i );
    if ( ch == QLatin1Char( '"' ) )
      inQ = !inQ;
    else if ( !inQ && ( ch == QLatin1Char( '\n' ) || ch == QLatin1Char( '\r' ) ) )
    {
      firstLine = content.mid( headerStartPos, i - headerStartPos ).trimmed();
      if ( !firstLine.isEmpty() )
        break;
      headerStartPos = i + 1;
    }
  }
  if ( firstLine.isEmpty() )
    firstLine = content.mid( headerStartPos ).trimmed();

  if ( firstLine.isEmpty() )
  {
    if ( issues )
      issues->append( QStringLiteral( "文件为空，无表头行" ) );
    return sheet;
  }

  int bestCount = 0;
  const QVector<QChar> candidates = { QLatin1Char( '\t' ), QLatin1Char( ';' ),
                                      QLatin1Char( ',' ) };
  for ( const QChar candidate : candidates )
  {
    int count = 0;
    bool inQuoteCount = false;
    for ( const QChar ch : firstLine )
    {
      if ( ch == QLatin1Char( '"' ) )
        inQuoteCount = !inQuoteCount;
      else if ( !inQuoteCount && ch == candidate )
        ++count;
    }
    if ( count > bestCount )
    {
      bestCount = count;
      delimiter = candidate;
    }
  }

  // 2. RFC 4180 状态机流式解析全文本为 records
  QVector<QStringList> records;
  QVector<int> recordLineNumbers;

  int currentLineNumber = 1;
  int recordStartLineNumber = 1;
  QString currentField;
  QStringList currentRecord;
  enum State { StartOfField, InUnquoted, InQuoted, AfterQuoted };
  State state = StartOfField;
  bool recordHasData = false;

  const int len = content.size();
  for ( int i = 0; i < len; ++i )
  {
    const QChar ch = content.at( i );
    const QChar nextCh = ( i + 1 < len ) ? content.at( i + 1 ) : QChar();

    if ( state == StartOfField )
    {
      if ( currentRecord.isEmpty() && currentField.isEmpty() )
        recordStartLineNumber = currentLineNumber;

      if ( ch == QLatin1Char( '"' ) )
      {
        state = InQuoted;
        recordHasData = true;
      }
      else if ( ch == delimiter )
      {
        currentRecord.append( QString() );
        recordHasData = true;
      }
      else if ( ch == QLatin1Char( '\r' ) )
      {
        if ( nextCh == QLatin1Char( '\n' ) )
          ++i;
        if ( recordHasData || !currentRecord.isEmpty() )
        {
          currentRecord.append( QString() );
          records.append( currentRecord );
          recordLineNumbers.append( recordStartLineNumber );
          currentRecord.clear();
          recordHasData = false;
        }
        ++currentLineNumber;
      }
      else if ( ch == QLatin1Char( '\n' ) )
      {
        if ( recordHasData || !currentRecord.isEmpty() )
        {
          currentRecord.append( QString() );
          records.append( currentRecord );
          recordLineNumbers.append( recordStartLineNumber );
          currentRecord.clear();
          recordHasData = false;
        }
        ++currentLineNumber;
      }
      else
      {
        state = InUnquoted;
        currentField.append( ch );
        recordHasData = true;
      }
    }
    else if ( state == InUnquoted )
    {
      if ( ch == delimiter )
      {
        currentRecord.append( currentField.trimmed() );
        currentField.clear();
        state = StartOfField;
      }
      else if ( ch == QLatin1Char( '\r' ) )
      {
        if ( nextCh == QLatin1Char( '\n' ) )
          ++i;
        currentRecord.append( currentField.trimmed() );
        currentField.clear();
        records.append( currentRecord );
        recordLineNumbers.append( recordStartLineNumber );
        currentRecord.clear();
        recordHasData = false;
        state = StartOfField;
        ++currentLineNumber;
      }
      else if ( ch == QLatin1Char( '\n' ) )
      {
        currentRecord.append( currentField.trimmed() );
        currentField.clear();
        records.append( currentRecord );
        recordLineNumbers.append( recordStartLineNumber );
        currentRecord.clear();
        recordHasData = false;
        state = StartOfField;
        ++currentLineNumber;
      }
      else
      {
        currentField.append( ch );
      }
    }
    else if ( state == InQuoted )
    {
      if ( ch == QLatin1Char( '"' ) )
      {
        if ( nextCh == QLatin1Char( '"' ) )
        {
          currentField.append( QLatin1Char( '"' ) );
          ++i;
        }
        else
        {
          state = AfterQuoted;
        }
      }
      else if ( ch == QLatin1Char( '\r' ) )
      {
        if ( nextCh == QLatin1Char( '\n' ) )
          ++i;
        currentField.append( QLatin1Char( '\n' ) );
        ++currentLineNumber;
      }
      else if ( ch == QLatin1Char( '\n' ) )
      {
        currentField.append( QLatin1Char( '\n' ) );
        ++currentLineNumber;
      }
      else
      {
        currentField.append( ch );
      }
    }
    else if ( state == AfterQuoted )
    {
      if ( ch == delimiter )
      {
        currentRecord.append( currentField );
        currentField.clear();
        state = StartOfField;
      }
      else if ( ch == QLatin1Char( '\r' ) )
      {
        if ( nextCh == QLatin1Char( '\n' ) )
          ++i;
        currentRecord.append( currentField );
        currentField.clear();
        records.append( currentRecord );
        recordLineNumbers.append( recordStartLineNumber );
        currentRecord.clear();
        recordHasData = false;
        state = StartOfField;
        ++currentLineNumber;
      }
      else if ( ch == QLatin1Char( '\n' ) )
      {
        currentRecord.append( currentField );
        currentField.clear();
        records.append( currentRecord );
        recordLineNumbers.append( recordStartLineNumber );
        currentRecord.clear();
        recordHasData = false;
        state = StartOfField;
        ++currentLineNumber;
      }
      else if ( !ch.isSpace() )
      {
        currentField.append( ch );
      }
    }
  }

  if ( state == InQuoted || state == InUnquoted || state == AfterQuoted || recordHasData || !currentRecord.isEmpty() )
  {
    currentRecord.append( state == InUnquoted ? currentField.trimmed() : currentField );
    records.append( currentRecord );
    recordLineNumbers.append( recordStartLineNumber );
  }

  // 3. 提取表头和数据行
  int headerRecordIndex = -1;
  for ( int r = 0; r < records.size(); ++r )
  {
    bool hasNonEmpty = false;
    for ( const QString &f : records.at( r ) )
    {
      if ( !f.trimmed().isEmpty() )
      {
        hasNonEmpty = true;
        break;
      }
    }
    if ( hasNonEmpty )
    {
      headerRecordIndex = r;
      break;
    }
  }

  if ( headerRecordIndex < 0 )
  {
    if ( issues )
      issues->append( QStringLiteral( "文件为空，无表头行" ) );
    return sheet;
  }

  sheet.headerRowNumber = recordLineNumbers.at( headerRecordIndex );
  sheet.headers = records.at( headerRecordIndex );
  const int expectedCols = sheet.headers.size();

  for ( int r = headerRecordIndex + 1; r < records.size(); ++r )
  {
    const QStringList &row = records.at( r );
    const int lineNo = recordLineNumbers.at( r );

    bool allEmpty = true;
    for ( const QString &f : row )
    {
      if ( !f.trimmed().isEmpty() )
      {
        allEmpty = false;
        break;
      }
    }
    if ( allEmpty )
      continue;

    // 错位行（列数不符）拒绝 + 列因（不再静默进库）
    if ( row.size() != expectedCols )
    {
      if ( issues )
      {
        issues->append( QStringLiteral( "工作表「%1」第 %2 行列数不符（期望 %3 列，实际 %4 列），跳过该行" )
                            .arg( name )
                            .arg( lineNo )
                            .arg( expectedCols )
                            .arg( row.size() ) );
      }
      continue;
    }

    sheet.rowNumbers.append( lineNo );
    sheet.rows.append( row );
  }

  return sheet;
}

namespace
{
WorkbookSheet textSheetFromLines( const QStringList &lines, const QString &name,
                                  QStringList *issues )
{
  return parseTextCuttings( lines.join( QLatin1Char( '\n' ) ), name, issues );
}
} // namespace

CuttingsTable readCuttingsFile( const QString &path )
{
  const QString suffix = QFileInfo( path ).suffix().toLower();
  if ( suffix == QLatin1String( "xlsx" ) || suffix == QLatin1String( "xml" ) )
  {
    const WorkbookReadResult workbook = readWorkbook( path );
    if ( !workbook.ok )
    {
      CuttingsTable table;
      table.error = workbook.error; // 文件级失败如实带因
      return table;
    }
    int chosenIndex = -1;
    CuttingsTable chosen;
    QStringList skippedSheets;
    for ( int i = 0; i < workbook.sheets.size(); ++i )
    {
      CuttingsTable parsed = parseCuttingsSheet( workbook.sheets.at( i ) );
      if ( parsed.ok && chosenIndex < 0 )
      {
        chosenIndex = i;
        chosen = parsed;
        continue;
      }
      if ( parsed.ok )
        skippedSheets.append( QStringLiteral( "工作表「%1」跳过：已选用首个可解析工作表「%2」" )
                                  .arg( workbook.sheets.at( i ).name, chosen.sheetName ) );
      else
        skippedSheets.append( QStringLiteral( "工作表「%1」跳过：%2" )
                                  .arg( workbook.sheets.at( i ).name, parsed.error ) );
    }
    if ( chosenIndex < 0 )
    {
      CuttingsTable table;
      if ( workbook.sheets.isEmpty() )
        table.error = QStringLiteral( "工作簿内没有工作表：%1" ).arg( path );
      else
        table.error =
            QStringLiteral( "没有可解析的岩屑录井工作表（需含顶深/底深/岩性列表头）：%1" )
                .arg( skippedSheets.isEmpty() ? QString() : skippedSheets.first() );
      return table;
    }
    chosen.issues.append( skippedSheets );
    return chosen;
  }
  if ( suffix == QLatin1String( "csv" ) || suffix == QLatin1String( "txt" ) ||
       suffix == QLatin1String( "tsv" ) )
  {
    CuttingsTable table;
    QFile file( path );
    if ( !file.open( QIODevice::ReadOnly ) )
    {
      table.error = QStringLiteral( "文件读取失败：%1（%2）" ).arg( path, file.errorString() );
      return table;
    }
    const QString content = EncodingDetect::decodeText( file.readAll() );
    file.close();
    QStringList textIssues;
    const WorkbookSheet sheet =
        parseTextCuttings( content, QFileInfo( path ).fileName(), &textIssues );
    table = parseCuttingsSheet( sheet );
    table.issues.append( textIssues );
    return table;
  }
  CuttingsTable table;
  table.error = QStringLiteral( "不支持的岩屑录井文件扩展名：.%1" ).arg( suffix );
  return table;
}

} // namespace paleo::io
