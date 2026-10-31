// 层：功能
#include "unitselectionbridge.h"

#include "selectioncontext.h"

namespace paleo::strat
{

QVariantMap AnalysisBinding::enrich( QVariantMap params ) const
{
  if ( !hasUnit || !unit.isValid() )
    return params;
  params.insert( QStringLiteral( "unitSelection" ), toMap( unit ) );
  params.insert( QStringLiteral( "analysisLayerKey" ), analysisKey );
  if ( !unit.subIntervalId.isEmpty() )
    params.insert( QStringLiteral( "subIntervalId" ), unit.subIntervalId );
  if ( !unit.intervalId.isEmpty() )
    params.insert( QStringLiteral( "intervalId" ), unit.intervalId );
  if ( !unit.schemeId.isEmpty() )
    params.insert( QStringLiteral( "schemeId" ), unit.schemeId );
  if ( !unit.revision.isEmpty() )
    params.insert( QStringLiteral( "schemeRevision" ), unit.revision );
  if ( !unit.displayName.isEmpty() )
    params.insert( QStringLiteral( "unitDisplayName" ), unit.displayName );
  if ( unit.subIndex > 0 )
    params.insert( QStringLiteral( "subIndex" ), unit.subIndex );
  return params;
}

QVariantMap AnalysisBinding::publishIdentity() const
{
  QVariantMap m;
  if ( !analysisKey.isEmpty() )
  {
    m.insert( QStringLiteral( "source_horizon" ), analysisKey );
    m.insert( QStringLiteral( "horizon" ), analysisKey );
  }
  if ( !hasUnit || !unit.isValid() )
    return m;
  m.insert(QStringLiteral("unitSelection"), toMap(unit));
  if ( !unit.subIntervalId.isEmpty() )
    m.insert( QStringLiteral( "subIntervalId" ), unit.subIntervalId );
  if ( !unit.intervalId.isEmpty() )
    m.insert( QStringLiteral( "intervalId" ), unit.intervalId );
  if ( !unit.schemeId.isEmpty() )
    m.insert( QStringLiteral( "schemeId" ), unit.schemeId );
  if ( !unit.revision.isEmpty() )
    m.insert( QStringLiteral( "schemeRevision" ), unit.revision );
  m.insert( QStringLiteral( "aggregate" ), unit.aggregate );
  return m;
}

AnalysisBinding bindAnalysis( const SelectionContext *selection, const QString &pageHorizonFallback )
{
  AnalysisBinding b;
  // combo / activeHorizon 只服务界面编辑（constraints.<top>）；不可写入分析成果键。
  const QString page = pageHorizonFallback.trimmed();
  const QString active = selection ? selection->activeHorizon().trimmed() : QString();
  const QString legacyEdit = !page.isEmpty() ? page : active;
  if ( selection )
  {
    b.unit = selection->activeUnit();
    b.hasUnit = b.unit.isValid();
  }
  if ( b.hasUnit )
  {
    b.constraintHorizon = !b.unit.topBoundary.isEmpty() ? b.unit.topBoundary : legacyEdit;
    b.analysisKey = analysisLayerKey( b.unit ); // 不带 legacy 回落
  }
  else
  {
    b.constraintHorizon = legacyEdit;
    b.analysisKey = {}; // 无 UnitSelection → 无分析键（combo 不可驱动）
  }
  return b;
}

bool matchesActiveAnalysis( const SelectionContext *selection, const QString &horizonOrToken )
{
  const QString key = horizonOrToken.trimmed();
  if ( !selection )
    return key.isEmpty();
  if ( !key.isEmpty() && key == selection->activeHorizon() )
    return true;
  const UnitSelection u = selection->activeUnit();
  if ( u.isValid() )
  {
    if ( key == analysisLayerKey( u ) )
      return true;
    if ( !u.topBoundary.isEmpty() && key == u.topBoundary )
      return true;
    if ( !u.subIntervalId.isEmpty() && key == u.subIntervalId )
      return true;
  }
  return key.isEmpty();
}

} // namespace paleo::strat
