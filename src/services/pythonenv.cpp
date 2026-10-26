// 层：数据
#include "pythonenv.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>

namespace
{
  // 取消/超时的优雅退出窗口：terminate 后给 pip/venv 清理机会，仍未退出
  // 再 kill（对照 scriptrunner 的递进击杀）。
  constexpr int kKillGraceMs = 2000;
} // namespace

// 层：数据
// ---------------------------------------------------------------------------
// PythonEnvService：venv 落在 <root>/venv/<name>，同一时刻只允许一个环境
// 操作（建 venv / pip / 解包互斥，busyChanged 暴露给上层置灰）。单步带
// 超时与取消（#479）：挂死的 pip/venv 不再把 busyChanged(true) 变成永真。
// ---------------------------------------------------------------------------

PythonEnvService::PythonEnvService( const QString &rootDir, QObject *parent )
  : QObject( parent )
  , m_rootDir( rootDir )
{
}

PythonEnvService::~PythonEnvService()
{
  if ( m_killTimer )
  {
    m_killTimer->stop();
    delete m_killTimer;
    m_killTimer = nullptr;
  }
  if ( m_proc )
  {
    // #588-L4.4：先断开进程信号再 kill+wait——waitForFinished 可能同步触发
    // finished/errorOccurred 槽（半销毁对象上 emit 是 UB），且析构后上层也
    // 不该再收到 busyChanged/stepFinished。
    disconnect( m_proc, nullptr, this, nullptr );
    m_proc->kill();
    m_proc->waitForFinished( 3000 );
  }
}

QString PythonEnvService::venvDir( const QString &name ) const
{
  return QDir( m_rootDir ).filePath( QStringLiteral( "venv/%1" ).arg( name ) );
}

QString PythonEnvService::pythonExecutable( const QString &name ) const
{
#ifdef Q_OS_WIN
  const QString rel = QStringLiteral( "Scripts/python.exe" );
#else
  const QString rel = QStringLiteral( "bin/python" );
#endif
  const QString path = QDir( venvDir( name ) ).filePath( rel );
  return QFileInfo::exists( path ) ? path : QString();
}

bool PythonEnvService::venvReady( const QString &name ) const
{
  if ( pythonExecutable( name ).isEmpty() )
    return false;
  // 基底换过（如内置 base 上线/更换）→ 旧 venv 不重用，交给 createVenv 重建。
  QFile cfg( QDir( venvDir( name ) ).filePath( QStringLiteral( "pyvenv.cfg" ) ) );
  if ( !cfg.open( QIODevice::ReadOnly ) )
    return false;
  const QByteArray want =
      QFileInfo( basePython() ).dir().canonicalPath().toUtf8(); // pyvenv.cfg home = <base>/bin
  const QByteArrayList lines = cfg.readAll().split( '\n' );
  for ( const QByteArray &line : lines )
  {
    if ( !line.trimmed().startsWith( "home" ) )
      continue;
    const QString home = QString::fromUtf8( line.mid( line.indexOf( '=' ) + 1 ) ).trimmed();
    return QDir( home ).canonicalPath().toUtf8() == want;
  }
  return false;
}

QString PythonEnvService::findBasePython()
{
  const QString configured = qEnvironmentVariable( "PALEO_PYTHON" ).trimmed();
  if ( !configured.isEmpty() && QFileInfo::exists( configured ) )
    return configured;
  for ( const QString &candidate : { QStringLiteral( "python3" ), QStringLiteral( "python" ) } )
  {
    const QString found = QStandardPaths::findExecutable( candidate );
    if ( !found.isEmpty() )
      return found;
  }
  return QString();
}

QString PythonEnvService::basePython() const
{
#ifdef Q_OS_WIN
  const QString rel = QStringLiteral( "python.exe" );
#else
  const QString rel = QStringLiteral( "bin/python3" );
#endif
  const QString bundled = QDir( m_rootDir ).filePath( QStringLiteral( "base/%1" ).arg( rel ) );
  if ( QFileInfo::exists( bundled ) )
    return bundled;
  return findBasePython();
}

void PythonEnvService::setTimeoutMs( int ms )
{
  m_timeoutMs = qMax( 0, ms );
}

void PythonEnvService::startStep( const QString &step, const QString &program,
                                  const QStringList &args )
{
  if ( m_proc )
  {
    emit stepFinished( step, false, tr( "另一项 Python 环境操作仍在进行" ) );
    return;
  }

  m_step = step;
  m_cancelRequested = false;
  m_timedOut = false;
  m_proc = new QProcess( this );
  m_proc->setProcessChannelMode( QProcess::SeparateChannels );
  // Python 3 子进程 IO 统一 UTF-8（#479）：pip/venv 的管道输出编码钉死，
  // 宿主侧统一 fromUtf8——Windows 非 UTF-8 locale 下中文不再乱码。
  auto env = QProcessEnvironment::systemEnvironment();
  env.insert( QStringLiteral( "PYTHONIOENCODING" ), QStringLiteral( "utf-8" ) );
  m_proc->setProcessEnvironment( env );
  connect( m_proc, &QProcess::readyReadStandardOutput, this, [this] {
    const QStringList lines = QString::fromUtf8( m_proc->readAllStandardOutput() )
                                  .split( QLatin1Char( '\n' ), Qt::SkipEmptyParts );
    for ( const QString &line : lines )
      emit outputLine( line.trimmed() );
  } );
  connect( m_proc, &QProcess::readyReadStandardError, this, [this] {
    const QStringList lines = QString::fromUtf8( m_proc->readAllStandardError() )
                                  .split( QLatin1Char( '\n' ), Qt::SkipEmptyParts );
    for ( const QString &line : lines )
      emit outputLine( line.trimmed() );
  } );
  // FailedToStart 排队送达（scriptrunner 同款）：Windows 上 CreateProcess
  // 失败在 start() 内同步发 errorOccurred，直连会在 start() 返回前把
  // m_proc 置空——随后 new QTimer(m_proc) 得到无父定时器（每次失败泄漏
  // 一枚），且 stepFinished/busyChanged 在 startStep 未返回时重入编排层。
  connect(
      m_proc, &QProcess::errorOccurred, this,
      [this]( QProcess::ProcessError err ) {
        if ( err != QProcess::FailedToStart )
          return;
        const QString step = m_step;
        const QString program = m_proc->program();
        m_proc->deleteLater();
        m_proc = nullptr;
        emit stepFinished( step, false, tr( "无法启动进程：%1" ).arg( program ) );
        emit busyChanged( false );
      },
      Qt::QueuedConnection );
  connect( m_proc, qOverload<int, QProcess::ExitStatus>( &QProcess::finished ), this,
           [this]( int exitCode, QProcess::ExitStatus status ) {
             const QString step = m_step;
             const QString program = m_proc->program();
             m_proc->deleteLater();
             m_proc = nullptr;
             if ( m_killTimer )
             {
               m_killTimer->stop();
               delete m_killTimer;
               m_killTimer = nullptr;
             }
             QString message;
             if ( m_timedOut )
               message = tr( "步骤超时（超过 %1 秒），已终止：%2" )
                             .arg( m_timeoutMs / 1000 )
                             .arg( program );
             else if ( m_cancelRequested )
               message = tr( "已取消：%1" ).arg( program );
             else if ( status == QProcess::NormalExit && exitCode == 0 )
               message = QString();
             else
               message = tr( "%1 退出码 %2" ).arg( program ).arg( exitCode );
             const bool ok = message.isEmpty();
             emit stepFinished( step, ok, message );
             emit busyChanged( false );
           } );

  emit busyChanged( true );
  m_proc->start( program, args );
  // 超时防线（#479）：挂死的 pip/venv 递进回收，busyChanged(false) 必达。
  if ( m_timeoutMs > 0 )
  {
    auto *timeoutTimer = new QTimer( m_proc );
    timeoutTimer->setSingleShot( true );
    connect( timeoutTimer, &QTimer::timeout, this, [this] {
      if ( !m_proc || m_killTimer )
        return;
      m_timedOut = true;
      terminateGracefully();
    } );
    timeoutTimer->start( m_timeoutMs );
  }
}

void PythonEnvService::cancel()
{
  if ( !m_proc || m_killTimer )
    return;
  m_cancelRequested = true;
  terminateGracefully();
}

void PythonEnvService::terminateGracefully()
{
  if ( !m_proc || m_killTimer )
    return;
  m_proc->terminate();
  m_killTimer = new QTimer( this );
  m_killTimer->setSingleShot( true );
  connect( m_killTimer, &QTimer::timeout, this, [this] {
    if ( m_proc )
      m_proc->kill();
  } );
  m_killTimer->start( kKillGraceMs );
}

void PythonEnvService::createVenv( const QString &name )
{
  const QString base = basePython();
  if ( base.isEmpty() )
  {
    emit stepFinished( QStringLiteral( "createVenv" ), false,
                       tr( "找不到可用的 Python 解释器（可设 PALEO_PYTHON 指定）" ) );
    return;
  }
  startStep( QStringLiteral( "createVenv" ), base,
             { QStringLiteral( "-m" ), QStringLiteral( "venv" ), venvDir( name ) } );
}

void PythonEnvService::installRequirements( const QString &name,
                                            const QString &requirementsPath,
                                            const QStringList &extraPipArgs )
{
  const QString py = pythonExecutable( name );
  if ( py.isEmpty() )
  {
    emit stepFinished( QStringLiteral( "installRequirements" ), false,
                       tr( "venv 尚未创建：%1" ).arg( venvDir( name ) ) );
    return;
  }
  QStringList args{ QStringLiteral( "-m" ), QStringLiteral( "pip" ), QStringLiteral( "install" ) };
  args << extraPipArgs << QStringLiteral( "-r" ) << requirementsPath;
  startStep( QStringLiteral( "installRequirements" ), py, args );
}

void PythonEnvService::extractZip( const QString &zipPath, const QString &destDir )
{
  const QString base = basePython();
  if ( base.isEmpty() )
  {
    emit stepFinished( QStringLiteral( "extractZip" ), false,
                       tr( "找不到可用的 Python 解释器（可设 PALEO_PYTHON 指定）" ) );
    return;
  }
  QDir().mkpath( destDir );
  startStep( QStringLiteral( "extractZip" ), base,
             { QStringLiteral( "-m" ), QStringLiteral( "zipfile" ), QStringLiteral( "-e" ),
               zipPath, destDir } );
}
