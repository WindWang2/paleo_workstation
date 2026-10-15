// 层：数据
#include "constraintsemantics.h"

#include <QJsonDocument>
#include <QObject>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>

#include <algorithm>
#include <cmath>

// 层：数据
namespace paleo::singlefactor
{
namespace
{

// constraint_semantics.py 非阻断词表（与 algorithms/singlefactor/structural.cpp
// kNonBlockingModes 同表；那边经 isFullBlockMode 调到这里，见 structural.cpp）。
const QSet<QString> &nonBlockingModes()
{
  static const QSet<QString> modes = { QStringLiteral( "none" ),     QStringLiteral( "off" ),
                                       QStringLiteral( "no_block" ), QStringLiteral( "soft" ),
                                       QStringLiteral( "partial" ),  QStringLiteral( "display_only" ),
                                       QStringLiteral( "0" ),        QStringLiteral( "false" ) };
  return modes;
}

bool readNumber( const QJsonObject &object, const QString &key, double *target, QString *error )
{
  if ( !object.contains( key ) )
    return true;
  const QJsonValue value = object.value( key );
  if ( !value.isDouble() )
  {
    *error = QStringLiteral( "约束参数 %1 不是数值" ).arg( key );
    return false;
  }
  *target = value.toDouble();
  return true;
}

// 线段 p1p2 与 p3p4 的相交关系（含端点接触与共线重叠）。
double cross( QPointF o, QPointF a, QPointF b )
{
  return ( a.x() - o.x() ) * ( b.y() - o.y() ) - ( a.y() - o.y() ) * ( b.x() - o.x() );
}

bool onSegment( QPointF p, QPointF q, QPointF r )
{
  return std::min( p.x(), r.x() ) <= q.x() && q.x() <= std::max( p.x(), r.x() ) &&
         std::min( p.y(), r.y() ) <= q.y() && q.y() <= std::max( p.y(), r.y() );
}

bool segmentsIntersect( QPointF p1, QPointF p2, QPointF p3, QPointF p4 )
{
  const double d1 = cross( p3, p4, p1 );
  const double d2 = cross( p3, p4, p2 );
  const double d3 = cross( p1, p2, p3 );
  const double d4 = cross( p1, p2, p4 );
  if ( ( ( d1 > 0 && d2 < 0 ) || ( d1 < 0 && d2 > 0 ) ) && ( ( d3 > 0 && d4 < 0 ) || ( d3 < 0 && d4 > 0 ) ) )
    return true;
  return ( d1 == 0 && onSegment( p3, p1, p4 ) ) || ( d2 == 0 && onSegment( p3, p2, p4 ) ) ||
         ( d3 == 0 && onSegment( p1, p3, p2 ) ) || ( d4 == 0 && onSegment( p1, p4, p2 ) );
}

// 相邻两段 a→b→c 是否共线回折（c 落回 ab 上，或 a 落在 bc 上）。
bool foldsBack( QPointF a, QPointF b, QPointF c )
{
  if ( cross( a, b, c ) != 0 )
    return false;
  const double dot = ( b.x() - a.x() ) * ( c.x() - b.x() ) + ( b.y() - a.y() ) * ( c.y() - b.y() );
  return dot < 0;
}

bool segmentTouchesRect( QPointF a, QPointF b, const QRectF &r )
{
  // Liang–Barsky：r 为闭矩形。
  double t0 = 0, t1 = 1;
  const double dx = b.x() - a.x();
  const double dy = b.y() - a.y();
  const double p[4] = { -dx, dx, -dy, dy };
  const double q[4] = { a.x() - r.left(), r.right() - a.x(), a.y() - r.top(), r.bottom() - a.y() };
  for ( int i = 0; i < 4; ++i )
  {
    if ( p[i] == 0 )
    {
      if ( q[i] < 0 )
        return false;
      continue;
    }
    const double t = q[i] / p[i];
    if ( p[i] < 0 )
      t0 = std::max( t0, t );
    else
      t1 = std::min( t1, t );
    if ( t0 > t1 )
      return false;
  }
  return true;
}

QString canonicalOrSelf( const QString &methodId )
{
  const QString canonical = canonicalConstraintMethod( methodId );
  return canonical.isEmpty() ? methodId : canonical;
}

} // namespace

QString normalizeBlockModeToken( const QString &mode )
{
  return mode.trimmed().toLower();
}

bool isFullBlockModeToken( const QString &mode )
{
  return !nonBlockingModes().contains( normalizeBlockModeToken( mode ) );
}

QString semanticTokenFromTypeColumn( const QString &type )
{
  if ( type == QLatin1String( "break_line" ) )
    return QStringLiteral( "hard_barrier" );
  if ( type == QLatin1String( "direction_line" ) )
    return QStringLiteral( "direction_guide" );
  if ( type == QLatin1String( "interpretive_boundary" ) || type == QLatin1String( "contour_stop" ) ||
       type == QLatin1String( "cartographic_detour" ) )
    return type;
  return QString();
}

bool isKnownSemanticToken( const QString &token )
{
  return token == QLatin1String( "hard_barrier" ) || token == QLatin1String( "direction_guide" ) ||
         token == QLatin1String( "interpretive_boundary" ) || token == QLatin1String( "contour_stop" ) ||
         token == QLatin1String( "cartographic_detour" );
}

ConstraintSemantics resolveConstraintSemantics( const QString &type, const QString &paramsJson,
                                                const QVariant &schemaColumn,
                                                const ConstraintSemanticsOptions &options,
                                                const QString &featureId )
{
  ConstraintSemantics out;
  const QString jsonText = paramsJson.trimmed();
  if ( jsonText.isEmpty() )
  {
    // 无 params_json 的旧行：只认 type 列；enabled/blockMode 取缺省（与
    // constraintparse 旧口径一致：type=break_line 即 full_block 硬隔断）。
    out.declaredSemantic = semanticTokenFromTypeColumn( type.trimmed() );
    if ( out.declaredSemantic.isEmpty() )
    {
      out.ignoredReason = type.trimmed().isEmpty() ? QStringLiteral( "unknown_type" )
                                                   : QStringLiteral( "unknown_type:%1" ).arg( type.trimmed() );
      return out;
    }
    out.semantic = out.declaredSemantic;
    return out;
  }
  out.fromParams = true;
  const auto invalid = [&out]( const QString &message ) {
    out.valid = false;
    out.error = message;
    out.semantic.clear();
    return out;
  };
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson( jsonText.toUtf8(), &parseError );
  if ( parseError.error != QJsonParseError::NoError || !document.isObject() )
    return invalid( QStringLiteral( "约束 %1 的 params_json 无法解析" ).arg( featureId ) );
  const QJsonObject object = document.object();
  int schema = 1;
  if ( object.contains( QStringLiteral( "schemaVersion" ) ) )
  {
    const QJsonValue schemaValue = object.value( QStringLiteral( "schemaVersion" ) );
    if ( !schemaValue.isDouble() )
      return invalid( QStringLiteral( "schemaVersion 不是整数" ) );
    schema = schemaValue.toInt();
  }
  else if ( schemaColumn.isValid() && !schemaColumn.isNull() )
  {
    schema = schemaColumn.toInt();
  }
  if ( schema > 1 )
    return invalid( QStringLiteral( "不认识的约束参数版本 %1" ).arg( schema ) );
  const QJsonValue semanticValue = object.value( QStringLiteral( "semantic" ) );
  if ( !semanticValue.isString() )
    return invalid( QStringLiteral( "约束 %1 的 params_json 缺少 semantic" ).arg( featureId ) );
  out.declaredSemantic = semanticValue.toString();
  if ( !isKnownSemanticToken( out.declaredSemantic ) )
    return invalid( QStringLiteral( "不认识的约束语义 %1" ).arg( out.declaredSemantic ) );
  if ( object.contains( QStringLiteral( "enabled" ) ) )
    out.enabled = object.value( QStringLiteral( "enabled" ) ).toBool( true );
  // 判定顺序与旧 constraintparse 逐字一致：停用 → blockMode 重分类 → 数值字段。
  // 被忽略的行不再读数值（非数值字段不让整次运行失败）。
  if ( !out.enabled && !options.keepDisabled )
  {
    out.ignoredReason = QStringLiteral( "disabled" );
    return out;
  }
  const QJsonValue blockModeValue = object.value( QStringLiteral( "blockMode" ) );
  if ( blockModeValue.isString() )
    out.blockMode = blockModeValue.toString().trimmed();
  out.semantic = out.declaredSemantic;
  // 上游 constraint_semantics.py：hard_barrier 的 blockMode 决定真实语义——
  // display_only 只停线不动趋势，其余非阻断词一律忽略。
  if ( out.semantic == QLatin1String( "hard_barrier" ) && !options.keepNonBlockingBarriers &&
       !isFullBlockModeToken( out.blockMode ) )
  {
    if ( normalizeBlockModeToken( out.blockMode ) == QLatin1String( "display_only" ) )
    {
      out.semantic = QStringLiteral( "contour_stop" );
    }
    else
    {
      out.semantic.clear();
      out.ignoredReason = QStringLiteral( "non_blocking" );
      return out;
    }
  }
  QString numberError;
  if ( !readNumber( object, QStringLiteral( "ratio" ), &out.ratio, &numberError ) ||
       !readNumber( object, QStringLiteral( "influenceRadius" ), &out.influenceRadius, &numberError ) ||
       !readNumber( object, QStringLiteral( "coreRadius" ), &out.coreRadius, &numberError ) ||
       !readNumber( object, QStringLiteral( "softStrength" ), &out.softStrength, &numberError ) ||
       !readNumber( object, QStringLiteral( "softRadius" ), &out.softRadius, &numberError ) ||
       !readNumber( object, QStringLiteral( "displayBuffer" ), &out.displayBuffer, &numberError ) ||
       !readNumber( object, QStringLiteral( "cartographicBuffer" ), &out.cartographicBuffer, &numberError ) )
    return invalid( numberError );
  if ( object.contains( QStringLiteral( "unit" ) ) && object.value( QStringLiteral( "unit" ) ).isString() )
    out.unit = object.value( QStringLiteral( "unit" ) ).toString();
  return out;
}

ConstraintPartCheck checkConstraintPart( const QVector<QPointF> &part )
{
  ConstraintPartCheck check;
  for ( const QPointF &p : part )
  {
    if ( !std::isfinite( p.x() ) || !std::isfinite( p.y() ) )
    {
      check.issue = QStringLiteral( "non_finite_coordinate" );
      return check;
    }
  }
  if ( part.size() < 2 )
  {
    check.issue = QStringLiteral( "short_line" );
    return check;
  }
  // 去连续重复顶点后再判自交：重复点本身只是零长段（引擎跳过），不是自交。
  QVector<QPointF> unique;
  unique.reserve( part.size() );
  double minX = part.first().x(), maxX = minX, minY = part.first().y(), maxY = minY;
  for ( const QPointF &p : part )
  {
    minX = std::min( minX, p.x() );
    maxX = std::max( maxX, p.x() );
    minY = std::min( minY, p.y() );
    maxY = std::max( maxY, p.y() );
    if ( !unique.isEmpty() && unique.last() == p )
    {
      check.duplicateVertices = true;
      continue;
    }
    if ( !unique.isEmpty() )
      check.length += std::hypot( p.x() - unique.last().x(), p.y() - unique.last().y() );
    unique.append( p );
  }
  check.bounds = QRectF( QPointF( minX, minY ), QPointF( maxX, maxY ) );
  if ( unique.size() < 2 || !( check.length > 0.0 ) )
  {
    check.issue = QStringLiteral( "zero_length" );
    return check;
  }
  const int segments = static_cast<int>( unique.size() ) - 1;
  const bool closed = unique.size() > 3 && unique.first() == unique.last();
  for ( int i = 0; i + 1 < segments; ++i )
  {
    if ( foldsBack( unique[i], unique[i + 1], unique[i + 2] ) )
    {
      check.issue = QStringLiteral( "self_intersecting" );
      return check;
    }
  }
  if ( closed && foldsBack( unique[segments - 1], unique[segments], unique[1] ) )
  {
    check.issue = QStringLiteral( "self_intersecting" );
    return check;
  }
  for ( int i = 0; i < segments; ++i )
  {
    for ( int j = i + 2; j < segments; ++j )
    {
      // 闭合线首末段共享端点不算自交。
      if ( closed && i == 0 && j == segments - 1 )
        continue;
      if ( segmentsIntersect( unique[i], unique[i + 1], unique[j], unique[j + 1] ) )
      {
        check.issue = QStringLiteral( "self_intersecting" );
        return check;
      }
    }
  }
  return check;
}

bool polylineTouchesRect( const QVector<QPointF> &part, const QRectF &rect )
{
  const QRectF r = rect.normalized();
  if ( part.size() == 1 )
    return r.contains( part.first() );
  for ( int i = 1; i < part.size(); ++i )
  {
    if ( segmentTouchesRect( part[i - 1], part[i], r ) )
      return true;
  }
  return false;
}

QString canonicalConstraintMethod( const QString &methodId )
{
  if ( methodId.isEmpty() || methodId == QLatin1String( "idw" ) || methodId == QLatin1String( "legacy" ) )
    return QStringLiteral( "legacy" );
  static const QStringList known = { QStringLiteral( "local_direction_idw" ), QStringLiteral( "local_direction_kriging" ),
                                     QStringLiteral( "surfer_idw" ),          QStringLiteral( "structural_idw" ),
                                     QStringLiteral( "kriging" ),             QStringLiteral( "sgs" ),
                                     QStringLiteral( "cokriging" ) };
  return known.contains( methodId ) ? methodId : QString();
}

bool methodReadsConstraints( const QString &methodId )
{
  const QString m = canonicalConstraintMethod( methodId );
  return !m.isEmpty() && m != QLatin1String( "kriging" ) && m != QLatin1String( "sgs" ) &&
         m != QLatin1String( "cokriging" );
}

bool methodRejectsInvalidParams( const QString &methodId )
{
  const QString m = canonicalConstraintMethod( methodId );
  return m == QLatin1String( "local_direction_idw" ) || m == QLatin1String( "local_direction_kriging" ) ||
         m == QLatin1String( "surfer_idw" ) || m == QLatin1String( "structural_idw" );
}

ConstraintSemanticsOptions semanticsOptionsForMethod( const QString &methodId )
{
  ConstraintSemanticsOptions options;
  if ( canonicalConstraintMethod( methodId ) == QLatin1String( "structural_idw" ) )
  {
    options.keepDisabled = true;
    options.keepNonBlockingBarriers = true;
  }
  return options;
}

QString methodConstraintCapabilityNote( const QString &methodId )
{
  const QString m = canonicalConstraintMethod( methodId );
  if ( m == QLatin1String( "local_direction_idw" ) || m == QLatin1String( "local_direction_kriging" ) )
    return QObject::tr( "消费：硬屏障（网格连通，有限线可绕端）、方向线（比值>1，逐线局部各向异性）、"
                        "解释软边界（强度>0）；停线/制图绕行只作用于制图，不改分析场。" );
  if ( m == QLatin1String( "surfer_idw" ) )
    return QObject::tr( "消费：硬屏障（有限断层绕行距离）；方向线与软边界不参与（各向异性只用全局比值/角度）；"
                        "停线/制图绕行只作用于制图。" );
  if ( m == QLatin1String( "structural_idw" ) )
    return QObject::tr( "消费：完全阻断线（硬隔断）、方向线（裁到成图域）、解释软边界；"
                        "仅显示停线按上游语义转为解释软边界（只改趋势权重，不隔断）。" );
  if ( m == QLatin1String( "legacy" ) )
    return QObject::tr( "消费：完全阻断打断线（网格连通+凸包裁剪）、方向线（全域长度加权平均方向，"
                        "逐线比值/半径不参与）；软边界/停线/制图绕行不参与。" );
  if ( m == QLatin1String( "kriging" ) || m == QLatin1String( "sgs" ) || m == QLatin1String( "cokriging" ) )
    return QObject::tr( "地统计方法不读取逐线约束：方向线、打断线、软边界均不参与本次数值计算。" );
  return QString();
}

QVariantMap ConstraintUseDecision::toVariant() const
{
  QVariantMap map;
  map.insert( QStringLiteral( "id" ), id );
  map.insert( QStringLiteral( "feature_id" ), featureId );
  map.insert( QStringLiteral( "stored_type" ), storedType );
  map.insert( QStringLiteral( "semantic" ), semantic );
  map.insert( QStringLiteral( "block_mode" ), blockMode );
  map.insert( QStringLiteral( "enabled" ), enabled );
  map.insert( QStringLiteral( "params_valid" ), paramsValid );
  map.insert( QStringLiteral( "geometry_valid" ), geometryValid );
  map.insert( QStringLiteral( "in_horizon" ), inHorizon );
  map.insert( QStringLiteral( "in_domain" ), inDomain );
  map.insert( QStringLiteral( "domain_known" ), domainKnown );
  map.insert( QStringLiteral( "supported" ), supported );
  map.insert( QStringLiteral( "consumed" ), consumed );
  map.insert( QStringLiteral( "cartographic_only" ), cartographicOnly );
  map.insert( QStringLiteral( "blocking" ), blocking );
  map.insert( QStringLiteral( "role" ), role );
  map.insert( QStringLiteral( "reason" ), reason );
  map.insert( QStringLiteral( "warnings" ), warnings );
  return map;
}

namespace
{

ConstraintUseDecision baseDecision( const QString &featureId, const QString &rowHorizon, const QString &storedType,
                                    const ConstraintSemantics &semantics, int partIndex, int partCount,
                                    const ConstraintMethodContext &context )
{
  ConstraintUseDecision d;
  d.featureId = featureId;
  d.id = partCount > 1 ? QStringLiteral( "%1#%2" ).arg( featureId ).arg( partIndex ) : featureId;
  d.storedType = storedType;
  d.semantic = semantics.semantic;
  d.blockMode = semantics.blockMode;
  d.enabled = semantics.enabled;
  d.paramsValid = semantics.valid;
  d.inHorizon = context.horizon.isEmpty() || rowHorizon == context.horizon;
  return d;
}

ConstraintUseDecision &reject( ConstraintUseDecision &d, const QString &reason )
{
  d.consumed = false;
  d.role.clear();
  d.reason = reason;
  return d;
}

ConstraintUseDecision &consume( ConstraintUseDecision &d, const QString &role )
{
  d.supported = true;
  d.consumed = true;
  d.role = role;
  d.reason.clear();
  return d;
}

// 语义/有效性已过关后的方法分派。
void decideByMethod( ConstraintUseDecision &d, const ConstraintSemantics &s, const QString &method,
                     const ConstraintMethodContext &context )
{
  const QString &sem = d.semantic;
  const bool cartographic = sem == QLatin1String( "contour_stop" ) || sem == QLatin1String( "cartographic_detour" );
  if ( method == QLatin1String( "kriging" ) || method == QLatin1String( "sgs" ) || method == QLatin1String( "cokriging" ) )
  {
    d.cartographicOnly = cartographic;
    reject( d, QStringLiteral( "method_does_not_read_line_constraints" ) );
    return;
  }
  if ( method == QLatin1String( "local_direction_idw" ) || method == QLatin1String( "local_direction_kriging" ) )
  {
    if ( sem == QLatin1String( "hard_barrier" ) )
    {
      d.supported = true;
      if ( d.domainKnown && !d.inDomain )
      {
        reject( d, QStringLiteral( "outside_compute_domain" ) );
        return;
      }
      consume( d, QStringLiteral( "hard_barrier" ) );
      return;
    }
    if ( sem == QLatin1String( "direction_guide" ) )
    {
      d.supported = true;
      if ( !std::isfinite( s.ratio ) || s.ratio < 1.0 || s.ratio > 100.0 )
      {
        d.blocking = true; // resolveParameters: directionRatio 必须在 [1,100]
        reject( d, QStringLiteral( "direction_ratio_out_of_range" ) );
        return;
      }
      if ( !( s.ratio > 1.0 ) )
      {
        reject( d, QStringLiteral( "direction_ratio_not_above_one" ) );
        return;
      }
      if ( d.domainKnown && !d.inDomain )
        d.warnings << QStringLiteral( "outside_compute_domain" );
      consume( d, QStringLiteral( "direction_guide" ) );
      return;
    }
    if ( sem == QLatin1String( "interpretive_boundary" ) )
    {
      d.supported = true;
      if ( !std::isfinite( s.softStrength ) || s.softStrength < 0.0 || s.softStrength > 0.8 )
      {
        d.blocking = true; // resolveParameters: softBoundaryStrength 必须在 [0,0.8]
        reject( d, QStringLiteral( "soft_strength_out_of_range" ) );
        return;
      }
      if ( !( s.softStrength > 0.0 ) )
      {
        reject( d, QStringLiteral( "soft_strength_zero" ) );
        return;
      }
      if ( d.domainKnown && !d.inDomain )
        d.warnings << QStringLiteral( "outside_compute_domain" );
      consume( d, QStringLiteral( "soft_boundary" ) );
      return;
    }
    d.cartographicOnly = true;
    reject( d, sem == QLatin1String( "contour_stop" ) ? QStringLiteral( "contour_stop_does_not_change_analysis" )
                                                       : QStringLiteral( "cartographic_detour_not_analysis_constraint" ) );
    return;
  }
  if ( method == QLatin1String( "surfer_idw" ) )
  {
    if ( sem == QLatin1String( "hard_barrier" ) )
    {
      // FaultPathMetric：有限线段增加绕行距离，不是无限隔断。
      consume( d, QStringLiteral( "hard_barrier" ) );
      d.warnings << QStringLiteral( "finite_fault_path_detour" );
      return;
    }
    if ( cartographic )
    {
      d.cartographicOnly = true;
      reject( d, QStringLiteral( "cartographic_only" ) );
      return;
    }
    reject( d, QStringLiteral( "unsupported_by_method" ) );
    return;
  }
  if ( method == QLatin1String( "structural_idw" ) )
  {
    // keepDisabled/keepNonBlockingBarriers 口径：hard_barrier 带原文 blockMode；
    // contour_stop/cartographic_detour → display_only 停线。
    QString mode = normalizeBlockModeToken( s.blockMode );
    if ( sem == QLatin1String( "contour_stop" ) || sem == QLatin1String( "cartographic_detour" ) )
      mode = isFullBlockModeToken( mode ) ? QStringLiteral( "display_only" ) : mode;
    if ( sem == QLatin1String( "hard_barrier" ) || cartographic )
    {
      d.supported = true;
      const bool full = sem == QLatin1String( "hard_barrier" ) && isFullBlockModeToken( mode );
      const bool stop = full || mode == QLatin1String( "display_only" );
      if ( full )
      {
        consume( d, QStringLiteral( "hard_barrier" ) );
        if ( !context.structuralEnableBarriers )
          reject( d, QStringLiteral( "barriers_disabled_by_request" ) );
        return;
      }
      if ( stop && context.structuralEnableBarriers && context.structuralInterpretiveStrengthPositive )
      {
        consume( d, QStringLiteral( "soft_boundary" ) );
        d.warnings << QStringLiteral( "display_only_as_interpretive_soft_boundary" );
        return;
      }
      if ( stop )
      {
        d.cartographicOnly = true;
        reject( d, QStringLiteral( "interpretive_strength_zero_or_barriers_disabled" ) );
        return;
      }
      reject( d, QStringLiteral( "non_blocking" ) );
      return;
    }
    if ( sem == QLatin1String( "direction_guide" ) )
    {
      d.supported = true;
      if ( !context.structuralEnableDirections )
      {
        reject( d, QStringLiteral( "directions_disabled_by_request" ) );
        return;
      }
      if ( d.domainKnown && !d.inDomain )
      {
        reject( d, QStringLiteral( "outside_compute_domain" ) ); // 方向线裁到成图域
        return;
      }
      consume( d, QStringLiteral( "direction_guide" ) );
      return;
    }
    if ( sem == QLatin1String( "interpretive_boundary" ) )
    {
      consume( d, QStringLiteral( "soft_boundary" ) );
      return;
    }
    reject( d, QStringLiteral( "unsupported_by_method" ) );
    return;
  }
  if ( method == QLatin1String( "legacy" ) )
  {
    if ( sem == QLatin1String( "hard_barrier" ) )
    {
      consume( d, QStringLiteral( "hard_barrier" ) );
      d.warnings << QStringLiteral( "also_clips_convex_hull" );
      return;
    }
    if ( sem == QLatin1String( "direction_guide" ) )
    {
      d.supported = true;
      if ( !( context.legacyAnisoRatio > 1.0 ) )
      {
        reject( d, QStringLiteral( "global_aniso_ratio_not_above_one" ) );
        return;
      }
      consume( d, QStringLiteral( "global_direction" ) );
      d.warnings << QStringLiteral( "per_line_ratio_radius_ignored" );
      return;
    }
    d.cartographicOnly = cartographic;
    reject( d, cartographic ? QStringLiteral( "cartographic_only" ) : QStringLiteral( "unsupported_by_method" ) );
    return;
  }
  reject( d, QStringLiteral( "unknown_method" ) );
}

} // namespace

ConstraintUseDecision decideConstraintPart( const QString &featureId, const QString &rowHorizon,
                                            const QString &storedType, const ConstraintSemantics &semantics,
                                            const QVector<QPointF> &part, int partIndex, int partCount,
                                            const ConstraintMethodContext &context )
{
  ConstraintUseDecision d = baseDecision( featureId, rowHorizon, storedType, semantics, partIndex, partCount, context );
  const QString method = canonicalOrSelf( context.methodId );
  if ( !d.inHorizon )
    return reject( d, QStringLiteral( "other_horizon" ) );
  if ( !semantics.valid )
  {
    d.blocking = methodRejectsInvalidParams( method );
    return reject( d, QStringLiteral( "invalid_params:%1" ).arg( semantics.error ) );
  }
  const ConstraintPartCheck check = checkConstraintPart( part );
  d.geometryValid = check.issue.isEmpty();
  if ( check.duplicateVertices )
    d.warnings << QStringLiteral( "duplicate_vertices" );
  if ( context.domain && d.geometryValid )
  {
    d.domainKnown = true;
    d.inDomain = polylineTouchesRect( part, *context.domain );
  }
  if ( semantics.semantic.isEmpty() )
  {
    // 旧 IDW：未知 type 的线照旧凸包裁剪（singlefactorrequest.h HullClip）。
    if ( method == QLatin1String( "legacy" ) && semantics.ignoredReason.startsWith( QLatin1String( "unknown_type" ) ) &&
         d.geometryValid )
    {
      d.supported = true;
      return consume( d, QStringLiteral( "hull_clip" ) );
    }
    return reject( d, semantics.ignoredReason.isEmpty() ? QStringLiteral( "ignored" ) : semantics.ignoredReason );
  }
  if ( !semantics.enabled )
    return reject( d, QStringLiteral( "disabled" ) );
  if ( !d.geometryValid )
    return reject( d, check.issue );
  decideByMethod( d, semantics, method, context );
  return d;
}

ConstraintUseDecision decideNonLineConstraint( const QString &featureId, const QString &rowHorizon,
                                               const QString &storedType, const ConstraintSemantics &semantics,
                                               bool emptyGeometry, const ConstraintMethodContext &context )
{
  ConstraintUseDecision d = baseDecision( featureId, rowHorizon, storedType, semantics, 0, 1, context );
  const QString method = canonicalOrSelf( context.methodId );
  if ( !d.inHorizon )
    return reject( d, QStringLiteral( "other_horizon" ) );
  if ( emptyGeometry )
    return reject( d, QStringLiteral( "empty_geometry" ) );
  // 旧 IDW 不判几何类型：未知 type 的点/面同样进凸包裁剪。
  if ( method == QLatin1String( "legacy" ) && semantics.semantic.isEmpty() &&
       semantics.ignoredReason.startsWith( QLatin1String( "unknown_type" ) ) )
  {
    d.geometryValid = true;
    d.supported = true;
    return consume( d, QStringLiteral( "hull_clip" ) );
  }
  if ( !semantics.valid && methodRejectsInvalidParams( method ) )
  {
    d.blocking = true;
    return reject( d, QStringLiteral( "invalid_params:%1" ).arg( semantics.error ) );
  }
  return reject( d, QStringLiteral( "not_line" ) );
}

QVariantMap summarizeConstraintDecisions( const QVector<ConstraintUseDecision> &decisions )
{
  QStringList consumed;
  QVariantList unused;
  QVariantMap roles;
  int blocking = 0;
  for ( const ConstraintUseDecision &d : decisions )
  {
    if ( d.consumed )
    {
      consumed << d.id;
      roles.insert( d.role, roles.value( d.role ).toInt() + 1 );
    }
    else
    {
      unused << QVariantMap{ { QStringLiteral( "id" ), d.id }, { QStringLiteral( "reason" ), d.reason } };
    }
    if ( d.blocking )
      ++blocking;
  }
  QVariantMap summary;
  summary.insert( QStringLiteral( "consumed" ), consumed );
  summary.insert( QStringLiteral( "unused" ), unused );
  summary.insert( QStringLiteral( "roles" ), roles );
  summary.insert( QStringLiteral( "blocking" ), blocking );
  return summary;
}

} // namespace paleo::singlefactor
