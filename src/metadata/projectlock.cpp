// 层：数据
#include "projectlock.h"
#include "storeerrors_internal.h"

#include <QDir>
#include <QFileInfo>
#include <QLockFile>

namespace
{
using paleo::store_detail::setError;

} // namespace

ProjectDirLock::ProjectDirLock( const QString &projectDir )
  : m_lockPath( QDir( projectDir ).filePath( QStringLiteral( "artifacts/metadata/.project.lock" ) ) )
{
}

ProjectDirLock::~ProjectDirLock()
{
  unlock();
}

bool ProjectDirLock::tryLock( QString *error )
{
  if ( m_held )
    return true;
  const QDir dir = QFileInfo( m_lockPath ).absoluteDir();
  if ( !dir.exists() && !dir.mkpath( QStringLiteral( "." ) ) )
  {
    setError( error, QStringLiteral( "cannot create directory for %1" ).arg( m_lockPath ) );
    return false;
  }

  auto lf = std::make_unique<QLockFile>( m_lockPath );
  // 持有者进程已死 → 锁视为陈旧，立即回收（崩溃不留死锁）。
  lf->setStaleLockTime( 0 );
  if ( !lf->tryLock( 0 ) )
  {
    qint64 pid = 0;
    QString hostname, appname;
    if ( lf->getLockInfo( &pid, &hostname, &appname ) && pid > 0 )
    {
      setError( error,
                QStringLiteral( "工程正被另一个实例编辑（pid %1@%2, %3）——"
                                "同一工程同时只允许一个写实例（SCHEMA_MIGRATION.md §6）" )
                    .arg( pid )
                    .arg( hostname, appname ) );
    }
    else
    {
      setError( error, QStringLiteral( "cannot acquire project lock %1" ).arg( m_lockPath ) );
    }
    return false;
  }
  m_lockFile = std::move( lf );
  m_held = true;
  return true;
}

void ProjectDirLock::unlock()
{
  if ( !m_held )
    return;
  m_lockFile->unlock();
  m_lockFile.reset();
  m_held = false;
}
