// 层：QGIS 封装
#include "qgisprojectservice.h"

#include "manifestprojection.h"
#include "../metadata/atomicfile.h"
#include "../metadata/paleoprojectfile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <qgsproject.h>

#include <cstdio>

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

  // project.paleo 清单入口（PROJECT_FILE_DESIGN）：.paleo → 解析出 qgz 成员
  // 再开；qgz 成员缺席 = 束损坏，拒开。其他成员缺失如实报 lastErrors 仍开。
  QString qgzFile = qgzPath;
  if ( qgzPath.endsWith( QLatin1String( ".paleo" ) ) )
  {
    bool ok = false;
    QString perr;
    const PaleoProjectFile pf = readProjectFile( qgzPath, &ok, &perr );
    if ( !ok )
    {
      m_errors << ( perr.isEmpty() ? tr( "Cannot read project file %1" ).arg( qgzPath )
                                   : perr );
      return false;
    }
    const QString dir = QFileInfo( qgzPath ).absolutePath();
    if ( pf.qgz.isEmpty() ||
         !QFile::exists( QDir( dir ).filePath( pf.qgz ) ) )
    {
      m_errors << tr( "Project bundle is damaged: qgz member missing (%1)" )
                      .arg( pf.qgz.isEmpty() ? QStringLiteral( "not declared" )
                                             : pf.qgz );
      return false;
    }
    qgzFile = QDir( dir ).filePath( pf.qgz );
    for ( const QString &m : missingMembers( dir, pf ) )
      m_errors << tr( "project member missing: %1" ).arg( m ); // 如实报，不拦开
  }
  else
  {
    // .qgz 直开：旁有 .paleo → 校验束成员；旁无 → 收养（写一份清单），
    // 老工程静默升级。校验失败只进 lastErrors——束检查不拦可用工程。
    const QString dir = QFileInfo( qgzFile ).absolutePath();
    const QString paleoPath = paleoProjectFilePath( dir );
    if ( QFile::exists( paleoPath ) )
    {
      bool ok = false;
      QString perr;
      const PaleoProjectFile pf = readProjectFile( paleoPath, &ok, &perr );
      if ( ok )
        for ( const QString &m : missingMembers( dir, pf ) )
          m_errors << tr( "project member missing: %1" ).arg( m );
      else
        m_errors << tr( "project manifest unreadable: %1" ).arg( perr );
    }
    else
    {
      QString werr;
      if ( !writeProjectFile( dir, projectFileForQgz( qgzFile ), &werr ) )
        m_errors << tr( "could not adopt project manifest: %1" ).arg( werr );
      else
        qInfo() << "QgisProjectService: adopted" << paleoPath;
    }
  }

  if ( !m_project->read( qgzFile ) )
  {
    const QString err = m_project->error();
    m_errors << ( err.isEmpty() ? tr( "Failed to read project: %1" ).arg( qgzFile ) : err );
    return false;
  }

  m_path = qgzFile;
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

  // 工程清单随新建落盘（PROJECT_FILE_DESIGN）：.qgz + project.paleo 双件。
  // 清单写失败不拦工程创建——如实进 lastErrors。
  {
    QString werr;
    if ( !writeProjectFile( QFileInfo( qgzPath ).absolutePath(),
                            projectFileForQgz( qgzPath ), &werr ) )
      m_errors << tr( "project manifest write failed: %1" ).arg( werr );
  }

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

  // §37: mirror the manifest's declared set into project custom properties so
  // the .qgz is a projection of ALL declarations — QgsProject::write() only
  // persists instantiated layers. An embed failure fails the write: saving
  // without the declared set would silently drop uninstantiated layers.
  if ( m_declarationProvider )
  {
    QVector<LayerDeclaration> decls;
    QString providerError;
    if ( !m_declarationProvider( &decls, &providerError ) )
    {
      m_errors << tr( "Failed to read manifest declarations: %1" ).arg( providerError );
      return false;
    }
    QString embedError;
    if ( !ManifestProjection::embedDeclarations( m_project, decls, &embedError ) )
    {
      m_errors << tr( "Failed to embed manifest declarations: %1" ).arg( embedError );
      return false;
    }
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

  // QFile::rename refuses to overwrite an existing destination (Qt 6.11,
  // including Linux). POSIX rename(2) replaces it atomically. Never unlink
  // the live .qgz first: a crash in that window deletes the project.
  if ( !paleoReplaceFile( tmpPath, m_path ) )
  {
    m_errors << tr( "Failed to replace project file %1 with %2" ).arg( m_path, tmpPath );
    return false;
  }

  emit projectWritten( m_path );
  return true;
}

QString QgisProjectService::projectPath() const
{
  return m_path;
}

void QgisProjectService::setDeclarationProvider( const std::function<bool( QVector<LayerDeclaration> *, QString * )> &provider )
{
  m_declarationProvider = provider;
}
