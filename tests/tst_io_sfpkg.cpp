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
  QVERIFY( result.array( QStringLiteral( "source_trend_grid" ) ) != nullptr );

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

QTEST_MAIN( SfPackageReaderTests )
#include "tst_io_sfpkg.moc"
