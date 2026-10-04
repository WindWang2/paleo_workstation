// 层：数据
#include "ziparchive.h"

#include <QFile>
#include <QFileInfo>

#if defined( PALEO_HAVE_ZLIB )
#include <zlib.h>
#else
#error "ziparchive 需要 zlib：deflate 条目（.sfpkg/.xlsx 默认压缩）无法在无 zlib 的构建里读取"
#endif

#include <cstring>

// 层：数据
namespace paleo::io
{
namespace
{

constexpr std::uint32_t kEndOfCentralDirectory = 0x06054b50u;
constexpr std::uint32_t kCentralHeader = 0x02014b50u;
constexpr std::uint32_t kLocalHeader = 0x04034b50u;
constexpr std::uint32_t kZip64Sentinel = 0xFFFFFFFFu;
constexpr qint64 kMaxArchiveBytes = 512ll * 1024 * 1024;

std::uint16_t readU16( const QByteArray &buffer, int offset )
{
  return static_cast<std::uint16_t>( static_cast<unsigned char>( buffer.at( offset ) ) ) |
         static_cast<std::uint16_t>( static_cast<unsigned char>( buffer.at( offset + 1 ) ) << 8 );
}

std::uint32_t readU32( const QByteArray &buffer, int offset )
{
  return static_cast<std::uint32_t>( static_cast<unsigned char>( buffer.at( offset ) ) ) |
         ( static_cast<std::uint32_t>( static_cast<unsigned char>( buffer.at( offset + 1 ) ) ) << 8 ) |
         ( static_cast<std::uint32_t>( static_cast<unsigned char>( buffer.at( offset + 2 ) ) ) << 16 ) |
         ( static_cast<std::uint32_t>( static_cast<unsigned char>( buffer.at( offset + 3 ) ) ) << 24 );
}

// 中央目录结束记录可能被尾部注释推前：从尾部往前找签名。
int findEndOfCentralDirectory( const QByteArray &archive )
{
  for ( int i = archive.size() - 22; i >= 0; --i )
  {
    if ( readU32( archive, i ) == kEndOfCentralDirectory )
      return i;
  }
  return -1;
}

} // namespace

QByteArray zipReadArchive( const QString &path, QString *error )
{
  const auto fail = [error]( const QString &message ) {
    if ( error )
      *error = message;
    return QByteArray();
  };
  const QFileInfo info( path );
  if ( !info.exists() || !info.isFile() )
    return fail( QStringLiteral( "文件不存在：%1" ).arg( path ) );
  if ( info.size() > kMaxArchiveBytes )
  {
    return fail( QStringLiteral( "包 %1 大小 %2 字节超过上限 %3 字节，暂不读取" )
                     .arg( info.fileName() )
                     .arg( info.size() )
                     .arg( kMaxArchiveBytes ) );
  }
  QFile file( path );
  if ( !file.open( QIODevice::ReadOnly ) )
    return fail( QStringLiteral( "无法打开文件：%1" ).arg( path ) );
  const QByteArray archive = file.readAll();
  if ( static_cast<qint64>( archive.size() ) != info.size() )
    return fail( QStringLiteral( "包 %1 读取不完整" ).arg( info.fileName() ) );
  return archive;
}

const ZipEntry *ZipListResult::entry( const QString &name ) const
{
  for ( const ZipEntry &item : entries )
  {
    if ( item.name == name )
      return &item;
  }
  return nullptr;
}

ZipListResult zipListBytes( const QByteArray &archive )
{
  ZipListResult result;
  const int eocd = findEndOfCentralDirectory( archive );
  if ( eocd < 0 )
  {
    result.error = QStringLiteral( "不是 ZIP 容器（没有中央目录结束记录）" );
    return result;
  }
  const std::uint16_t count = readU16( archive, eocd + 10 );
  const std::uint32_t directorySize = readU32( archive, eocd + 12 );
  const std::uint32_t directoryOffset = readU32( archive, eocd + 16 );
  if ( directoryOffset == kZip64Sentinel || directorySize == kZip64Sentinel || count == 0xFFFFu )
  {
    result.error = QStringLiteral( "ZIP64 容器暂不支持（字段是 64 位哨兵值）" );
    return result;
  }
  if ( static_cast<qint64>( directoryOffset ) + directorySize > archive.size() )
  {
    result.error = QStringLiteral( "ZIP 中央目录偏移越界" );
    return result;
  }
  int offset = static_cast<int>( directoryOffset );
  for ( int i = 0; i < count; ++i )
  {
    if ( offset + 46 > archive.size() || readU32( archive, offset ) != kCentralHeader )
    {
      result.error = QStringLiteral( "ZIP 中央目录第 %1 项签名不符" ).arg( i + 1 );
      result.entries.clear();
      return result;
    }
    ZipEntry item;
    const std::uint16_t flags = readU16( archive, offset + 8 );
    item.method = readU16( archive, offset + 10 );
    const std::uint32_t compressed = readU32( archive, offset + 20 );
    const std::uint32_t uncompressed = readU32( archive, offset + 24 );
    const std::uint16_t nameLength = readU16( archive, offset + 28 );
    const std::uint16_t extraLength = readU16( archive, offset + 30 );
    const std::uint16_t commentLength = readU16( archive, offset + 32 );
    item.localHeaderOffset = readU32( archive, offset + 42 );
    item.encrypted = ( flags & 0x1u ) != 0;
    if ( offset + 46 + nameLength > archive.size() )
    {
      result.error = QStringLiteral( "ZIP 中央目录条目名被截断" );
      result.entries.clear();
      return result;
    }
    item.name = QString::fromUtf8( archive.mid( offset + 46, nameLength ) );
    item.compressedSize = compressed;
    item.uncompressedSize = uncompressed;
    if ( compressed == kZip64Sentinel || uncompressed == kZip64Sentinel ||
         item.localHeaderOffset == kZip64Sentinel )
    {
      result.error = QStringLiteral( "ZIP64 条目「%1」暂不支持" ).arg( item.name );
      result.entries.clear();
      return result;
    }
    result.entries.append( item );
    offset += 46 + nameLength + extraLength + commentLength;
  }
  result.ok = true;
  return result;
}

bool zipExtractBytes( const QByteArray &archive, const QString &entryName, QByteArray *out,
                      QString *error )
{
  const auto fail = [error]( const QString &message ) {
    if ( error )
      *error = message;
    return false;
  };
  const ZipListResult listing = zipListBytes( archive );
  if ( !listing.ok )
    return fail( listing.error );
  const ZipEntry *item = listing.entry( entryName );
  if ( !item )
    return fail( QStringLiteral( "ZIP 内没有条目：%1" ).arg( entryName ) );
  if ( item->encrypted )
    return fail( QStringLiteral( "条目「%1」已加密，不支持" ).arg( entryName ) );
  if ( item->method != 0 && item->method != 8 )
  {
    return fail( QStringLiteral( "条目「%1」压缩方法 %2 不支持（只支持 stored/deflate）" )
                     .arg( entryName )
                     .arg( item->method ) );
  }
  const qint64 headerOffset = item->localHeaderOffset;
  if ( headerOffset + 30 > archive.size() || readU32( archive, static_cast<int>( headerOffset ) ) != kLocalHeader )
    return fail( QStringLiteral( "条目「%1」本地头签名不符" ).arg( entryName ) );
  const std::uint16_t nameLength = readU16( archive, static_cast<int>( headerOffset ) + 26 );
  const std::uint16_t extraLength = readU16( archive, static_cast<int>( headerOffset ) + 28 );
  const qint64 dataOffset = headerOffset + 30 + nameLength + extraLength;
  if ( dataOffset + static_cast<qint64>( item->compressedSize ) > archive.size() )
    return fail( QStringLiteral( "条目「%1」数据段被截断" ).arg( entryName ) );
  const QByteArray raw = archive.mid( static_cast<int>( dataOffset ),
                                      static_cast<int>( item->compressedSize ) );
  if ( !out )
    return true;
  if ( item->method == 0 )
  {
    *out = raw;
    return true;
  }

  // deflate：raw inflate（-MAX_WBITS），按中央目录给出的未压缩长度预分配。
  if ( item->uncompressedSize > static_cast<std::uint64_t>( kMaxArchiveBytes ) )
    return fail( QStringLiteral( "条目「%1」解压后大小超过上限" ).arg( entryName ) );
  z_stream stream;
  std::memset( &stream, 0, sizeof( stream ) );
  if ( inflateInit2( &stream, -MAX_WBITS ) != Z_OK )
    return fail( QStringLiteral( "条目「%1」zlib 初始化失败" ).arg( entryName ) );
  QByteArray inflated;
  inflated.resize( static_cast<int>( item->uncompressedSize ) );
  stream.next_in = reinterpret_cast<Bytef *>( const_cast<char *>( raw.constData() ) );
  stream.avail_in = static_cast<uInt>( raw.size() );
  stream.next_out = reinterpret_cast<Bytef *>( inflated.data() );
  stream.avail_out = static_cast<uInt>( inflated.size() );
  const int status = inflate( &stream, Z_FINISH );
  const uInt produced = stream.total_out;
  inflateEnd( &stream );
  if ( status != Z_STREAM_END || produced != item->uncompressedSize )
  {
    return fail( QStringLiteral( "条目「%1」解压失败（zlib 状态 %2，得到 %3 字节，期望 %4）" )
                     .arg( entryName )
                     .arg( status )
                     .arg( produced )
                     .arg( item->uncompressedSize ) );
  }
  inflated.resize( static_cast<int>( produced ) );
  *out = inflated;
  return true;
}

} // namespace paleo::io
