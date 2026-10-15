// 层：功能
#include "constraintuseplan.h"

#include <qgsgeometry.h>
#include <qgswkbtypes.h>

namespace paleo::singlefactor
{

namespace
{

// d 来自缺省语义选项 + 无域的本地方向上下文：只用于方法无关的「可用」口径。
void countAvailability( ConstraintUsePlan &plan, const ConstraintUseDecision &d )
{
  if ( !d.inHorizon )
    return;
  if ( !d.paramsValid )
  {
    ++plan.invalid;
    return;
  }
  if ( d.reason == QLatin1String( "disabled" ) )
  {
    ++plan.disabled;
    return;
  }
  if ( d.semantic.isEmpty() )
    return; // non_blocking / unknown_type：不是可用约束
  if ( !d.geometryValid )
  {
    if ( d.reason != QLatin1String( "not_line" ) )
      ++plan.invalid; // short_line / zero_length / self_intersecting / non_finite / empty
    return;
  }
  if ( d.semantic == QLatin1String( "hard_barrier" ) )
    ++plan.availableBarriers; // 缺省选项下 hard_barrier 必为完全阻断
  else if ( d.semantic == QLatin1String( "direction_guide" ) )
    ++plan.availableDirections;
  else if ( d.semantic == QLatin1String( "interpretive_boundary" ) )
    ++plan.availableSoft;
  else
    ++plan.availableCartographic;
}

} // namespace

ConstraintUsePlan planConstraintUse( const QVector<QVariantMap> &rows, const ConstraintMethodContext &context )
{
  ConstraintUsePlan plan;
  plan.capabilityNote = methodConstraintCapabilityNote( context.methodId );
  const ConstraintSemanticsOptions options = semanticsOptionsForMethod( context.methodId );
  // 「可用」计数与方法无关：用缺省选项再解析一次（结构面选项保留停用/非阻断原文）。
  const bool sameOptions = !options.keepDisabled && !options.keepNonBlockingBarriers;
  ConstraintMethodContext availabilityContext;
  availabilityContext.methodId = QStringLiteral( "local_direction_idw" );
  availabilityContext.horizon = context.horizon;
  for ( const QVariantMap &row : rows )
  {
    const QString id = row.value( QStringLiteral( "id" ) ).toString();
    const QString horizon = row.value( QStringLiteral( "horizon" ) ).toString();
    const QString type = row.value( QStringLiteral( "type" ) ).toString().trimmed();
    const QString json = row.value( QStringLiteral( "params_json" ) ).toString();
    const QVariant schema = row.value( QStringLiteral( "schema_version" ) );
    const ConstraintSemantics semantics = resolveConstraintSemantics( type, json, schema, options, id );
    const ConstraintSemantics plain =
        sameOptions ? semantics : resolveConstraintSemantics( type, json, schema, {}, id );
    const QgsGeometry geometry = QgsGeometry::fromWkt( row.value( QStringLiteral( "wkt" ) ).toString() );
    if ( geometry.isNull() || geometry.isEmpty() ||
         QgsWkbTypes::geometryType( geometry.wkbType() ) != Qgis::GeometryType::Line )
    {
      const bool empty = geometry.isNull() || geometry.isEmpty();
      plan.decisions << decideNonLineConstraint( id, horizon, type, semantics, empty, context );
      countAvailability( plan, decideNonLineConstraint( id, horizon, type, plain, empty, availabilityContext ) );
      continue;
    }
    const QgsMultiPolylineXY parts =
        geometry.isMultipart() ? geometry.asMultiPolyline() : QgsMultiPolylineXY{ geometry.asPolyline() };
    for ( int k = 0; k < parts.size(); ++k )
    {
      QVector<QPointF> vertices;
      vertices.reserve( parts.at( k ).size() );
      for ( const QgsPointXY &point : parts.at( k ) )
        vertices.append( point.toQPointF() );
      const int count = static_cast<int>( parts.size() );
      plan.decisions << decideConstraintPart( id, horizon, type, semantics, vertices, k, count, context );
      countAvailability( plan, decideConstraintPart( id, horizon, type, plain, vertices, k, count, availabilityContext ) );
    }
  }
  for ( const ConstraintUseDecision &d : plan.decisions )
  {
    if ( d.consumed )
      ++plan.consumed;
    if ( d.blocking )
      ++plan.blocking;
  }
  plan.summary = summarizeConstraintDecisions( plan.decisions );
  return plan;
}

} // namespace paleo::singlefactor
