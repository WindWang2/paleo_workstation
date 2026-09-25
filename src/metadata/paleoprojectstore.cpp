#include <QHash> // must precede the header: m_busy is a QHash member (fwd-decl only there)
#include "paleoprojectstore.h"

#include <QFile>
#include <QMutexLocker>

#include <cstdio>

// §41.2 — sole write choke point for project.gpkg / metadata.sqlite / .qgz.
//
// Serialization mechanism: m_writeMutex is held for the whole duration of each
// enqueued write (and for the whole saveAll sequence), so under WAL's single
// writer there can never be two writers racing the .gpkg — a bypass would
// surface as SQLITE_BUSY, which is exactly what the §33 spine test asserts
// does not happen on the queue path.
//
// Signals are emitted AFTER the write mutex is released: a slot connected
// DirectConnection that re-entered the store would otherwise deadlock on the
// non-recursive mutex.

PaleoProjectStore::PaleoProjectStore( QObject *parent )
  : QObject( parent )
{
}

void PaleoProjectStore::setProjectPaths( const QString &qgzPath, const QString &gpkgPath, const QString &metaSqlitePath )
{
  m_qgzPath = qgzPath;
  m_gpkgPath = gpkgPath;
  m_metaPath = metaSqlitePath;
}

PaleoProjectStore::WriteResult PaleoProjectStore::enqueueWrite( const std::function<WriteResult()> &fn )
{
  WriteResult result;
  {
    QMutexLocker locker( &m_writeMutex );
    result = fn();
  }

  // Target is unknown to the store for a free-form write — empty string.
  if ( result.ok )
    emit writeCompleted( QString() );
  else
    emit writeFailed( QString(), result.error );
  return result;
}

PaleoProjectStore::WriteResult PaleoProjectStore::saveAll( const std::function<WriteResult()> &gpkgCommit,
                                                           const std::function<WriteResult()> &writeQgz )
{
  struct PendingEmission { bool failed; QString target; QString error; };
  QList<PendingEmission> pending;
  WriteResult result;

  {
    QMutexLocker locker( &m_writeMutex );

    // 1. gpkg commit — the authoritative data state. Failure aborts the whole
    //    sequence before any .qgz mutation happens.
    result = gpkgCommit();
    if ( result.ok )
      pending.append( { false, m_gpkgPath, QString() } );
    else
      pending.append( { true, m_gpkgPath, result.error } );

    if ( result.ok )
    {
      // 2. .qgz backup — .qgz is equally protected (a truncated zip is a
      //    corrupt project file). First save has nothing to back up.
      if ( !m_qgzPath.isEmpty() && QFile::exists( m_qgzPath ) )
      {
        const QString bakPath = m_qgzPath + QStringLiteral( ".bak" );
        const QString bakTmp = bakPath + QStringLiteral( ".tmp" );
        QFile::remove( bakTmp );
        if ( !QFile::copy( m_qgzPath, bakTmp ) )
        {
          result = { false, tr( "Failed to back up %1 to %2" ).arg( m_qgzPath, bakPath ) };
          pending.append( { true, m_qgzPath, result.error } );
        }
        else if ( ::rename( QFile::encodeName( bakTmp ).constData(),
                            QFile::encodeName( bakPath ).constData() ) != 0 )
        {
          QFile::remove( bakTmp );
          result = { false, tr( "Failed to replace backup %1" ).arg( bakPath ) };
          pending.append( { true, m_qgzPath, result.error } );
        }
      }

      // 3. .qgz atomic write (temp+rename is the callback's responsibility —
      //    QgisProjectService::writeProject implements it). Failure here is
      //    recoverable: gpkg data state is already committed, display state
      //    is regenerable.
      if ( result.ok )
      {
        result = writeQgz();
        if ( result.ok )
          pending.append( { false, m_qgzPath, QString() } );
        else
          pending.append( { true, m_qgzPath, result.error } );
      }
    }
  }

  for ( const PendingEmission &sig : pending )
  {
    if ( sig.failed )
      emit writeFailed( sig.target, sig.error );
    else
      emit writeCompleted( sig.target );
  }
  return result;
}

void PaleoProjectStore::markLayerBusy( const QString &layerId, const QString &taskId, const QString &reason )
{
  QMutexLocker locker( &m_busyMutex );
  m_busy.insert( layerId, { taskId, reason } );
}

void PaleoProjectStore::markLayerFree( const QString &layerId )
{
  QMutexLocker locker( &m_busyMutex );
  m_busy.remove( layerId );
}

bool PaleoProjectStore::layerBusy( const QString &layerId, QString *reason ) const
{
  QMutexLocker locker( &m_busyMutex );
  const auto it = m_busy.constFind( layerId );
  if ( it == m_busy.constEnd() )
    return false;
  if ( reason )
    *reason = QStringLiteral( "%1 — %2" ).arg( it->first, it->second ); // "taskId — reason"
  return true;
}

QVector<PaleoProjectStore::BusyEntry> PaleoProjectStore::busyLayers() const
{
  QMutexLocker locker( &m_busyMutex );
  QVector<BusyEntry> out;
  out.reserve( m_busy.size() );
  for ( auto it = m_busy.constBegin(); it != m_busy.constEnd(); ++it )
    out.append( { it.key(), it->first, it->second } );
  return out;
}
