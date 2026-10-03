// 层：数据
#include "sequenceframework.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>

namespace SequenceFramework
{

namespace
{

  // 同级排序：ordinal 升序，ordinal 相等按 name（稳定、可复现）。
  void sortSiblings( QVector<FrameworkUnit> &out )
  {
    std::stable_sort( out.begin(), out.end(), []( const FrameworkUnit &a, const FrameworkUnit &b ) {
      if ( a.ordinal != b.ordinal )
        return a.ordinal < b.ordinal;
      return a.name.localeAwareCompare( b.name ) < 0;
    } );
  }

  QString levelToken( UnitLevel level )
  {
    return level == UnitLevel::SystemsTract ? QStringLiteral( "systems_tract" )
                                            : QStringLiteral( "sequence" );
  }

  UnitLevel levelFromToken( const QString &token, bool *ok )
  {
    if ( token == QLatin1String( "systems_tract" ) )
    {
      *ok = true;
      return UnitLevel::SystemsTract;
    }
    if ( token == QLatin1String( "sequence" ) )
    {
      *ok = true;
      return UnitLevel::Sequence;
    }
    *ok = false;
    return UnitLevel::Sequence;
  }
} // namespace

QVector<FrameworkUnit> rootsOf( const Framework &fw )
{
  QVector<FrameworkUnit> out;
  for ( const FrameworkUnit &u : fw.units )
    if ( u.parentId.isEmpty() )
      out.append( u );
  sortSiblings( out );
  return out;
}

QVector<FrameworkUnit> childrenOf( const Framework &fw, const QString &parentId )
{
  if ( parentId.isEmpty() )
    return rootsOf( fw );
  QVector<FrameworkUnit> out;
  for ( const FrameworkUnit &u : fw.units )
    if ( u.parentId == parentId )
      out.append( u );
  sortSiblings( out );
  return out;
}

const FrameworkUnit *unitById( const Framework &fw, const QString &id )
{
  if ( id.isEmpty() )
    return nullptr;
  for ( const FrameworkUnit &u : fw.units )
    if ( u.id == id )
      return &u;
  return nullptr;
}

const MarkerBed *markerById( const Framework &fw, const QString &id )
{
  if ( id.isEmpty() )
    return nullptr;
  for ( const MarkerBed &m : fw.markers )
    if ( m.id == id )
      return &m;
  return nullptr;
}

QString unitPath( const Framework &fw, const QString &id )
{
  const FrameworkUnit *u = unitById( fw, id );
  if ( u == nullptr )
    return QString();
  // 只两级：父名 + 自身名。父缺失（悬空 parentId）时如实只回自身名。
  const FrameworkUnit *parent = u->parentId.isEmpty() ? nullptr : unitById( fw, u->parentId );
  if ( parent == nullptr )
    return u->name;
  return parent->name + QStringLiteral( " / " ) + u->name;
}

int depthOf( const Framework &fw, const QString &id )
{
  const FrameworkUnit *u = unitById( fw, id );
  if ( u == nullptr )
    return -1;
  return u->parentId.isEmpty() ? 0 : 1;
}

bool isLeaf( const Framework &fw, const QString &id )
{
  for ( const FrameworkUnit &u : fw.units )
    if ( u.parentId == id )
      return false;
  return unitById( fw, id ) != nullptr;
}

QVector<FrameworkUnit> leavesOf( const Framework &fw )
{
  QVector<FrameworkUnit> out;
  for ( const FrameworkUnit &u : fw.units )
    if ( isLeaf( fw, u.id ) )
      out.append( u );
  return out;
}

QStringList horizonsCoveredBy( const Framework &fw, const QString &unitId,
                               const QStringList &horizons )
{
  QStringList out;
  const FrameworkUnit *u = unitById( fw, unitId );
  if ( u == nullptr )
    return out;
  const int top = horizons.indexOf( u->topBoundary );
  if ( top < 0 )
    return out; // 顶界不在集合 → 区间无从定义，由诊断报 BoundaryOutsideSet
  int end = horizons.size();
  if ( !u->baseBoundary.isEmpty() )
  {
    const int base = horizons.indexOf( u->baseBoundary );
    if ( base < 0 )
      return out; // 底界不在集合 → 同上
    end = base;   // 底界归下一单元
  }
  for ( int i = top; i < end && i < horizons.size(); ++i )
    out.append( horizons.at( i ) );
  return out;
}

QHash<QString, QStringList> horizonUnitMap( const Framework &fw, const QStringList &horizons )
{
  QHash<QString, QStringList> out;
  for ( const QString &h : horizons )
    out.insert( h, QStringList() );
  // 只看叶子：层序被体系域细分后，归属由体系域承担（见 isLeaf 注）。
  for ( const FrameworkUnit &u : leavesOf( fw ) )
    for ( const QString &h : horizonsCoveredBy( fw, u.id, horizons ) )
      out[h].append( u.id );
  return out;
}

QStringList unitsCoveringHorizon( const Framework &fw, const QString &horizon,
                                  const QStringList &horizons )
{
  return horizonUnitMap( fw, horizons ).value( horizon );
}

QJsonObject toJson( const Framework &fw )
{
  QJsonObject root;
  root.insert( QStringLiteral( "schema_version" ), fw.schemaVersion );
  root.insert( QStringLiteral( "name" ), fw.name );

  QJsonArray units;
  for ( const FrameworkUnit &u : fw.units )
  {
    QJsonObject o;
    o.insert( QStringLiteral( "id" ), u.id );
    o.insert( QStringLiteral( "parent_id" ), u.parentId );
    o.insert( QStringLiteral( "name" ), u.name );
    o.insert( QStringLiteral( "level" ), levelToken( u.level ) );
    o.insert( QStringLiteral( "ordinal" ), u.ordinal );
    o.insert( QStringLiteral( "top_boundary" ), u.topBoundary );
    o.insert( QStringLiteral( "base_boundary" ), u.baseBoundary );
    o.insert( QStringLiteral( "thickness" ), u.thickness );
    o.insert( QStringLiteral( "color_key" ), u.colorKey );
    o.insert( QStringLiteral( "note" ), u.note );
    units.append( o );
  }
  root.insert( QStringLiteral( "units" ), units );

  QJsonArray markers;
  for ( const MarkerBed &m : fw.markers )
  {
    QJsonObject o;
    o.insert( QStringLiteral( "id" ), m.id );
    o.insert( QStringLiteral( "name" ), m.name );
    o.insert( QStringLiteral( "unit_id" ), m.unitId );
    QJsonArray names;
    for ( const QString &n : m.layerNames )
      names.append( n );
    o.insert( QStringLiteral( "layer_names" ), names );
    o.insert( QStringLiteral( "note" ), m.note );
    markers.append( o );
  }
  root.insert( QStringLiteral( "markers" ), markers );
  return root;
}

Framework fromJson( const QJsonObject &obj, QString *error )
{
  const auto fail = [error]( const QString &msg ) {
    if ( error )
      *error = msg;
    return Framework();
  };

  Framework fw;
  if ( obj.contains( QStringLiteral( "schema_version" ) ) )
  {
    const QJsonValue v = obj.value( QStringLiteral( "schema_version" ) );
    if ( !v.isDouble() )
      return fail( QStringLiteral( "格架 schema_version 不是数字" ) );
    fw.schemaVersion = v.toInt();
  }
  // 未来版本信号：高于本二进制所知的版本如实报错，不静默截断字段。
  if ( fw.schemaVersion > kSchemaVersion )
    return fail( QStringLiteral( "格架 schema_version 过新：%1" ).arg( fw.schemaVersion ) );
  fw.name = obj.value( QStringLiteral( "name" ) ).toString();

  const QJsonValue uv = obj.value( QStringLiteral( "units" ) );
  if ( !uv.isArray() )
    return fail( QStringLiteral( "格架 units 缺失或不是数组" ) );
  for ( const QJsonValue &v : uv.toArray() )
  {
    if ( !v.isObject() )
      return fail( QStringLiteral( "格架 units 含非对象成员" ) );
    const QJsonObject o = v.toObject();
    FrameworkUnit u;
    u.id = o.value( QStringLiteral( "id" ) ).toString();
    if ( u.id.isEmpty() )
      return fail( QStringLiteral( "格架单元 id 为空" ) );
    u.parentId = o.value( QStringLiteral( "parent_id" ) ).toString();
    u.name = o.value( QStringLiteral( "name" ) ).toString();
    bool levelOk = false;
    u.level = levelFromToken( o.value( QStringLiteral( "level" ) ).toString(), &levelOk );
    if ( !levelOk )
      return fail( QStringLiteral( "格架单元 %1 的 level 非法" ).arg( u.id ) );
    u.ordinal = o.value( QStringLiteral( "ordinal" ) ).toInt();
    u.topBoundary = o.value( QStringLiteral( "top_boundary" ) ).toString();
    u.baseBoundary = o.value( QStringLiteral( "base_boundary" ) ).toString();
    u.thickness = o.value( QStringLiteral( "thickness" ) ).toDouble();
    u.colorKey = o.value( QStringLiteral( "color_key" ) ).toString();
    u.note = o.value( QStringLiteral( "note" ) ).toString();
    fw.units.append( u );
  }

  const QJsonValue mv = obj.value( QStringLiteral( "markers" ) );
  if ( !mv.isArray() )
    return fail( QStringLiteral( "格架 markers 缺失或不是数组" ) );
  for ( const QJsonValue &v : mv.toArray() )
  {
    if ( !v.isObject() )
      return fail( QStringLiteral( "格架 markers 含非对象成员" ) );
    const QJsonObject o = v.toObject();
    MarkerBed m;
    m.id = o.value( QStringLiteral( "id" ) ).toString();
    if ( m.id.isEmpty() )
      return fail( QStringLiteral( "标志层 id 为空" ) );
    m.name = o.value( QStringLiteral( "name" ) ).toString();
    m.unitId = o.value( QStringLiteral( "unit_id" ) ).toString();
    for ( const QJsonValue &nv : o.value( QStringLiteral( "layer_names" ) ).toArray() )
      m.layerNames.append( nv.toString() );
    m.note = o.value( QStringLiteral( "note" ) ).toString();
    fw.markers.append( m );
  }
  if ( error )
    error->clear();
  return fw;
}

namespace
{

  int maxSuffix( const QStringList &ids, const QString &prefix )
  {
    int best = 0;
    for ( const QString &id : ids )
    {
      if ( !id.startsWith( prefix ) )
        continue;
      bool ok = false;
      const int n = id.mid( prefix.size() ).toInt( &ok );
      if ( ok && n > best )
        best = n;
    }
    return best;
  }
} // namespace

QString nextUnitId( const Framework &fw )
{
  QStringList ids;
  for ( const FrameworkUnit &u : fw.units )
    ids.append( u.id );
  return QStringLiteral( "sfu-%1" ).arg( maxSuffix( ids, QStringLiteral( "sfu-" ) ) + 1 );
}

QString nextMarkerId( const Framework &fw )
{
  QStringList ids;
  for ( const MarkerBed &m : fw.markers )
    ids.append( m.id );
  return QStringLiteral( "sfm-%1" ).arg( maxSuffix( ids, QStringLiteral( "sfm-" ) ) + 1 );
}

QByteArray toJsonBytes( const Framework &fw )
{
  return QJsonDocument( toJson( fw ) ).toJson( QJsonDocument::Indented );
}

} // namespace SequenceFramework
