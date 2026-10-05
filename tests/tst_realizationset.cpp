// 层：测试壳（realization 集合契约：extra 键族 + 集合/成员寻址 + 缺号诚实面）
#include <QtTest>
#include <QTemporaryDir>

#include "../src/catalog/datacatalog.h"
#include "../src/catalog/realizationset.h"

// 方向 47 契约测试（src/catalog/realizationset.h 头注是契约本文）：
// - 集合 = 一个 realization_set 资产 + N 个带 extra 键的 DERIVED 成员版本；
// - 寻址 round-trip：写入 → 重开 catalog（sqlite 落盘）→ 枚举/寻址一致；
// - 缺号如实列缺号集；N=1 合法但单成员；零成员集合不 crash；
// - 「迁移」面：extra 键族无 schema_epoch bump——旧 catalog（无键版本）
//   打开不炸、枚举为空；键坏值版本不冒充成员；
// - 统计面寻址：realization_stat 资产 + realizationSetId 回锚 + token 排序。

using namespace paleo::realization;

namespace
{
// 造一个带 N 个成员的集合资产；memberIndices 可缺号（诚实面用例）。
// 返回集合资产 id。
QString makeSet( DataCatalog &catalog, const QString &title,
                 const QVector<int> &memberIndices, int declaredCount )
{
  CatalogAsset asset;
  asset.id = catalog.nextAssetId();
  asset.type = kAssetTypeSet;
  asset.format = QStringLiteral( "tif" );
  asset.displayName = title;
  if ( !catalog.addAsset( asset ) )
    return QString();
  for ( const int idx : memberIndices )
  {
    CatalogVersion v;
    v.id = catalog.nextVersionId();
    v.assetId = asset.id;
    v.stage = QStringLiteral( "DERIVED" );
    v.versionNumber = idx + 1; // 分配序 = index+1（契约：不带新旧语义）
    v.managed = true;
    v.fileName = QStringLiteral( "REALIZATION_r%1.tif" ).arg( idx, 3, 10, QLatin1Char( '0' ) );
    v.path = DataCatalog::managedPath( QStringLiteral( "derived" ), asset.id, v.id, v.fileName );
    v.extra.insert( kKeySetId, asset.id );
    v.extra.insert( kKeyIndex, idx );
    v.extra.insert( kKeyMemberCount, declaredCount );
    v.extra.insert( kKeySeed, static_cast<qulonglong>( 42 ) );
    if ( !catalog.addVersion( v ) )
      return QString();
  }
  return asset.id;
}

QString addStatVersion( DataCatalog &catalog, const QString &setId,
                        const QString &token, int memberCount,
                        const QStringList &parentIds )
{
  CatalogAsset asset;
  asset.id = catalog.nextAssetId();
  asset.type = kAssetTypeStat;
  asset.format = QStringLiteral( "tif" );
  asset.displayName = QStringLiteral( "stat %1" ).arg( token );
  if ( !catalog.addAsset( asset ) )
    return QString();
  CatalogVersion v;
  v.id = catalog.nextVersionId();
  v.assetId = asset.id;
  v.stage = QStringLiteral( "DERIVED" );
  v.versionNumber = 1;
  v.managed = true;
  v.fileName = QStringLiteral( "STAT_%1.tif" ).arg( token );
  v.path = DataCatalog::managedPath( QStringLiteral( "derived" ), asset.id, v.id, v.fileName );
  v.parentVersionIds = parentIds;
  v.extra.insert( kKeySetId, setId );
  v.extra.insert( kKeyStatistic, token );
  v.extra.insert( kKeyMemberCount, memberCount );
  if ( !catalog.addVersion( v ) )
    return QString();
  return v.id;
}
} // namespace

class RealizationSetTests : public QObject
{
  Q_OBJECT

  private slots:
    void membersRoundTripAndAddressing();
    void missingIndicesReported();
    void nonMemberVersionsIgnored();
    void statSurfaceAddressing();
    void singleMemberAndEmptySetsAreHonest();
    void memberRasterPathsKeepIndexOrder();
    void legacyCatalogWithoutKeysOpensClean();
    void statisticLabelsStable();
};

void RealizationSetTests::membersRoundTripAndAddressing()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  QString setId;
  {
    DataCatalog catalog;
    QVERIFY( catalog.open( dir.path() ) );
    setId = makeSet( catalog, QStringLiteral( "D61·poro SGS 集合" ), { 0, 1, 2 }, 3 );
    QVERIFY( !setId.isEmpty() );
  }
  // 重开：extra_json 经 sqlite round-trip（契约的持久化面）。
  DataCatalog reopened;
  QVERIFY( reopened.open( dir.path() ) );
  const QVector<RealizationSet> sets = enumerateSets( reopened );
  QCOMPARE( sets.size(), 1 );
  const RealizationSet &set = sets.first();
  QCOMPARE( set.setId, setId );
  QCOMPARE( set.assetId, setId );
  QCOMPARE( set.title, QStringLiteral( "D61·poro SGS 集合" ) );
  QCOMPARE( set.declaredCount, 3 );
  QCOMPARE( set.members.size(), 3 );
  QVERIFY( set.complete() );
  QVERIFY( set.missingIndices.isEmpty() );
  for ( int i = 0; i < 3; ++i )
  {
    QCOMPARE( set.members.at( i ).index, i );
    QCOMPARE( set.members.at( i ).seed, quint64( 42 ) );
    QVERIFY( !memberVersionId( set, i ).isEmpty() );
  }
  QVERIFY( memberVersionId( set, 3 ).isEmpty() ); // 未知 index → 空，不猜
  QVERIFY( memberVersionId( set, -1 ).isEmpty() );
}

void RealizationSetTests::missingIndicesReported()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  DataCatalog catalog;
  QVERIFY( catalog.open( dir.path() ) );
  const QString setId = makeSet( catalog, QStringLiteral( "缺号集合" ), { 0, 1, 3 }, 4 );
  QVERIFY( !setId.isEmpty() );
  const RealizationSet set = setById( catalog, setId );
  QCOMPARE( set.members.size(), 3 );
  QCOMPARE( set.declaredCount, 4 );
  QVERIFY( !set.complete() );
  QCOMPARE( set.missingIndices, QVector<int>( { 2 } ) ); // 缺号如实列
}

void RealizationSetTests::nonMemberVersionsIgnored()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  DataCatalog catalog;
  QVERIFY( catalog.open( dir.path() ) );

  CatalogAsset asset;
  asset.id = catalog.nextAssetId();
  asset.type = QStringLiteral( "single_factor_raster" );
  asset.format = QStringLiteral( "tif" );
  asset.displayName = QStringLiteral( "普通派生栅格" );
  QVERIFY( catalog.addAsset( asset ) );

  // 无键版本（旧产物形态）——缺键 = 非成员，枚举不得收录。
  CatalogVersion plain;
  plain.id = catalog.nextVersionId();
  plain.assetId = asset.id;
  plain.stage = QStringLiteral( "DERIVED" );
  plain.versionNumber = 1;
  QVERIFY( catalog.addVersion( plain ) );

  // 半键版本：有 index 无 setId / 有 setId 无 index / index 为负——都非成员。
  for ( int i = 0; i < 3; ++i )
  {
    CatalogVersion bad;
    bad.id = catalog.nextVersionId();
    bad.assetId = asset.id;
    bad.stage = QStringLiteral( "DERIVED" );
    bad.versionNumber = 2 + i;
    if ( i == 0 )
      bad.extra.insert( kKeyIndex, 0 );
    else if ( i == 1 )
      bad.extra.insert( kKeySetId, asset.id );
    else
    {
      bad.extra.insert( kKeySetId, asset.id );
      bad.extra.insert( kKeyIndex, -3 );
    }
    QVERIFY( catalog.addVersion( bad ) );
  }

  QVERIFY( enumerateSets( catalog ).isEmpty() );
  QVERIFY( setById( catalog, asset.id ).setId.isEmpty() );
}

void RealizationSetTests::statSurfaceAddressing()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  DataCatalog catalog;
  QVERIFY( catalog.open( dir.path() ) );
  const QString setId = makeSet( catalog, QStringLiteral( "集合A" ), { 0, 1 }, 2 );
  QVERIFY( !setId.isEmpty() );
  QStringList memberIds;
  for ( const RealizationMember &m : setById( catalog, setId ).members )
    memberIds << m.versionId;

  const QString meanV = addStatVersion( catalog, setId, kStatMean, 2, memberIds );
  QVERIFY( !meanV.isEmpty() );
  QVERIFY( !addStatVersion( catalog, setId, kStatStdDev, 2, memberIds ).isEmpty() );
  QVERIFY( !addStatVersion( catalog, setId, kStatP90, 2, memberIds ).isEmpty() );
  QVERIFY( !addStatVersion( catalog, setId, kStatP10, 2, memberIds ).isEmpty() );

  // 干扰项：别的集合的统计面 + 非 stat 资产上的 stat 键（脏数据）不得混入。
  const QString otherSet = makeSet( catalog, QStringLiteral( "集合B" ), { 0, 1 }, 2 );
  QVERIFY( !addStatVersion( catalog, otherSet, kStatMean, 2, QStringList() ).isEmpty() );
  CatalogAsset fake;
  fake.id = catalog.nextAssetId();
  fake.type = QStringLiteral( "single_factor_raster" ); // 非 realization_stat
  fake.displayName = QStringLiteral( "脏数据" );
  QVERIFY( catalog.addAsset( fake ) );
  CatalogVersion fv;
  fv.id = catalog.nextVersionId();
  fv.assetId = fake.id;
  fv.stage = QStringLiteral( "DERIVED" );
  fv.versionNumber = 1;
  fv.extra.insert( kKeySetId, setId );
  fv.extra.insert( kKeyStatistic, kStatMean );
  QVERIFY( catalog.addVersion( fv ) );

  const QVector<StatisticSurface> stats = statSurfaces( catalog, setId );
  QCOMPARE( stats.size(), 4 ); // mean/stddev/p10/p90 —— 脏数据与别集合不混入
  QCOMPARE( stats.at( 0 ).token, kStatMean );
  QCOMPARE( stats.at( 1 ).token, kStatStdDev );
  QCOMPARE( stats.at( 2 ).token, kStatP10 );
  QCOMPARE( stats.at( 3 ).token, kStatP90 );
  for ( const StatisticSurface &s : stats )
    QCOMPARE( s.memberCount, 2 );
  QCOMPARE( meanSurfaceVersionId( catalog, setId ), meanV );
  QVERIFY( statSurfaces( catalog, QStringLiteral( "ast-nonexistent" ) ).isEmpty() );
}

void RealizationSetTests::singleMemberAndEmptySetsAreHonest()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  DataCatalog catalog;
  QVERIFY( catalog.open( dir.path() ) );

  // N=1：集合合法存在、完整，但成员数=1——下游据此标「单成员无不确定性」。
  const QString singleId = makeSet( catalog, QStringLiteral( "单成员集合" ), { 0 }, 1 );
  QVERIFY( !singleId.isEmpty() );
  const RealizationSet single = setById( catalog, singleId );
  QCOMPARE( single.members.size(), 1 );
  QVERIFY( single.complete() );

  // 零成员：资产存在但没有任何成员版本 → 枚举不收（members 为空不进表）。
  CatalogAsset empty;
  empty.id = catalog.nextAssetId();
  empty.type = kAssetTypeSet;
  empty.displayName = QStringLiteral( "空集合" );
  QVERIFY( catalog.addAsset( empty ) );
  QVERIFY( setById( catalog, empty.id ).setId.isEmpty() );

  // 全集枚举只有单成员那一个。
  QCOMPARE( enumerateSets( catalog ).size(), 1 );
}

void RealizationSetTests::memberRasterPathsKeepIndexOrder()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  DataCatalog catalog;
  QVERIFY( catalog.open( dir.path() ) );
  const QString setId = makeSet( catalog, QStringLiteral( "有序集合" ), { 2, 0, 1 }, 3 );
  QVERIFY( !setId.isEmpty() );
  const QStringList paths = memberRasterPaths( catalog, dir.path(), setId );
  QCOMPARE( paths.size(), 3 );
  // 成员按 index 升序；路径含版本 id 段——按序回查 index。
  const RealizationSet set = setById( catalog, setId );
  for ( int i = 0; i < 3; ++i )
    QVERIFY2( paths.at( i ).contains( set.members.at( i ).versionId ),
              qPrintable( paths.at( i ) ) );
  QVERIFY( memberRasterPaths( catalog, dir.path(), QStringLiteral( "ast-none" ) ).isEmpty() );
}

void RealizationSetTests::legacyCatalogWithoutKeysOpensClean()
{
  // 「迁移」面：无 realization 键的旧 catalog（上代构建的产物形态）打开不炸、
  // 枚举为空——extra 键族无 schema_epoch bump，缺键 = 非成员即兼容语义。
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  {
    DataCatalog catalog;
    QVERIFY( catalog.open( dir.path() ) );
    CatalogAsset a;
    a.id = catalog.nextAssetId();
    a.type = QStringLiteral( "single_factor_raster" );
    a.displayName = QStringLiteral( "旧产物" );
    QVERIFY( catalog.addAsset( a ) );
    CatalogVersion v;
    v.id = catalog.nextVersionId();
    v.assetId = a.id;
    v.stage = QStringLiteral( "DERIVED" );
    v.versionNumber = 1;
    QVERIFY( catalog.addVersion( v ) );
  }
  DataCatalog reopened;
  QVERIFY2( reopened.open( dir.path() ), "旧 catalog 打开不得失败" );
  QVERIFY( enumerateSets( reopened ).isEmpty() );
  QVERIFY( statSurfaces( reopened, QStringLiteral( "anything" ) ).isEmpty() );
}

void RealizationSetTests::statisticLabelsStable()
{
  // 口径词逐字钉死：图签/版本面/树节点共用同一映射，改动要过本断言。
  QCOMPARE( statisticDisplayLabel( kStatMean ), QStringLiteral( "成员均值" ) );
  QCOMPARE( statisticDisplayLabel( kStatStdDev ), QStringLiteral( "成员总体标准差（÷N）" ) );
  QCOMPARE( statisticDisplayLabel( kStatP10 ), QStringLiteral( "成员 P10 分位" ) );
  QCOMPARE( statisticDisplayLabel( kStatP90 ), QStringLiteral( "成员 P90 分位" ) );
  QCOMPARE( statisticDisplayLabel( kStatMeanDiff ), QStringLiteral( "两集合均值差" ) );
  QVERIFY( statisticDisplayLabel( QStringLiteral( "bogus" ) ).isEmpty() );
}

QTEST_MAIN( RealizationSetTests )
#include "tst_realizationset.moc"
