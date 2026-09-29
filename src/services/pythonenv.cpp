// 层：数据
#include "pythonenv.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

// 层：数据
// ---------------------------------------------------------------------------
// PythonEnvService：venv 落在 <root>/venv/<name>，同一时刻只允许一个环境
// 操作（建 venv / pip / 解包互斥，busyChanged 暴露给上层置灰）。
// ---------------------------------------------------------------------------

PythonEnvService::PythonEnvService( const QString &rootDir, QObject *parent )
  : QObject( parent )
  , m_rootDir( rootDir )
{
}

PythonEnvService::~PythonEnvService()
{
  if ( m_proc )
  {
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
  return !pythonExecutable( name ).isEmpty();
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

void PythonEnvService::startStep( const QString &step, const QString &program,
                                  const QStringList &args )
{
  if ( m_proc )
  {
    emit stepFinished( step, false, tr( "另一项 Python 环境操作仍在进行" ) );
    return;
  }

  m_step = step;
  m_proc = new QProcess( this );
  m_proc->setProcessChannelMode( QProcess::SeparateChannels );
  connect( m_proc, &QProcess::readyReadStandardOutput, this, [this] {
    const QStringList lines = QString::fromLocal8Bit( m_proc->readAllStandardOutput() )
                                  .split( QLatin1Char( '\n' ), Qt::SkipEmptyParts );
    for ( const QString &line : lines )
      emit outputLine( line.trimmed() );
  } );
  connect( m_proc, &QProcess::readyReadStandardError, this, [this] {
    const QStringList lines = QString::fromLocal8Bit( m_proc->readAllStandardError() )
                                  .split( QLatin1Char( '\n' ), Qt::SkipEmptyParts );
    for ( const QString &line : lines )
      emit outputLine( line.trimmed() );
  } );
  connect( m_proc, &QProcess::errorOccurred, this, [this]( QProcess::ProcessError err ) {
    if ( err != QProcess::FailedToStart )
      return;
    const QString step = m_step;
    const QString program = m_proc->program();
    m_proc->deleteLater();
    m_proc = nullptr;
    emit stepFinished( step, false, tr( "无法启动进程：%1" ).arg( program ) );
    emit busyChanged( false );
  } );
  connect( m_proc, qOverload<int, QProcess::ExitStatus>( &QProcess::finished ), this,
           [this]( int exitCode, QProcess::ExitStatus status ) {
             const QString step = m_step;
             const QString program = m_proc->program();
             m_proc->deleteLater();
             m_proc = nullptr;
             const bool ok = ( status == QProcess::NormalExit && exitCode == 0 );
             emit stepFinished( step, ok,
                                ok ? QString()
                                   : tr( "%1 退出码 %2" ).arg( program ).arg( exitCode ) );
             emit busyChanged( false );
           } );

  emit busyChanged( true );
  m_proc->start( program, args );
}

void PythonEnvService::createVenv( const QString &name )
{
  const QString base = findBasePython();
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
                                            const QString &requirementsPath )
{
  const QString py = pythonExecutable( name );
  if ( py.isEmpty() )
  {
    emit stepFinished( QStringLiteral( "installRequirements" ), false,
                       tr( "venv 尚未创建：%1" ).arg( venvDir( name ) ) );
    return;
  }
  startStep( QStringLiteral( "installRequirements" ), py,
             { QStringLiteral( "-m" ), QStringLiteral( "pip" ), QStringLiteral( "install" ),
               QStringLiteral( "-r" ), requirementsPath } );
}

void PythonEnvService::extractZip( const QString &zipPath, const QString &destDir )
{
  const QString base = findBasePython();
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
