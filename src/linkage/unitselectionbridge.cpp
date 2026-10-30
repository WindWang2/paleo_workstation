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

AnalysisBinding bindAnalysis( const SelectionContext *selection, const QString &pageHorizonFallback )
{
  AnalysisBinding b;
  const QString page = pageHorizonFallback.trimmed();
  const QString active = selection ? selection->activeHorizon().trimmed() : QString();
  const QString legacy = !page.isEmpty() ? page : active;
  if ( selection )
  {
    b.unit = selection->activeUnit();
    b.hasUnit = b.unit.isValid();
  }
  if ( b.hasUnit )
  {
    b.constraintHorizon = !b.unit.topBoundary.isEmpty() ? b.unit.topBoundary : legacy;
    b.analysisKey = analysisLayerKey( b.unit, legacy );
  }
  else
  {
    b.constraintHorizon = legacy;
    b.analysisKey = legacy;
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
