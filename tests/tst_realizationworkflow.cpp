// 层：测试壳（realization 集合编排：成员收编/统计派生/两集合差值/provenance/诚实面）
#include <QtTest>
#include <QTemporaryDir>
#include <QSignalSpy>

#include "../src/catalog/datacatalog.h"
#include "../src/catalog/realizationset.h"
#include "../src/workflow/realizationworkflow.h"
#include "../src/algorithms/ensemblestats.h"
#include "../src/services/paleotaskservice.h"

#include <gdal.h>
#include <cmath>
#include <limits>

// 方向 47 Oracle：provenance（统计面 parents 覆盖全体成员）+ 派生数值与
// 直算一致 + 缺成员/单成员/零成员诚实面 + 重开存活。统计口径词表见
// catalog/realizationset.h——token 落盘、label 只进展示层。

namespace
{
const double kNaN = std::numeric_limits<double>::quiet_NaN();

// 测试成员栅格：Float32 + nodata -9999，几何固定（成员同网格是契约前置）。
bool writeMember( const QString &path, const std::vector<double> &values,
                  int cols, int rows )
{
  GDALAllRegister();
  GDALDriverH drv = GDALGetDriverByName( "GTiff" );
  GDALDatasetH ds = GDALCreate( drv, path.toUtf8().constData(), cols, rows, 1,
                                GDT_Float32, nullptr );
  if ( !ds )
    return false;
  const double gt[6] = { 0, 1, 0, 0, 0, -1 };
  GDALSetGeoTransform( ds, gt );
  GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
  GDALSetRasterNoDataValue( band, -9999.0 );
  std::vector<float> buf( values.size() );
  for ( std::size_t i = 0; i < values.size(); ++i )
    buf[i] = std::isfinite( values[i] ) ? static_cast<float>( values[i] ) : -9999.0f;
  const CPLErr err = GDALRasterIO( band, GF_Write, 0, 0, cols, rows, buf.data(),
                                   cols, rows, GDT_Float32, 0, 0 );
  GDALClose( ds );
  return err == CE_None;
}

std::vector<double> readRaster( const QString &path, int *cols = nullptr,
                                int *rows = nullptr )
{
  GDALAllRegister();
  GDALDatasetH ds = GDALOpen( path.toUtf8().constData(), GA_ReadOnly );
  if ( !ds )
    return {};
  const int c = GDALGetRasterXSize( ds ), r = GDALGetRasterYSize( ds );
  if ( cols )
    *cols = c;
  if ( rows )
    *rows = r;
  std::vector<double> out( static_cast<std::size_t>( c ) * r );
  GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
  int hasNodata = 0;
  const double nodata = GDALGetRasterNoDataValue( band, &hasNodata );
  GDALRasterIO( band, GF_Read, 0, 0, c, r, out.data(), c, r, GDT_Float64, 0, 0 );
  GDALClose( ds );
  for ( double &v : out )
    if ( hasNodata && v == nodata )
      v = kNaN;
  return out;
}

// 目录下写 members 个成员栅格（member k 的像元 i = base + k×1.0 + sin(i×k) 可
// 复算扰动），返回按 index 序的 MemberInput。
QVector<RealizationWorkflow::MemberInput> makeMembers( const QString &dir,
                                                       int members, int cols, int rows,
                                                       quint64 seed,
                                                       double base = 100.0,
                                                       std::vector<std::vector<double>> *fields = nullptr )
{
  QVector<RealizationWorkflow::MemberInput> out;
  const std::size_t cells = static_cast<std::size_t>( cols ) * rows;
  for ( int k = 0; k < members; ++k )
  {
    std::vector<double> field( cells );
    for ( std::size_t i = 0; i < cells; ++i )
      field[i] = base + k + 0.01 * std::sin( static_cast<double>( i ) * ( k + 1 ) );
    if ( fields )
      fields->push_back( field );
    const QString path = QDir( dir ).filePath(
        QStringLiteral( "m%1.tif" ).arg( k ) );
    if ( writeMember( path, field, cols, rows ) )
      out.push_back( { path, seed } );
  }
  return out;
}
} // namespace

class RealizationWorkflowTests : public QObject
{
  Q_OBJECT

  private slots:
    void publishRoundTripsContract();
    void emptySetRejected();
    void singleMemberDeriveRefused();
    void deriveStatisticsPixelTruthAndProvenance();
    void reDeriveIsIdempotent();
    void corruptMemberFileHonest();
    void gridMismatchRefused();
    void diffOfMeansMatchesDirect();
    void frameOrderAndSignals();
    void restartSurvival();
    void deriveAllStatisticsAsyncWithTaskService();
};

void RealizationWorkflowTests::publishRoundTripsContract()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  DataCatalog cat;
  QVERIFY( cat.open( dir.path() ) );

  RealizationWorkflow wf;
  wf.bind( &cat, dir.path(), nullptr );
  const auto members = makeMembers( dir.path(), 4, 8, 6, 777 );
  QString err;
  const QString setId = wf.publishSet( QStringLiteral( "测试集合" ),
                                       QStringLiteral( "D61" ), members,
                                       { QStringLiteral( "ver-parent-1" ) },
                                       QVariantMap{ { QStringLiteral( "method" ),
                                                      QStringLiteral( "sgs" ) } },
                                       &err );
  QVERIFY2( !setId.isEmpty(), qPrintable( err ) );

  const paleo::realization::RealizationSet set =
      paleo::realization::setById( cat, setId );
  QCOMPARE( set.members.size(), 4 );
  QCOMPARE( set.declaredCount, 4 );
  QVERIFY( set.complete() );
  QVERIFY( set.missingIndices.isEmpty() );
  for ( int i = 0; i < 4; ++i )
  {
    QCOMPARE( set.members[i].index, i ); // 成员序 = index 升序
    QCOMPARE( set.members[i].seed, quint64( 777 ) );
    const CatalogVersion v = cat.versionById( set.members[i].versionId );
    QCOMPARE( v.stage, QStringLiteral( "DERIVED" ) );
    QCOMPARE( v.assetId, setId );
    QCOMPARE( v.versionNumber, i + 1 ); // 分配序 = index+1（契约钉死）
    QCOMPARE( v.extra.value( paleo::realization::kKeySetId ).toString(), setId );
    QCOMPARE( v.extra.value( paleo::realization::kKeyIndex ).toInt(), i );
    QCOMPARE( v.extra.value( paleo::realization::kKeyMemberCount ).toInt(), 4 );
    QCOMPARE( v.parentVersionIds, QStringList{ QStringLiteral( "ver-parent-1" ) } );
    // 成员栅格经受管路径 round-trip 可读。
    QVERIFY( QFile::exists( wf.memberRasterPath( setId, i ) ) );
  }
  QCOMPARE( paleo::realization::enumerateSets( cat ).size(), 1 );
}

void RealizationWorkflowTests::emptySetRejected()
{
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY( cat.open( dir.path() ) );
  RealizationWorkflow wf;
  wf.bind( &cat, dir.path(), nullptr );
  QString err;
  QVERIFY( wf.publishSet( QStringLiteral( "空集合" ), QStringLiteral( "D61" ), {},
                          {}, {}, &err ).isEmpty() );
  QVERIFY( !err.isEmpty() );
  QVERIFY( paleo::realization::enumerateSets( cat ).isEmpty() );
}

void RealizationWorkflowTests::singleMemberDeriveRefused()
{
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY( cat.open( dir.path() ) );
  RealizationWorkflow wf;
  wf.bind( &cat, dir.path(), nullptr );
  const auto members = makeMembers( dir.path(), 1, 4, 4, 1 );
  QString err;
  const QString setId = wf.publishSet( QStringLiteral( "单成员" ),
                                       QStringLiteral( "D61" ), members, {}, {},
                                       &err );
  QVERIFY( !setId.isEmpty() );
  paleo::ensemble::StatsRequest want;
  want.mean = want.stddev = true;
  QVERIFY2( !wf.deriveStatistics( setId, want, &err ), "N=1 必须拒绝派生" );
  QVERIFY2( err.contains( QStringLiteral( "单成员" ) ), qPrintable( err ) );
  QVERIFY( paleo::realization::statSurfaces( cat, setId ).isEmpty() );
}

void RealizationWorkflowTests::deriveStatisticsPixelTruthAndProvenance()
{
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY( cat.open( dir.path() ) );
  RealizationWorkflow wf;
  wf.bind( &cat, dir.path(), nullptr );

  std::vector<std::vector<double>> fields;
  const auto members = makeMembers( dir.path(), 6, 12, 9, 42, 50.0, &fields );
  QString err;
  const QString setId = wf.publishSet( QStringLiteral( "统计集合" ),
                                       QStringLiteral( "D61" ), members, {}, {},
                                       &err );
  QVERIFY( !setId.isEmpty() );

  paleo::ensemble::StatsRequest want;
  want.mean = want.stddev = want.p10 = want.p90 = true;
  QVERIFY2( wf.deriveStatistics( setId, want, &err ), qPrintable( err ) );

  // 4 个统计面各落独立 realization_stat 版本；parents 覆盖全部成员。
  const auto stats = paleo::realization::statSurfaces( cat, setId );
  QCOMPARE( stats.size(), 4 );
  QStringList memberIds;
  const auto set = paleo::realization::setById( cat, setId );
  for ( const auto &m : set.members )
    memberIds << m.versionId;
  memberIds.sort();

  // 真值：统计核整幅口径（与派生的流式/带式路径同核——两边都验数值）。
  std::vector<const double *> ptrs;
  for ( const auto &f : fields )
    ptrs.push_back( f.data() );
  const paleo::ensemble::StatsResult truth =
      paleo::ensemble::compute( ptrs, 12 * 9, want );

  for ( const paleo::realization::StatisticSurface &s : stats )
  {
    const CatalogVersion v = cat.versionById( s.versionId );
    QStringList parents = v.parentVersionIds;
    parents.sort();
    QCOMPARE( parents, memberIds ); // Oracle 5：parents 锚全体在场成员
    QCOMPARE( v.extra.value( paleo::realization::kKeyMemberCount ).toInt(), 6 );
    QCOMPARE( v.assetId.isEmpty(), false );
    const QString abs = DataCatalog::resolvedVersionPath( dir.path(), v );
    const std::vector<double> got = readRaster( abs );
    QCOMPARE( got.size(), truth.mean.size() );
    const std::vector<double> *expected = nullptr;
    if ( s.token == paleo::realization::kStatMean )
      expected = &truth.mean;
    else if ( s.token == paleo::realization::kStatStdDev )
      expected = &truth.stddev;
    else if ( s.token == paleo::realization::kStatP10 )
      expected = &truth.p10;
    else if ( s.token == paleo::realization::kStatP90 )
      expected = &truth.p90;
    QVERIFY2( expected, qPrintable( s.token ) );
    for ( std::size_t i = 0; i < got.size(); ++i )
      QVERIFY2( std::fabs( got[i] - ( *expected )[i] ) < 1e-4,
                qPrintable( QStringLiteral( "%1[%2]: got %3 want %4" )
                                .arg( s.token ).arg( i ).arg( got[i] )
                                .arg( ( *expected )[i] ) ) );
  }
}

void RealizationWorkflowTests::reDeriveIsIdempotent()
{
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY( cat.open( dir.path() ) );
  RealizationWorkflow wf;
  wf.bind( &cat, dir.path(), nullptr );
  const auto members = makeMembers( dir.path(), 3, 8, 8, 7 );
  QString err;
  const QString setId = wf.publishSet( QStringLiteral( "幂等" ),
                                       QStringLiteral( "D61" ), members, {}, {}, &err );
  paleo::ensemble::StatsRequest want;
  want.mean = true;
  QVERIFY( wf.deriveStatistics( setId, want, &err ) );
  QVERIFY( wf.deriveStatistics( setId, want, &err ) ); // 第二遍幂等跳过重派生
  QCOMPARE( paleo::realization::statSurfaces( cat, setId ).size(), 1 );
}

void RealizationWorkflowTests::corruptMemberFileHonest()
{
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY( cat.open( dir.path() ) );
  RealizationWorkflow wf;
  wf.bind( &cat, dir.path(), nullptr );
  const auto members = makeMembers( dir.path(), 3, 8, 8, 7 );
  QString err;
  const QString setId = wf.publishSet( QStringLiteral( "缺文件" ),
                                       QStringLiteral( "D61" ), members, {}, {}, &err );
  QVERIFY( !setId.isEmpty() );
  // 删掉成员 1 的受管文件：catalog 行还在但字节缺席——派生必须点名拒绝，
  // 不是静默按两成员算。
  const QString victim = wf.memberRasterPath( setId, 1 );
  QVERIFY( QFile::exists( victim ) );
  QVERIFY( QFile::setPermissions( victim, QFileDevice::WriteOwner ) );
  QVERIFY( QFile::remove( victim ) );
  paleo::ensemble::StatsRequest want;
  want.mean = true;
  QVERIFY( !wf.deriveStatistics( setId, want, &err ) );
  QVERIFY2( err.contains( QStringLiteral( "成员 1" ) ), qPrintable( err ) );
}

void RealizationWorkflowTests::gridMismatchRefused()
{
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY( cat.open( dir.path() ) );
  RealizationWorkflow wf;
  wf.bind( &cat, dir.path(), nullptr );
  // 混入异网格成员——集合契约要求同网格，派生如实拒绝（不产错位假面）。
  QVector<RealizationWorkflow::MemberInput> members =
      makeMembers( dir.path(), 2, 8, 8, 7 );
  writeMember( QDir( dir.path() ).filePath( QStringLiteral( "odd.tif" ) ),
               std::vector<double>( 16 * 16, 1.0 ), 16, 16 );
  members.push_back( { QDir( dir.path() ).filePath( QStringLiteral( "odd.tif" ) ), 7 } );
  QString err;
  const QString setId = wf.publishSet( QStringLiteral( "异网格" ),
                                       QStringLiteral( "D61" ), members, {}, {}, &err );
  QVERIFY( !setId.isEmpty() );
  paleo::ensemble::StatsRequest want;
  want.mean = true;
  QVERIFY( !wf.deriveStatistics( setId, want, &err ) );
  QVERIFY2( err.contains( QStringLiteral( "网格" ) ), qPrintable( err ) );
}

void RealizationWorkflowTests::diffOfMeansMatchesDirect()
{
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY( cat.open( dir.path() ) );
  RealizationWorkflow wf;
  wf.bind( &cat, dir.path(), nullptr );

  // 集合 A：成员均值面 = 10 + 扰动；集合 B：= 4 + 不同扰动。
  QDir().mkpath( dir.path() + QStringLiteral( "/a" ) );
  QDir().mkpath( dir.path() + QStringLiteral( "/b" ) );
  const auto membersA = makeMembers( dir.path() + QStringLiteral( "/a" ), 3, 10, 8,
                                     11, 10.0 );
  const auto membersB = makeMembers( dir.path() + QStringLiteral( "/b" ), 4, 10, 8,
                                     22, 4.0 );
  QString err;
  const QString setA = wf.publishSet( QStringLiteral( "集合A" ),
                                      QStringLiteral( "D61" ), membersA, {}, {}, &err );
  const QString setB = wf.publishSet( QStringLiteral( "集合B" ),
                                      QStringLiteral( "D61" ), membersB, {}, {}, &err );
  QVERIFY( !setA.isEmpty() && !setB.isEmpty() );
  QVERIFY( setA != setB );

  QVERIFY2( wf.differenceOfMeans( setA, setB, &err ), qPrintable( err ) );

  // 差值面 = A 均值 − B 均值，逐像元与直算一致（Oracle 4）。
  const QString vidA = paleo::realization::meanSurfaceVersionId( cat, setA );
  const QString vidB = paleo::realization::meanSurfaceVersionId( cat, setB );
  QVERIFY( !vidA.isEmpty() && !vidB.isEmpty() );
  const std::vector<double> meanA =
      readRaster( DataCatalog::resolvedVersionPath( dir.path(), cat.versionById( vidA ) ) );
  const std::vector<double> meanB =
      readRaster( DataCatalog::resolvedVersionPath( dir.path(), cat.versionById( vidB ) ) );

  // 找差值版本（realization_diff 资产）。
  QString diffPath;
  QStringList diffParents;
  for ( const CatalogAsset &a : cat.assets() )
    if ( a.type == paleo::realization::kAssetTypeDiff )
      for ( const CatalogVersion &v : cat.versionsForAsset( a.id ) )
      {
        diffPath = DataCatalog::resolvedVersionPath( dir.path(), v );
        diffParents = v.parentVersionIds;
        QCOMPARE( v.extra.value( paleo::realization::kKeySetIds ).toStringList(),
                  ( QStringList{ setA, setB } ) );
        QCOMPARE( v.extra.value( paleo::realization::kKeyStatistic ).toString(),
                  paleo::realization::kStatMeanDiff );
      }
  QVERIFY( !diffPath.isEmpty() );
  diffParents.sort();
  QStringList expectedParents{ vidA, vidB };
  expectedParents.sort();
  QCOMPARE( diffParents, expectedParents ); // Oracle 5：锚两侧均值版本

  const std::vector<double> diff = readRaster( diffPath );
  QCOMPARE( diff.size(), meanA.size() );
  for ( std::size_t i = 0; i < diff.size(); ++i )
    QVERIFY2( std::fabs( diff[i] - ( meanA[i] - meanB[i] ) ) < 1e-4,
              qPrintable( QStringLiteral( "diff[%1]=%2 direct %3" )
                              .arg( i ).arg( diff[i] ).arg( meanA[i] - meanB[i] ) ) );
}

void RealizationWorkflowTests::frameOrderAndSignals()
{
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY( cat.open( dir.path() ) );
  RealizationWorkflow wf;
  wf.bind( &cat, dir.path(), nullptr );
  QSignalSpy pubSpy( &wf, &RealizationWorkflow::realizationSetPublished );
  QSignalSpy statSpy( &wf, &RealizationWorkflow::realizationStatsDerived );
  QSignalSpy diffSpy( &wf, &RealizationWorkflow::realizationDiffReady );

  const auto members = makeMembers( dir.path(), 5, 8, 8, 3 );
  QString err;
  const QString setId = wf.publishSet( QStringLiteral( "帧序" ),
                                       QStringLiteral( "D61" ), members, {}, {}, &err );
  QVERIFY( !setId.isEmpty() );
  QCOMPARE( pubSpy.size(), 1 );
  QCOMPARE( pubSpy.first().at( 1 ).toInt(), 5 );

  // 动画帧序列 = 成员 index 升序的图层 id（Oracle 4）。
  const QStringList frames = wf.memberFrameOrder( setId );
  QCOMPARE( frames.size(), 5 );
  for ( int i = 0; i < 5; ++i )
    QCOMPARE( frames[i], RealizationWorkflow::memberLayerId( setId, i ) );

  paleo::ensemble::StatsRequest want;
  want.mean = want.p10 = true;
  QVERIFY( wf.deriveStatistics( setId, want, &err ) );
  QCOMPARE( statSpy.size(), 1 );
  QCOMPARE( statSpy.first().at( 1 ).toStringList(),
            ( QStringList{ paleo::realization::kStatMean,
                           paleo::realization::kStatP10 } ) );
  QCOMPARE( wf.statLayerIds( setId ),
            ( QStringList{ RealizationWorkflow::statLayerId(
                               setId, paleo::realization::kStatMean ),
                           RealizationWorkflow::statLayerId(
                               setId, paleo::realization::kStatP10 ) } ) );
}

void RealizationWorkflowTests::restartSurvival()
{
  QString setId;
  QString projectDir;
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    projectDir = dir.path();
    DataCatalog cat;
    QVERIFY( cat.open( projectDir ) );
    RealizationWorkflow wf;
    wf.bind( &cat, projectDir, nullptr );
    const auto members = makeMembers( projectDir, 3, 8, 8, 5 );
    QString err;
    setId = wf.publishSet( QStringLiteral( "重启存活" ),
                           QStringLiteral( "D61" ), members, {}, {}, &err );
    QVERIFY( !setId.isEmpty() );
    paleo::ensemble::StatsRequest want;
    want.mean = want.stddev = true;
    QVERIFY( wf.deriveStatistics( setId, want, &err ) );
    dir.setAutoRemove( false ); // 保留目录给第二会话
  }
  // 第二会话重开 catalog：集合/成员/统计面仍在（sqlite 是持久面）。
  DataCatalog cat;
  QVERIFY( cat.open( projectDir ) );
  const auto set = paleo::realization::setById( cat, setId );
  QCOMPARE( set.members.size(), 3 );
  QVERIFY( set.complete() );
  QCOMPARE( paleo::realization::statSurfaces( cat, setId ).size(), 2 );
  RealizationWorkflow wf;
  wf.bind( &cat, projectDir, nullptr );
  for ( int i = 0; i < 3; ++i )
    QVERIFY( QFile::exists( wf.memberRasterPath( setId, i ) ) );
  QDir( projectDir ).removeRecursively();
}

void RealizationWorkflowTests::deriveAllStatisticsAsyncWithTaskService()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  DataCatalog cat;
  QVERIFY( cat.open( dir.path() ) );

  PaleoTaskService taskService;
  RealizationWorkflow wf;
  wf.bind( &cat, dir.path(), nullptr );
  wf.setTaskService( &taskService );

  const auto members = makeMembers( dir.path(), 4, 8, 8, 999 );
  QString err;
  const QString setId = wf.publishSet( QStringLiteral( "异步统计" ),
                                       QStringLiteral( "D61" ), members, {}, {}, &err );
  QVERIFY( !setId.isEmpty() );

  QSignalSpy derivedSpy( &wf, &RealizationWorkflow::realizationStatsDerived );
  QSignalSpy busySpy( &wf, &RealizationWorkflow::busyChanged );

  PaleoTask *task = wf.deriveAllStatisticsAsync( setId, &err );
  QVERIFY2( task, qPrintable( err ) );

  // 等待任务完成
  QTRY_VERIFY_WITH_TIMEOUT( task->isFinished(), 10000 );
  QCOMPARE( task->state(), PaleoTask::State::Succeeded );
  QTRY_VERIFY( !wf.isBusy() );

  // 断言发布结果
  QCOMPARE( derivedSpy.count(), 1 );
  const auto stats = paleo::realization::statSurfaces( cat, setId );
  QCOMPARE( stats.size(), 4 );
}

QTEST_MAIN( RealizationWorkflowTests )
#include "tst_realizationworkflow.moc"
