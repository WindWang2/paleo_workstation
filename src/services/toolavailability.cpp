// 层：数据
#include "toolavailability.h"
#include "../metadata/paleoprojectstore.h"

// §35 + eng review — global gate ANDs with the store's per-layer busy registry;
// every denial surfaces a tr()'d reason for the tooltip (§42.13 / §41.5).

ToolAvailabilityService::ToolAvailabilityService( PaleoProjectStore *store, QObject *parent )
  : QObject( parent )
  , m_store( store )
{
}

void ToolAvailabilityService::setGlobalGate( bool allowed, const QString &reason )
{
  if ( m_globalAllowed == allowed && m_globalReason == reason )
    return;
  m_globalAllowed = allowed;
  m_globalReason = reason;
  // Global gate flips every layer at once — empty layerId means "all".
  emit availabilityChanged( QString() );
}

bool ToolAvailabilityService::check( const QString &layerId, QString *reasonOut ) const
{
  if ( !m_globalAllowed )
  {
    if ( reasonOut )
      *reasonOut = m_globalReason.isEmpty() ? tr( "Tools unavailable" ) : m_globalReason;
    return false;
  }
  QString busy;
  if ( m_store && m_store->layerBusy( layerId, &busy ) )
  {
    // Store contract: busy reason is prefixed with the task info that owns the
    // layer ("taskId — reason") so the tooltip names the blocker (§42.13).
    if ( reasonOut )
      *reasonOut = busy.isEmpty() ? tr( "Layer busy — task in progress" ) : busy;
    return false;
  }
  if ( reasonOut )
    reasonOut->clear();
  return true;
}

void ToolAvailabilityService::noteTaskOnLayer( const QString &layerId, const QString &taskId, const QString &reason )
{
  if ( !m_store )
    return;
  QString before;
  const bool wasBusy = m_store->layerBusy( layerId, &before );
  m_store->markLayerBusy( layerId, taskId, reason );
  QString after;
  m_store->layerBusy( layerId, &after );
  if ( !wasBusy || before != after )
    emit availabilityChanged( layerId );
}

void ToolAvailabilityService::clearTaskOnLayer( const QString &layerId )
{
  if ( !m_store )
    return;
  const bool wasBusy = m_store->layerBusy( layerId );
  m_store->markLayerFree( layerId );
  if ( wasBusy )
    emit availabilityChanged( layerId );
}
