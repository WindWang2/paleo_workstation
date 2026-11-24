// 层：功能
// workflow/geostatatominputs.cpp — 见 geostatatominputs.h。
#include "geostatatominputs.h"
#include "algorithms/geostat/types.h" // 重头直含（transitive 闸口径）

#include "qgis/geostatsamplereader.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>

#include <cmath>

namespace paleo::geostat_atom
{
namespace
{

constexpr char kSnapshotSchema[] = "paleo.geostat-snapshot.v1";

bool regularFile( const QFileInfo &file )
{
#ifdef Q_OS_WIN
  if( file.isJunction() )
    return false;
#endif
  return file.isFile() && !file.isSymLink();
}

QByteArray sha256Hex( QByteArrayView bytes )
{
  return QCryptographicHash::hash( bytes, QCryptographicHash::Sha256 ).toHex();
}

// <root>/.paleo/capability-ops/geostat-snapshots 的逐段可信解析（与
// pythoninputbundle 的 privateRoot 同口径）：每一段存在即必须是无符号链接
// 的目录且规范路径不变；create=false 时不建任何目录。
QString snapshotRoot( const QString &project, bool create, QString *code )
{
  const QString root = QFileInfo( project ).canonicalFilePath();
  if( root.isEmpty() || !QFileInfo( root ).isDir() )
  {
    if( code )
      *code = QStringLiteral( "geostat_snapshot_path_unsafe" );
    return {};
  }
  QString current = root;
  for( const auto &part : { QStringLiteral( ".paleo" ), QStringLiteral( "capability-ops" ),
                            QStringLiteral( "geostat-snapshots" ) } )
  {
    current += QLatin1Char( '/' );
    current += part;
    const QFileInfo info( current );
    if( info.isSymLink() || ( info.exists() && !info.isDir() ) ||
        ( create && !QDir().mkpath( current ) ) || !QFileInfo( current ).isDir() ||
        QFileInfo( current ).canonicalFilePath() != current )
    {
      if( code )
        *code = QStringLiteral( "geostat_snapshot_path_unsafe" );
      return {};
    }
  }
  return current;
}

// 快照文件的可信定位：相对路径形状与摘要目录名逐字比对（杜绝 ../、绝对
// 路径与链接别名），再确认是快照根下的常规文件。空串 = 不可用。
QString locateSnapshotFile( const QString &projectDir, const GeostatSamplePlan &plan,
                            QString *code )
{
  const auto fail = [code]() {
    if( code )
      *code = QStringLiteral( "geostat_snapshot_unavailable" );
    return QString();
  };
  if( plan.snapshotPath.isEmpty() || QDir::isAbsolutePath( plan.snapshotPath ) ||
      QDir::cleanPath( plan.snapshotPath ) != plan.snapshotPath ||
      plan.snapshotPath != geostatSnapshotRelativePath( plan.snapshotSha256 ) )
    return fail();
  for( const auto &ch : plan.snapshotSha256 )
    if( !( ( ch >= '0' && ch <= '9' ) || ( ch >= 'a' && ch <= 'f' ) ) )
      return fail();
  if( plan.snapshotSha256.size() != 64 )
    return fail();
  QString rootCode;
  const QString root = snapshotRoot( projectDir, false, &rootCode );
  if( root.isEmpty() )
  {
    if( code )
      *code = rootCode;
    return {};
  }
  const QString path = root + QLatin1Char( '/' ) + plan.snapshotSha256 +
                       QStringLiteral( "/samples.json" );
  const QFileInfo info( path );
  const QFileInfo directory( info.absolutePath() );
  if( !regularFile( info ) || directory.isSymLink() ||
      directory.canonicalFilePath() != directory.absoluteFilePath() )
    return fail();
  if( code )
    code->clear();
  return path;
}

// 读回并核对摘要；任何不符 → geostat_snapshot_unavailable。
QByteArray readSnapshotBytes( const QString &projectDir, const GeostatSamplePlan &plan,
                              QString *code )
{
  const QString path = locateSnapshotFile( projectDir, plan, code );
  if( path.isEmpty() )
    return {};
  const auto fail = [code]() {
    if( code )
      *code = QStringLiteral( "geostat_snapshot_unavailable" );
    return QByteArray();
  };
  QFile file( path );
  if( !file.open( QIODevice::ReadOnly ) || file.size() < 1 ||
      file.size() > kGeostatAtomMaxSnapshotBytes )
    return fail();
  const QByteArray bytes = file.read( kGeostatAtomMaxSnapshotBytes + 1 );
  if( bytes.size() != file.size() || sha256Hex( bytes ) != plan.snapshotSha256.toLatin1() )
    return fail();
  return bytes;
}

} // namespace

GeostatMaterializedSamples materializeGeostatSampleSnapshot(
    const QString &projectDir, const QString &sourcePath, const QString &versionId,
    const QString &versionSha256, const QString &factorId, const QString &field,
    const std::function<bool()> &cancelled )
{
  GeostatMaterializedSamples result;
  const auto fail = [&result]( const QString &code, const QString &message ) {
    result.ok = false;
    result.code = code;
    result.message = message;
    return result;
  };
  if( sourcePath.isEmpty() || versionId.isEmpty() || versionSha256.isEmpty() || factorId.isEmpty() ||
      field.isEmpty() || !regularFile( QFileInfo( sourcePath ) ) )
    return fail( QStringLiteral( "geostat_source_unavailable" ), QString() );

  const paleo::geostat::GeostatSampleReadResult read =
      paleo::geostat::readGeostatPointSamples( sourcePath, field, kGeostatAtomMaxSamples,
                                               cancelled );
  if( cancelled && cancelled() )
    return fail( QStringLiteral( "cancelled" ), QString() );
  if( !read.ok )
    return fail( read.code, read.message );

  const QString crsWktSha256 = QString::fromLatin1( sha256Hex( read.crsWkt.toUtf8() ) );
  QJsonArray rows;
  for( const auto &sample : read.samples )
    rows.append( QJsonArray{ sample.x, sample.y, sample.value } );
  const QJsonObject header{
    { "schema", QLatin1String( kSnapshotSchema ) }, { "version_id", versionId },
    { "source_sha256", versionSha256 },            { "factor_id", factorId },
    { "field", field },                            { "crs_wkt", read.crsWkt },
    { "crs_authid", read.crsAuthId },              { "crs_wkt_sha256", crsWktSha256 },
    { "sample_count", int( read.samples.size() ) }, { "skipped_rows", read.skippedRows },
    { "samples", rows } };
  const QByteArray bytes = QJsonDocument( header ).toJson( QJsonDocument::Compact );
  if( bytes.size() > kGeostatAtomMaxSnapshotBytes )
    return fail( QStringLiteral( "geostat_snapshot_budget_exceeded" ), QString() );
  // 内容寻址：目录名 = 文件 SHA-256；同版本同字段的快照天然去重。
  const QString sha = QString::fromLatin1( sha256Hex( bytes ) );
  const QString relative = geostatSnapshotRelativePath( sha );

  QString rootCode;
  const QString root = snapshotRoot( projectDir, true, &rootCode );
  if( root.isEmpty() )
    return fail( rootCode, QString() );
  const QString directory = root + QLatin1Char( '/' ) + sha;
  if( !QFileInfo::exists( directory + QStringLiteral( "/samples.json" ) ) )
  {
    // 同文件系统内的暂存目录 + 整目录 rename = 原子发布；并发同内容快照
    // 先到者胜，后到者转入下方的读回复核。
    QTemporaryDir temporary( root + QStringLiteral( "/.prepare-XXXXXX" ) );
    if( !temporary.isValid() )
      return fail( QStringLiteral( "geostat_snapshot_write_failed" ), QString() );
    QSaveFile file( temporary.path() + QStringLiteral( "/samples.json" ) );
    if( !file.open( QIODevice::WriteOnly ) || file.write( bytes ) != bytes.size() ||
        !file.commit() )
      return fail( QStringLiteral( "geostat_snapshot_write_failed" ), QString() );
    if( !QDir().rename( temporary.path(), directory ) &&
        !QFileInfo::exists( directory + QStringLiteral( "/samples.json" ) ) )
      return fail( QStringLiteral( "geostat_snapshot_write_failed" ), QString() );
  }
  if( cancelled && cancelled() )
    return fail( QStringLiteral( "cancelled" ), QString() );

  // 读回复核走与恢复完全相同的可信定位与摘要比对。
  GeostatSamplePlan probe;
  probe.snapshotPath = relative;
  probe.snapshotSha256 = sha;
  QString code;
  const QByteArray stored = readSnapshotBytes( projectDir, probe, &code );
  if( stored.isEmpty() )
    return fail( code, QString() );

  result.ok = true;
  result.samples = read.samples;
  result.snapshotPath = relative;
  result.snapshotSha256 = sha;
  result.sampleCount = static_cast<int>( read.samples.size() );
  result.skippedRows = read.skippedRows;
  result.crsWkt = read.crsWkt;
  result.crsAuthId = read.crsAuthId;
  result.crsWktSha256 = crsWktSha256;
  return result;
}

GeostatRestoredSamples restoreGeostatSampleSnapshot( const QString &projectDir,
                                                     const GeostatSamplePlan &plan )
{
  GeostatRestoredSamples result;
  const auto fail = [&result]( const QString &code ) {
    result.ok = false;
    result.code = code;
    return result;
  };
  QString code;
  const QByteArray bytes = readSnapshotBytes( projectDir, plan, &code );
  if( bytes.isEmpty() )
    return fail( code );
  // 快照头与冻结计划逐项对账：来源版本、因子、字段、计数、CRS 摘要。
  const QJsonObject root = QJsonDocument::fromJson( bytes ).object();
  const QJsonArray rows = root.value( "samples" ).toArray();
  if( root.value( "schema" ).toString() != QLatin1String( kSnapshotSchema ) ||
      root.value( "version_id" ).toString() != plan.versionId ||
      root.value( "source_sha256" ).toString() != plan.sourceSha256 ||
      root.value( "factor_id" ).toString() != plan.factorId ||
      root.value( "field" ).toString() != plan.field ||
      root.value( "crs_authid" ).toString() != plan.crsAuthId ||
      root.value( "crs_wkt_sha256" ).toString() != plan.crsWktSha256 ||
      root.value( "sample_count" ).toInt() != plan.sampleCount ||
      root.value( "skipped_rows" ).toInt() != plan.skippedRows || rows.size() != plan.sampleCount )
    return fail( QStringLiteral( "geostat_snapshot_unavailable" ) );
  const QString crsWkt = root.value( "crs_wkt" ).toString();
  if( crsWkt.isEmpty() || sha256Hex( crsWkt.toUtf8() ) != plan.crsWktSha256.toLatin1() )
    return fail( QStringLiteral( "geostat_snapshot_unavailable" ) );
  std::vector<paleo::geostat::Sample> samples;
  samples.reserve( static_cast<std::size_t>( rows.size() ) );
  for( const auto &value : rows )
  {
    const QJsonArray row = value.toArray();
    if( row.size() != 3 || !row.at( 0 ).isDouble() || !row.at( 1 ).isDouble() ||
        !row.at( 2 ).isDouble() )
      return fail( QStringLiteral( "geostat_snapshot_unavailable" ) );
    const double x = row.at( 0 ).toDouble();
    const double y = row.at( 1 ).toDouble();
    const double z = row.at( 2 ).toDouble();
    if( !std::isfinite( x ) || !std::isfinite( y ) || !std::isfinite( z ) )
      return fail( QStringLiteral( "geostat_snapshot_unavailable" ) );
    samples.push_back( paleo::geostat::Sample{ x, y, z } );
  }
  result.ok = true;
  result.samples = std::move( samples );
  result.crsWkt = crsWkt;
  result.crsAuthId = plan.crsAuthId;
  result.crsWktSha256 = plan.crsWktSha256;
  return result;
}

QString verifyGeostatSnapshotFile( const QString &projectDir, const GeostatSamplePlan &plan )
{
  QString code;
  readSnapshotBytes( projectDir, plan, &code );
  return code;
}

} // namespace paleo::geostat_atom
