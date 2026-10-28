// 层：功能
#include "constraintuseplan.h"

#include <qgsgeometry.h>
#include <qgswkbtypes.h>

#include <QObject>

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
    // 与三个引擎读者同一 id 口径（constraintparse/constraintbarriers/
    // paleoalgorithms：id 列去空白，空则 fid）——手工编辑/外部 GPKG 的行
    // 也能让回执 id 与 UI 列表 join 上（PR457-closure R5）。
    QString id = row.value( QStringLiteral( "id" ) ).toString().trimmed();
    if ( id.isEmpty() )
    {
      const qlonglong fid = row.value( QStringLiteral( "fid" ) ).toLongLong();
      id = fid > 0 ? QString::number( fid ) : QStringLiteral( "?" );
    }
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
      plan.partGeometries << QgsGeometry();
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
      plan.partGeometries << QgsGeometry::fromPolylineXY( parts.at( k ) );
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

ConstraintReadiness assessConstraintReadiness( const ConstraintUsePlan &plan )
{
  ConstraintReadiness r;
  const int available = plan.availableBarriers + plan.availableDirections + plan.availableSoft;
  // 停用约束是「线在、但被显式排除」的事实：任何文案都不得反称「没有约束线」
  // ——否则用户画了线却被告知没有（plan.disabled 一直无人消费，#187）。
  const QString disabledNote = plan.disabled > 0
      ? QObject::tr( "另有 %1 条约束线已停用，不参与计算。" ).arg( plan.disabled )
      : QString();
  if ( plan.blocking > 0 )
  {
    r.state = ConstraintReadinessState::RequiredUnmet;
    r.runAllowed = false;
    r.message = QObject::tr( "%1 条约束参数越界，当前方法会拒绝运行；请修正或停用这些约束，或改用不读取约束的方法。" )
                    .arg( plan.blocking );
  }
  else if ( plan.consumed > 0 )
  {
    r.state = ConstraintReadinessState::Consumed;
    r.message = QObject::tr( "当前方法将使用 %1 条约束。" ).arg( plan.consumed );
    if ( plan.invalid > 0 )
      r.message += QObject::tr( "%1 条无效约束不参与计算。" ).arg( plan.invalid );
    if ( !disabledNote.isEmpty() )
      r.message += disabledNote;
  }
  else if ( available > 0 )
  {
    r.state = ConstraintReadinessState::PresentUnused;
    r.message = QObject::tr( "本层位有 %1 条可用约束，但当前算法未使用这些约束；可直接按井点计算。" ).arg( available );
    if ( !disabledNote.isEmpty() )
      r.message += disabledNote;
  }
  else if ( plan.invalid > 0 )
  {
    r.state = ConstraintReadinessState::InvalidExcluded;
    r.message = QObject::tr( "%1 条约束无效，不参与计算；可直接按井点计算。" ).arg( plan.invalid );
    if ( !disabledNote.isEmpty() )
      r.message += disabledNote;
  }
  else if ( plan.disabled > 0 )
  {
    // 只剩停用约束：如实说「有线但已停用」，不宣称「没有约束线」。
    r.state = ConstraintReadinessState::NoneRunnable;
    r.message = QObject::tr( "本层位有 %1 条约束线已停用，不参与计算；按井点直接计算。" )
                    .arg( plan.disabled );
  }
  else
  {
    r.message = plan.availableCartographic > 0
                    ? QObject::tr( "本层位没有参与数值计算的约束线（%1 条仅影响制图）；按井点直接计算。" )
                          .arg( plan.availableCartographic )
                    : QObject::tr( "本层位没有约束线，按井点直接计算；如需考虑断层或展布方向可再绘制。" );
  }
  return r;
}

} // namespace paleo::singlefactor
