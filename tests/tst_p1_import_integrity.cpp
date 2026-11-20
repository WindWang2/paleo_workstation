// tst_p1_import_integrity —— P1（#1023/#1028/#1029）宿主解析完整性的跨层回归。
// 覆盖面（对应任务验收矩阵）：
//   A. ZIP64 中央目录 sentinel 选择性字段的独立夹具矩阵（真实 zipListBytes/
//      zipExtractEntry，不依赖本项目 writer 的全字段写法）；
//   B. 空 deflate 的真实解码/CRC 验证（合法空流成功，截断/声明空但有数据/
//      CRC 矛盾失败）；
//   C. DTD 前导 token 策略在字符串/流式/井斜时深/身份探测入口的一致性；
//   D. OOXML 辅助部件（rels/sharedStrings/styles）在场但损坏的可见失败与
//      兄弟工作表保留；
//   E. 预览预算（limited/损坏/成功三分）与行粒度取消；
//   F. 坏容器经宿主真实导入入口（importProjectFileEx）的回执：拒收、合法
//      外委簿照常导入、取消后重试成功、解析报告计数入库。
#include <QtTest>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <zlib.h>

#include "../src/io/dataimportservice.h"
#include "../src/io/importclassification.h"
#include "../src/io/outsourceworkbook.h"
#include "../src/io/wellcompositexml.h"
#include "../src/io/ziparchive.h"
#include "../src/metadata/paleoprojectstore.h"

using namespace paleo::io;

namespace {

void writeU16( QByteArray &b, quint16 v )
{
  b.append( static_cast<char>( v & 0xFF ) );
  b.append( static_cast<char>( ( v >> 8 ) & 0xFF ) );
}
void writeU32( QByteArray &b, quint32 v )
{
  b.append( static_cast<char>( v & 0xFF ) );
  b.append( static_cast<char>( ( v >> 8 ) & 0xFF ) );
  b.append( static_cast<char>( ( v >> 16 ) & 0xFF ) );
  b.append( static_cast<char>( ( v >> 24 ) & 0xFF ) );
}
void writeU64( QByteArray &b, quint64 v )
{
  writeU32( b, static_cast<quint32>( v & 0xFFFFFFFFULL ) );
  writeU32( b, static_cast<quint32>( v >> 32 ) );
}
quint32 crc32Of( const QByteArray &data )
{
  return static_cast<quint32>(
      crc32( 0L, reinterpret_cast<const Bytef *>( data.constData() ), static_cast<uInt>( data.size() ) ) );
}
bool writeFile( const QString &path, const QByteArray &content )
{
  QDir().mkpath( QFileInfo( path ).absolutePath() );
  QFile f( path );
  return f.open( QIODevice::WriteOnly ) && f.write( content ) == content.size();
}

// raw deflate（与生产同参数：-MAX_WBITS），测试夹具用。
QByteArray deflateRaw( const QByteArray &input )
{
  z_stream stream;
  std::memset( &stream, 0, sizeof( stream ) );
  if ( deflateInit2( &stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY ) != Z_OK )
    return {};
  QByteArray out;
  out.resize( static_cast<qsizetype>( deflateBound( &stream, static_cast<uLong>( input.size() ) ) ) );
  stream.next_in = reinterpret_cast<Bytef *>( const_cast<char *>( input.constData() ) );
  stream.avail_in = static_cast<uInt>( input.size() );
  stream.next_out = reinterpret_cast<Bytef *>( out.data() );
  stream.avail_out = static_cast<uInt>( out.size() );
  const int status = deflate( &stream, Z_FINISH );
  const bool ok = status == Z_STREAM_END;
  out.resize( static_cast<qsizetype>( stream.total_out ) );
  deflateEnd( &stream );
  return ok ? out : QByteArray();
}

// ---- 独立 ZIP 夹具（手工组包，不复用生产 writer——writer 恒写全字段，
// 测不出「他源只写所需子集」的选择性消费缺陷）----

struct CentralEntrySpec
{
  quint32 usizeField;   // 中央目录 24 处的 32 位值（可为 0xFFFFFFFF 哨兵）
  quint32 csizeField;   // 中央目录 20 处
  quint32 offsetField;  // 中央目录 42 处
  quint32 crc;
  QByteArray extra;     // 中央目录 extra 区原样写入（调用方构造 0x0001 块）
  QByteArray data;      // stored 条目数据
};

// 本地段 + 中央目录 + EOCD 一次组好（手工夹具，见文件头注释）。
struct BuiltArchive
{
  QByteArray bytes;
  quint32 cdOffset;
  quint32 cdSize;
};
BuiltArchive buildArchive( const QList<CentralEntrySpec> &specs, const QStringList &names )
{
  BuiltArchive out;
  QByteArray &a = out.bytes;
  QVector<quint32> localOffsets;
  for ( int i = 0; i < specs.size(); ++i )
  {
    const CentralEntrySpec &spec = specs.at( i );
    const QByteArray nameBytes = names.at( i ).toUtf8();
    localOffsets.append( static_cast<quint32>( a.size() ) );
    a.append( "\x50\x4b\x03\x04", 4 );
    writeU16( a, 10 );
    writeU16( a, 0 );
    writeU16( a, 0 );
    writeU32( a, 0 );
    writeU32( a, spec.crc );
    writeU32( a, static_cast<quint32>( spec.data.size() ) );
    writeU32( a, static_cast<quint32>( spec.data.size() ) );
    writeU16( a, static_cast<quint16>( nameBytes.size() ) );
    writeU16( a, 0 );
    a.append( nameBytes );
    a.append( spec.data );
  }
  out.cdOffset = static_cast<quint32>( a.size() );
  for ( int i = 0; i < specs.size(); ++i )
  {
    const CentralEntrySpec &spec = specs.at( i );
    const QByteArray nameBytes = names.at( i ).toUtf8();
    a.append( "\x50\x4b\x01\x02", 4 );
    writeU16( a, 20 );
    writeU16( a, 10 );
    writeU16( a, 0 );
    writeU16( a, 0 );
    writeU32( a, 0 );
    writeU32( a, spec.crc );
    writeU32( a, spec.csizeField );
    writeU32( a, spec.usizeField );
    writeU16( a, static_cast<quint16>( nameBytes.size() ) );
    writeU16( a, static_cast<quint16>( spec.extra.size() ) );
    writeU16( a, 0 );
    writeU16( a, 0 );
    writeU16( a, 0 );
    writeU32( a, 0 );
    writeU32( a, spec.offsetField );
    a.append( nameBytes );
    a.append( spec.extra );
  }
  out.cdSize = static_cast<quint32>( a.size() ) - out.cdOffset;
  a.append( "\x50\x4b\x05\x06", 4 );
  writeU16( a, 0 );
  writeU16( a, 0 );
  writeU16( a, static_cast<quint16>( specs.size() ) );
  writeU16( a, static_cast<quint16>( specs.size() ) );
  writeU32( a, out.cdSize );
  writeU32( a, out.cdOffset );
  writeU16( a, 0 );
  return out;
}

// 0x0001 扩展块：按「所需字段 + 可选盘号」顺序拼装（真实第三方 writer 的
// 选择性写法），不再固定 24 字节。
QByteArray zip64Extra( bool withUsize, bool withCsize, bool withOffset, bool withDisk,
                       quint64 usize, quint64 csize, quint64 offset, quint32 disk )
{
  QByteArray payload;
  if ( withUsize )
    writeU64( payload, usize );
  if ( withCsize )
    writeU64( payload, csize );
  if ( withOffset )
    writeU64( payload, offset );
  if ( withDisk )
    writeU32( payload, disk );
  QByteArray extra;
  writeU16( extra, 0x0001 );
  writeU16( extra, static_cast<quint16>( payload.size() ) );
  extra.append( payload );
  return extra;
}

// ---- XML / OOXML 夹具 ----

const char *kSsXmlns = "urn:schemas-microsoft-com:office:spreadsheet";

QByteArray spreadsheetMl( const QByteArray &innerRows, bool withDtd )
{
  QByteArray xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  if ( withDtd )
    xml += "<!DOCTYPE Workbook [<!ENTITY boom \"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\">]>\n";
  xml += "<Workbook xmlns=\"" + QByteArray( kSsXmlns ) + "\" xmlns:ss=\"" + QByteArray( kSsXmlns ) + "\">\n";
  xml += " <Worksheet ss:Name=\"数据表\">\n  <Table>\n";
  xml += "   <Row><Cell><Data ss:Type=\"String\">井号</Data></Cell>"
         "<Cell><Data ss:Type=\"String\">层位</Data></Cell>"
         "<Cell><Data ss:Type=\"String\">顶深</Data></Cell></Row>\n";
  xml += innerRows;
  xml += "  </Table>\n </Worksheet>\n</Workbook>\n";
  return xml;
}

QByteArray deviationXml( bool withDtd, bool truncated )
{
  QByteArray xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  if ( withDtd )
    xml += "<!DOCTYPE Workbook [<!ENTITY e \"x\">]>\n";
  xml += "<Workbook xmlns=\"" + QByteArray( kSsXmlns ) + "\" xmlns:ss=\"" + QByteArray( kSsXmlns ) + "\">\n";
  xml += " <Worksheet ss:Name=\"井斜\">\n  <Table>\n";
  xml += "   <Row><Cell><Data>MD</Data></Cell><Cell><Data>INC</Data></Cell><Cell><Data>AZI</Data></Cell></Row>\n";
  xml += "   <Row><Cell><Data>0</Data></Cell><Cell><Data>0</Data></Cell><Cell><Data>0</Data></Cell></Row>\n";
  xml += "   <Row><Cell><Data>100</Data></Cell><Cell><Data>30</Data></Cell><Cell><Data>45</Data></Cell></Row>\n";
  xml += "  </Table>\n </Worksheet>\n</Workbook>\n";
  if ( truncated )
    xml.truncate( xml.size() - 30 ); // 截在中途：无 </Workbook> 收尾
  return xml;
}

struct XlsxParts
{
  QByteArray workbook;
  QByteArray rels;
  QByteArray sheet1;
  QByteArray sheet2;   // 空 = 不写第二张表
  QByteArray shared;   // 空 = 无 sharedStrings 条目
  QByteArray styles;   // 空 = 无 styles 条目
};

QByteArray defaultWorkbookXml( bool twoSheets )
{
  QByteArray xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
      "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">\n<sheets>\n"
      "<sheet name=\"数据\" r:id=\"rId1\"/>\n";
  if ( twoSheets )
    xml += "<sheet name=\"说明\" r:id=\"rId2\"/>\n";
  xml += "</sheets>\n</workbook>\n";
  return xml;
}

QByteArray defaultRelsXml( bool externalFirst )
{
  QByteArray xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n";
  if ( externalFirst )
    xml += "<Relationship Id=\"rId1\" "
           "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" "
           "Target=\"http://example.invalid/other.xlsx\" TargetMode=\"External\"/>\n"
           "<Relationship Id=\"rId2\" "
           "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" "
           "Target=\"worksheets/sheet2.xml\"/>\n";
  else
    xml += "<Relationship Id=\"rId1\" "
           "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" "
           "Target=\"worksheets/sheet1.xml\"/>\n";
  xml += "</Relationships>\n";
  return xml;
}

QByteArray defaultSheetXml( bool malformed )
{
  QByteArray xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">\n<sheetData>\n"
      "<row r=\"1\"><c r=\"A1\" t=\"inlineStr\"><is><t>井号</t></is></c>"
      "<c r=\"B1\" t=\"inlineStr\"><is><t>层位</t></is></c>"
      "<c r=\"C1\" t=\"inlineStr\"><is><t>顶深</t></is></c></row>\n"
      "<row r=\"2\"><c r=\"A2\" t=\"inlineStr\"><is><t>W1</t></is></c>"
      "<c r=\"B2\" t=\"inlineStr\"><is><t>T70</t></is></c>"
      "<c r=\"C2\"><v>1000</v></c></row>\n";
  if ( malformed )
  {
    xml += "<row r=\"3\"><c r=\"A3\" t=\"inlineStr\"><is><t>W2"; // 截断的元素
    return xml;
  }
  xml += "</sheetData>\n</worksheet>\n";
  return xml;
}

QByteArray buildXlsx( const XlsxParts &parts )
{
  QVector<ZipWriteEntry> entries;
  entries << ZipWriteEntry{ QStringLiteral( "xl/workbook.xml" ), parts.workbook };
  entries << ZipWriteEntry{ QStringLiteral( "xl/_rels/workbook.xml.rels" ), parts.rels };
  entries << ZipWriteEntry{ QStringLiteral( "xl/worksheets/sheet1.xml" ), parts.sheet1 };
  if ( !parts.sheet2.isEmpty() )
    entries << ZipWriteEntry{ QStringLiteral( "xl/worksheets/sheet2.xml" ), parts.sheet2 };
  if ( !parts.shared.isEmpty() )
    entries << ZipWriteEntry{ QStringLiteral( "xl/sharedStrings.xml" ), parts.shared };
  if ( !parts.styles.isEmpty() )
    entries << ZipWriteEntry{ QStringLiteral( "xl/styles.xml" ), parts.styles };
  QString error;
  const QByteArray archive = zipBuildArchive( entries, ZipBuildOptions{}, &error );
  if ( archive.isEmpty() )
    qWarning( "buildXlsx: %s", qPrintable( error ) );
  return archive;
}

// 井分层 xlsx（井号/层位/顶深），带 shared 字符串与日期样式——合法导入夹具。
QByteArray validWellStratXlsx()
{
  XlsxParts parts;
  parts.workbook = defaultWorkbookXml( false );
  parts.rels = defaultRelsXml( false );
  parts.sheet1 = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">\n<sheetData>\n"
      "<row r=\"1\"><c r=\"A1\" t=\"s\"><v>0</v></c><c r=\"B1\" t=\"s\"><v>1</v></c>"
      "<c r=\"C1\" t=\"s\"><v>2</v></c></row>\n"
      "<row r=\"2\"><c r=\"A2\" t=\"s\"><v>3</v></c><c r=\"B2\" t=\"s\"><v>4</v></c>"
      "<c r=\"C2\"><v>1000</v></c></row>\n"
      "<row r=\"3\"><c r=\"A3\" t=\"s\"><v>3</v></c><c r=\"B3\" t=\"s\"><v>5</v></c>"
      "<c r=\"C3\"><v>1200</v></c></row>\n"
      "</sheetData>\n</worksheet>\n";
  parts.shared = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
      "count=\"6\" uniqueCount=\"6\">"
      "<si><t>井号</t></si><si><t>层位</t></si><si><t>顶深</t></si>"
      "<si><t>W1</t></si><si><t>T70</t></si><si><t>T71</t></si></sst>\n";
  parts.styles = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
      "<numFmts><numFmt numFmtId=\"164\" formatCode=\"yyyy\\-mm\\-dd\"/></numFmts>"
      "<cellXfs count=\"1\"><xf numFmtId=\"164\"/></cellXfs></styleSheet>\n";
  return buildXlsx( parts );
}

} // namespace

class TestP1ImportIntegrity : public QObject
{
  Q_OBJECT

private slots:
  // A. ZIP64 sentinel 矩阵
  void zip64OffsetOnlySentinelRestoresTrueOffset();
  void zip64SizeOnlySentinelsRestoreSizes();
  void zip64MixedSentinelsWithHoleConsumeInOrder();
  void zip64FullRecordStillWorks();
  void zip64MissingExtraFails();
  void zip64TruncatedFieldFails();
  void zip64DuplicateBlockFails();
  void zip64SubBlockOverrunFails();
  void zip64NonzeroDiskFails();
  // B. 空 deflate
  void emptyDeflateValidStreamsExtract();
  void emptyDeflateTruncatedFails();
  void emptyDeflateDeclaredEmptyButHasDataFails();
  void emptyDeflateCrcContractEnforced();
  void extractFailureKeepsOutParamUntouched();
  // C. DTD 策略
  void dtdRejectedAcrossStringAndStreamingEntries();
  void dtdRejectedByDeviationAndTimeDepthEntry();
  void dtdRejectedInOoxmlParts();
  void leadingDeclarationBomCommentStillAccepted();
  void truncatedDeviationXmlFailsInsteadOfPartialRows();
  // D. OOXML 部件失败可见
  void malformedSharedStringsFailsWholeWorkbook();
  void malformedRelsFailsWholeWorkbook();
  void malformedStylesFailsWholeWorkbook();
  void externalRelationshipNotTreatedAsWorksheet();
  void malformedSheetKeepsHealthySibling();
  void missingSharedStringsKeepsCellLevelIssue();
  // E. 预算与取消
  void previewLimitsDistinguishCorruptFromLimited();
  void previewExpandedBudgetLimit();
  void cancelStopsWorkbookReadAtRowGranularity();
  // F. 宿主导入回执
  void corruptXlsxFailsImportWithRealError();
  void validNonWellWorkbookStillImportsAsOutsource();
  void cancelledImportFailsFastAndRetrySucceeds();
  void changedSourceAfterPreviewRejectedByHash();
  void partialAcceptanceKeepsRowNumbersAndCounts();
  void wellStratImportRecordsParseReport();
};

// ---------- A. ZIP64 ----------

void TestP1ImportIntegrity::zip64OffsetOnlySentinelRestoresTrueOffset()
{
  const QByteArray data = "AAA";
  CentralEntrySpec spec;
  spec.data = data;
  spec.crc = crc32Of( data );
  spec.usizeField = 3;               // 非哨兵：usize 字段不在 extra 里
  spec.csizeField = 3;
  spec.offsetField = 0xFFFFFFFFu;    // 只有偏移走哨兵
  // 单条目本地头在 0：extra = [offset=0(8), disk=0(4)]，共 12 字节——
  // 旧固定 +20 偏移读法会读到块外。
  spec.extra = zip64Extra( false, false, true, true, 0, 0, 0, 0 );
  const BuiltArchive built = buildArchive( { spec }, { QStringLiteral( "a.txt" ) } );
  const ZipListResult list = zipListBytes( built.bytes );
  QVERIFY2( list.ok, qPrintable( list.error ) );
  QCOMPARE( list.entries.size(), 1 );
  const ZipEntry &entry = list.entries.first();
  QCOMPARE( entry.uncompressedSize, Q_UINT64_C( 3 ) ); // 非哨兵字段不被误消费/改写
  QCOMPARE( entry.compressedSize, Q_UINT64_C( 3 ) );
  QCOMPARE( entry.localHeaderOffset, Q_UINT64_C( 0 ) ); // 哨兵还原成真实偏移
  QByteArray out;
  QString err;
  QVERIFY2( zipExtractEntry( built.bytes, entry, &out, &err ), qPrintable( err ) );
  QCOMPARE( out, data );
}

void TestP1ImportIntegrity::zip64SizeOnlySentinelsRestoreSizes()
{
  const QByteArray data = "ABCDEFGH";
  CentralEntrySpec spec;
  spec.data = data;
  spec.crc = crc32Of( data );
  spec.usizeField = 0xFFFFFFFFu;
  spec.csizeField = 8; // 只有 usize 走哨兵
  spec.offsetField = 0;
  spec.extra = zip64Extra( true, false, false, false, 8, 0, 0, 0 ); // [usize(8)]
  const BuiltArchive built = buildArchive( { spec }, { QStringLiteral( "b.txt" ) } );
  const ZipListResult list = zipListBytes( built.bytes );
  QVERIFY2( list.ok, qPrintable( list.error ) );
  QCOMPARE( list.entries.first().uncompressedSize, Q_UINT64_C( 8 ) );
  QCOMPARE( list.entries.first().compressedSize, Q_UINT64_C( 8 ) );
  QCOMPARE( list.entries.first().localHeaderOffset, Q_UINT64_C( 0 ) );

  CentralEntrySpec csizeOnly = spec;
  csizeOnly.usizeField = 8;
  csizeOnly.csizeField = 0xFFFFFFFFu;
  csizeOnly.extra = zip64Extra( false, true, false, false, 0, 8, 0, 0 ); // [csize(8)]
  const BuiltArchive built2 = buildArchive( { csizeOnly }, { QStringLiteral( "c.txt" ) } );
  const ZipListResult list2 = zipListBytes( built2.bytes );
  QVERIFY2( list2.ok, qPrintable( list2.error ) );
  QCOMPARE( list2.entries.first().compressedSize, Q_UINT64_C( 8 ) );
}

void TestP1ImportIntegrity::zip64MixedSentinelsWithHoleConsumeInOrder()
{
  // usize 与 offset 哨兵、csize 非哨兵：extra = [usize(8), offset(8), disk(4)]
  // ——旧固定偏移读法会把 offset 读到 +20（块外），顺序消费必须正确。
  const QByteArray data = "MIXED";
  CentralEntrySpec spec;
  spec.data = data;
  spec.crc = crc32Of( data );
  spec.usizeField = 0xFFFFFFFFu;
  spec.csizeField = 5;
  spec.offsetField = 0xFFFFFFFFu;
  spec.extra = zip64Extra( true, false, true, true, 5, 0, 0, 0 ); // [usize(8), offset(8), disk(4)]
  const BuiltArchive built = buildArchive( { spec }, { QStringLiteral( "m.txt" ) } );
  const ZipListResult list = zipListBytes( built.bytes );
  QVERIFY2( list.ok, qPrintable( list.error ) );
  QCOMPARE( list.entries.first().uncompressedSize, Q_UINT64_C( 5 ) );
  QCOMPARE( list.entries.first().localHeaderOffset, Q_UINT64_C( 0 ) );
  QByteArray out;
  QString err;
  QVERIFY( zipExtractEntry( built.bytes, list.entries.first(), &out, &err ) );
  QCOMPARE( out, data );
}

void TestP1ImportIntegrity::zip64FullRecordStillWorks()
{
  const QByteArray data = "FULL-24-BYTE-RECORD";
  CentralEntrySpec spec;
  spec.data = data;
  spec.crc = crc32Of( data );
  spec.usizeField = 0xFFFFFFFFu;
  spec.csizeField = 0xFFFFFFFFu;
  spec.offsetField = 0xFFFFFFFFu;
  spec.extra = zip64Extra( true, true, true, true, 19, 19, 0, 0 );
  const BuiltArchive built = buildArchive( { spec }, { QStringLiteral( "f.txt" ) } );
  const ZipListResult list = zipListBytes( built.bytes );
  QVERIFY2( list.ok, qPrintable( list.error ) );
  QCOMPARE( list.entries.first().uncompressedSize, Q_UINT64_C( 19 ) );
  QCOMPARE( list.entries.first().compressedSize, Q_UINT64_C( 19 ) );
  QCOMPARE( list.entries.first().localHeaderOffset, Q_UINT64_C( 0 ) );
}

void TestP1ImportIntegrity::zip64MissingExtraFails()
{
  CentralEntrySpec spec;
  spec.data = "X";
  spec.crc = crc32Of( "X" );
  spec.usizeField = 0xFFFFFFFFu;
  spec.csizeField = 1;
  spec.offsetField = 0;
  spec.extra = zip64Extra( false, false, false, false, 0, 0, 0, 0 ); // id 在但载荷 0
  const BuiltArchive built = buildArchive( { spec }, { QStringLiteral( "n.txt" ) } );
  const ZipListResult list = zipListBytes( built.bytes );
  QVERIFY( !list.ok );
  QVERIFY2( list.error.contains( QStringLiteral( "原始大小" ) ) || list.error.contains( QStringLiteral( "usize" ) ),
            qPrintable( list.error ) );
  QVERIFY( list.error.contains( QStringLiteral( "n.txt" ) ) );
}

void TestP1ImportIntegrity::zip64TruncatedFieldFails()
{
  CentralEntrySpec spec;
  spec.data = "Y";
  spec.crc = crc32Of( "Y" );
  spec.usizeField = 1;
  spec.csizeField = 1;
  spec.offsetField = 0xFFFFFFFFu;
  // offset 哨兵但块里只有 4 字节垃圾——不足 8 字节字段
  QByteArray extra;
  writeU16( extra, 0x0001 );
  writeU16( extra, 4 );
  writeU32( extra, 0 );
  spec.extra = extra;
  const BuiltArchive built = buildArchive( { spec }, { QStringLiteral( "t.txt" ) } );
  const ZipListResult list = zipListBytes( built.bytes );
  QVERIFY( !list.ok );
  QVERIFY2( list.error.contains( QStringLiteral( "偏移" ) ), qPrintable( list.error ) );
}

void TestP1ImportIntegrity::zip64DuplicateBlockFails()
{
  CentralEntrySpec spec;
  spec.data = "D";
  spec.crc = crc32Of( "D" );
  spec.usizeField = 0xFFFFFFFFu;
  spec.csizeField = 1;
  spec.offsetField = 0;
  spec.extra = zip64Extra( true, false, false, false, 1, 0, 0, 0 ) +
               zip64Extra( true, false, false, false, 2, 0, 0, 0 ); // 两块 0x0001
  const BuiltArchive built = buildArchive( { spec }, { QStringLiteral( "d.txt" ) } );
  const ZipListResult list = zipListBytes( built.bytes );
  QVERIFY( !list.ok );
  QVERIFY2( list.error.contains( QStringLiteral( "重复" ) ), qPrintable( list.error ) );
}

void TestP1ImportIntegrity::zip64SubBlockOverrunFails()
{
  CentralEntrySpec spec;
  spec.data = "O";
  spec.crc = crc32Of( "O" );
  spec.usizeField = 0xFFFFFFFFu;
  spec.csizeField = 1;
  spec.offsetField = 0;
  // 前置一个声明超长的无关子块 + 合法 0x0001
  QByteArray bogus;
  writeU16( bogus, 0x9901 );
  writeU16( bogus, 200 ); // 声明 200 字节，实际没有
  spec.extra = bogus + zip64Extra( true, false, false, false, 1, 0, 0, 0 );
  const BuiltArchive built = buildArchive( { spec }, { QStringLiteral( "o.txt" ) } );
  const ZipListResult list = zipListBytes( built.bytes );
  QVERIFY( !list.ok );
  QVERIFY2( list.error.contains( QStringLiteral( "越界" ) ), qPrintable( list.error ) );
}

void TestP1ImportIntegrity::zip64NonzeroDiskFails()
{
  CentralEntrySpec spec;
  spec.data = "K";
  spec.crc = crc32Of( "K" );
  spec.usizeField = 1;
  spec.csizeField = 1;
  spec.offsetField = 0xFFFFFFFFu;
  spec.extra = zip64Extra( false, false, true, true, 0, 0, 0, 7 ); // 盘号 7
  const BuiltArchive built = buildArchive( { spec }, { QStringLiteral( "k.txt" ) } );
  const ZipListResult list = zipListBytes( built.bytes );
  QVERIFY( !list.ok );
  QVERIFY2( list.error.contains( QStringLiteral( "跨盘" ) ), qPrintable( list.error ) );
}

// ---------- B. 空 deflate ----------

namespace {

// 单条目 deflate 压缩段夹具：中央目录如实声明 method/csize/usize/crc，
// 压缩字节由调用方给。
BuiltArchive deflateEntryArchive( const QByteArray &compressed, quint64 declaredUsize,
                                  quint32 declaredCrc, quint32 csizeField, quint32 usizeField )
{
  const QString name = QStringLiteral( "empty.bin" );
  const QByteArray nameBytes = name.toUtf8();
  BuiltArchive out;
  QByteArray &a = out.bytes;
  a.append( "\x50\x4b\x03\x04", 4 );
  writeU16( a, 20 );
  writeU16( a, 0 );
  writeU16( a, 8 ); // deflate
  writeU32( a, 0 );
  writeU32( a, declaredCrc );
  writeU32( a, csizeField );
  writeU32( a, usizeField );
  writeU16( a, static_cast<quint16>( nameBytes.size() ) );
  writeU16( a, 0 );
  a.append( nameBytes );
  a.append( compressed );
  out.cdOffset = static_cast<quint32>( a.size() );
  a.append( "\x50\x4b\x01\x02", 4 );
  writeU16( a, 20 );
  writeU16( a, 20 );
  writeU16( a, 0 );
  writeU16( a, 8 );
  writeU32( a, 0 );
  writeU32( a, declaredCrc );
  writeU32( a, csizeField );
  writeU32( a, usizeField );
  writeU16( a, static_cast<quint16>( nameBytes.size() ) );
  writeU16( a, 0 );
  writeU16( a, 0 );
  writeU16( a, 0 );
  writeU16( a, 0 );
  writeU32( a, 0 );
  writeU32( a, 0 ); // 本地头偏移 0
  a.append( nameBytes );
  out.cdSize = static_cast<quint32>( a.size() ) - out.cdOffset;
  a.append( "\x50\x4b\x05\x06", 4 );
  writeU16( a, 0 );
  writeU16( a, 0 );
  writeU16( a, 1 );
  writeU16( a, 1 );
  writeU32( a, out.cdSize );
  writeU32( a, out.cdOffset );
  writeU16( a, 0 );
  return out;
}

} // namespace

void TestP1ImportIntegrity::emptyDeflateValidStreamsExtract()
{
  // 两种合法空流：final 固定 Huffman 空块 (03 00) 与 final stored 空块
  // (01 00 00 FF FF：BFINAL=1 BTYPE=00 + LEN=0 + NLEN=~LEN=0xFFFF)
  for ( const QByteArray &stream : { QByteArray::fromHex( "0300" ),
                                     QByteArray::fromHex( "010000ffff" ) } )
  {
    const BuiltArchive built = deflateEntryArchive( stream, 0, 0, quint32( stream.size() ), 0 );
    const ZipListResult list = zipListBytes( built.bytes );
    QVERIFY2( list.ok, qPrintable( list.error ) );
    QByteArray out = QByteArrayLiteral( "SENTINEL" );
    QString err;
    QVERIFY2( zipExtractEntry( built.bytes, list.entries.first(), &out, &err ), qPrintable( err ) );
    QVERIFY( out.isEmpty() ); // 成功时写 *out：空内容
  }
}

void TestP1ImportIntegrity::emptyDeflateTruncatedFails()
{
  // 一个字节成不了任何合法流；零字节更不行
  for ( const QByteArray &stream : { QByteArray::fromHex( "03" ), QByteArray() } )
  {
    const BuiltArchive built = deflateEntryArchive( stream, 0, 0, quint32( stream.size() ), 0 );
    const ZipListResult list = zipListBytes( built.bytes );
    QVERIFY2( list.ok, qPrintable( list.error ) );
    QByteArray out;
    QString err;
    QVERIFY2( !zipExtractEntry( built.bytes, list.entries.first(), &out, &err ), "截断空流必须失败" );
    QVERIFY2( err.contains( QStringLiteral( "空解压流" ) ), qPrintable( err ) );
  }
}

void TestP1ImportIntegrity::emptyDeflateDeclaredEmptyButHasDataFails()
{
  const QByteArray payload = QByteArrayLiteral( "hello data" );
  const QByteArray compressed = deflateRaw( payload );
  QVERIFY( !compressed.isEmpty() );
  // 声明 usize=0 但压缩段解出真实数据；CRC 也按声明空给 0——仍必须失败
  const BuiltArchive built = deflateEntryArchive( compressed, 0, 0,
                                                   quint32( compressed.size() ), 0 );
  const ZipListResult list = zipListBytes( built.bytes );
  QVERIFY2( list.ok, qPrintable( list.error ) );
  QByteArray out;
  QString err;
  QVERIFY2( !zipExtractEntry( built.bytes, list.entries.first(), &out, &err ),
            "声明空但实际产生数据的流必须失败" );
  QVERIFY2( err.contains( QStringLiteral( "空解压流" ) ), qPrintable( err ) );
}

void TestP1ImportIntegrity::emptyDeflateCrcContractEnforced()
{
  // 合法空流但中央目录 CRC 声明非零——矛盾必须暴露（空数据 CRC32==0）
  const BuiltArchive built = deflateEntryArchive( QByteArray::fromHex( "0300" ), 0,
                                                   0xDEADBEEF, 2, 0 );
  const ZipListResult list = zipListBytes( built.bytes );
  QVERIFY2( list.ok, qPrintable( list.error ) );
  QByteArray out;
  QString err;
  QVERIFY2( !zipExtractEntry( built.bytes, list.entries.first(), &out, &err ),
            "空数据 CRC 非零声明必须失败" );
  QVERIFY2( err.contains( QStringLiteral( "CRC32" ) ), qPrintable( err ) );
}

void TestP1ImportIntegrity::extractFailureKeepsOutParamUntouched()
{
  const QByteArray payload = QByteArrayLiteral( "kept" );
  const QByteArray compressed = deflateRaw( payload );
  const BuiltArchive built = deflateEntryArchive( compressed, 0, crc32Of( payload ),
                                                   quint32( compressed.size() ), 0 );
  const ZipListResult list = zipListBytes( built.bytes );
  QVERIFY2( list.ok, qPrintable( list.error ) );
  QByteArray out = QByteArrayLiteral( "PREVIOUS" );
  QString err;
  QVERIFY( !zipExtractEntry( built.bytes, list.entries.first(), &out, &err ) );
  QCOMPARE( out, QByteArrayLiteral( "PREVIOUS" ) ); // 失败不碰 *out（调用约定）
}

// ---------- C. DTD 策略 ----------

void TestP1ImportIntegrity::dtdRejectedAcrossStringAndStreamingEntries()
{
  const QByteArray withDtd = spreadsheetMl(
      "   <Row><Cell><Data ss:Type=\"String\">W1</Data></Cell>"
      "<Cell><Data ss:Type=\"String\">T70</Data></Cell>"
      "<Cell><Data ss:Type=\"Number\">1000</Data></Cell></Row>\n", true );

  // 字符串入口（原有防线保持）
  const WorkbookReadResult viaString = readSpreadsheetMlWorkbook( QStringLiteral( "dtd.xml" ), withDtd );
  QVERIFY( !viaString.ok );
  QVERIFY2( viaString.error.contains( QStringLiteral( "DTD" ) ), qPrintable( viaString.error ) );

  // 流式入口（QIODevice）：此前 readNextStartElement 会把前导 DTD 静默消耗
  QBuffer buffer;
  buffer.setData( withDtd );
  buffer.open( QIODevice::ReadOnly );
  const WorkbookReadResult viaStream = streamSpreadsheetMlWorkbook(
      &buffer, QStringLiteral( "dtd.xml" ),
      []( const QString &, const QStringList &, const QStringList &, int, bool ) { return true; } );
  QVERIFY( !viaStream.ok );
  QVERIFY2( viaStream.error.contains( QStringLiteral( "DTD" ) ), qPrintable( viaStream.error ) );
}

void TestP1ImportIntegrity::dtdRejectedByDeviationAndTimeDepthEntry()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString path = dir.filePath( QStringLiteral( "dev.xml" ) );
  QVERIFY( writeFile( path, deviationXml( true, false ) ) );
  QVector<WellComposite::XmlDeviationStation> stations;
  QString err;
  QVERIFY( !WellComposite::parseDeviationSurvey( path, stations, &err ) );
  QVERIFY2( err.contains( QStringLiteral( "DTD" ) ), qPrintable( err ) );

  QVector<WellComposite::XmlTimeDepthPair> pairs;
  QVERIFY( !WellComposite::parseTimeDepthTable( path, pairs, &err ) );
  QVERIFY( err.contains( QStringLiteral( "DTD" ) ) );

  // 合法井斜文档仍可解析（同入口不误伤）
  const QString okPath = dir.filePath( QStringLiteral( "dev_ok.xml" ) );
  QVERIFY( writeFile( okPath, deviationXml( false, false ) ) );
  QVERIFY( WellComposite::parseDeviationSurvey( okPath, stations, &err ) );
  QCOMPARE( stations.size(), 2 );
  QCOMPARE( stations.at( 1 ).md, 100.0 );
}

void TestP1ImportIntegrity::dtdRejectedInOoxmlParts()
{
  XlsxParts parts;
  parts.workbook = defaultWorkbookXml( false );
  parts.rels = defaultRelsXml( false );
  parts.sheet1 = defaultSheetXml( false );
  // sharedStrings 前导 DTD——部件级拒绝（不是「空共享字符串表」）
  parts.shared = "<?xml version=\"1.0\"?>\n<!DOCTYPE sst [<!ENTITY x \"y\">]>\n"
                 "<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"/>";
  const QByteArray archive = buildXlsx( parts );
  QVERIFY( !archive.isEmpty() );
  QTemporaryDir dir;
  const QString path = dir.filePath( QStringLiteral( "dtd.xlsx" ) );
  QVERIFY( writeFile( path, archive ) );
  const WorkbookReadResult result = readWorkbook( path );
  QVERIFY( !result.ok );
  QVERIFY2( result.error.contains( QStringLiteral( "DTD" ) ) ||
                result.error.contains( QStringLiteral( "sharedStrings" ) ),
            qPrintable( result.error ) );
}

void TestP1ImportIntegrity::leadingDeclarationBomCommentStillAccepted()
{
  // XML 声明 + BOM + 注释 + 命名空间 + 普通转义：全部照常接受
  QByteArray xml = QByteArray::fromHex( "EFBBBF" );
  xml += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!-- leading comment -->\n";
  xml += "<Workbook xmlns=\"" + QByteArray( kSsXmlns ) + "\" xmlns:ss=\"" + QByteArray( kSsXmlns ) + "\">\n";
  xml += " <Worksheet ss:Name=\"S&amp;1\">\n  <Table>\n";
  xml += "   <Row><Cell><Data ss:Type=\"String\">A&amp;B</Data></Cell></Row>\n";
  xml += "  </Table>\n </Worksheet>\n</Workbook>\n";
  const WorkbookReadResult result = readSpreadsheetMlWorkbook( QStringLiteral( "ok.xml" ), xml );
  QVERIFY2( result.ok, qPrintable( result.error ) );
  QCOMPARE( result.sheets.size(), 1 );
  QCOMPARE( result.sheets.first().headers, QStringList{ QStringLiteral( "A&B" ) } );
}

void TestP1ImportIntegrity::truncatedDeviationXmlFailsInsteadOfPartialRows()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString path = dir.filePath( QStringLiteral( "trunc.xml" ) );
  QVERIFY( writeFile( path, deviationXml( false, true ) ) );
  QVector<WellComposite::XmlDeviationStation> stations;
  QString err;
  QVERIFY( !WellComposite::parseDeviationSurvey( path, stations, &err ) );
  QVERIFY2( err.contains( QStringLiteral( "解析错误" ) ), qPrintable( err ) );
  QVERIFY( stations.isEmpty() ); // 不交回部分行
}

// ---------- D. OOXML 部件失败可见 ----------

void TestP1ImportIntegrity::malformedSharedStringsFailsWholeWorkbook()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  XlsxParts parts;
  parts.workbook = defaultWorkbookXml( false );
  parts.rels = defaultRelsXml( false );
  parts.sheet1 = defaultSheetXml( false );
  parts.shared = "<?xml version=\"1.0\"?>\n<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\""
                 "><si><t>W1</t></si><si><t>残"; // 中途截断
  const QString path = dir.filePath( QStringLiteral( "bad_shared.xlsx" ) );
  QVERIFY( writeFile( path, buildXlsx( parts ) ) );
  const WorkbookReadResult result = readWorkbook( path );
  QVERIFY( !result.ok );
  QVERIFY2( result.error.contains( QStringLiteral( "sharedStrings" ) ), qPrintable( result.error ) );
  QVERIFY( result.error.contains( QStringLiteral( "解析错误" ) ) );
}

void TestP1ImportIntegrity::malformedRelsFailsWholeWorkbook()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  XlsxParts parts;
  parts.workbook = defaultWorkbookXml( false );
  parts.rels = "<?xml version=\"1.0\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\""
               "><Relationship Id=\"rId1\""; // 截断
  parts.sheet1 = defaultSheetXml( false );
  const QString path = dir.filePath( QStringLiteral( "bad_rels.xlsx" ) );
  QVERIFY( writeFile( path, buildXlsx( parts ) ) );
  const WorkbookReadResult result = readWorkbook( path );
  QVERIFY( !result.ok );
  QVERIFY2( result.error.contains( QStringLiteral( "workbook.xml.rels" ) ), qPrintable( result.error ) );
}

void TestP1ImportIntegrity::malformedStylesFailsWholeWorkbook()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  XlsxParts parts;
  parts.workbook = defaultWorkbookXml( false );
  parts.rels = defaultRelsXml( false );
  parts.sheet1 = defaultSheetXml( false );
  parts.styles = "<?xml version=\"1.0\"?>\n<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\""
                 "><cellXfs><xf numFmtId="; // 截断
  const QString path = dir.filePath( QStringLiteral( "bad_styles.xlsx" ) );
  QVERIFY( writeFile( path, buildXlsx( parts ) ) );
  const WorkbookReadResult result = readWorkbook( path );
  QVERIFY( !result.ok );
  QVERIFY2( result.error.contains( QStringLiteral( "styles" ) ), qPrintable( result.error ) );
}

void TestP1ImportIntegrity::externalRelationshipNotTreatedAsWorksheet()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  XlsxParts parts;
  parts.workbook = defaultWorkbookXml( true ); // rId1(外部) + rId2(内部 sheet2)
  parts.rels = defaultRelsXml( true );
  parts.sheet1 = defaultSheetXml( false );
  parts.sheet2 = defaultSheetXml( false );
  const QString path = dir.filePath( QStringLiteral( "ext_rel.xlsx" ) );
  QVERIFY( writeFile( path, buildXlsx( parts ) ) );
  const WorkbookReadResult result = readWorkbook( path );
  QVERIFY2( result.ok, qPrintable( result.error ) );
  QCOMPARE( result.sheets.size(), 1 ); // 只有内部表被读
  QCOMPARE( result.sheets.first().name, QStringLiteral( "说明" ) );
  bool reported = false;
  for ( const QString &issue : result.issues )
    if ( issue.contains( QStringLiteral( "rId1" ) ) || issue.contains( QStringLiteral( "没有对应目标" ) ) )
      reported = true;
  QVERIFY2( reported, "外部关系导致的工作表缺位必须如实报因" );
}

void TestP1ImportIntegrity::malformedSheetKeepsHealthySibling()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  XlsxParts parts;
  parts.workbook = defaultWorkbookXml( true );
  parts.rels = "<?xml version=\"1.0\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
               "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
               "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet2.xml\"/>"
               "</Relationships>";
  parts.sheet1 = defaultSheetXml( true ); // 坏表
  parts.sheet2 = defaultSheetXml( false );
  const QString path = dir.filePath( QStringLiteral( "sibling.xlsx" ) );
  QVERIFY( writeFile( path, buildXlsx( parts ) ) );
  const WorkbookReadResult result = readWorkbook( path );
  QVERIFY2( result.ok, qPrintable( result.error ) );
  QCOMPARE( result.sheets.size(), 1 );
  QCOMPARE( result.sheets.first().name, QStringLiteral( "说明" ) );
  bool recorded = false;
  for ( const QString &issue : result.issues )
    if ( issue.contains( QStringLiteral( "数据" ) ) && issue.contains( QStringLiteral( "解析错误" ) ) )
      recorded = true;
  QVERIFY2( recorded, "坏表的失败来源必须记录（表名 + 行列）" );
}

void TestP1ImportIntegrity::missingSharedStringsKeepsCellLevelIssue()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  XlsxParts parts;
  parts.workbook = defaultWorkbookXml( false );
  parts.rels = defaultRelsXml( false );
  parts.sheet1 = "<?xml version=\"1.0\"?>\n"
      "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData>"
      "<row r=\"1\"><c r=\"A1\" t=\"inlineStr\"><is><t>表头</t></is></c></row>"
      "<row r=\"2\"><c r=\"A2\" t=\"s\"><v>0</v></c></row>"
      "</sheetData></worksheet>";
  // 不写 sharedStrings（合法缺失）：t="s" 格按格报因、置空，不升级为整簿失败
  const QString path = dir.filePath( QStringLiteral( "no_shared.xlsx" ) );
  QVERIFY( writeFile( path, buildXlsx( parts ) ) );
  const WorkbookReadResult result = readWorkbook( path );
  QVERIFY2( result.ok, qPrintable( result.error ) );
  bool hasCellIssue = false;
  for ( const QString &issue : result.issues )
    if ( issue.contains( QStringLiteral( "sharedStrings" ) ) )
      hasCellIssue = true;
  QVERIFY( hasCellIssue );
}

// ---------- E. 预算与取消 ----------

void TestP1ImportIntegrity::previewLimitsDistinguishCorruptFromLimited()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  // 损坏（非 ZIP）：错误如实可辨，不是「受限」
  const QString corrupt = dir.filePath( QStringLiteral( "corrupt.xlsx" ) );
  QVERIFY( writeFile( corrupt, QByteArrayLiteral( "not a zip at all........" ) ) );
  bool limited = false;
  WorkbookReadResult result = previewXlsxWorkbook( corrupt, &limited );
  QVERIFY( !result.ok );
  QVERIFY( !limited );
  QVERIFY( !result.error.isEmpty() );

  // 合法小簿：成功、未受限
  const QString ok = dir.filePath( QStringLiteral( "ok.xlsx" ) );
  QVERIFY( writeFile( ok, validWellStratXlsx() ) );
  limited = true;
  result = previewXlsxWorkbook( ok, &limited );
  QVERIFY2( result.ok, qPrintable( result.error ) );
  QVERIFY( !limited );
  QCOMPARE( result.sheets.first().rows.size(), 2 );

  // 大于 2 MiB 的文件：受限（不足以下「损坏/成功」结论）
  const QString big = dir.filePath( QStringLiteral( "big.xlsx" ) );
  QVERIFY( writeFile( big, QByteArray( 3 * 1024 * 1024, 'z' ) ) );
  limited = false;
  result = previewXlsxWorkbook( big, &limited );
  QVERIFY( !result.ok );
  QVERIFY( limited );
  QVERIFY( result.error.isEmpty() ); // limited ≠ 损坏
}

void TestP1ImportIntegrity::previewExpandedBudgetLimit()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  // 一个 deflate 条目声明 ~4 MiB 零（压缩后极小、文件远小于 2 MiB）：
  // 预览的「展开总量 > 4 MiB」预算必须独立命中
  ZipWriteEntry big;
  big.name = QStringLiteral( "xl/worksheets/sheet1.xml" );
  big.data = QByteArray( 4 * 1024 * 1024 + 1, '\0' );
  ZipBuildOptions options;
  options.compression = ZipCompression::Deflate;
  QString err;
  const QByteArray archive = zipBuildArchive( { big }, options, &err );
  QVERIFY2( !archive.isEmpty(), qPrintable( err ) );
  const QString path = dir.filePath( QStringLiteral( "expanded.xlsx" ) );
  QVERIFY( writeFile( path, archive ) );
  QVERIFY( QFileInfo( path ).size() < 2 * 1024 * 1024 ); // 文件面不触发
  bool limited = false;
  const WorkbookReadResult result = previewXlsxWorkbook( path, &limited );
  QVERIFY( !result.ok );
  QVERIFY( limited );
}

void TestP1ImportIntegrity::cancelStopsWorkbookReadAtRowGranularity()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  // 井分层表，行数足够多；取消谓词在第 5 次检查后置位
  QByteArray rows;
  for ( int i = 0; i < 200; ++i )
    rows += QString( "   <Row><Cell><Data ss:Type=\"String\">W%1</Data></Cell>"
                     "<Cell><Data ss:Type=\"String\">T%2</Data></Cell>"
                     "<Cell><Data ss:Type=\"Number\">%3</Data></Cell></Row>\n" )
                .arg( i )
                .arg( i )
                .arg( 1000 + i )
                .toUtf8();
  const QByteArray xml = spreadsheetMl( rows, false );
  const QString path = dir.filePath( QStringLiteral( "many.xml" ) );
  QVERIFY( writeFile( path, xml ) );

  int calls = 0;
  const ReadCancelCheck cancel = [&calls]() { return ++calls > 5; };
  const WorkbookReadResult cancelled = readWorkbook( path, cancel );
  QVERIFY( !cancelled.ok );
  QVERIFY( cancelled.cancelled );
  QVERIFY2( cancelled.error.startsWith( QStringLiteral( "已取消" ) ), qPrintable( cancelled.error ) );
  QVERIFY( calls < 200 ); // 行粒度叫停，没有读完整表

  // 同一文件不取消时正常读完（取消不留后遗症）
  const WorkbookReadResult full = readWorkbook( path );
  QVERIFY2( full.ok, qPrintable( full.error ) );
  QVERIFY( full.sheets.first().rows.size() > 100 );
}

// ---------- F. 宿主导入回执 ----------

void TestP1ImportIntegrity::corruptXlsxFailsImportWithRealError()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString project = dir.filePath( QStringLiteral( "proj" ) );
  PaleoProjectStore store;
  DataImportService importer( &store );
  importer.setProjectDir( project );
  QVERIFY2( importer.catalogWritable(), qPrintable( importer.catalogOpenError() ) );

  // ZIP 结构完好但 sharedStrings 部件 XML 截断——「格式已识别（.xlsx 是
  // OOXML）但内容损坏」，必须带真实原因拒收；与「ZIP 截断/缺条目」（不是
  // OOXML 工作簿 → 基线按外委簿收，见 validNonWellWorkbookStillImportsAs
  // Outsource 与 tst_datapreview office.xlsx 夹具）严格区分。
  XlsxParts parts;
  parts.workbook = defaultWorkbookXml( false );
  parts.rels = defaultRelsXml( false );
  parts.sheet1 = defaultSheetXml( false );
  parts.shared = "<?xml version=\"1.0\"?>\n<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\""
                 "><si><t>W1</t></si><si><t>残"; // 中途截断的 XML 部件
  const QString path = dir.filePath( QStringLiteral( "broken.xlsx" ) );
  QVERIFY( writeFile( path, buildXlsx( parts ) ) );

  QString err;
  const DataImportService::ImportResult result = importer.importProjectFileEx( path, {}, &err );
  QCOMPARE( result.outcome, DataImportService::ImportOutcome::Failed );
  QVERIFY( result.assetId.isEmpty() ); // 致命解析失败不得返回成功资产 ID
  QVERIFY2( !err.isEmpty(), "拒收理由必须透传" );
  QVERIFY2( err.contains( QStringLiteral( "sharedStrings" ) ), qPrintable( err ) );
  QVERIFY( importer.assets( QStringLiteral( "outsource_workbook" ) ).isEmpty() );

  // 分类详情同一结论：坏容器 ≠ 未知格式
  const ImportClassificationDetail detail = classifyImportFileDetailed( path );
  QVERIFY( !detail.cancelled );
  QVERIFY2( !detail.containerError.isEmpty(), "坏容器必须携带拒收理由" );

  // 截断 ZIP（结构缺条目）保持基线语义：不是 OOXML 工作簿 → 按外委簿收，
  // 不升级为拒收（否则 docx 字节冒充 .xlsx 的合法参考导入全被误杀）。
  const QByteArray good = validWellStratXlsx();
  const QString truncated = dir.filePath( QStringLiteral( "truncated.xlsx" ) );
  QVERIFY( writeFile( truncated, good.left( good.size() / 3 ) ) );
  const ImportClassificationDetail truncatedDetail = classifyImportFileDetailed( truncated );
  QVERIFY2( truncatedDetail.containerError.isEmpty(),
            qPrintable( truncatedDetail.containerError ) );
}

void TestP1ImportIntegrity::validNonWellWorkbookStillImportsAsOutsource()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString project = dir.filePath( QStringLiteral( "proj" ) );
  PaleoProjectStore store;
  DataImportService importer( &store );
  importer.setProjectDir( project );
  QVERIFY2( importer.catalogWritable(), qPrintable( importer.catalogOpenError() ) );

  // 合法但不是井表的工作簿（无井名列）——照常按外委簿导入（回归不误伤）
  XlsxParts parts;
  parts.workbook = defaultWorkbookXml( false );
  parts.rels = defaultRelsXml( false );
  parts.sheet1 = "<?xml version=\"1.0\"?>\n"
      "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData>"
      "<row r=\"1\"><c r=\"A1\" t=\"inlineStr\"><is><t>备注</t></is></c></row>"
      "<row r=\"2\"><c r=\"A2\" t=\"inlineStr\"><is><t>外委说明</t></is></c></row>"
      "</sheetData></worksheet>";
  const QString path = dir.filePath( QStringLiteral( "notes.xlsx" ) );
  QVERIFY( writeFile( path, buildXlsx( parts ) ) );
  const ImportClassificationDetail detail = classifyImportFileDetailed( path );
  QVERIFY2( detail.containerError.isEmpty(), qPrintable( detail.containerError ) );

  QString err;
  const DataImportService::ImportResult result = importer.importProjectFileEx( path, {}, &err );
  QVERIFY2( result.outcome == DataImportService::ImportOutcome::Imported, qPrintable( err ) );
  QVERIFY( !result.assetId.isEmpty() );
  QCOMPARE( importer.assets( QStringLiteral( "outsource_workbook" ) ).size(), 1 );
}

void TestP1ImportIntegrity::cancelledImportFailsFastAndRetrySucceeds()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString project = dir.filePath( QStringLiteral( "proj" ) );
  PaleoProjectStore store;
  DataImportService importer( &store );
  importer.setProjectDir( project );
  QVERIFY2( importer.catalogWritable(), qPrintable( importer.catalogOpenError() ) );
  const QString path = dir.filePath( QStringLiteral( "strat.xlsx" ) );
  QVERIFY( writeFile( path, validWellStratXlsx() ) );

  DataImportService::ImportOptions options;
  options.cancelled = []() { return true; };
  QString err;
  const DataImportService::ImportResult cancelled = importer.importProjectFileEx( path, options, &err );
  QCOMPARE( cancelled.outcome, DataImportService::ImportOutcome::Failed );
  QVERIFY2( err.startsWith( QStringLiteral( "已取消" ) ), qPrintable( err ) );
  QVERIFY( importer.assets( QStringLiteral( "well_stratification" ) ).isEmpty() ); // 取消不新增提交

  // 同一合法输入取消后重试：应成功（取消不被记成解析失败）
  const DataImportService::ImportResult retry = importer.importProjectFileEx( path, {}, &err );
  QVERIFY2( retry.outcome == DataImportService::ImportOutcome::Imported, qPrintable( err ) );
  QVERIFY( !retry.assetId.isEmpty() );
}

void TestP1ImportIntegrity::wellStratImportRecordsParseReport()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString project = dir.filePath( QStringLiteral( "proj" ) );
  PaleoProjectStore store;
  DataImportService importer( &store );
  importer.setProjectDir( project );
  QVERIFY2( importer.catalogWritable(), qPrintable( importer.catalogOpenError() ) );
  const QString path = dir.filePath( QStringLiteral( "strat2.xlsx" ) );
  QVERIFY( writeFile( path, validWellStratXlsx() ) );

  QString err;
  const DataImportService::ImportResult result = importer.importProjectFileEx( path, {}, &err );
  QVERIFY2( result.outcome == DataImportService::ImportOutcome::Imported, qPrintable( err ) );
  // 回执保留接受行计数；版本 extra 落 wellParseReport（含行号 issue 通道）
  const auto assets = importer.assets( QStringLiteral( "well_stratification" ) );
  QCOMPARE( assets.size(), 1 );
  const auto versions = importer.catalog()->versionsForAsset( assets.first() );
  QVERIFY( !versions.isEmpty() );
  const auto report = versions.first().extra.value( QStringLiteral( "wellParseReport" ) ).toMap();
  QCOMPARE( report.value( QStringLiteral( "acceptedRows" ) ).toInt(), 2 );
}

void TestP1ImportIntegrity::changedSourceAfterPreviewRejectedByHash()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString project = dir.filePath( QStringLiteral( "proj" ) );
  PaleoProjectStore store;
  DataImportService importer( &store );
  importer.setProjectDir( project );
  QVERIFY2( importer.catalogWritable(), qPrintable( importer.catalogOpenError() ) );
  const QString path = dir.filePath( QStringLiteral( "previewed.xlsx" ) );
  QVERIFY( writeFile( path, validWellStratXlsx() ) );

  // 预览确认后源文件被替换（同路径不同字节）——过期预览不得导入
  DataImportService::ImportOptions options;
  options.expectedSourceSha256 = QStringLiteral( "0000000000000000000000000000000000000000000000000000000000000000" );
  QString err;
  const DataImportService::ImportResult result = importer.importProjectFileEx( path, options, &err );
  QCOMPARE( result.outcome, DataImportService::ImportOutcome::Failed );
  QVERIFY2( err.contains( QStringLiteral( "内容已不同于预览" ) ), qPrintable( err ) );
  QVERIFY( importer.assets( QStringLiteral( "well_stratification" ) ).isEmpty() );
}

void TestP1ImportIntegrity::partialAcceptanceKeepsRowNumbersAndCounts()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString project = dir.filePath( QStringLiteral( "proj" ) );
  PaleoProjectStore store;
  DataImportService importer( &store );
  importer.setProjectDir( project );
  QVERIFY2( importer.catalogWritable(), qPrintable( importer.catalogOpenError() ) );

  // 两行合法 + 一行深度非数值：坏行按行号拒收，合法兄弟行照常接受
  XlsxParts parts;
  parts.workbook = defaultWorkbookXml( false );
  parts.rels = defaultRelsXml( false );
  parts.sheet1 = "<?xml version=\"1.0\"?>\n"
      "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData>"
      "<row r=\"1\"><c r=\"A1\" t=\"inlineStr\"><is><t>井号</t></is></c>"
      "<c r=\"B1\" t=\"inlineStr\"><is><t>层位</t></is></c>"
      "<c r=\"C1\" t=\"inlineStr\"><is><t>顶深</t></is></c></row>"
      "<row r=\"2\"><c r=\"A2\" t=\"inlineStr\"><is><t>W1</t></is></c>"
      "<c r=\"B2\" t=\"inlineStr\"><is><t>T70</t></is></c>"
      "<c r=\"C2\"><v>1000</v></c></row>"
      "<row r=\"3\"><c r=\"A3\" t=\"inlineStr\"><is><t>W1</t></is></c>"
      "<c r=\"B3\" t=\"inlineStr\"><is><t>T71</t></is></c>"
      "<c r=\"C3\" t=\"inlineStr\"><is><t>缺测</t></is></c></row>"
      "<row r=\"4\"><c r=\"A4\" t=\"inlineStr\"><is><t>W2</t></is></c>"
      "<c r=\"B4\" t=\"inlineStr\"><is><t>T70</t></is></c>"
      "<c r=\"C4\"><v>2000</v></c></row>"
      "</sheetData></worksheet>";
  const QString path = dir.filePath( QStringLiteral( "partial.xlsx" ) );
  QVERIFY( writeFile( path, buildXlsx( parts ) ) );

  QString err;
  const DataImportService::ImportResult result = importer.importProjectFileEx( path, {}, &err );
  QVERIFY2( result.outcome == DataImportService::ImportOutcome::Imported, qPrintable( err ) );
  const auto assets = importer.assets( QStringLiteral( "well_stratification" ) );
  QCOMPARE( assets.size(), 1 );
  const auto versions = importer.catalog()->versionsForAsset( assets.first() );
  const auto report = versions.first().extra.value( QStringLiteral( "wellParseReport" ) ).toMap();
  QCOMPARE( report.value( QStringLiteral( "acceptedRows" ) ).toInt(), 2 );
  QCOMPARE( report.value( QStringLiteral( "rejectedRows" ) ).toInt(), 1 );
  const QStringList issues = report.value( QStringLiteral( "issues" ) ).toStringList();
  QVERIFY2( !issues.isEmpty() && issues.first().contains( QStringLiteral( "第 3 行" ) ),
            qPrintable( issues.join( QLatin1Char( '\n' ) ) ) );
}

QTEST_GUILESS_MAIN( TestP1ImportIntegrity )
#include "tst_p1_import_integrity.moc"
