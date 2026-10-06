// 层：数据（测试壳位于 tests/，被测对象为数据层读取面）
#include <QtTest/QtTest>

#include "io/sfpkgreader.h"

#include <QDir>
#include <QFileInfo>

#include <cmath>

using namespace paleo::io;

namespace
{

QString fixturePath( const QString &name )
{
  const QString fromSource = QFileInfo( QString::fromUtf8( __FILE__ ) )
                                 .dir()
                                 .filePath( QStringLiteral( "fixtures/sfpkg/" ) + name );
  if ( QFileInfo::exists( fromSource ) )
    return fromSource;
  return QStringLiteral( "tests/fixtures/sfpkg/" ) + name;
}

double expectedGridValue( int row, int column )
{
  return 0.4 + 0.1 * std::sin( row * 0.7 + column * 0.3 );
}

} // namespace

class SfPackageReaderTests : public QObject
{
  Q_OBJECT
  private slots:
    void readsCompleteFields();
    void readsAllArrays();
    void rejectsChecksumMismatch();
    void rejectsPickle();
    void rejectsMissingGridZ();
    void rejectsEmptyChecksum();
    void toleratesMissingChecksumWithIssue();
    void transposesFortranOrderArray();
    void parsesNpyDirectly();
    void reportsMissingFile();
};

// Oracle 4：完整字段读取 —— manifest 逐个字段 + NPZ 数组逐个到位。
void SfPackageReaderTests::readsCompleteFields()
{
  const SfPackageReadResult result = readSfPackage( fixturePath( QStringLiteral( "demo.sfpkg" ) ) );
  QVERIFY2( result.ok, qPrintable( result.error ) );
  QVERIFY( result.error.isEmpty() );
  QCOMPARE( result.manifest.format, QStringLiteral( "sfpkg" ) );
  QCOMPARE( result.manifest.version, QStringLiteral( "1.0" ) );
  QCOMPARE( result.manifest.factorName, QStringLiteral( "砂地比" ) );
  QCOMPARE( result.manifest.horizon, QStringLiteral( "T1" ) );
  QCOMPARE( result.manifest.method, QStringLiteral( "idw" ) );
  QCOMPARE( result.manifest.valueSource, QStringLiteral( "analysis_grid" ) );
  QCOMPARE( result.manifest.barrierValuePolicy, QStringLiteral( "preserve" ) );
  QCOMPARE( result.manifest.analysisValuePolicy, QStringLiteral( "original_interpolation" ) );
  QCOMPARE( result.manifest.interpolationModel, QStringLiteral( "local_direction_idw" ) );
  QCOMPARE( result.manifest.partitionVersion, QStringLiteral( "17" ) );
  QVERIFY( result.manifest.partitionComplete );
  QVERIFY( !result.manifest.hasPickle );
  QCOMPARE( result.manifest.barrierBufferDistance, 30.0 );
  QCOMPARE( result.manifest.contourStopBufferDistance, 12.0 );
  QCOMPARE( result.manifest.methodParams.value( QStringLiteral( "power" ) ).toInt(), 2 );
  QCOMPARE( result.manifest.levels.size(), 3 );
  QCOMPARE( result.manifest.barriers.size(), 1 );
  QCOMPARE( result.manifest.directions.size(), 1 );
  QCOMPARE( result.manifest.partitionExtensions.size(), 1 );
  QCOMPARE( result.manifest.gridShape.size(), 2 );
  QCOMPARE( result.manifest.gridShape.at( 0 ).toInt(), 17 );
  QCOMPARE( result.manifest.gridShape.at( 1 ).toInt(), 21 );
  QCOMPARE( result.manifest.globalAnisotropyRatio, 1.0 );
  QCOMPARE( result.manifest.globalAnisotropyAngle, 0.0 );
  QCOMPARE( result.manifest.dataSource, QStringLiteral( "外委夹具" ) );
  QCOMPARE( result.manifest.coverageInfo.value( QStringLiteral( "mode" ) ).toString(),
            QStringLiteral( "well_supported" ) );
  QCOMPARE( result.manifest.boundaries.size(), 1 );
  QCOMPARE( result.manifest.partitionBarriers.size(), 1 );
  QCOMPARE( result.manifest.fieldModel.value( QStringLiteral( "surface_kind" ) ).toString(),
            QStringLiteral( "analysis" ) );
  QCOMPARE( result.manifest.createdAt, QStringLiteral( "2026-10-04T00:00:00Z" ) );
  QCOMPARE( result.manifest.contourPartition.value( QStringLiteral( "geometry_policy" ) ).toString(),
            QStringLiteral( "local_interpretive_detour" ) );
  QVERIFY( result.manifest.crsWkt.startsWith( QStringLiteral( "PROJCS" ) ) );
  // 「完整字段」不靠枚举穷举：未单列的键在 raw 里原样保留。
  QVERIFY( result.manifest.raw.contains( QStringLiteral( "barrier_shape_parameters" ) ) );
  QVERIFY( result.manifest.raw.contains( QStringLiteral( "coverage_info" ) ) );
  QVERIFY( result.checksumPresent );
  QVERIFY( result.sidecars.isEmpty() );
}

// 包内每个数组都要读到，形状/类型/数值逐条核对（NaN nodata 保留）。
void SfPackageReaderTests::readsAllArrays()
{
  const SfPackageReadResult result = readSfPackage( fixturePath( QStringLiteral( "demo.sfpkg" ) ) );
  QVERIFY2( result.ok, qPrintable( result.error ) );
  QCOMPARE( result.arrays.size(), 9 );

  const SfPackageArray *grid = result.array( QStringLiteral( "grid_z" ) );
  QVERIFY( grid != nullptr );
  QCOMPARE( grid->rows, 17 );
  QCOMPARE( grid->cols, 21 );
  QCOMPARE( grid->dtype, QStringLiteral( "<f8" ) );
  QVERIFY( !grid->integral );
  QVERIFY( std::isnan( grid->values[0] ) );
  QVERIFY( std::fabs( grid->values[22] - expectedGridValue( 1, 1 ) ) <= 1e-12 );
  QVERIFY( std::fabs( grid->values[16 * 21 + 20] - expectedGridValue( 16, 20 ) ) <= 1e-12 );

  const SfPackageArray *gridX = result.array( QStringLiteral( "grid_x" ) );
  QVERIFY( gridX != nullptr );
  QCOMPARE( gridX->rows, 21 );
  QCOMPARE( gridX->cols, 1 );
  QCOMPARE( gridX->values.front(), 100.0 );
  QCOMPARE( gridX->values.back(), 200.0 );
  const SfPackageArray *gridY = result.array( QStringLiteral( "grid_y" ) );
  QVERIFY( gridY != nullptr );
  QCOMPARE( gridY->values.front(), 10.0 );
  QCOMPARE( gridY->values.back(), 90.0 );

  const SfPackageArray *valid = result.array( QStringLiteral( "valid_mask" ) );
  QVERIFY( valid != nullptr );
  QVERIFY( valid->integral );
  QCOMPARE( valid->values[0], 0.0 );
  QCOMPARE( valid->values[1], 1.0 );
  const SfPackageArray *boundary = result.array( QStringLiteral( "boundary_mask" ) );
  QVERIFY( boundary != nullptr );
  for ( int column = 0; column < 21; ++column )
    QCOMPARE( boundary->values[16 * 21 + column], 0.0 );

  QVERIFY( result.array( QStringLiteral( "coverage_status" ) ) != nullptr );
  const SfPackageArray *regions = result.array( QStringLiteral( "region_ids" ) );
  QVERIFY( regions != nullptr );
  QCOMPARE( regions->dtype, QStringLiteral( "<i4" ) );
  const SfPackageArray *wellRegions = result.array( QStringLiteral( "well_region_ids" ) );
  QVERIFY( wellRegions != nullptr );
  QCOMPARE( wellRegions->rows, 3 );
  const SfPackageArray *trend = result.array( QStringLiteral( "source_trend_grid" ) );
  QVERIFY( trend != nullptr );
  QVERIFY( trend->fortranOrder );

  // 读取面不粉饰：缺 checksum 或全有限 grid 都要列因，测试夹具本身合规。
  for ( const QString &issue : result.issues )
  {
    QVERIFY2( !issue.contains( QStringLiteral( "校验和" ) ), qPrintable( issue ) );
    QVERIFY2( !issue.contains( QStringLiteral( "缺少" ) ), qPrintable( issue ) );
  }
}

// Oracle 2（诚实面）：包内字节被改写 → 校验必须失败，不得照常返回数据。
void SfPackageReaderTests::rejectsChecksumMismatch()
{
  const SfPackageReadResult result = readSfPackage( fixturePath( QStringLiteral( "bad_checksum.sfpkg" ) ) );
  QVERIFY( !result.ok );
  QVERIFY2( result.error.contains( QStringLiteral( "SHA256 不匹配" ) ), qPrintable( result.error ) );
  QVERIFY( result.array( QStringLiteral( "grid_z" ) ) == nullptr );
}

void SfPackageReaderTests::rejectsPickle()
{
  const SfPackageReadResult result = readSfPackage( fixturePath( QStringLiteral( "pickle_forbidden.sfpkg" ) ) );
  QVERIFY( !result.ok );
  QVERIFY2( result.error.contains( QStringLiteral( "禁止 pickle" ) ), qPrintable( result.error ) );
}

void SfPackageReaderTests::rejectsMissingGridZ()
{
  const SfPackageReadResult result = readSfPackage( fixturePath( QStringLiteral( "missing_grid_z.sfpkg" ) ) );
  QVERIFY( !result.ok );
  QVERIFY2( result.error.contains( QStringLiteral( "缺少 grid_z" ) ), qPrintable( result.error ) );
}

void SfPackageReaderTests::reportsMissingFile()
{
  const SfPackageReadResult result = readSfPackage( fixturePath( QStringLiteral( "no_such_file.sfpkg" ) ) );
  QVERIFY( !result.ok );
  QVERIFY( result.error.contains( QStringLiteral( "文件不存在" ) ) );
}

// Oracle 2（诚实面）：checksum.json 存在但没有任何 sha256 字段 → 拒绝读取，
// 不允许「有校验文件却什么都没校验」。
void SfPackageReaderTests::rejectsEmptyChecksum()
{
  const SfPackageReadResult result = readSfPackage( fixturePath( QStringLiteral( "empty_checksum.sfpkg" ) ) );
  QVERIFY( !result.ok );
  QVERIFY2( result.error.contains( QStringLiteral( "没有任何 sha256 字段" ) ), qPrintable( result.error ) );
}

// 包内没有 checksum.json：上游容忍（读面照常返回数据），但必须留 issue。
void SfPackageReaderTests::toleratesMissingChecksumWithIssue()
{
  const SfPackageReadResult result = readSfPackage( fixturePath( QStringLiteral( "no_checksum.sfpkg" ) ) );
  QVERIFY2( result.ok, qPrintable( result.error ) );
  QVERIFY( !result.checksumPresent );
  QVERIFY( std::any_of( result.issues.cbegin(), result.issues.cend(), []( const QString &issue ) {
    return issue.contains( QStringLiteral( "没有 checksum.json" ) );
  } ) );
}

// 列优先（fortran_order=True）数组必须转置回行主序：夹具值 = (i+1)*100 + (j+1)。
void SfPackageReaderTests::transposesFortranOrderArray()
{
  const SfPackageReadResult result = readSfPackage( fixturePath( QStringLiteral( "demo.sfpkg" ) ) );
  QVERIFY2( result.ok, qPrintable( result.error ) );
  const SfPackageArray *trend = result.array( QStringLiteral( "source_trend_grid" ) );
  QVERIFY( trend != nullptr );
  QCOMPARE( trend->rows, 17 );
  QCOMPARE( trend->cols, 21 );
  // 转置错了会得到 (j+1)*100 + (i+1)：用非对称位置 (i=1,j=2) 断言。
  QCOMPARE( trend->values[1 * 21 + 2], 203.0 );
  QCOMPARE( trend->values[2 * 21 + 1], 302.0 );
  QCOMPARE( trend->values[0], 101.0 );
}

// NPY 单文件入口（含 v1 头、fortran_order、整数 dtype）直接单测。
void SfPackageReaderTests::parsesNpyDirectly()
{
  // 手工构造 2×3 列优先 NPY：值 = col*10 + row（行主序读回 = [0,10,20,1,11,21]）。
  const QByteArray header =
      QStringLiteral( "{'descr': '<i4', 'fortran_order': True, 'shape': (2, 3), }\n" ).toUtf8();
  QByteArray bytes;
  bytes.append( "\x93NUMPY", 6 );
  bytes.append( char( 1 ) );
  bytes.append( char( 0 ) );
  bytes.append( char( header.size() & 0xFF ) );
  bytes.append( char( ( header.size() >> 8 ) & 0xFF ) );
  bytes.append( header );
  for ( int column = 0; column < 3; ++column )
  {
    for ( int row = 0; row < 2; ++row )
    {
      const qint32 value = column * 10 + row;
      bytes.append( char( value & 0xFF ) );
      bytes.append( char( ( value >> 8 ) & 0xFF ) );
      bytes.append( char( ( value >> 16 ) & 0xFF ) );
      bytes.append( char( ( value >> 24 ) & 0xFF ) );
    }
  }
  const NpyArray parsed = parseNpy( bytes );
  QVERIFY2( parsed.ok, qPrintable( parsed.error ) );
  QCOMPARE( parsed.descr, QStringLiteral( "<i4" ) );
  QVERIFY( parsed.fortranOrder );
  QVERIFY( parsed.integral );
  QCOMPARE( parsed.rows, 2 );
  QCOMPARE( parsed.cols, 3 );
  QCOMPARE( parsed.values.size(), std::size_t{ 6 } );
  QCOMPARE( parsed.values[0], 0.0 );
  QCOMPARE( parsed.values[1], 10.0 );
  QCOMPARE( parsed.values[2], 20.0 );
  QCOMPARE( parsed.values[3], 1.0 );
  QCOMPARE( parsed.values[5], 21.0 );

  // 0 维标量（shape=()）与截断数据都要如实报因。
  const QByteArray scalarHeader = QStringLiteral( "{'descr': '<f8', 'fortran_order': False, 'shape': (), }\n" ).toUtf8();
  QByteArray scalar;
  scalar.append( "\x93NUMPY", 6 );
  scalar.append( char( 1 ) );
  scalar.append( char( 0 ) );
  scalar.append( char( scalarHeader.size() & 0xFF ) );
  scalar.append( char( ( scalarHeader.size() >> 8 ) & 0xFF ) );
  scalar.append( scalarHeader );
  scalar.append( QByteArray( 8, '\0' ) );
  const NpyArray scalarParsed = parseNpy( scalar );
  QVERIFY( !scalarParsed.ok );
  QVERIFY2( scalarParsed.error.contains( QStringLiteral( "0 维" ) ), qPrintable( scalarParsed.error ) );
  QVERIFY( !parseNpy( QByteArray( "not an npy" ) ).ok );

  // #216：shape 声明巨大（2^30 × (2^31-1)，count*8 > 2^63）时 count*width 有符号
  // 溢出曾回绕成负值击穿长度闸 → 巨量分配崩溃。必须走报错路径。
  const QByteArray hugeHeader =
      QStringLiteral( "{'descr': '<f8', 'fortran_order': False, 'shape': (1073741824, 2147483647), }\n" ).toUtf8();
  QByteArray huge;
  huge.append( "\x93NUMPY", 6 );
  huge.append( char( 1 ) );
  huge.append( char( 0 ) );
  huge.append( char( hugeHeader.size() & 0xFF ) );
  huge.append( char( ( hugeHeader.size() >> 8 ) & 0xFF ) );
  huge.append( hugeHeader );
  huge.append( QByteArray( 64, '\0' ) );
  const NpyArray hugeParsed = parseNpy( huge );
  QVERIFY( !hugeParsed.ok );
  QVERIFY( hugeParsed.values.empty() );
  QVERIFY2( hugeParsed.error.contains( QStringLiteral( "长度与 shape 不符" ) ), qPrintable( hugeParsed.error ) );
}

QTEST_MAIN( SfPackageReaderTests )
#include "tst_io_sfpkg.moc"
