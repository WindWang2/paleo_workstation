// 层：功能
#include "mamcltool.h"

#include "../services/pythonenv.h"

#include <QCoreApplication>
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
  // 构建树路径优先；安装/分发后回落 <bin>/../share/paleo/mamcl/<包名>（#142/#86）。
  const QString buildTree = QStringLiteral( PALEO_MAMCL_PACKAGE );
  if ( QFileInfo::exists( buildTree ) )
    return buildTree;
  const QString installed = QDir( QCoreApplication::applicationDirPath() )
                                .filePath( QStringLiteral( "../share/paleo/mamcl/" ) +
                                           QFileInfo( buildTree ).fileName() );
  if ( QFileInfo::exists( installed ) )
    return QDir::cleanPath( installed );
  return buildTree;
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

QString MamclTool::lockFile() const
{
  const QFileInfo zip( packageZip() );
  if ( zip.fileName().isEmpty() )
    return {};
  return zip.dir().filePath( zip.completeBaseName() + QStringLiteral( ".requirements.lock" ) );
}

QString MamclTool::expectedZipSha256() const
{
  const QString fromEnv = qEnvironmentVariable( "PALEO_MAMCL_ZIP_SHA256" ).trimmed().toLower();
  if ( !fromEnv.isEmpty() )
    return fromEnv;
#ifdef PALEO_MAMCL_SHA256
  // 只有走内置默认包（无环境变量、无 setPackageZip）才信编译期哈希。
  if ( qEnvironmentVariable( "PALEO_MAMCL_ZIP" ).trimmed().isEmpty() && m_zipPath.isEmpty() )
    return QStringLiteral( PALEO_MAMCL_SHA256 ).toLower();
#endif
  return {};
}

QString MamclTool::sha256OfFile( const QString &path )
{
  QFile f( path );
  if ( !f.open( QIODevice::ReadOnly ) )
    return {};
  QCryptographicHash hash( QCryptographicHash::Sha256 );
  if ( !hash.addData( &f ) )
    return {};
  return QString::fromLatin1( hash.result().toHex() );
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
  QByteArray content = normalizedRequirements();
  QFile lock( lockFile() );
  if ( lock.open( QIODevice::ReadOnly ) )
    content = QByteArrayLiteral( "lock\n" ) + lock.readAll();
  const QByteArray hash =
      QCryptographicHash::hash( content, QCryptographicHash::Sha1 ).toHex();
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
  if ( !marker.open( QIODevice::ReadOnly ) || m_zipSha256.isEmpty() )
    return false;
  // 旗标内容 = 程序包 SHA-256（#142：旧版 size+mtime 可被同尺寸替换绕过）。
  return QString::fromUtf8( marker.readAll() ).trimmed() == m_zipSha256 &&
         QFileInfo::exists( appScript() );
}

void MamclTool::writeExtractMarker() const
{
  QFile marker( extractMarkerPath( packageDir() ) );
  if ( marker.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
    marker.write( m_zipSha256.toUtf8() );
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
  // #142 完整性：解包/执行前核对程序包 SHA-256。
  const QString expected = expectedZipSha256();
  if ( expected.isEmpty() )
  {
    emit launchFinished( false, tr( "MAMCL 程序包未登记 SHA-256，拒绝解包执行：%1"
                                    "（外部程序包请设置 PALEO_MAMCL_ZIP_SHA256）" )
                                    .arg( packageZip() ) );
    return;
  }
  const QString actual = sha256OfFile( packageZip() );
  if ( actual != expected )
  {
    emit launchFinished( false, tr( "MAMCL 程序包 SHA-256 不符，拒绝解包执行：%1\n期望 %2\n实际 %3" )
                                    .arg( packageZip(), expected, actual ) );
    return;
  }
  m_zipSha256 = actual;
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
      if ( QFileInfo::exists( lockFile() ) )
      {
        // #142：锁文件 = 全量钉版本 + 逐文件哈希；只装 wheel，不跑 sdist 构建脚本。
        m_env->installRequirements( venvName(), lockFile(),
                                    { QStringLiteral( "--require-hashes" ),
                                      QStringLiteral( "--only-binary=:all:" ) } );
        return;
      }
      if ( qEnvironmentVariable( "PALEO_MAMCL_ALLOW_UNPINNED" ) != QLatin1String( "1" ) )
      {
        finish( false, tr( "缺少依赖锁文件 %1，拒绝安装未钉版本的依赖"
                           "（确需请设置 PALEO_MAMCL_ALLOW_UNPINNED=1）" )
                           .arg( lockFile() ) );
        return;
      }
      emit statusMessage( tr( "警告：按未钉版本的 requirements.txt 安装 MAMCL 依赖" ) );
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
