// 层：数据
#include "realizationset.h"
#include "datacatalog.h"

#include <algorithm>

namespace paleo::realization
{
namespace
{
QString text( const char *source )
{
  return QCoreApplication::translate( "RealizationSet", source );
}

// 成员判定：extra 同带集合锚与合法 index（>=0 的整数）。坏值/缺键不计成员。
bool memberOf( const CatalogVersion &v, QString *setId, int *index )
{
  const QString sid = v.extra.value( kKeySetId ).toString();
  bool indexOk = false;
  const int idx = v.extra.value( kKeyIndex ).toInt( &indexOk );
  if ( sid.isEmpty() || !indexOk || idx < 0 )
    return false;
  if ( setId )
    *setId = sid;
  if ( index )
    *index = idx;
  return true;
}

int tokenRank( const QString &token )
{
  if ( token == kStatMean )
    return 0;
  if ( token == kStatStdDev )
    return 1;
  if ( token == kStatP10 )
    return 2;
  if ( token == kStatP90 )
    return 3;
  return 4;
}
} // namespace

QString statisticDisplayLabel( const QString &token )
{
  if ( token == kStatMean )
    return text( QT_TRANSLATE_NOOP( "RealizationSet", "成员均值" ) );
  if ( token == kStatStdDev )
    return text( QT_TRANSLATE_NOOP( "RealizationSet", "成员总体标准差（÷N）" ) );
  if ( token == kStatP10 )
    return text( QT_TRANSLATE_NOOP( "RealizationSet", "成员 P10 分位" ) );
  if ( token == kStatP90 )
    return text( QT_TRANSLATE_NOOP( "RealizationSet", "成员 P90 分位" ) );
  if ( token == kStatMeanDiff )
    return text( QT_TRANSLATE_NOOP( "RealizationSet", "两集合均值差" ) );
  return QString();
}

QVector<RealizationSet> enumerateSets( const DataCatalog &catalog )
{
  QVector<RealizationSet> out;
  if ( !catalog.isOpen() )
    return out;
  // 按集合锚分组收集成员版本（一遍全表扫；集合通常个位数级）。
  QHash<QString, QVector<RealizationMember>> bySet;
  QHash<QString, int> declared;
  for ( const CatalogVersion &v : catalog.versions() )
  {
    QString sid;
    int idx = -1;
    if ( !memberOf( v, &sid, &idx ) )
      continue;
    RealizationMember m;
    m.versionId = v.id;
    m.index = idx;
    m.seed = v.extra.value( kKeySeed ).toULongLong();
    m.fileName = v.fileName;
    bySet[sid].append( m );
    const int declaredHere = v.extra.value( kKeyMemberCount ).toInt();
    declared[sid] = std::max( declared.value( sid ), declaredHere );
  }
  for ( auto it = bySet.constBegin(); it != bySet.constEnd(); ++it )
  {
    RealizationSet set;
    set.setId = it.key();
    set.assetId = it.key(); // 契约：集合即资产
    set.declaredCount = declared.value( it.key() );
    set.members = it.value();
    const CatalogAsset asset = catalog.assetById( set.assetId );
    set.title = asset.displayName.isEmpty() ? set.assetId : asset.displayName;
    std::sort( set.members.begin(), set.members.end(),
               []( const RealizationMember &a, const RealizationMember &b ) {
                 return a.index == b.index ? a.versionId < b.versionId
                                           : a.index < b.index;
               } );
    QSet<int> present;
    for ( const RealizationMember &m : set.members )
      present.insert( m.index );
    for ( int i = 0; i < set.declaredCount; ++i )
      if ( !present.contains( i ) )
        set.missingIndices.append( i );
    out.append( set );
  }
  std::sort( out.begin(), out.end(),
             []( const RealizationSet &a, const RealizationSet &b ) {
               return a.setId < b.setId;
             } );
  return out;
}

RealizationSet setById( const DataCatalog &catalog, const QString &setId )
{
  if ( setId.isEmpty() )
    return {};
  for ( const RealizationSet &set : enumerateSets( catalog ) )
    if ( set.setId == setId )
      return set;
  return {};
}

QString memberVersionId( const RealizationSet &set, int index )
{
  for ( const RealizationMember &m : set.members )
    if ( m.index == index )
      return m.versionId;
  return QString();
}

QVector<StatisticSurface> statSurfaces( const DataCatalog &catalog,
                                      const QString &setId )
{
  QVector<StatisticSurface> out;
  if ( !catalog.isOpen() || setId.isEmpty() )
    return out;
  for ( const CatalogVersion &v : catalog.versions() )
  {
    if ( v.extra.value( kKeySetId ).toString() != setId )
      continue;
    const QString token = v.extra.value( kKeyStatistic ).toString();
    if ( token.isEmpty() || token == kStatMeanDiff )
      continue; // 差值面不算集合内统计面（锚的是两集合）
    const CatalogAsset asset = catalog.assetById( v.assetId );
    if ( asset.type != kAssetTypeStat )
      continue;
    StatisticSurface s;
    s.versionId = v.id;
    s.assetId = v.assetId;
    s.token = token;
    s.memberCount = v.extra.value( kKeyMemberCount ).toInt();
    out.append( s );
  }
  // 排序前预取 versionNumber：比较器里逐次 versionById 是哈希查 + 整个
  // CatalogVersion（含 QVariantMap extra）按值返回——O(n log n) 次比较 ×2 次
  // 深拷贝。预取一遍后比较器只做 int 比较，全序不变。
  QHash<QString, int> versionNumberOf;
  versionNumberOf.reserve( out.size() * 2 );
  for ( const StatisticSurface &s : out )
    versionNumberOf.insert( s.versionId, catalog.versionById( s.versionId ).versionNumber );
  std::sort( out.begin(), out.end(),
             [&versionNumberOf]( const StatisticSurface &a, const StatisticSurface &b ) {
               const int ra = tokenRank( a.token );
               const int rb = tokenRank( b.token );
               if ( ra != rb )
                 return ra < rb;
               if ( a.token != b.token )
                 return a.token < b.token;
               // 同 token 多条（重派生）→ versionNumber 高者在前（最新派生）。
               return versionNumberOf.value( a.versionId, 0 ) >
                      versionNumberOf.value( b.versionId, 0 );
             } );
  return out;
}

QString meanSurfaceVersionId( const DataCatalog &catalog, const QString &setId )
{
  for ( const StatisticSurface &s : statSurfaces( catalog, setId ) )
    if ( s.token == kStatMean )
      return s.versionId; // 排序保证同 token 最新在前
  return QString();
}

QStringList memberRasterPaths( const DataCatalog &catalog, const QString &projectDir,
                               const QString &setId )
{
  QStringList paths;
  const RealizationSet set = setById( catalog, setId );
  for ( const RealizationMember &m : set.members )
  {
    const CatalogVersion v = catalog.versionById( m.versionId );
    const QString abs = DataCatalog::resolvedVersionPath( projectDir, v );
    if ( !abs.isEmpty() )
      paths.append( abs );
  }
  return paths;
}

} // namespace paleo::realization
