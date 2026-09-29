// 层：功能
#include "mamcltool.h"

#include "../services/pythonenv.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>

// 层：功能
// ---------------------------------------------------------------------------
// MamclTool：状态机 Extract → CreateVenv → InstallDeps → Launch，每步先看
// 旗标/产物是否已就位（幂等：第二次 open 直接进 Launch）。解包与 pip 输出
// 逐行进 statusMessage 之外的 outputLine——由壳决定落日志还是状态条。
// ---------------------------------------------------------------------------

namespace
{
  QString extractMarkerPath( const QString &packageDir )
  {
    return QDir( packageDir ).filePath( QStringLiteral( ".paleo-extract.ok" ) );
  }

  // conda 系基底解释器建的 venv 不带 TCL/TK 库路径，tkinter 启动即
  // "Can't find a usable init.tcl"。从 pyvenv.cfg 的 home 推基底 prefix，
  // 找到 lib/tcl8.* / lib/tk8.* 就补进环境（系统已设则不动）。
  void fixupTclEnv( const QString &venvDir, QProcessEnvironment &env )
  {
    if ( env.contains( QStringLiteral( "TCL_LIBRARY" ) ) )
      return;
    QFile cfg( QDir( venvDir ).filePath( QStringLiteral( "pyvenv.cfg" ) ) );
    if ( !cfg.open( QIODevice::ReadOnly ) )
      return;
    QString home;
    const QByteArrayList lines = cfg.readAll().split( '\n' );
    for ( const QByteArray &line : lines )
    {
      if ( !line.trimmed().startsWith( "home" ) )
        continue;
      home = QString::fromUtf8( line.mid( line.indexOf( '=' ) + 1 ) ).trimmed();
      break;
    }
    if ( home.isEmpty() )
      return;
    const QDir libDir( QDir( home ).filePath( QStringLiteral( "../lib" ) ) );
    const QStringList tcl = libDir.entryList( { QStringLiteral( "tcl8.*" ) }, QDir::Dirs );
    const QStringList tk = libDir.entryList( { QStringLiteral( "tk8.*" ) }, QDir::Dirs );
    if ( !tcl.isEmpty() && !tk.isEmpty() )
    {
      env.insert( QStringLiteral( "TCL_LIBRARY" ), libDir.filePath( tcl.first() ) );
      env.insert( QStringLiteral( "TK_LIBRARY" ), libDir.filePath( tk.first() ) );
    }
  }
} // namespace

MamclTool::MamclTool( PythonEnvService *env, QObject *parent )
  : QObject( parent )
  , m_env( env )
{
  connect( m_env, &PythonEnvService::stepFinished, this, &MamclTool::onStepFinished );
}

void MamclTool::setPackageZip( const QString &zipPath )
{
  m_zipPath = zipPath;
}

QString MamclTool::packageZip() const
{
  const QString fromEnv = qEnvironmentVariable( "PALEO_MAMCL_ZIP" ).trimmed();
  if ( !fromEnv.isEmpty() )
    return fromEnv;
  if ( !m_zipPath.isEmpty() )
    return m_zipPath;
#ifdef PALEO_MAMCL_PACKAGE
  return QStringLiteral( PALEO_MAMCL_PACKAGE );
#else
  return QString();
#endif
}

QString MamclTool::packageDir() const
{
  const QString base = QFileInfo( packageZip() ).completeBaseName();
  return QDir( m_env->rootDir() ).filePath( QStringLiteral( "pkg/%1" ).arg( base ) );
}

QString MamclTool::appScript() const
{
  return QDir( packageDir() ).filePath( QStringLiteral( "app.py" ) );
}

QString MamclTool::requirementsFile() const
{
  return QDir( packageDir() ).filePath( QStringLiteral( "requirements.txt" ) );
}

QByteArray MamclTool::normalizedRequirements() const
{
  QFile req( requirementsFile() );
  if ( !req.open( QIODevice::ReadOnly ) )
    return {};
  QByteArray out;
  const QByteArrayList lines = req.readAll().split( '\n' );
  for ( QByteArray line : lines )
  {
    line = line.trimmed(); // 顺手剥掉上游文件里的杂散 \r
    if ( line.isEmpty() )
      continue;
    if ( line.toLower().startsWith( "openzgy" ) )
    {
      out += "# paleo: openzgy 不在 PyPI（OpenZGY SDK 单独分发），跳过自动安装\n";
      continue;
    }
    out += line + '\n';
  }
  return out;
}

QString MamclTool::depsMarkerPath() const
{
  const QByteArray hash =
      QCryptographicHash::hash( normalizedRequirements(), QCryptographicHash::Sha1 ).toHex();
  return QDir( m_env->venvDir( venvName() ) )
      .filePath( QStringLiteral( ".paleo-deps-%1.ok" ).arg( QString::fromLatin1( hash ) ) );
}

bool MamclTool::ready() const
{
  return m_env->venvReady( venvName() ) && QFileInfo::exists( appScript() ) &&
         QFileInfo::exists( depsMarkerPath() );
}

bool MamclTool::extractMarkerValid() const
{
  QFile marker( extractMarkerPath( packageDir() ) );
  if ( !marker.open( QIODevice::ReadOnly ) )
    return false;
  const QFileInfo zip( packageZip() );
  // 旗标内容 = zip 尺寸 + mtime，换包/换版本即失效重解。
  const QString expect = QStringLiteral( "%1 %2" )
                             .arg( zip.size() )
                             .arg( zip.lastModified().toSecsSinceEpoch() );
  return QString::fromUtf8( marker.readAll() ).trimmed() == expect &&
         QFileInfo::exists( appScript() );
}

void MamclTool::writeExtractMarker() const
{
  const QFileInfo zip( packageZip() );
  QFile marker( extractMarkerPath( packageDir() ) );
  if ( marker.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
    marker.write( QStringLiteral( "%1 %2" )
                      .arg( zip.size() )
                      .arg( zip.lastModified().toSecsSinceEpoch() )
                      .toUtf8() );
}

void MamclTool::open()
{
  if ( m_stage != Idle )
    return;
  if ( packageZip().isEmpty() || !QFileInfo::exists( packageZip() ) )
  {
    emit launchFinished( false, tr( "未找到内置 MAMCL 程序包：%1" ).arg( packageZip() ) );
    return;
  }
  m_stage = Extract;
  emit busyChanged( true );
  advance();
}

void MamclTool::advance()
{
  switch ( m_stage )
  {
    case Extract:
      if ( extractMarkerValid() )
      {
        m_stage = CreateVenv;
        advance();
        return;
      }
      emit statusMessage( tr( "首次运行：正在解包 MAMCL 程序…" ) );
      // zip 内含同名顶层目录 → 解到 pkg/ 父目录即得 packageDir。
      m_env->extractZip( packageZip(), QFileInfo( packageDir() ).dir().absolutePath() );
      return;

    case CreateVenv:
      if ( m_env->venvReady( venvName() ) )
      {
        m_stage = InstallDeps;
        advance();
        return;
      }
      emit statusMessage( tr( "正在创建 Python 虚拟环境…" ) );
      m_env->createVenv( venvName() );
      return;

    case InstallDeps:
      if ( QFileInfo::exists( depsMarkerPath() ) )
      {
        m_stage = Launch;
        advance();
        return;
      }
      emit statusMessage( tr( "正在安装 MAMCL 依赖（含 PyTorch，首次较慢）…" ) );
      {
        // 规范化副本落 venv 目录（程序包目录保持原样），pip 装副本。
        const QString normPath = QDir( m_env->venvDir( venvName() ) )
                                     .filePath( QStringLiteral( "requirements.paleo.txt" ) );
        QFile norm( normPath );
        if ( !norm.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
        {
          finish( false, tr( "无法写入依赖清单：%1" ).arg( normPath ) );
          return;
        }
        norm.write( normalizedRequirements() );
        norm.close();
        m_env->installRequirements( venvName(), normPath );
      }
      return;

    case Launch:
    {
      const QString py = m_env->pythonExecutable( venvName() );
      // app.py 以 MAMCL_PYTHON 为默认解释器（GUI 内「本地 Python 环境」一栏），
      // 钉成同一 venv，避免用户在界面里再选一次。
      QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
      env.insert( QStringLiteral( "MAMCL_PYTHON" ), py );
      fixupTclEnv( m_env->venvDir( venvName() ), env );
      QProcess proc;
      proc.setProgram( py );
      proc.setArguments( { appScript() } );
      proc.setWorkingDirectory( packageDir() );
      proc.setProcessEnvironment( env );
      const bool ok = proc.startDetached();
      finish( ok, ok ? tr( "MAMCL 已启动" )
                     : tr( "MAMCL 启动失败：%1" ).arg( py ) );
      return;
    }

    case Idle:
      return;
  }
}

void MamclTool::onStepFinished( const QString &step, bool ok, const QString &message )
{
  if ( m_stage == Idle )
    return;
  if ( step == QLatin1String( "extractZip" ) && m_stage == Extract )
  {
    if ( !ok )
    {
      finish( false, tr( "MAMCL 程序包解包失败：%1" ).arg( message ) );
      return;
    }
    writeExtractMarker();
    m_stage = CreateVenv;
    advance();
  }
  else if ( step == QLatin1String( "createVenv" ) && m_stage == CreateVenv )
  {
    if ( !ok )
    {
      finish( false, tr( "Python 虚拟环境创建失败：%1" ).arg( message ) );
      return;
    }
    m_stage = InstallDeps;
    advance();
  }
  else if ( step == QLatin1String( "installRequirements" ) && m_stage == InstallDeps )
  {
    if ( !ok )
    {
      finish( false, tr( "MAMCL 依赖安装失败：%1" ).arg( message ) );
      return;
    }
    QFile marker( depsMarkerPath() );
    if ( marker.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
      marker.write( "ok\n" );
    m_stage = Launch;
    advance();
  }
}

void MamclTool::finish( bool ok, const QString &message )
{
  m_stage = Idle;
  emit busyChanged( false );
  emit launchFinished( ok, message );
}
