// 层：数据
#include "sfpkgreader.h"
#include "sfpkg_internal.h"

#include "ziparchive.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

#include <algorithm>
#include <cmath>
#include <cstring>

// 层：数据
namespace paleo::io
{
namespace
{

using paleo::io_detail::sha256Hex;
using paleo::io_detail::kManifestEntry;
using paleo::io_detail::kNpzEntry;
using paleo::io_detail::kChecksumEntry;


// ZIP 容器判据由 ziparchive 的中央目录解析给出：非 ZIP（没有中央目录结束记录）
// 在这里如实失败，不依赖 QMimeDatabase 的扩展名/mime 判定。

// NPY header 是 Python 字面量字典。只取三个键，逐字符扫描（不引入 Python 解析器）。
QString headerString( const QString &header, const QString &key, bool *found )
{
  const int at = header.indexOf( QStringLiteral( "'%1'" ).arg( key ) );
  if ( at < 0 )
  {
    *found = false;
    return QString();
  }
  const int colon = header.indexOf( QLatin1Char( ':' ), at );
  if ( colon < 0 )
  {
    *found = false;
    return QString();
  }
  const int quote = header.indexOf( QLatin1Char( '\'' ), colon );
  if ( quote < 0 )
  {
    *found = false;
    return QString();
  }
  const int end = header.indexOf( QLatin1Char( '\'' ), quote + 1 );
  if ( end < 0 )
  {
    *found = false;
    return QString();
  }
  *found = true;
  return header.mid( quote + 1, end - quote - 1 );
}

QString headerScalar( const QString &header, const QString &key, bool *found )
{
  const int at = header.indexOf( QStringLiteral( "'%1'" ).arg( key ) );
  if ( at < 0 )
  {
    *found = false;
    return QString();
  }
  const int colon = header.indexOf( QLatin1Char( ':' ), at );
  if ( colon < 0 )
  {
    *found = false;
    return QString();
  }
  int end = header.indexOf( QLatin1Char( ',' ), colon );
  if ( end < 0 )
    end = header.indexOf( QLatin1Char( '}' ), colon );
  if ( end < 0 )
    end = header.size();
  *found = true;
  return header.mid( colon + 1, end - colon - 1 ).trimmed();
}

QStringList headerShape( const QString &header, bool *found )
{
  const int at = header.indexOf( QStringLiteral( "'shape'" ) );
  if ( at < 0 )
  {
    *found = false;
    return {};
  }
  const int open = header.indexOf( QLatin1Char( '(' ), at );
  const int close = header.indexOf( QLatin1Char( ')' ), open < 0 ? at : open );
  if ( open < 0 || close < 0 )
  {
    *found = false;
    return {};
  }
  *found = true;
  const QString inner = header.mid( open + 1, close - open - 1 ).trimmed();
  if ( inner.isEmpty() )
    return {};
  return inner.split( QLatin1Char( ',' ), Qt::SkipEmptyParts );
}

int dtypeBytes( const QString &descr, bool *integral, QString *error )
{
  QString kind = descr;
  kind.remove( QLatin1Char( '<' ) );
  kind.remove( QLatin1Char( '>' ) );
  kind.remove( QLatin1Char( '|' ) );
  kind.remove( QLatin1Char( '=' ) );
  if ( kind.isEmpty() )
  {
    *error = QStringLiteral( "空 dtype" );
    return 0;
  }
  const QChar code = kind.at( 0 );
  const int width = kind.size() > 1 ? kind.mid( 1 ).toInt() : 0;
  *integral = false;
  switch ( code.unicode() )
  {
    case 'f':
      if ( width == 4 || width == 8 )
        return width;
      break;
    case 'i':
    case 'u':
      if ( width == 1 || width == 2 || width == 4 || width == 8 )
      {
        *integral = true;
        return width;
      }
      break;
    case 'b':
      if ( width == 1 )
      {
        *integral = true;
        return 1;
      }
      break;
    default:
      break;
  }
  *error = QStringLiteral( "不支持的 dtype：%1" ).arg( descr );
  return 0;
}

double readNumeric( const char *data, const QString &descr, bool integral )
{
  if ( integral )
  {
    if ( descr.contains( QLatin1String( "u1" ) ) || descr.contains( QLatin1String( "b1" ) ) )
      return static_cast<double>( static_cast<std::uint8_t>( data[0] ) );
    if ( descr.contains( QLatin1String( "i1" ) ) )
      return static_cast<double>( static_cast<std::int8_t>( data[0] ) );
    if ( descr.contains( QLatin1String( "u2" ) ) )
    {
      std::uint16_t value = 0;
      std::memcpy( &value, data, 2 );
      return static_cast<double>( value );
    }
    if ( descr.contains( QLatin1String( "i2" ) ) )
    {
      std::int16_t value = 0;
      std::memcpy( &value, data, 2 );
      return static_cast<double>( value );
    }
    if ( descr.contains( QLatin1String( "u4" ) ) )
    {
      std::uint32_t value = 0;
      std::memcpy( &value, data, 4 );
      return static_cast<double>( value );
    }
    if ( descr.contains( QLatin1String( "i4" ) ) )
    {
      std::int32_t value = 0;
      std::memcpy( &value, data, 4 );
      return static_cast<double>( value );
    }
    if ( descr.contains( QLatin1String( "u8" ) ) )
    {
      std::uint64_t value = 0;
      std::memcpy( &value, data, 8 );
      return static_cast<double>( value );
    }
    std::int64_t value = 0;
    std::memcpy( &value, data, 8 );
    return static_cast<double>( value );
  }
  if ( descr.contains( QLatin1String( "f4" ) ) )
  {
    float value = 0;
    std::memcpy( &value, data, 4 );
    return static_cast<double>( value );
  }
  double value = 0;
  std::memcpy( &value, data, 8 );
  return value;
}

QVariantMap jsonObjectToMap( const QJsonObject &object )
{
  return object.toVariantMap();
}

QString typeName( const QJsonValue &value )
{
  switch ( value.type() )
  {
    case QJsonValue::Null:
      return QStringLiteral( "null" );
    case QJsonValue::Bool:
      return QStringLiteral( "bool" );
    case QJsonValue::Double:
      return QStringLiteral( "number" );
    case QJsonValue::String:
      return QStringLiteral( "string" );
    case QJsonValue::Array:
      return QStringLiteral( "array" );
    case QJsonValue::Object:
      return QStringLiteral( "object" );
    case QJsonValue::Undefined:
      break;
  }
  return QStringLiteral( "undefined" );
}

// manifest 里被本读取面文档化的字段：类型不对就逐条列因（不静默当缺省）。
void readManifest( const QJsonObject &object, SfPackageManifest *manifest, QStringList *issues )
{
  manifest->raw = jsonObjectToMap( object );
  const auto stringField = [&]( const QString &key, QString *target ) {
    const QJsonValue value = object.value( key );
    if ( value.isUndefined() )
    {
      issues->append( QStringLiteral( "manifest.%1 缺失" ).arg( key ) );
      return;
    }
    if ( !value.isString() )
    {
      issues->append( QStringLiteral( "manifest.%1 类型是 %2，期望 string" ).arg( key, typeName( value ) ) );
      return;
    }
    *target = value.toString();
  };
  const auto numberField = [&]( const QString &key, double *target, bool *present ) {
    const QJsonValue value = object.value( key );
    if ( value.isUndefined() || value.isNull() )
    {
      if ( present )
        *present = false;
      return;
    }
    if ( !value.isDouble() )
    {
      issues->append( QStringLiteral( "manifest.%1 类型是 %2，期望 number" ).arg( key, typeName( value ) ) );
      return;
    }
    *target = value.toDouble();
    if ( present )
      *present = true;
  };
  const auto objectField = [&]( const QString &key, QVariantMap *target ) {
    const QJsonValue value = object.value( key );
    if ( value.isUndefined() || value.isNull() )
      return;
    if ( !value.isObject() )
    {
      issues->append( QStringLiteral( "manifest.%1 类型是 %2，期望 object" ).arg( key, typeName( value ) ) );
      return;
    }
    *target = value.toObject().toVariantMap();
  };
  const auto arrayField = [&]( const QString &key, QVariantList *target ) {
    const QJsonValue value = object.value( key );
    if ( value.isUndefined() || value.isNull() )
      return;
    if ( !value.isArray() )
    {
      issues->append( QStringLiteral( "manifest.%1 类型是 %2，期望 array" ).arg( key, typeName( value ) ) );
      return;
    }
    *target = value.toArray().toVariantList();
  };

  stringField( QStringLiteral( "format" ), &manifest->format );
  stringField( QStringLiteral( "version" ), &manifest->version );
  stringField( QStringLiteral( "factor_name" ), &manifest->factorName );
  stringField( QStringLiteral( "horizon" ), &manifest->horizon );
  stringField( QStringLiteral( "method" ), &manifest->method );
  stringField( QStringLiteral( "value_source" ), &manifest->valueSource );
  stringField( QStringLiteral( "barrier_value_policy" ), &manifest->barrierValuePolicy );
  stringField( QStringLiteral( "analysis_value_policy" ), &manifest->analysisValuePolicy );
  stringField( QStringLiteral( "interpolation_model" ), &manifest->interpolationModel );
  stringField( QStringLiteral( "crs_wkt" ), &manifest->crsWkt );
  stringField( QStringLiteral( "data_source" ), &manifest->dataSource );
  stringField( QStringLiteral( "created_at" ), &manifest->createdAt );
  stringField( QStringLiteral( "partition_version" ), &manifest->partitionVersion );
  numberField( QStringLiteral( "value_min" ), &manifest->valueMin, &manifest->hasValueRange );
  numberField( QStringLiteral( "value_max" ), &manifest->valueMax, nullptr );
  numberField( QStringLiteral( "barrier_buffer_distance" ), &manifest->barrierBufferDistance, nullptr );
  numberField( QStringLiteral( "contour_stop_buffer_distance" ), &manifest->contourStopBufferDistance, nullptr );
  numberField( QStringLiteral( "global_anisotropy_ratio" ), &manifest->globalAnisotropyRatio, nullptr );
  numberField( QStringLiteral( "global_anisotropy_angle" ), &manifest->globalAnisotropyAngle, nullptr );
  objectField( QStringLiteral( "method_params" ), &manifest->methodParams );
  objectField( QStringLiteral( "field_model" ), &manifest->fieldModel );
  objectField( QStringLiteral( "contour_partition" ), &manifest->contourPartition );
  objectField( QStringLiteral( "coverage_info" ), &manifest->coverageInfo );
  arrayField( QStringLiteral( "levels" ), &manifest->levels );
  arrayField( QStringLiteral( "barriers" ), &manifest->barriers );
  arrayField( QStringLiteral( "directions" ), &manifest->directions );
  arrayField( QStringLiteral( "boundaries" ), &manifest->boundaries );
  arrayField( QStringLiteral( "partition_barriers" ), &manifest->partitionBarriers );
  arrayField( QStringLiteral( "partition_extensions" ), &manifest->partitionExtensions );
  arrayField( QStringLiteral( "grid_shape" ), &manifest->gridShape );

  const QJsonValue pickle = object.value( QStringLiteral( "has_pickle" ) );
  if ( pickle.isBool() )
    manifest->hasPickle = pickle.toBool();
  else if ( !pickle.isUndefined() )
    issues->append( QStringLiteral( "manifest.has_pickle 类型是 %1，期望 bool" ).arg( typeName( pickle ) ) );

  const QJsonValue complete = object.value( QStringLiteral( "partition_complete" ) );
  if ( complete.isBool() )
    manifest->partitionComplete = complete.toBool();
  else if ( !complete.isUndefined() )
    issues->append(
        QStringLiteral( "manifest.partition_complete 类型是 %1，期望 bool" ).arg( typeName( complete ) ) );

  // contour_stop_buffer_distance 缺省时沿用 barrier_buffer_distance（上游口径）。
  if ( !object.contains( QStringLiteral( "contour_stop_buffer_distance" ) ) )
    manifest->contourStopBufferDistance = manifest->barrierBufferDistance;
}

bool manifestForbidsPickle( const SfPackageManifest &manifest )
{
  return manifest.hasPickle || manifest.format.contains( QLatin1String( "pickle" ), Qt::CaseInsensitive );
}

} // namespace

const SfPackageArray *SfPackageReadResult::array( const QString &name ) const
{
  for ( const SfPackageArray &item : arrays )
  {
    if ( item.name == name )
      return &item;
  }
  return nullptr;
}

NpyArray parseNpy( const QByteArray &bytes )
{
  NpyArray result;
  static const char magic[6] = { '\x93', 'N', 'U', 'M', 'P', 'Y' };
  if ( bytes.size() < 10 || std::memcmp( bytes.constData(), magic, 6 ) != 0 )
  {
    result.error = QStringLiteral( "不是 NPY（magic 不匹配）" );
    return result;
  }
  const int major = static_cast<unsigned char>( bytes.at( 6 ) );
  const int minor = static_cast<unsigned char>( bytes.at( 7 ) );
  int prefix = 8;
  qint64 headerLength = 0;
  if ( major == 1 )
  {
    headerLength = static_cast<unsigned char>( bytes.at( 8 ) ) |
                   ( static_cast<qint64>( static_cast<unsigned char>( bytes.at( 9 ) ) ) << 8 );
    prefix = 10;
  }
  else if ( major == 2 || major == 3 )
  {
    if ( bytes.size() < 12 )
    {
      result.error = QStringLiteral( "NPY 头部被截断" );
      return result;
    }
    headerLength = static_cast<unsigned char>( bytes.at( 8 ) ) |
                   ( static_cast<qint64>( static_cast<unsigned char>( bytes.at( 9 ) ) ) << 8 ) |
                   ( static_cast<qint64>( static_cast<unsigned char>( bytes.at( 10 ) ) ) << 16 ) |
                   ( static_cast<qint64>( static_cast<unsigned char>( bytes.at( 11 ) ) ) << 24 );
    prefix = 12;
  }
  else
  {
    result.error = QStringLiteral( "不支持的 NPY 版本：%1.%2" ).arg( major ).arg( minor );
    return result;
  }
  if ( headerLength <= 0 || prefix + headerLength > bytes.size() )
  {
    result.error = QStringLiteral( "NPY 头部长度非法" );
    return result;
  }
  const QString header = QString::fromLatin1( bytes.mid( prefix, static_cast<int>( headerLength ) ) );

  bool found = false;
  result.descr = headerString( header, QStringLiteral( "descr" ), &found );
  if ( !found || result.descr.isEmpty() )
  {
    result.error = QStringLiteral( "NPY 头部缺少 descr" );
    return result;
  }
  const QString order = headerScalar( header, QStringLiteral( "fortran_order" ), &found );
  if ( !found )
  {
    result.error = QStringLiteral( "NPY 头部缺少 fortran_order" );
    return result;
  }
  result.fortranOrder = order.compare( QLatin1String( "True" ), Qt::CaseInsensitive ) == 0;
  const QStringList shape = headerShape( header, &found );
  if ( !found )
  {
    result.error = QStringLiteral( "NPY 头部缺少 shape" );
    return result;
  }
  std::vector<int> dims;
  for ( const QString &item : shape )
  {
    bool ok = false;
    const int value = item.trimmed().toInt( &ok );
    if ( !ok || value < 0 )
    {
      result.error = QStringLiteral( "NPY shape 含非整数项：%1" ).arg( item );
      return result;
    }
    dims.push_back( value );
  }
  if ( dims.size() > 2 )
  {
    result.error = QStringLiteral( "只支持 1D/2D 数组，shape 维度 %1" ).arg( dims.size() );
    return result;
  }
  if ( dims.empty() )
  {
    // numpy 的 0 维标量（shape=()）不是 1×1 数组，不按数组读。
    result.error = QStringLiteral( "不支持 0 维标量数组（shape=()）" );
    return result;
  }
  result.rows = dims[0];
  result.cols = dims.size() < 2 ? 1 : dims[1];

  QString dtypeError;
  const int width = dtypeBytes( result.descr, &result.integral, &dtypeError );
  if ( width == 0 )
  {
    result.error = dtypeError;
    return result;
  }
  const qint64 count = static_cast<qint64>( result.rows ) * result.cols;
  const qint64 dataStart = prefix + headerLength;
  // #216：count*width 在 shape 声明超大时会有符号溢出回绕成负值、击穿长度闸；
  // 改用除法比较（dataStart <= bytes.size() 已由头部长度闸保证）。
  if ( count < 0 || count > ( bytes.size() - dataStart ) / width )
  {
    result.error = QStringLiteral( "NPY 数据段长度与 shape 不符" );
    return result;
  }
  result.values.assign( static_cast<std::size_t>( count ), 0.0 );
  const char *data = bytes.constData() + dataStart;
  for ( int row = 0; row < result.rows; ++row )
  {
    for ( int col = 0; col < result.cols; ++col )
    {
      // fortran_order 的磁盘序是列优先 → 读回行主序要转置。
      const qint64 source = result.fortranOrder ? static_cast<qint64>( col ) * result.rows + row
                                                : static_cast<qint64>( row ) * result.cols + col;
      const std::size_t target = static_cast<std::size_t>( row ) * static_cast<std::size_t>( result.cols ) +
                                 static_cast<std::size_t>( col );
      result.values[target] = readNumeric( data + source * width, result.descr, result.integral );
    }
  }
  result.ok = true;
  return result;
}

SfPackageReadResult readSfPackage( const QString &path )
{
  SfPackageReadResult result;
  result.path = path;
  const QFileInfo info( path );
  if ( !info.exists() || !info.isFile() )
  {
    result.error = QStringLiteral( "文件不存在：%1" ).arg( path );
    return result;
  }
  QString archiveError;
  const QByteArray archive = zipReadArchive( path, &archiveError );
  if ( archive.isEmpty() )
  {
    result.error = archiveError.isEmpty() ? QStringLiteral( "无法读取 .sfpkg 容器" ) : archiveError;
    return result;
  }
  const ZipListResult listing = zipListBytes( archive );
  if ( !listing.ok )
  {
    result.error = QStringLiteral( "不是合法 ZIP 容器（%1）：%2" ).arg( listing.error, path );
    return result;
  }
  for ( const ZipEntry &entry : listing.entries )
  {
    result.zipEntries.append( entry.name );
    if ( entry.name != kManifestEntry && entry.name != kNpzEntry && entry.name != kChecksumEntry )
      result.sidecars.append( entry.name );
  }
  if ( !listing.contains( kManifestEntry ) || !listing.contains( kNpzEntry ) )
  {
    result.error = QStringLiteral( "无效的 .sfpkg：缺少 manifest.json 或 surface.npz" );
    return result;
  }

  QByteArray manifestBytes;
  QByteArray npzBytes;
  QByteArray checksumBytes;
  QString entryError;
  if ( !zipExtractBytes( archive, kManifestEntry, &manifestBytes, &entryError ) )
  {
    result.error = QStringLiteral( "读取 manifest.json 失败：%1" ).arg( entryError );
    return result;
  }
  if ( !zipExtractBytes( archive, kNpzEntry, &npzBytes, &entryError ) )
  {
    result.error = QStringLiteral( "读取 surface.npz 失败：%1" ).arg( entryError );
    return result;
  }
  result.checksumPresent = result.zipEntries.contains( kChecksumEntry );
  if ( result.checksumPresent )
  {
    if ( !zipExtractBytes( archive, kChecksumEntry, &checksumBytes, &entryError ) )
    {
      result.error = QStringLiteral( "读取 checksum.json 失败：%1" ).arg( entryError );
      return result;
    }
    QJsonParseError checksumError;
    const QJsonDocument document = QJsonDocument::fromJson( checksumBytes, &checksumError );
    if ( checksumError.error != QJsonParseError::NoError || !document.isObject() )
    {
      result.error = QStringLiteral( "checksum.json 不是合法 JSON：%1" ).arg( checksumError.errorString() );
      return result;
    }
    const QJsonObject object = document.object();
    // 三个字段都要类型校验：非字符串（null/number/array）不能靠 toString() 变成
    // 空串后被「!isEmpty()」跳过——那等于静默免校验。
    const auto checksumField = [&]( const QString &key, QString *target ) {
      const QJsonValue value = object.value( key );
      if ( value.isUndefined() || value.isNull() )
        return true;
      if ( !value.isString() )
      {
        result.error = QStringLiteral( "checksum.json 的 %1 类型是 %2，期望 string（拒绝跳过校验）" )
                           .arg( key, typeName( value ) );
        return false;
      }
      *target = value.toString();
      return true;
    };
    QString npzHash;
    QString manifestHash;
    QString algorithm;
    if ( !checksumField( QStringLiteral( "surface_npz_sha256" ), &npzHash ) ||
         !checksumField( QStringLiteral( "manifest_sha256" ), &manifestHash ) ||
         !checksumField( QStringLiteral( "algorithm" ), &algorithm ) )
      return result;
    if ( !algorithm.isEmpty() && algorithm.compare( QLatin1String( "sha256" ), Qt::CaseInsensitive ) != 0 )
    {
      result.error = QStringLiteral( "checksum.json 算法不是 sha256：%1" ).arg( algorithm );
      return result;
    }
    if ( npzHash.isEmpty() && manifestHash.isEmpty() )
    {
      result.error = QStringLiteral(
          "checksum.json 存在但没有任何 sha256 字段：拒绝在未校验的情况下读取" );
      return result;
    }
    if ( !npzHash.isEmpty() && npzHash.compare( sha256Hex( npzBytes ), Qt::CaseInsensitive ) != 0 )
    {
      result.error = QStringLiteral( ".sfpkg 校验失败：surface.npz SHA256 不匹配" );
      return result;
    }
    if ( !manifestHash.isEmpty() && manifestHash.compare( sha256Hex( manifestBytes ), Qt::CaseInsensitive ) != 0 )
    {
      result.error = QStringLiteral( ".sfpkg 校验失败：manifest.json SHA256 不匹配" );
      return result;
    }
  }
  else
  {
    result.issues.append( QStringLiteral( "包内没有 checksum.json，未做 SHA256 校验" ) );
  }

  QJsonParseError manifestError;
  const QJsonDocument manifestDocument = QJsonDocument::fromJson( manifestBytes, &manifestError );
  if ( manifestError.error != QJsonParseError::NoError || !manifestDocument.isObject() )
  {
    result.error = QStringLiteral( "manifest.json 不是合法 JSON：%1" ).arg( manifestError.errorString() );
    return result;
  }
  readManifest( manifestDocument.object(), &result.manifest, &result.issues );
  if ( manifestForbidsPickle( result.manifest ) )
  {
    result.error = QStringLiteral( ".sfpkg 禁止 pickle（format=%1，has_pickle=%2）" )
                       .arg( result.manifest.format )
                       .arg( result.manifest.hasPickle ? QStringLiteral( "true" ) : QStringLiteral( "false" ) );
    return result;
  }
  if ( !result.manifest.format.isEmpty() && result.manifest.format.compare( QLatin1String( "sfpkg" ),
                                                                           Qt::CaseInsensitive ) != 0 )
  {
    result.issues.append(
        QStringLiteral( "manifest.format 不是 sfpkg：%1" ).arg( result.manifest.format ) );
  }

  // surface.npz 自身还是 ZIP（条目名 = 数组名 + .npy）。同一个内存 ZIP 读面
  // 直接解内层包：不落临时文件，也不依赖 mime 库。
  const ZipListResult npzListing = zipListBytes( npzBytes );
  if ( !npzListing.ok )
  {
    result.error = QStringLiteral( "surface.npz 不是合法 NPZ 容器：%1" ).arg( npzListing.error );
    return result;
  }
  for ( const ZipEntry &entry : npzListing.entries )
  {
    if ( !entry.name.endsWith( QLatin1String( ".npy" ) ) )
    {
      result.issues.append( QStringLiteral( "surface.npz 含非 .npy 条目：%1" ).arg( entry.name ) );
      continue;
    }
    QByteArray arrayBytes;
    QString arrayError;
    if ( !zipExtractBytes( npzBytes, entry.name, &arrayBytes, &arrayError ) )
    {
      result.issues.append(
          QStringLiteral( "surface.npz 条目 %1 读取失败：%2" ).arg( entry.name, arrayError ) );
      continue;
    }
    const NpyArray parsed = parseNpy( arrayBytes );
    SfPackageArray array;
    array.name = entry.name.left( entry.name.size() - 4 );
    if ( !parsed.ok )
    {
      result.issues.append( QStringLiteral( "数组 %1 解析失败：%2" ).arg( array.name, parsed.error ) );
      continue;
    }
    array.dtype = parsed.descr;
    array.fortranOrder = parsed.fortranOrder;
    array.integral = parsed.integral;
    array.rows = parsed.rows;
    array.cols = parsed.cols;
    array.values = parsed.values;
    result.arrays.push_back( array );
  }

  const SfPackageArray *grid = result.array( QStringLiteral( "grid_z" ) );
  if ( !grid )
  {
    result.error = QStringLiteral( "surface.npz 缺少 grid_z" );
    return result;
  }
  if ( result.manifest.gridShape.size() == 2 )
  {
    const int rows = result.manifest.gridShape.at( 0 ).toInt();
    const int cols = result.manifest.gridShape.at( 1 ).toInt();
    if ( rows != grid->rows || cols != grid->cols )
    {
      result.issues.append( QStringLiteral( "manifest.grid_shape [%1, %2] 与 grid_z %3×%4 不符" )
                                .arg( rows )
                                .arg( cols )
                                .arg( grid->rows )
                                .arg( grid->cols ) );
    }
  }
  // nodata 像元统计如实报告（上游用 NaN 记 nodata，不另设哨兵值）。
  int nodata = 0;
  for ( double value : grid->values )
  {
    if ( !std::isfinite( value ) )
      ++nodata;
  }
  if ( nodata == 0 )
    result.issues.append( QStringLiteral( "grid_z 没有 nodata 像元（全部有限）" ) );
  if ( !result.array( QStringLiteral( "grid_x" ) ) || !result.array( QStringLiteral( "grid_y" ) ) )
    result.issues.append( QStringLiteral( "surface.npz 缺少 grid_x/grid_y：无法还原坐标轴" ) );
  if ( !result.array( QStringLiteral( "valid_mask" ) ) )
    result.issues.append( QStringLiteral( "surface.npz 缺少 valid_mask" ) );

  result.ok = true;
  return result;
}

} // namespace paleo::io
