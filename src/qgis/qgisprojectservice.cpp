#include "qgisprojectservice.h"

#include <QFile>
#include <QFileInfo>

#include <qgsproject.h>

// P0 spine service — owns a QgsProject per service instance. We deliberately
// use `new QgsProject()` rather than the QgsProject::instance() singleton:
// the singleton is global mutable state shared with any other QGIS consumer in
// the process (tests, server-mode hooks), while this service's contract is to
// own one project's open/save lifecycle (§37 manifest projections included).
//
// Write contract (§41.2): never write the .qgz in place. writeProject() writes
// a sibling temp file then renames it over the target, so a crash mid-write
// can never leave a truncated/corrupt .qgz at the authoritative path. Ordering
// relative to the .gpkg commit and the .qgz.bak backup is NOT done here — that
// sequencing belongs to PaleoProjectStore::saveAll(), which calls this via the
// writeQgz callback.

QgisProjectService::QgisProjectService( QObject *parent )
  : QObject( parent )
  , m_project( new QgsProject( this ) )
{
}

QgisProjectService::~QgisProjectService() = default;

QgsProject *QgisProjectService::project() const
{
  return m_project;
}

bool QgisProjectService::openProject( const QString &qgzPath )
{
  m_errors.clear();

  if ( qgzPath.isEmpty() || !QFile::exists( qgzPath ) )
  {
    m_errors << tr( "Project file does not exist: %1" ).arg( qgzPath );
    return false;
  }

  if ( !m_project->read( qgzPath ) )
  {
    const QString err = m_project->error();
    m_errors << ( err.isEmpty() ? tr( "Failed to read project: %1" ).arg( qgzPath ) : err );
    return false;
  }

  m_path = qgzPath;
  emit projectOpened( m_path );
  return true;
}

bool QgisProjectService::createProject( const QString &qgzPath )
{
  m_errors.clear();

  if ( qgzPath.isEmpty() )
  {
    m_errors << tr( "Cannot create a project with an empty path" );
    return false;
  }

  m_project->clear();
  m_path = qgzPath;

  // Materialize the file immediately so the path is authoritative from t=0 and
  // later saveAll() cycles always have an existing .qgz to back up.
  if ( !writeProject() )
    return false;

  emit projectOpened( m_path );
  return true;
}

bool QgisProjectService::writeProject()
{
  m_errors.clear();

  if ( m_path.isEmpty() )
  {
    m_errors << tr( "No project path set — open or create a project first" );
    return false;
  }

  // Temp file lives in the same directory (required for atomic rename across
  // filesystems) and MUST keep the same suffix: QgsProject::write() picks the
  // zip (.qgz) vs xml (.qgs) storage backend from the filename extension.
  const QFileInfo fi( m_path );
  const QString tmpPath = fi.dir().filePath(
    fi.completeBaseName() + QStringLiteral( ".tmp.%1" ).arg( fi.suffix() ) );

  QFile::remove( tmpPath );
  if ( !m_project->write( tmpPath ) )
  {
    const QString err = m_project->error();
    m_errors << ( err.isEmpty() ? tr( "Failed to write project: %1" ).arg( tmpPath ) : err );
    QFile::remove( tmpPath );
    return false;
  }

  // write(filename) retargets fileName() to the temp path — restore the
  // authoritative path now that the content is safely on disk.
  m_project->setFileName( m_path );

  if ( !QFile::rename( tmpPath, m_path ) )
  {
    // POSIX rename(2) replaces the target atomically, but some platforms
    // refuse to overwrite an existing destination — drop it and retry.
    QFile::remove( m_path );
    if ( !QFile::rename( tmpPath, m_path ) )
    {
      m_errors << tr( "Failed to replace project file %1 with %2" ).arg( m_path, tmpPath );
      return false;
    }
  }

  emit projectWritten( m_path );
  return true;
}

QString QgisProjectService::projectPath() const
{
  return m_path;
}
