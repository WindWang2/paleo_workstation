// 层：数据
#include "processprobe.h"

#include <QStringList>
#include <QSysInfo>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <limits>
#include <sys/types.h>
#endif

namespace paleo::proc
{

ProcessState probeProcess( qint64 pid )
{
  if ( pid <= 0 )
    return ProcessState::Unknown;
#ifdef Q_OS_WIN
  if ( pid > qint64( 0xFFFFFFFFu ) )
    return ProcessState::Unknown;
  HANDLE h = ::OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, DWORD( pid ) );
  if ( !h )
  {
    const DWORD err = ::GetLastError();
    if ( err == ERROR_INVALID_PARAMETER )
      return ProcessState::Dead; // 系统里没有这个 pid
    if ( err == ERROR_ACCESS_DENIED )
      return ProcessState::Alive; // 存在但受保护/跨会话——保守按存活
    return ProcessState::Unknown;
  }
  const DWORD w = ::WaitForSingleObject( h, 0 );
  ::CloseHandle( h );
  if ( w == WAIT_TIMEOUT )
    return ProcessState::Alive;
  if ( w == WAIT_OBJECT_0 )
    return ProcessState::Dead; // 已退出，进程对象仅因他人句柄残留
  return ProcessState::Unknown;
#else
  if ( pid > qint64( std::numeric_limits<pid_t>::max() ) )
    return ProcessState::Unknown;
  if ( ::kill( pid_t( pid ), 0 ) == 0 )
    return ProcessState::Alive;
  if ( errno == ESRCH )
    return ProcessState::Dead;
  if ( errno == EPERM )
    return ProcessState::Alive;
  return ProcessState::Unknown;
#endif
}

namespace
{

QString shortName( const QString &host )
{
  const int dot = host.indexOf( QLatin1Char( '.' ) );
  return dot > 0 ? host.left( dot ) : host;
}

QStringList localHostNames()
{
  QStringList names;
  const auto add = [&names]( const QString &n ) {
    const QString t = n.trimmed();
    if ( !t.isEmpty() && !names.contains( t, Qt::CaseInsensitive ) )
      names << t;
  };
  add( QSysInfo::machineHostName() );
#ifdef Q_OS_WIN
  // Qt 的 QLockFile 在 Windows 写/比的是 COMPUTERNAME 环境变量；环境变量可
  // 能被父进程裁掉，再补两种系统 API 口径（不依赖环境）。
  add( qEnvironmentVariable( "COMPUTERNAME" ) );
  for ( COMPUTER_NAME_FORMAT fmt : { ComputerNameNetBIOS, ComputerNameDnsHostname,
                                     ComputerNameDnsFullyQualified } )
  {
    wchar_t buf[256];
    DWORD len = DWORD( sizeof( buf ) / sizeof( buf[0] ) );
    if ( ::GetComputerNameExW( fmt, buf, &len ) )
      add( QString::fromWCharArray( buf, int( len ) ) );
  }
#endif
  return names;
}

} // namespace

bool lockHostIsThisMachine( const QString &lockHostName, const QByteArray &lockHostId )
{
  const QByteArray hostId = lockHostId.trimmed();
  if ( !hostId.isEmpty() )
  {
    const QByteArray ours = QSysInfo::machineUniqueId();
    if ( !ours.isEmpty() )
      return ours == hostId;
  }
  const QString host = lockHostName.trimmed();
  if ( host.isEmpty() )
    return true;
  const QString hostShort = shortName( host );
  for ( const QString &local : localHostNames() )
  {
    if ( local.compare( host, Qt::CaseInsensitive ) == 0 )
      return true;
    if ( shortName( local ).compare( hostShort, Qt::CaseInsensitive ) == 0 )
      return true;
  }
  return false;
}

} // namespace paleo::proc
