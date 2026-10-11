// 层：数据（测试壳位于 tests/，被测对象为数据层 io 写面与读面 round-trip）
#include <QtTest/QtTest>

#include "io/sfpkgreader.h"
#include "io/sfpkgwriter.h"
#include "io/ziparchive.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cmath>
#include <vector>

using namespace paleo::io;

namespace
{

double surfaceValue( int row, int column )
{
  return 0.25 + 0.5 * std::sin( row * 0.31 + column * 0.17 ) - 0.1 * column;
}

QVector<ZipWriteEntry> sampleEntries()
{
  return { { QStringLiteral( "manifest.json" ), QByteArray( R"({"format":"sfpkg"})" ) },
           { QStringLiteral( "surface.npz" ), QByteArray( 4096, '\x7F' ) },
           { QStringLiteral( "notes/边车.txt" ), QByteArray( "中文边车内容" ) } };
}

QVariantMap demoManifest()
{
  QVariantMap manifest;
  manifest.insert( QStringLiteral( "format" ), QStringLiteral( "sfpkg" ) );
  manifest.insert( QStringLiteral( "version" ), QStringLiteral( "1.0" ) );
  manifest.insert( QStringLiteral( "factor_name" ), QStringLiteral( "砂地比" ) );
  manifest.insert( QStringLiteral( "horizon" ), QStringLiteral( "T2" ) );
  manifest.insert( QStringLiteral( "method" ), QStringLiteral( "local_direction_idw" ) );
  manifest.insert( QStringLiteral( "value_source" ), QStringLiteral( "analysis_grid" ) );
  manifest.insert( QStringLiteral( "crs_wkt" ), QStringLiteral( "EPSG:32650" ) );
  manifest.insert( QStringLiteral( "data_source" ), QStringLiteral( "写面测试" ) );
  manifest.insert( QStringLiteral( "value_min" ), 0.1 );
  manifest.insert( QStringLiteral( "value_max" ), 0.9 );
  manifest.insert( QStringLiteral( "method_params" ),
                   QVariantMap{ { QStringLiteral( "power" ), 2 } } );
  return manifest;
}

// 17×21 的 grid_z（含 NaN nodata）+ 坐标轴 + 掩码。
QVector<SfPackageArrayInput> demoArrays( int rows, int cols )
{
  QVector<SfPackageArrayInput> arrays;
  SfPackageArrayInput gridZ;
  gridZ.name = QStringLiteral( "grid_z" );
  gridZ.dtype = QStringLiteral( "<f8" );
  gridZ.rows = rows;
  gridZ.cols = cols;
  gridZ.values.resize( static_cast<std::size_t>( rows ) * cols );
  for ( int row = 0; row < rows; ++row )
    for ( int column = 0; column < cols; ++column )
      gridZ.values[static_cast<std::size_t>( row ) * cols + column] =
          ( row + column ) % 11 == 0 ? std::numeric_limits<double>::quiet_NaN()
                                     : surfaceValue( row, column );
  arrays.append( gridZ );

  SfPackageArrayInput gridX;
  gridX.name = QStringLiteral( "grid_x" );
  gridX.rows = cols;
  gridX.cols = 1;
  for ( int column = 0; column < cols; ++column )
    gridX.values.push_back( 100.0 + 20.0 * column );
  arrays.append( gridX );

  SfPackageArrayInput gridY;
  gridY.name = QStringLiteral( "grid_y" );
  gridY.rows = rows;
  gridY.cols = 1;
  for ( int row = 0; row < rows; ++row )
    gridY.values.push_back( 5000.0 - 20.0 * row );
  arrays.append( gridY );

  SfPackageArrayInput mask;
  mask.name = QStringLiteral( "valid_mask" );
  mask.dtype = QStringLiteral( "|b1" );
  mask.rows = rows;
  mask.cols = cols;
  mask.values.assign( static_cast<std::size_t>( rows ) * cols, 1.0 );
  for ( int row = 0; row < rows; ++row )
    for ( int column = 0; column < cols; ++column )
      if ( ( row + column ) % 11 == 0 )
        mask.values[static_cast<std::size_t>( row ) * cols + column] = 0.0;
  arrays.append( mask );
  return arrays;
}

} // namespace

class SfPackageWriteTests : public QObject
{
  Q_OBJECT
  private slots:
    void zipStoredRoundTrip();
    void zipDeflateRoundTrip();
    void zip64ForcedStructureRoundTrip();
    void zip64EntryCountOverflowRoundTrip();
    void zipRejectsDuplicateNames();
    void sfpkgRoundTrip();
    void sfpkgDeflateRoundTrip();
    void sfpkgBadChecksumRejected();
    void sfpkgWriterRefusals();
};

void SfPackageWriteTests::zipStoredRoundTrip()
{
  QString error;
  const QByteArray archive =
      zipBuildArchive( sampleEntries(), ZipBuildOptions{}, &error );
  QVERIFY2( !archive.isEmpty(), qPrintable( error ) );
  const ZipListResult listing = zipListBytes( archive );
  QVERIFY2( listing.ok, qPrintable( listing.error ) );
  QCOMPARE( listing.entries.size(), 3 );
  QCOMPARE( listing.entries.at( 0 ).method, 0 );
  QCOMPARE( listing.entries.at( 1 ).uncompressedSize, std::uint64_t( 4096 ) );
  QCOMPARE( listing.entries.at( 0 ).name, QStringLiteral( "manifest.json" ) );
  QByteArray out;
  QVERIFY( zipExtractBytes( archive, QStringLiteral( "notes/边车.txt" ), &out, &error ) );
  QCOMPARE( out, QByteArray( "中文边车内容" ) ); // 非 ASCII 名（bit 11）与内容逐位一致
}

void SfPackageWriteTests::zipDeflateRoundTrip()
{
  ZipBuildOptions options;
  options.compression = ZipCompression::Deflate;
  QString error;
  const QByteArray archive = zipBuildArchive( sampleEntries(), options, &error );
  QVERIFY2( !archive.isEmpty(), qPrintable( error ) );
  const ZipListResult listing = zipListBytes( archive );
  QVERIFY2( listing.ok, qPrintable( listing.error ) );
  QCOMPARE( listing.entries.at( 1 ).method, 8 );
  QVERIFY( listing.entries.at( 1 ).compressedSize < 4096 ); // 重复字节必压缩
  QByteArray out;
  QVERIFY( zipExtractBytes( archive, QStringLiteral( "surface.npz" ), &out, &error ) );
  QCOMPARE( out.size(), 4096 );
  QCOMPARE( out.at( 123 ), '\x7F' );
}

// 结构性 ZIP64（小包强制哨兵 + extra + EOCD64）：读面按 ZIP64 路径还原真值。
void SfPackageWriteTests::zip64ForcedStructureRoundTrip()
{
  ZipBuildOptions options;
  options.forceZip64 = true;
  bool zip64Used = false;
  QString error;
  const QByteArray archive = zipBuildArchive( sampleEntries(), options, &error, &zip64Used );
  QVERIFY2( !archive.isEmpty(), qPrintable( error ) );
  QVERIFY( zip64Used );
  const ZipListResult listing = zipListBytes( archive );
  QVERIFY2( listing.ok, qPrintable( listing.error ) );
  QCOMPARE( listing.entries.size(), 3 );
  QCOMPARE( listing.entries.at( 1 ).uncompressedSize, std::uint64_t( 4096 ) );
  // 第 0 条目（manifest.json，13 字节名 + 18 字节数据 + 20 字节 ZIP64 extra）全长：
  // 本地头 30 + 名 13 + extra 20 + 数据 18 = 81 → 第 1 条目偏移（64 位经 extra 还原）。
  QCOMPARE( listing.entries.at( 1 ).localHeaderOffset, std::uint64_t( 81 ) );
  QByteArray out;
  QVERIFY( zipExtractBytes( archive, QStringLiteral( "manifest.json" ), &out, &error ) );
  QCOMPARE( out, QByteArray( R"({"format":"sfpkg"})" ) );
}

// Oracle 3：条目数 > 65535 → 自动 ZIP64，round-trip 全量可列、抽查可解。
void SfPackageWriteTests::zip64EntryCountOverflowRoundTrip()
{
  QVector<ZipWriteEntry> entries;
  entries.reserve( 70000 );
  for ( int i = 0; i < 70000; ++i )
    entries.append( { QStringLiteral( "e/%1" ).arg( i ), QByteArray( "0123456789" ) } );
  // stored 起手（deflate×7 万次流初始化在本机 ~3 分钟，纯结构测试不值得）。
  ZipBuildOptions options;
  options.compression = ZipCompression::Stored;
  bool zip64Used = false;
  QString error;
  const QByteArray archive = zipBuildArchive( entries, options, &error, &zip64Used );
  QVERIFY2( !archive.isEmpty(), qPrintable( error ) );
  QVERIFY( zip64Used );
  const ZipListResult listing = zipListBytes( archive );
  QVERIFY2( listing.ok, qPrintable( listing.error ) );
  QCOMPARE( listing.entries.size(), 70000 );
  QCOMPARE( listing.entries.at( 69999 ).name, QStringLiteral( "e/69999" ) );
  QByteArray out;
  QVERIFY( zipExtractBytes( archive, QStringLiteral( "e/0" ), &out, &error ) );
  QCOMPARE( out, QByteArray( "0123456789" ) );
  QVERIFY( zipExtractBytes( archive, QStringLiteral( "e/69999" ), &out, &error ) );
  QCOMPARE( out, QByteArray( "0123456789" ) );
}

void SfPackageWriteTests::zipRejectsDuplicateNames()
{
  QString error;
  const QByteArray archive = zipBuildArchive(
      { { QStringLiteral( "a" ), QByteArray( "1" ) }, { QStringLiteral( "a" ), QByteArray( "2" ) } },
      ZipBuildOptions{}, &error );
  QVERIFY( archive.isEmpty() );
  QVERIFY( error.contains( QStringLiteral( "重名" ) ) );
}

// Oracle 3：写→读 round-trip，数值逐位一致（NaN 口径、dtype、manifest、边车、校验和）。
void SfPackageWriteTests::sfpkgRoundTrip()
{
  QTemporaryDir dir;
  const QString path = QDir( dir.path() ).filePath( QStringLiteral( "demo.sfpkg" ) );
  QMap<QString, QByteArray> sidecars;
  sidecars.insert( QStringLiteral( "shp/wells.dbf" ), QByteArray( "DBF-BYTES" ) );
  const SfPackageWriteResult written =
      writeSfPackage( path, demoManifest(), demoArrays( 17, 21 ), sidecars );
  QVERIFY2( written.ok, qPrintable( written.error ) );
  QVERIFY( written.bytesWritten > 0 );
  QVERIFY( !written.zip64Used );

  const SfPackageReadResult read = readSfPackage( path );
  QVERIFY2( read.ok, qPrintable( read.error ) );
  QVERIFY( read.checksumPresent );
  QCOMPARE( read.manifest.factorName, QStringLiteral( "砂地比" ) );
  QCOMPARE( read.manifest.horizon, QStringLiteral( "T2" ) );
  QCOMPARE( read.manifest.method, QStringLiteral( "local_direction_idw" ) );
  QCOMPARE( read.manifest.valueMin, 0.1 );
  QCOMPARE( read.manifest.valueMax, 0.9 );
  QCOMPARE( read.manifest.methodParams.value( QStringLiteral( "power" ) ).toInt(), 2 );
  QCOMPARE( read.manifest.gridShape.at( 0 ).toInt(), 17 );
  QCOMPARE( read.manifest.gridShape.at( 1 ).toInt(), 21 ); // 写面缺省补写 grid_shape
  QVERIFY( read.sidecars.contains( QStringLiteral( "shp/wells.dbf" ) ) );

  const SfPackageArray *gridZ = read.array( QStringLiteral( "grid_z" ) );
  QVERIFY( gridZ );
  QCOMPARE( gridZ->rows, 17 );
  QCOMPARE( gridZ->cols, 21 );
  QCOMPARE( gridZ->dtype, QStringLiteral( "<f8" ) );
  for ( int row = 0; row < 17; ++row )
    for ( int column = 0; column < 21; ++column )
    {
      const double value = gridZ->values[static_cast<std::size_t>( row ) * 21 + column];
      if ( ( row + column ) % 11 == 0 )
        QVERIFY( std::isnan( value ) );
      else
        QCOMPARE( value, surfaceValue( row, column ) ); // 逐位一致
    }
  const SfPackageArray *mask = read.array( QStringLiteral( "valid_mask" ) );
  QVERIFY( mask );
  QVERIFY( mask->integral );
  // 夹具口径：(row+column)%11==0 处为 0/False——[0][0] 是 0，[0][1] 是 1。
  QCOMPARE( mask->values[0], 0.0 );
  QCOMPARE( mask->values[1], 1.0 );
  QCOMPARE( mask->values[11], 0.0 );
  const SfPackageArray *gridX = read.array( QStringLiteral( "grid_x" ) );
  QVERIFY( gridX );
  QCOMPARE( gridX->cols, 1 ); // 一维 shape (n,)
  QCOMPARE( gridX->values.size(), std::size_t( 21 ) );
}

void SfPackageWriteTests::sfpkgDeflateRoundTrip()
{
  QTemporaryDir dir;
  const QString path = QDir( dir.path() ).filePath( QStringLiteral( "deflate.sfpkg" ) );
  const SfPackageWriteResult written =
      writeSfPackage( path, demoManifest(), demoArrays( 9, 13 ), {},
                      ZipCompression::Deflate );
  QVERIFY2( written.ok, qPrintable( written.error ) );
  const SfPackageReadResult read = readSfPackage( path );
  QVERIFY2( read.ok, qPrintable( read.error ) );
  const SfPackageArray *gridZ = read.array( QStringLiteral( "grid_z" ) );
  QVERIFY( gridZ );
  QCOMPARE( gridZ->values.size(), std::size_t( 9 * 13 ) );
  QCOMPARE( gridZ->values[1], surfaceValue( 0, 1 ) );
}

// Oracle 3：坏 checksum 拒收——stored 包里精确定位一个数据字节翻转。
void SfPackageWriteTests::sfpkgBadChecksumRejected()
{
  QTemporaryDir dir;
  const QString path = QDir( dir.path() ).filePath( QStringLiteral( "bad.sfpkg" ) );
  QVERIFY( writeSfPackage( path, demoManifest(), demoArrays( 5, 7 ) ).ok );
  QFile file( path );
  QVERIFY( file.open( QIODevice::ReadOnly ) );
  QByteArray bytes = file.readAll();
  file.close();
  // surfaceValue 的负号字节未必唯一；用 grid_z 数据段独有的魔数位置：翻转 npz
  // 本地头后的第一个数据字节（npz 本地头签名 0x04034b50 的位置固定可查）。
  const int npzLocal = bytes.indexOf( QByteArray::fromHex( "504b0304" ),
                                      bytes.indexOf( QByteArray::fromHex( "504b0304" ) ) + 4 );
  QVERIFY( npzLocal > 0 );
  const int firstDataByte = npzLocal + 30 + QStringLiteral( "surface.npz" ).size();
  bytes[firstDataByte] = bytes[firstDataByte] == '\x00' ? '\x01' : '\x00';
  QVERIFY( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
  file.write( bytes );
  file.close();
  const SfPackageReadResult read = readSfPackage( path );
  QVERIFY( !read.ok );
  QVERIFY( read.error.contains( QStringLiteral( "校验失败" ) ) );
}

// 写面诚实拒收：缺 grid_z / grid_shape 不符 / pickle / 整型越界 / 关键字段缺失。
void SfPackageWriteTests::sfpkgWriterRefusals()
{
  QTemporaryDir dir;
  const QString path = QDir( dir.path() ).filePath( QStringLiteral( "reject.sfpkg" ) );

  QVector<SfPackageArrayInput> arrays = demoArrays( 4, 5 );
  arrays.erase( arrays.begin() ); // 去掉 grid_z
  SfPackageWriteResult result = writeSfPackage( path, demoManifest(), arrays );
  QVERIFY( !result.ok );
  QVERIFY( result.error.contains( QStringLiteral( "grid_z" ) ) );

  QVariantMap badShape = demoManifest();
  badShape.insert( QStringLiteral( "grid_shape" ), QVariantList{ 3, 4 } );
  result = writeSfPackage( path, badShape, demoArrays( 4, 5 ) );
  QVERIFY( !result.ok );
  QVERIFY( result.error.contains( QStringLiteral( "grid_shape" ) ) );

  QVariantMap pickle = demoManifest();
  pickle.insert( QStringLiteral( "has_pickle" ), true );
  result = writeSfPackage( path, pickle, demoArrays( 4, 5 ) );
  QVERIFY( !result.ok );
  QVERIFY( result.error.contains( QStringLiteral( "pickle" ) ) );

  QVector<SfPackageArrayInput> overflow = demoArrays( 4, 5 );
  SfPackageArrayInput small;
  small.name = QStringLiteral( "region_ids" );
  small.dtype = QStringLiteral( "<u1" );
  small.rows = 2;
  small.cols = 1;
  small.values = { 0.0, 256.0 }; // u1 越界
  overflow.append( small );
  result = writeSfPackage( path, demoManifest(), overflow );
  QVERIFY( !result.ok );
  QVERIFY( result.error.contains( QStringLiteral( "region_ids" ) ) );

  // 有符号 dtype 越界：旧实现上界误用无符号最大值（i4 放行 3e9，落盘回绕成
  // 负数），读面按补码解释——校验通过但往返不一致。
  QVector<SfPackageArrayInput> signedOverflow = demoArrays( 4, 5 );
  SfPackageArrayInput regions;
  regions.name = QStringLiteral( "region_ids" );
  regions.dtype = QStringLiteral( "<i4" );
  regions.rows = 2;
  regions.cols = 1;
  regions.values = { 0.0, 3000000000.0 };
  signedOverflow.append( regions );
  result = writeSfPackage( path, demoManifest(), signedOverflow );
  QVERIFY( !result.ok );
  QVERIFY( result.error.contains( QStringLiteral( "region_ids" ) ) );

  QVariantMap noFactor = demoManifest();
  noFactor.remove( QStringLiteral( "factor_name" ) );
  result = writeSfPackage( path, noFactor, demoArrays( 4, 5 ) );
  QVERIFY( !result.ok );
  QVERIFY( result.error.contains( QStringLiteral( "factor_name" ) ) );

  // 非致命：去掉 grid_x/grid_y/valid_mask → issues 记录但包照写、读面可读。
  QVector<SfPackageArrayInput> onlyZ = demoArrays( 4, 5 );
  onlyZ.erase( onlyZ.begin() + 1, onlyZ.end() );
  result = writeSfPackage( path, demoManifest(), onlyZ );
  QVERIFY2( result.ok, qPrintable( result.error ) );
  QCOMPARE( result.issues.size(), 3 );
  const SfPackageReadResult read = readSfPackage( path );
  QVERIFY2( read.ok, qPrintable( read.error ) );
  QVERIFY( read.issues.size() >= 2 );
}

QTEST_MAIN( SfPackageWriteTests )
#include "tst_io_sfpkg_write.moc"
