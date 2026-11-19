// 层：功能
#include "constraintanalysisidentity.h"

#include "services/analysisunitcontext.h"
#include "services/subintervalfactors.h"
#include "catalog/datacatalog.h"
#include "workflow/capabilitygateway.h"

#include <utility>

namespace paleo::constraint_identity
{

QVariantMap FrozenAnalysisIdentity::publishIdentity() const
{
  QVariantMap m;
  if( !analysisLayerKey.isEmpty() )
  {
    m.insert( QStringLiteral( "source_horizon" ), analysisLayerKey );
    m.insert( QStringLiteral( "horizon" ), analysisLayerKey );
  }
  if( !hasUnit || !unit.isValid() )
    return m;
  m.insert( QStringLiteral( "unitSelection" ), paleo::strat::toMap( unit ) );
  if( !unit.subIntervalId.isEmpty() )
    m.insert( QStringLiteral( "subIntervalId" ), unit.subIntervalId );
  if( !unit.intervalId.isEmpty() )
    m.insert( QStringLiteral( "intervalId" ), unit.intervalId );
  if( !unit.schemeId.isEmpty() )
    m.insert( QStringLiteral( "schemeId" ), unit.schemeId );
  if( !unit.revision.isEmpty() )
    m.insert( QStringLiteral( "schemeRevision" ), unit.revision );
  m.insert( QStringLiteral( "aggregate" ), unit.aggregate );
  return m;
}

void FrozenAnalysisIdentity::enrichParams( QVariantMap &params ) const
{
  if( hasUnit && unit.isValid() )
    params.insert( QStringLiteral( "unitSelection" ), paleo::strat::toMap( unit ) );
  if( !analysisLayerKey.isEmpty() )
    params.insert( QStringLiteral( "analysisLayerKey" ), analysisLayerKey );
  if( !constraintHorizon.isEmpty() )
    params.insert( QStringLiteral( "constraintHorizon" ), constraintHorizon );
}

bool projectHasAnalysisScheme( const QString &projectDir )
{
  if( projectDir.isEmpty() )
    return false;
  QVector<paleo::strat::SchemeChoice> schemes;
  return paleo::strat::listProjectSchemesFromDir( projectDir, schemes, nullptr ) && !schemes.isEmpty();
}

bool callerParentVersionsExist( DataCatalog *catalog, const QStringList &parentVersionIds,
                                QString *error )
{
  for( const QString &id : parentVersionIds )
  {
    if( id.isEmpty() || !catalog || catalog->versionById( id ).id != id )
    {
      if( error )
        *error = QStringLiteral( "任务携带的父版本 %1 不存在或不可读，已拒绝" ).arg( id );
      return false;
    }
  }
  return true;
}

QVariantMap ConstraintIdwFrozenContext::identityInputs() const
{
  QVariantMap m;
  m.insert( QStringLiteral( "subinterval_id" ), unit.subIntervalId );
  QVariantMap selection = paleo::strat::toMap( unit );
  if( !selection.isEmpty() )
    m.insert( QStringLiteral( "unit_selection" ), selection );
  m.insert( QStringLiteral( "scheme_id" ), unit.schemeId );
  m.insert( QStringLiteral( "scheme_revision" ), unit.revision );
  m.insert( QStringLiteral( "interval_id" ), unit.intervalId );
  if( !parentVersionIds.isEmpty() )
    m.insert( QStringLiteral( "parent_version_ids" ), parentVersionIds );
  return m;
}

QVariantMap ConstraintIdwFrozenContext::publishExtra() const
{
  QVariantMap m;
  m.insert( QStringLiteral( "unitSelection" ), paleo::strat::toMap( unit ) );
  m.insert( QStringLiteral( "subIntervalId" ), unit.subIntervalId );
  m.insert( QStringLiteral( "intervalId" ), unit.intervalId );
  m.insert( QStringLiteral( "schemeId" ), unit.schemeId );
  m.insert( QStringLiteral( "schemeRevision" ), unit.revision );
  m.insert( QStringLiteral( "aggregate" ), unit.aggregate );
  m.insert( QStringLiteral( "algorithm_id" ), QStringLiteral( "paleo:paleo_constraint_idw" ) );
  m.insert( QStringLiteral( "kind" ), QStringLiteral( "single_factor_raster" ) );
  m.insert( QStringLiteral( "subinterval_verified" ), true );
  return m;
}

QString resolveManagedVersionRef( DataCatalog *catalog, const QString &reference ){
  const QString ref = reference.trimmed();
  if( ref.isEmpty() || !catalog )
    return {};
  if( ref.startsWith( QStringLiteral( "catalog:" ) ) )
  {
    const QString id = ref.mid( 8 );
    return catalog->versionById( id ).id == id ? id : QString();
  }
  // 层键精确匹配：版本 extra 的 manifest_layer_id / layer_id 与引用一致。
  // 只认当前版本（消费的是「现在仍指向的」受管产物，不把历史候选全算父）。
  for( const auto &asset : catalog->assets() )
  {
    const auto v = catalog->currentVersion( asset.id );
    if( v.id.isEmpty() )
      continue;
    if( v.extra.value( QStringLiteral( "manifest_layer_id" ) ).toString() == ref ||
        v.extra.value( QStringLiteral( "layer_id" ) ).toString() == ref )
      return v.id;
  }
  return {};
}

QString freezeConstraintIdwContext( const QString &projectDir, DataCatalog *catalog,
                                    const paleo::strat::UnitSelection &activeUnit,
                                    const QString &requestedSubinterval,
                                    const QString &pointsRef, const QString &constraintsRef,
                                    ConstraintIdwFrozenContext &out )
{
  out = {};
  if( !activeUnit.isValid() )
    return QStringLiteral( "constraint_idw_requires_unit" );
  paleo::strat::UnitSelection frozen;
  QString detail;
  if( !paleo::strat::freezeAnalysisUnit( projectDir, activeUnit, frozen, &detail,
                                         /*allowAggregate=*/false ) )
    return QStringLiteral( "constraint_idw_unit_invalid" );
  const QString requested = requestedSubinterval.trimmed();
  if( !requested.isEmpty() && requested != frozen.subIntervalId )
    return QStringLiteral( "subinterval_mismatch" );
  out.unit = frozen;
  const QString points = resolveManagedVersionRef( catalog, pointsRef );
  if( !points.isEmpty() )
    out.parentVersionIds.append( points );
  const QString constraints = resolveManagedVersionRef( catalog, constraintsRef );
  if( !constraints.isEmpty() && constraints != points )
    out.parentVersionIds.append( constraints );
  out.parentVersionIds.removeDuplicates();
  return {};
}

QString freezeConstraintIdwRequest( ExecuteRequest &request, const QString &projectDir,
                                    DataCatalog *catalog,
                                    const paleo::strat::UnitSelection &activeUnit )
{
  ConstraintIdwFrozenContext frozen;
  const QString code = freezeConstraintIdwContext(
      projectDir, catalog, activeUnit,
      request.inputs.value( QStringLiteral( "subinterval_id" ) ).toString(),
      request.inputs.value( QStringLiteral( "points_ref" ) ).toString(),
      request.inputs.value( QStringLiteral( "constraints_ref" ) ).toString(), frozen );
  if( !code.isEmpty() )
    return code;
  const QVariantMap identity = frozen.identityInputs();
  for( auto it = identity.cbegin(); it != identity.cend(); ++it )
    request.inputs.insert( it.key(), it.value() );
  // 调用方自带的 parent_version_ids 一律丢弃：父版本只认宿主解析结果
  //（工具 JSON 不自称版本授权）。
  if( !frozen.parentVersionIds.isEmpty() )
    request.inputs.insert( QStringLiteral( "parent_version_ids" ), frozen.parentVersionIds );
  else
    request.inputs.remove( QStringLiteral( "parent_version_ids" ) );
  return {};
}

bool resolveAnalysisIdentity( const QString &projectDir, const QString &horizon,
                              const QVariantMap &params, AggregatePolicy aggregatePolicy,
                              FrozenAnalysisIdentity &out, QString *error )
{
  out = {};
  const auto fail = [error]( const QString &reason ) {
    if( error )
      *error = reason;
    return false;
  };
  const auto requested = paleo::strat::fromMap( params.value( QStringLiteral( "unitSelection" ) ).toMap() );
  if( !params.value( QStringLiteral( "unitSelection" ) ).toMap().isEmpty() )
  {
    if( !requested.isValid() )
      return fail( QStringLiteral( "分析单元身份不完整：缺少方案/单元/子层段必填字段" ) );
    paleo::strat::UnitSelection frozen;
    if( !paleo::strat::freezeAnalysisUnit( projectDir, requested, frozen, error,
                                           aggregatePolicy == AggregatePolicy::Allow ) )
    {
      if( error && error->isEmpty() )
        *error = QStringLiteral( "分析单元身份核验失败" );
      return false;
    }
    out.unit = frozen;
    out.hasUnit = true;
    out.analysisLayerKey = paleo::strat::analysisLayerKey( frozen );
    if( out.analysisLayerKey.isEmpty() )
      return fail( QStringLiteral( "分析单元身份核验失败：无法导出稳定成果键" ) );
    // 约束/井点编辑键优先取方案派生的父单元上界面；调用方自带的
    // constraintHorizon 仅作补充（freezeAnalysisUnit 后者非空，一般用不到）。
    out.constraintHorizon = !frozen.topBoundary.isEmpty() ? frozen.topBoundary
                                                          : params.value( QStringLiteral( "constraintHorizon" ) ).toString();
    if( out.constraintHorizon.isEmpty() )
      out.constraintHorizon = horizon.trimmed();
    return true;
  }
  // 无 UnitSelection：工程已有方案 → 正式分析必须先选单元，界面名不可驱动。
  if( projectHasAnalysisScheme( projectDir ) )
    return fail( QStringLiteral( "单因素/插值分析须先在层段选择器选定具体子层段（S1–S4）；层位名不可驱动正式成果" ) );
  // legacy 工程（无任何已提交方案，不存在可选单元）：成果键回落入口 horizon。
  out.hasUnit = false;
  out.analysisLayerKey = horizon.trimmed();
  const QString explicitKey = params.value( QStringLiteral( "constraintHorizon" ) ).toString().trimmed();
  out.constraintHorizon = !explicitKey.isEmpty() ? explicitKey : horizon.trimmed();
  return true;
}

} // namespace paleo::constraint_identity
