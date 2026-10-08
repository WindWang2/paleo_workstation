// 层：数据
#include "projectlock.h"
#include "processprobe.h"
#include "storeerrors_internal.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>

namespace
{
using paleo::store_detail::setError;

struct LockOwner
{
  qint64 pid = 0;
  QString hostName;
  QByteArray hostId;
};

// QLockFile 磁盘格式（qlockfile.cpp lockFileContents）：pid / 应用名 / 主机名
// / machineUniqueId / bootUniqueId 各一行；Qt<5.10 及手写锁只有前三行。
// getLockInfo() 不暴露 hostid，这里自读。
bool readLockOwner( const QString &path, LockOwner *out )
{
  QFile f( path );
  if ( !f.open( QIODevice::ReadOnly | QIODevice::Text ) )
    return false;
  const QList<QByteArray> lines = f.read( 4096 ).split( '\n' );
  if ( lines.isEmpty() )
    return false;
  bool ok = false;
  out->pid = lines.value( 0 ).trimmed().toLongLong( &ok );
  if ( !ok || out->pid <= 0 )
    return false;
  out->hostName = QString::fromUtf8( lines.value( 2 ).trimmed() );
  out->hostId = lines.value( 3 ).trimmed();
  return true;
}

// Qt 拒绝取锁后的兜底复核（方向 81 pid 真修）：持有者在本机且 pid 确认已
// 不存在 → 回收陈旧锁。Qt 的「同机」判据在 Windows 只认 COMPUTERNAME 全等，
// 无 hostid 的锁（旧格式/手写/环境被裁）会被永远判「别的机器」而不可恢复。
// 并发安全对齐 Qt：在 <lock>.rmlock 互斥下重读锁再判，防 A 刚回收重建的新锁
// 被持有旧快照的 B 误删。返回 true = 已删除陈旧锁，调用方应重试 tryLock。
bool reclaimIfOwnerGone( QLockFile &lf, const QString &lockPath )
{
  LockOwner owner;
  if ( !readLockOwner( lockPath, &owner ) )
    return false;
  if ( !paleo::proc::lockHostIsThisMachine( owner.hostName, owner.hostId ) )
    return false; // 别的机器（共享盘）上的持有者——本机无法证明其死亡
  if ( paleo::proc::probeProcess( owner.pid ) != paleo::proc::ProcessState::Dead )
    return false; // 存活或无法确认：绝不抢锁
  QLockFile rmlock( lockPath + QStringLiteral( ".rmlock" ) );
  if ( !rmlock.tryLock( 0 ) )
    return false;
  LockOwner again;
  if ( !readLockOwner( lockPath, &again ) || again.pid != owner.pid ||
       again.hostName != owner.hostName || again.hostId != owner.hostId )
    return false; // 期间锁已易主
  return lf.removeStaleLockFile();
}

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
  bool locked = lf->tryLock( 0 );
  if ( !locked && lf->error() == QLockFile::LockFailedError &&
       reclaimIfOwnerGone( *lf, m_lockPath ) )
    locked = lf->tryLock( 0 );
  if ( !locked )
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
