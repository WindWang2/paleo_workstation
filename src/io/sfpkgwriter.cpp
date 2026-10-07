// 层：数据
#include "sfpkgwriter.h"
#include "sfpkg_internal.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <cstdint>
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

// 值 → 定宽整型（合法性先行：有限、无小数部分、在 [lo, hi] 内）。
bool integralValue( double value, double lo, double hi, const QString &dtype, const QString &name,
                    QString *error, std::int64_t *out )
{
  if ( !std::isfinite( value ) || std::trunc( value ) != value || value < lo || value > hi )
  {
    *error = QStringLiteral( "数组 %1 的值 %2 不是 %3 可表示的整数" ).arg( name ).arg( value ).arg( dtype );
    return false;
  }
  *out = static_cast<std::int64_t>( value );
  return true;
}

// 数值 → NPY 数据段（小端）。返回 false 时 error 已给因（整型越界/非整数/dtype 不支持）。
bool appendNumeric( QByteArray *out, const QString &dtype, double value, const QString &name,
                    QString *error )
{
  const auto put64 = [out]( std::uint64_t bits ) {
    for ( int shift = 0; shift < 64; shift += 8 )
      out->append( static_cast<char>( ( bits >> shift ) & 0xFF ) );
  };
  const auto put32 = [out]( std::uint32_t bits ) {
    for ( int shift = 0; shift < 32; shift += 8 )
      out->append( static_cast<char>( ( bits >> shift ) & 0xFF ) );
  };
  if ( dtype == QLatin1String( "<f8" ) )
  {
    std::uint64_t bits = 0;
    std::memcpy( &bits, &value, 8 ); // 小端主机直写（支持的构建均为 LE）
    put64( bits );
    return true;
  }
  if ( dtype == QLatin1String( "<f4" ) )
  {
    const float narrowed = static_cast<float>( value );
    std::uint32_t bits = 0;
    std::memcpy( &bits, &narrowed, 4 );
    put32( bits );
    return true; // f4 是调用方选择的低精度口径，不另发 issue
  }
  if ( dtype == QLatin1String( "|b1" ) )
  {
    out->append( value != 0 ? '\x01' : '\x00' );
    return true;
  }
  std::int64_t iv = 0;
  if ( dtype == QLatin1String( "<i8" ) || dtype == QLatin1String( "<u8" ) )
  {
    if ( !integralValue( value, dtype == QLatin1String( "<u8" ) ? 0.0 : -9.2233720368547758e18,
                         9.2233720368547758e18, dtype, name, error, &iv ) )
      return false;
    put64( static_cast<std::uint64_t>( iv ) );
    return true;
  }
  if ( dtype == QLatin1String( "<i4" ) || dtype == QLatin1String( "<u4" ) )
  {
    if ( !integralValue( value, dtype == QLatin1String( "<u4" ) ? 0.0 : -2147483648.0, 4294967295.0,
                         dtype, name, error, &iv ) )
      return false;
    put32( static_cast<std::uint32_t>( iv ) );
    return true;
  }
  if ( dtype == QLatin1String( "<i2" ) || dtype == QLatin1String( "<u2" ) )
  {
    if ( !integralValue( value, dtype == QLatin1String( "<u2" ) ? 0.0 : -32768.0, 65535.0, dtype,
                         name, error, &iv ) )
      return false;
    const std::uint16_t bits = static_cast<std::uint16_t>( iv );
    out->append( static_cast<char>( bits & 0xFF ) );
    out->append( static_cast<char>( ( bits >> 8 ) & 0xFF ) );
    return true;
  }
  if ( dtype == QLatin1String( "<i1" ) || dtype == QLatin1String( "<u1" ) )
  {
    if ( !integralValue( value, dtype == QLatin1String( "<u1" ) ? 0.0 : -128.0, 255.0, dtype, name,
                         error, &iv ) )
      return false;
    out->append( static_cast<char>( static_cast<std::uint8_t>( iv ) ) );
    return true;
  }
  *error = QStringLiteral( "数组 %1 的 dtype 不支持：%2" ).arg( name, dtype );
  return false;
}

// NPY 1.0 序列化（v2 头仅在 header > 65535 时需要，单数组头远小于该值）。
QByteArray buildNpy( const SfPackageArrayInput &array, QString *error )
{
  QString dtype = array.dtype;
  if ( dtype.isEmpty() )
    dtype = QStringLiteral( "<f8" );
  QByteArray data;
  data.reserve( static_cast<int>( array.values.size() ) * 8 );
  for ( double value : array.values )
  {
    if ( !appendNumeric( &data, dtype, value, array.name, error ) )
      return {};
  }
  const QString shape = array.cols <= 1
                            ? QStringLiteral( "(%1,)" ).arg( array.rows )
                            : QStringLiteral( "(%1, %2)" ).arg( array.rows ).arg( array.cols );
  QString header = QStringLiteral( "{'descr': '%1', 'fortran_order': False, 'shape': %2, " )
                       .arg( dtype, shape );
  // 头部按 64 字节对齐（10 字节前缀 + 头长含结尾 \n 是 64 的倍数）。
  const int totalForAlignment = 10 + header.size() + 1; // + '\n'
  const int padding = ( 64 - totalForAlignment % 64 ) % 64;
  header += QString( padding, QLatin1Char( ' ' ) );
  header += QLatin1Char( '\n' );
  if ( header.size() > 65535 )
  {
    *error = QStringLiteral( "数组 %1 的 NPY 头超长" ).arg( array.name );
    return {};
  }
  QByteArray out;
  out.append( "\x93NUMPY", 6 );
  out.append( static_cast<char>( 1 ) ); // major
  out.append( static_cast<char>( 0 ) ); // minor
  const std::uint16_t headerLength = static_cast<std::uint16_t>( header.size() );
  out.append( static_cast<char>( headerLength & 0xFF ) );
  out.append( static_cast<char>( ( headerLength >> 8 ) & 0xFF ) );
  out.append( header.toLatin1() );
  out.append( data );
  return out;
}


} // namespace

SfPackageWriteResult writeSfPackage( const QString &path,
                                     const QVariantMap &manifest,
                                     const QVector<SfPackageArrayInput> &arrays,
                                     const QMap<QString, QByteArray> &sidecars,
                                     ZipCompression compression )
{
  SfPackageWriteResult result;
  const auto fail = [&result]( const QString &message ) {
    result.error = message;
    return result;
  };

  // ---- manifest 校验（读面会 issue 的缺字段，写面按语义分档处理）----
  QVariantMap effective = manifest;
  const QJsonValue pickleValue = QJsonObject::fromVariantMap( manifest ).value( QStringLiteral( "has_pickle" ) );
  if ( pickleValue.toBool( false ) )
    return fail( QStringLiteral( ".sfpkg 禁止 pickle（has_pickle=true）" ) );
  const QVariant formatValue = manifest.value( QStringLiteral( "format" ) );
  const QString format = formatValue.toString();
  if ( !formatValue.isNull() && formatValue.type() != QVariant::String )
    return fail( QStringLiteral( "manifest.format 类型不是 string：%1" ).arg( formatValue.typeName() ) );
  if ( !format.isEmpty() && format.compare( QLatin1String( "sfpkg" ), Qt::CaseInsensitive ) != 0 )
    return fail( QStringLiteral( "manifest.format 不是 sfpkg：%1" ).arg( format ) );
  if ( format.contains( QLatin1String( "pickle" ), Qt::CaseInsensitive ) )
    return fail( QStringLiteral( "manifest.format 含 pickle：%1" ).arg( format ) );
  if ( format.isEmpty() )
    effective.insert( QStringLiteral( "format" ), QStringLiteral( "sfpkg" ) );
  if ( !manifest.contains( QStringLiteral( "version" ) ) )
    effective.insert( QStringLiteral( "version" ), QStringLiteral( "1.0" ) );
  for ( const char *key : { "factor_name", "horizon", "method" } )
  {
    const QVariant value = manifest.value( QLatin1String( key ) );
    if ( value.type() != QVariant::String || value.toString().isEmpty() )
      return fail( QStringLiteral( "manifest.%1 缺失/为空/类型不对（写面要求 string 语义必填）" )
                       .arg( QLatin1String( key ) ) );
  }

  // ---- 数组校验 ----
  const SfPackageArrayInput *gridZ = nullptr;
  for ( const SfPackageArrayInput &array : arrays )
  {
    if ( array.name.isEmpty() )
      return fail( QStringLiteral( "数组条目名为空" ) );
    if ( array.rows <= 0 || array.cols <= 0 )
      return fail( QStringLiteral( "数组 %1 尺寸非法（rows=%2, cols=%3）" )
                       .arg( array.name )
                       .arg( array.rows )
                       .arg( array.cols ) );
    if ( array.values.size() != static_cast<std::size_t>( array.rows ) * array.cols )
      return fail( QStringLiteral( "数组 %1 的 values 长度 %2 与 rows×cols=%3 不符" )
                       .arg( array.name )
                       .arg( array.values.size() )
                       .arg( static_cast<long long>( array.rows ) * array.cols ) );
    for ( const SfPackageArrayInput &other : arrays )
    {
      if ( &other != &array && other.name == array.name )
        return fail( QStringLiteral( "数组条目重名：%1" ).arg( array.name ) );
    }
    if ( array.name == QLatin1String( "grid_z" ) )
      gridZ = &array;
  }
  if ( !gridZ )
    return fail( QStringLiteral( "surface.npz 缺少 grid_z（读面硬要求，写面拒写）" ) );

  // grid_shape：给出则必须与 grid_z 一致；缺省按 grid_z 补写（读面对照用）。
  if ( effective.contains( QStringLiteral( "grid_shape" ) ) )
  {
    const QVariantList shape = effective.value( QStringLiteral( "grid_shape" ) ).toList();
    if ( shape.size() != 2 )
      return fail( QStringLiteral( "manifest.grid_shape 必须是二元数组" ) );
    if ( shape.at( 0 ).toInt() != gridZ->rows || shape.at( 1 ).toInt() != gridZ->cols )
      return fail( QStringLiteral( "manifest.grid_shape [%1, %2] 与 grid_z %3×%4 不符（写面拒写自相矛盾的包）" )
                       .arg( shape.at( 0 ).toInt() )
                       .arg( shape.at( 1 ).toInt() )
                       .arg( gridZ->rows )
                       .arg( gridZ->cols ) );
  }
  else
  {
    effective.insert( QStringLiteral( "grid_shape" ),
                      QVariantList{ gridZ->rows, gridZ->cols } );
  }
  // 读面会逐条 issue 的软性缺项：照写但如实提示（不静默产出「读起来有 issue」的包）。
  QStringList names;
  names.reserve( arrays.size() );
  for ( const SfPackageArrayInput &array : arrays )
    names.append( array.name );
  for ( const QString &expected : { QStringLiteral( "grid_x" ), QStringLiteral( "grid_y" ),
                                    QStringLiteral( "valid_mask" ) } )
  {
    if ( !names.contains( expected ) )
      result.issues.append( QStringLiteral( "surface.npz 缺少 %1（读面将逐条 issue）" ).arg( expected ) );
  }

  // ---- 组包 ----
  const QByteArray manifestBytes =
      QJsonDocument( QJsonObject::fromVariantMap( effective ) ).toJson( QJsonDocument::Compact );
  QVector<ZipWriteEntry> npzEntries;
  for ( const SfPackageArrayInput &array : arrays )
  {
    QString npyError;
    const QByteArray npy = buildNpy( array, &npyError );
    if ( npy.isEmpty() )
      return fail( npyError );
    npzEntries.append( { array.name + QStringLiteral( ".npy" ), npy } );
  }
  ZipBuildOptions npzOptions;
  npzOptions.compression = ZipCompression::Stored; // npz 内层 numpy 习惯不压缩
  QString npzError;
  const QByteArray npzBytes = zipBuildArchive( npzEntries, npzOptions, &npzError );
  if ( npzBytes.isEmpty() )
    return fail( QStringLiteral( "surface.npz 组包失败：%1" ).arg( npzError ) );

  QJsonObject checksum;
  checksum.insert( QStringLiteral( "algorithm" ), QStringLiteral( "sha256" ) );
  checksum.insert( QStringLiteral( "manifest_sha256" ), sha256Hex( manifestBytes ) );
  checksum.insert( QStringLiteral( "surface_npz_sha256" ), sha256Hex( npzBytes ) );
  const QByteArray checksumBytes = QJsonDocument( checksum ).toJson( QJsonDocument::Compact );

  QVector<ZipWriteEntry> entries;
  entries.append( { kManifestEntry, manifestBytes } );
  entries.append( { kNpzEntry, npzBytes } );
  entries.append( { kChecksumEntry, checksumBytes } );
  for ( auto it = sidecars.constBegin(); it != sidecars.constEnd(); ++it )
  {
    if ( it.key().isEmpty() )
      return fail( QStringLiteral( "边车条目名为空" ) );
    if ( it.key() == kManifestEntry || it.key() == kNpzEntry || it.key() == kChecksumEntry )
      return fail( QStringLiteral( "边车条目名与包内保留名冲突：%1" ).arg( it.key() ) );
    entries.append( { it.key(), it.value() } );
  }
  ZipBuildOptions options;
  options.compression = compression;
  bool zip64 = false;
  QString writeError;
  if ( !zipWriteArchive( path, entries, options, &writeError, &zip64 ) )
    return fail( writeError );
  result.zip64Used = zip64;
  result.bytesWritten = QFileInfo( path ).size();
  result.ok = true;
  return result;
}

} // namespace paleo::io
