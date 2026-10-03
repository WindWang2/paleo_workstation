// 层：数据
#include "wellacquisition.h"

#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

namespace paleo::singlefactor
{
namespace
{
// 上游 _attr_value：`value not in (None, "")`——None 或空字符串视为缺失；
// 数值 0 / False 等其它类型均有效。
bool isMissing( const QVariant &value )
{
  if ( !value.isValid() || value.isNull() )
    return true;
  return value.metaType().id() == QMetaType::QString && value.toString().isEmpty();
}

QStringList attributeKeys( const FeatureAttributes &attributes )
{
  QStringList keys;
  for ( const auto &pair : attributes )
  {
    if ( !keys.contains( pair.first ) )
      keys.append( pair.first );
  }
  return keys;
}

QString factorDebugHint( const FeatureAttributes &attributes,
                         const WellAcquisitionRequest &request )
{
  // 上游 _factor_debug_hint（workflow.py L1648-1660）：可用字段按字典序前 8 个。
  const QStringList keys = attributeKeys( attributes );
  QStringList preview = keys.mid( 0, 8 );
  if ( keys.size() > 8 )
    preview.append( QStringLiteral( "..." ) );
  return QStringLiteral( "模式=%1; 指标字段=%2; 分子=%3; 分母=%4; 可用字段=%5" )
      .arg( request.factorMode.isEmpty() ? QStringLiteral( "direct" ) : request.factorMode )
      .arg( request.valueField )
      .arg( request.numeratorField )
      .arg( request.denominatorField )
      .arg( preview.join( QStringLiteral( ", " ) ) );
}

std::optional<double> parseNumericText( const QString &textIn )
{
  QString text = textIn.trimmed();
  if ( text.isEmpty() )
    return std::nullopt;
  text.remove( QLatin1Char( ',' ) );
  if ( text.endsWith( QLatin1Char( '%' ) ) )
    text.chop( 1 );
  bool ok = false;
  const double numeric = text.toDouble( &ok );
  if ( !ok || !std::isfinite( numeric ) )
    return std::nullopt;
  return numeric;
}
} // namespace

QVariant attrValue( const FeatureAttributes &attributes,
                    std::initializer_list<QString> names )
{
  for ( const QString &name : names )
  {
    if ( name.isEmpty() )
      continue;
    for ( const auto &pair : attributes )
    {
      if ( pair.first == name && !isMissing( pair.second ) )
        return pair.second;
    }
  }
  // 大小写不敏感回退：lower() 映射首个声明键，命中后同样过滤缺失值。
  for ( const QString &name : names )
  {
    if ( name.isEmpty() )
      continue;
    const QString lowered = name.toLower();
    for ( const auto &pair : attributes )
    {
      if ( pair.first.toLower() == lowered && !isMissing( pair.second ) )
        return pair.second;
    }
  }
  return QVariant();
}

std::optional<double> parseNumeric( const QVariant &value )
{
  if ( !value.isValid() || value.isNull() )
    return std::nullopt;
  const int typeId = value.metaType().id();
  if ( typeId == QMetaType::Bool || typeId == QMetaType::Int ||
       typeId == QMetaType::UInt || typeId == QMetaType::LongLong ||
       typeId == QMetaType::ULongLong || typeId == QMetaType::Float ||
       typeId == QMetaType::Double )
  {
    const double numeric = value.toDouble();
    if ( !std::isfinite( numeric ) )
      return std::nullopt;
    return numeric;
  }
  return parseNumericText( value.toString() );
}

bool toBool( const QVariant &value, bool defaultValue )
{
  if ( isMissing( value ) )
    return defaultValue;
  const int typeId = value.metaType().id();
  if ( typeId == QMetaType::Bool )
    return value.toBool();
  if ( typeId == QMetaType::Int || typeId == QMetaType::UInt ||
       typeId == QMetaType::LongLong || typeId == QMetaType::ULongLong ||
       typeId == QMetaType::Float || typeId == QMetaType::Double )
  {
    // 上游 not math.isclose(v, 0.0)：默认容差下等价于 v != 0
    return value.toDouble() != 0.0;
  }
  const QString text = value.toString().trimmed().toLower();
  static const QSet<QString> truthy = {
      QStringLiteral( "1" ), QStringLiteral( "true" ), QStringLiteral( "yes" ),
      QStringLiteral( "y" ), QStringLiteral( "启用" ), QStringLiteral( "是" ) };
  static const QSet<QString> falsy = {
      QStringLiteral( "0" ), QStringLiteral( "false" ), QStringLiteral( "no" ),
      QStringLiteral( "n" ), QStringLiteral( "停用" ), QStringLiteral( "否" ) };
  if ( truthy.contains( text ) )
    return true;
  if ( falsy.contains( text ) )
    return false;
  return defaultValue;
}

double toFloat( const QVariant &value, double defaultValue )
{
  const std::optional<double> parsed = parseNumeric( value );
  return parsed ? *parsed : defaultValue;
}

int toInt( const QVariant &value, int defaultValue )
{
  const std::optional<double> parsed = parseNumeric( value );
  // 上游 int(round(x))：round 为银行家舍入（half-even）
  return parsed ? static_cast<int>( std::nearbyint( *parsed ) ) : defaultValue;
}

bool pointOnSegment( Point2 point, Point2 a, Point2 b, double tolerance )
{
  const double cross = ( point.x - a.x ) * ( b.y - a.y ) -
                       ( point.y - a.y ) * ( b.x - a.x );
  if ( std::abs( cross ) > tolerance )
    return false;
  const double dot = ( point.x - a.x ) * ( point.x - b.x ) +
                     ( point.y - a.y ) * ( point.y - b.y );
  return dot <= tolerance;
}

bool pointInRing( Point2 point, const Ring &ring )
{
  const std::vector<Point2> &pts = ring.points;
  if ( pts.size() < 3 )
    return false;
  for ( std::size_t i = 0; i < pts.size(); ++i )
  {
    if ( pointOnSegment( point, pts[i], pts[( i + 1 ) % pts.size()] ) )
      return true;
  }
  const double x = point.x;
  const double y = point.y;
  bool inside = false;
  std::size_t j = pts.size() - 1;
  for ( std::size_t i = 0; i < pts.size(); ++i )
  {
    const double xi = pts[i].x, yi = pts[i].y;
    const double xj = pts[j].x, yj = pts[j].y;
    if ( ( yi > y ) != ( yj > y ) )
    {
      const double denom = yj - yi;
      if ( std::abs( denom ) > 1e-30 )
      {
        const double xIntersect = ( xj - xi ) * ( y - yi ) / denom + xi;
        if ( x < xIntersect )
          inside = !inside;
      }
    }
    j = i;
  }
  return inside;
}

bool pointInBoundary( Point2 point, const Polygon &boundary )
{
  if ( !pointInRing( point, boundary.exterior ) )
    return false;
  for ( const Ring &hole : boundary.holes )
  {
    if ( pointInRing( point, hole ) )
      return false;
  }
  return true;
}

bool pointInBoundaryBbox( Point2 point, const Polygon &boundary, double marginRatio )
{
  if ( boundary.exterior.points.empty() )
    return false;
  double minx = std::numeric_limits<double>::max();
  double maxx = std::numeric_limits<double>::lowest();
  double miny = std::numeric_limits<double>::max();
  double maxy = std::numeric_limits<double>::lowest();
  for ( const Point2 &p : boundary.exterior.points )
  {
    minx = std::min( minx, p.x );
    maxx = std::max( maxx, p.x );
    miny = std::min( miny, p.y );
    maxy = std::max( maxy, p.y );
  }
  const double mx = ( maxx - minx ) * std::max( marginRatio, 0.0 );
  const double my = ( maxy - miny ) * std::max( marginRatio, 0.0 );
  return ( minx - mx ) <= point.x && point.x <= ( maxx + mx ) &&
         ( miny - my ) <= point.y && point.y <= ( maxy + my );
}

double pointDistanceToRing( Point2 point, const Ring &ring )
{
  const std::vector<Point2> &pts = ring.points;
  if ( pts.size() < 2 )
    return std::numeric_limits<double>::infinity();
  double best = std::numeric_limits<double>::infinity();
  const double px = point.x, py = point.y;
  const std::size_t n = pts.size();
  for ( std::size_t i = 0; i < n; ++i )
  {
    const Point2 &a = pts[i];
    const Point2 &b = pts[( i + 1 ) % n];
    const double vx = b.x - a.x, vy = b.y - a.y;
    const double ll = vx * vx + vy * vy;
    double d;
    if ( ll <= 1e-24 )
    {
      d = std::hypot( px - a.x, py - a.y );
    }
    else
    {
      const double t =
          std::max( 0.0, std::min( 1.0, ( ( px - a.x ) * vx + ( py - a.y ) * vy ) / ll ) );
      d = std::hypot( px - ( a.x + t * vx ), py - ( a.y + t * vy ) );
    }
    if ( d < best )
      best = d;
  }
  return best;
}

std::optional<double> resolveDirectFactorValue( const FeatureAttributes &attributes,
                                                const WellAcquisitionRequest &request )
{
  const QVariant raw = attrValue( attributes,
                                  { request.valueField,
                                    request.factorName,
                                    QStringLiteral( "factor_value" ),
                                    QStringLiteral( "value" ),
                                    QStringLiteral( "sand_ratio" ),
                                    QStringLiteral( "sand_rati" ),
                                    QStringLiteral( "sandratio" ),
                                    QStringLiteral( "raw_sand_ratio" ),
                                    QStringLiteral( "raw_sand" ),
                                    QStringLiteral( "sand_thk" ),
                                    QStringLiteral( "sand_thick" ),
                                    QStringLiteral( "sandthick" ),
                                    QStringLiteral( "thickness" ),
                                    QStringLiteral( "thk" ),
                                    QStringLiteral( "h_sand" ),
                                    QStringLiteral( "sand_h" ),
                                    QStringLiteral( "砂厚" ),
                                    QStringLiteral( "砂岩厚度" ),
                                    QStringLiteral( "厚度" ),
                                    QStringLiteral( "有效厚度" ),
                                    QStringLiteral( "stratum_thk" ),
                                    QStringLiteral( "layer_thk" ) } );
  // 上游尾部的 _first_numeric_factor_value 兜底被有意移除：
  // 它会借用 OCR_CONF 等无关数值列冒充因素值。
  return parseNumeric( raw );
}

std::optional<double> resolveCurrentFactorValue( const FeatureAttributes &attributes,
                                                 const WellAcquisitionRequest &request )
{
  std::optional<double> value;
  if ( request.factorMode == QLatin1String( "ratio" ) )
  {
    const std::optional<double> numerator =
        parseNumeric( attrValue( attributes, { request.numeratorField } ) );
    const std::optional<double> denominator =
        parseNumeric( attrValue( attributes, { request.denominatorField } ) );
    if ( numerator && denominator && *denominator != 0.0 )
      value = *numerator / *denominator;
    else
      value = resolveDirectFactorValue( attributes, request );
  }
  else
  {
    value = resolveDirectFactorValue( attributes, request );
  }
  if ( !value )
    return std::nullopt;
  // 值域裁剪：仅对「确实落在 0–1 附近」的比值类数据生效。
  // 厚度类（如 102、279）若仍按 0–1 硬裁会全部变成 1.0，趋势面一片平、像“井没参与”。
  const std::optional<double> vmin = request.valueMin;
  const std::optional<double> vmax = request.valueMax;
  if ( vmin && vmax )
  {
    const double lo = *vmin;
    const double hi = *vmax;
    if ( hi <= 1.0 + 1e-6 && lo >= -1e-9 )
    {
      // 物理量纲明显不是比值，跳过 0–1 裁剪
      if ( *value > 1.5 || *value < -0.05 )
        return *value;
    }
    value = std::max( lo, std::min( hi, *value ) );
  }
  else if ( vmin )
  {
    value = std::max( *vmin, *value );
  }
  else if ( vmax )
  {
    value = std::min( *vmax, *value );
  }
  return *value;
}

WellAcquisitionResult acquireWells( const std::vector<WellFeatureRow> &rows,
                                    const std::vector<Polygon> &boundaries,
                                    const WellAcquisitionRequest &request )
{
  WellAcquisitionResult result;

  const auto strictInside = [&boundaries]( Point2 p ) {
    if ( boundaries.empty() )
      return true;
    for ( const Polygon &boundary : boundaries )
    {
      if ( pointInBoundary( p, boundary ) )
        return true;
    }
    return false;
  };
  const auto softInside = [&boundaries, &strictInside]( Point2 p ) {
    if ( boundaries.empty() )
      return true;
    if ( strictInside( p ) )
      return true;
    // 外包框 5% 边距（自交多边形时 PIP 常失败，但井仍在图幅内）
    for ( const Polygon &boundary : boundaries )
    {
      if ( pointInBoundaryBbox( p, boundary, 0.05 ) )
        return true;
    }
    // 距边界折线很近（贴边井）
    double diag = 0.0;
    for ( const Polygon &boundary : boundaries )
    {
      if ( boundary.exterior.points.empty() )
        continue;
      double minx = std::numeric_limits<double>::max();
      double maxx = std::numeric_limits<double>::lowest();
      double miny = std::numeric_limits<double>::max();
      double maxy = std::numeric_limits<double>::lowest();
      for ( const Point2 &q : boundary.exterior.points )
      {
        minx = std::min( minx, q.x );
        maxx = std::max( maxx, q.x );
        miny = std::min( miny, q.y );
        maxy = std::max( maxy, q.y );
      }
      diag = std::max( diag, std::hypot( maxx - minx, maxy - miny ) );
    }
    const double near = std::max( diag * 0.008, 1.0 );
    for ( const Polygon &boundary : boundaries )
    {
      if ( pointDistanceToRing( p, boundary.exterior ) <= near )
        return true;
    }
    return false;
  };

  for ( std::size_t index = 0; index < rows.size(); ++index )
  {
    const WellFeatureRow &feature = rows[index];
    if ( !feature.isPoint )
      continue;
    const FeatureAttributes &attrs = feature.attributes;
    const QVariant idRaw = attrValue( attrs,
                                      { request.wellIdField,
                                        QStringLiteral( "well_name" ),
                                        QStringLiteral( "wellname" ),
                                        QStringLiteral( "井名" ),
                                        QStringLiteral( "井号" ),
                                        QStringLiteral( "well_id" ),
                                        QStringLiteral( "Well_ID" ),
                                        QStringLiteral( "WellName" ) } );
    const QString wellId = isMissing( idRaw )
                               ? QStringLiteral( "WELL_%1" ).arg( index + 1 )
                               : idRaw.toString();

    const std::optional<double> value = resolveCurrentFactorValue( attrs, request );
    if ( !value || !std::isfinite( *value ) )
    {
      result.skipped.append(
          QStringLiteral( "%1: 指标值无效(%2)" )
              .arg( wellId, factorDebugHint( attrs, request ) ) );
      continue;
    }

    // 优先几何坐标；若属性表有 x/y 且与几何差异巨大，尝试属性坐标（防导入错位）
    Point2 chosen{ feature.x, feature.y };
    const std::optional<double> ax = parseNumeric(
        attrValue( attrs, { QStringLiteral( "x" ), QStringLiteral( "X" ),
                            QStringLiteral( "lon" ), QStringLiteral( "longitude" ),
                            QStringLiteral( "east" ), QStringLiteral( "east_x" ) } ) );
    const std::optional<double> ay = parseNumeric(
        attrValue( attrs, { QStringLiteral( "y" ), QStringLiteral( "Y" ),
                            QStringLiteral( "lat" ), QStringLiteral( "latitude" ),
                            QStringLiteral( "north" ), QStringLiteral( "north_y" ) } ) );
    const bool hasAttrPoint = ax && ay;
    const Point2 attrPoint{ ax ? *ax : 0.0, ay ? *ay : 0.0 };

    bool strict = strictInside( chosen );
    if ( !strict && hasAttrPoint )
    {
      if ( strictInside( attrPoint ) ||
           ( !softInside( chosen ) && softInside( attrPoint ) ) )
      {
        chosen = attrPoint;
        strict = strictInside( chosen );
      }
    }

    const bool isControl =
        toBool( attrValue( attrs, { QStringLiteral( "is_ctrl" ),
                                    QStringLiteral( "is_control_point" ),
                                    QStringLiteral( "control" ),
                                    QStringLiteral( "控制点" ) } ),
                false );

    if ( strict )
    {
      result.wells.push_back(
          AcquiredWell{ wellId.toStdString(), chosen.x, chosen.y, *value, isControl } );
      continue;
    }
    if ( softInside( chosen ) )
    {
      result.wells.push_back(
          AcquiredWell{ wellId.toStdString(), chosen.x, chosen.y, *value, isControl } );
      result.softIncluded += 1;
      continue;
    }
    result.skipped.append( QStringLiteral( "%1: 边界外，已剔除" ).arg( wellId ) );
  }

  if ( result.softIncluded > 0 )
  {
    result.skipped.append(
        QStringLiteral( "提示: 有 %1 口井按「贴边/外包框」软纳入参与插值"
                        "（严格点-in-面失败，常见于边界自交或贴边）。" )
            .arg( result.softIncluded ) );
  }
  return result;
}

ResolvedValueRange resolveValueRangeForWells( const std::vector<AcquiredWell> &wells,
                                              const std::optional<double> &requestMin,
                                              const std::optional<double> &requestMax,
                                              const std::vector<double> &gridValues )
{
  // 上游 _resolve_value_range_for_wells（workflow.py L691-742）：井观测与
  // 网格值合并的数据范围；声明为 0~1 但数据明显超出时放弃声明范围。
  std::vector<double> values;
  values.reserve( wells.size() );
  for ( const AcquiredWell &well : wells )
  {
    if ( std::isfinite( well.value ) )
      values.push_back( well.value );
  }
  std::optional<double> gmin, gmax;
  {
    bool any = false;
    double lo = 0.0, hi = 0.0;
    for ( const double v : gridValues )
    {
      if ( !std::isfinite( v ) )
        continue;
      if ( !any )
      {
        lo = hi = v;
        any = true;
      }
      else
      {
        lo = std::min( lo, v );
        hi = std::max( hi, v );
      }
    }
    if ( any )
    {
      gmin = lo;
      gmax = hi;
    }
  }

  if ( values.empty() && !gmin )
    return ResolvedValueRange{ requestMin, requestMax };

  const auto vecMin = []( const std::vector<double> &v ) {
    return *std::min_element( v.begin(), v.end() );
  };
  const auto vecMax = []( const std::vector<double> &v ) {
    return *std::max_element( v.begin(), v.end() );
  };

  double dataMin = !values.empty() ? vecMin( values ) : *gmin;
  double dataMax = !values.empty() ? vecMax( values ) : *gmax;
  if ( !values.empty() && gmin )
  {
    dataMin = std::min( dataMin, *gmin );
    dataMax = std::max( dataMax, *gmax );
  }

  const std::optional<double> rmin = requestMin;
  const std::optional<double> rmax = requestMax;

  // 声明范围与数据冲突：数据超出声明上界（常见：厚度用了 0~1 比值裁剪）
  if ( rmin && rmax && *rmax > *rmin )
  {
    const double span = *rmax - *rmin;
    const double tolerance = std::max( 0.01 * span, 1e-3 );
    if ( dataMax > *rmax + tolerance || dataMin < *rmin - tolerance )
      return ResolvedValueRange{ dataMin, dataMax };
    return ResolvedValueRange{ rmin, rmax };
  }

  if ( !rmin && !rmax )
    return ResolvedValueRange{ dataMin, dataMax };
  if ( !rmin )
    return ResolvedValueRange{ dataMin, rmax };
  if ( !rmax )
    return ResolvedValueRange{ rmin, dataMax };
  return ResolvedValueRange{ rmin, rmax };
}

} // namespace paleo::singlefactor
