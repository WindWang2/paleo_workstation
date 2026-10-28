// 层：功能
#include "mamcltool.h"

#include "../services/pythonenv.h"
#include "../io/atomicwrite.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QUuid>

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

bool MamclTool::writeExtractMarker( QString *error ) const
{
  // 旗标写入也要校验（#731）：空/半截旗标会让 extractMarkerValid() 判失效
  // （下次重复解包，代价低但不诚实），写失败如实报错。
  return paleo::writeFileAtomic( extractMarkerPath( packageDir() ), m_zipSha256.toUtf8(),
                                 error );
}

// #735：把已核对 SHA-256 的程序包拷成我们自己的副本并再核一遍——解包读的
// 就是「已校验字节」，原路径在核对之后被换掉不再影响本次解包（TOCTOU 窗口
// 从「原文件可被任意替换」缩到「须替换我们刚建的唯一名副本」）。
bool MamclTool::stageVerifiedZipCopy( QString *copyPath, QString *error )
{
  const QString dir = QDir( m_env->rootDir() ).filePath( QStringLiteral( "pkg" ) );
  if ( !QDir().mkpath( dir ) )
  {
    if ( error )
      *error = tr( "无法创建目录 %1" ).arg( dir );
    return false;
  }
  const QString copy = QDir( dir ).filePath( QStringLiteral( ".verify-%1.zip" )
                            .arg( QUuid::createUuid().toString( QUuid::Id128 ) ) );
  if ( !QFile::copy( packageZip(), copy ) )
  {
    if ( error )
      *error = tr( "无法复制程序包 %1 → %2" ).arg( packageZip(), copy );
    return false;
  }
  if ( sha256OfFile( copy ) != m_zipSha256 )
  {
    QFile::remove( copy );
    if ( error )
      *error = tr( "程序包副本校验失败（%1），拒绝解包执行" ).arg( packageZip() );
    return false;
  }
  *copyPath = copy;
  return true;
}

void MamclTool::discardVerifiedZipCopy()
{
  if ( !m_verifiedZipCopy.isEmpty() )
  {
    QFile::remove( m_verifiedZipCopy );
    m_verifiedZipCopy.clear();
  }
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
      // #735：解包源取「已校验字节」副本；解到临时目录，整棵 rename 到位后
      // 才写旗标——半解压的树不会被当成「已解包」。
      if ( m_verifiedZipCopy.isEmpty() )
      {
        QString copyErr;
        if ( !stageVerifiedZipCopy( &m_verifiedZipCopy, &copyErr ) )
        {
          finish( false, copyErr );
          return;
        }
      }
      m_extractTmpDir = QStringLiteral( "%1.extract-%2" )
                            .arg( packageDir(), QUuid::createUuid().toString( QUuid::Id128 ) );
      m_env->extractZip( m_verifiedZipCopy, m_extractTmpDir );
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
      { // 本 case 声明了带初始化的变量（normalized），必须整体加括号，
        // 否则后续 case 标签跳过其初始化（C2360）。
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
      // 依赖清单缺失/为空 ≠ 「无依赖」（#196）：静默写空清单交给 pip
      // 「成功」，随后 app.py 因缺包崩溃且错误被完全吞掉。
      const QByteArray normalized = normalizedRequirements();
      if ( normalized.isEmpty() )
      {
        finish( false, tr( "MAMCL 依赖清单缺失或为空：%1，拒绝继续启动" )
                           .arg( requirementsFile() ) );
        return;
      }
      emit statusMessage( tr( "警告：按未钉版本的 requirements.txt 安装 MAMCL 依赖" ) );
      {
        // 规范化副本落 venv 目录（程序包目录保持原样），pip 装副本。
        const QString normPath = QDir( m_env->venvDir( venvName() ) )
                                     .filePath( QStringLiteral( "requirements.paleo.txt" ) );
        // 原子整写并校验：半截依赖清单会让 pip 静默少装包。
        QString writeErr;
        if ( !paleo::writeFileAtomic( normPath, normalized, &writeErr ) )
        {
          finish( false, tr( "无法写入依赖清单：%1（%2）" ).arg( normPath, writeErr ) );
          return;
        }
        m_env->installRequirements( venvName(), normPath );
      }
      return;
      }

    case Launch:
    {
      const QString py = m_env->pythonExecutable( venvName() );
      // app.py 以 MAMCL_PYTHON 为默认解释器（GUI 内「本地 Python 环境」一栏），
      // 钉成同一 venv，避免用户在界面里再选一次。
      QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
      env.insert( QStringLiteral( "MAMCL_PYTHON" ), py );
      fixupTclEnv( m_env->venvDir( venvName() ), env );
      // 本地 Tk 运行库（免 root 自愈）：external/mamcl/tk-runtime/usr/lib 放有
      // libtk8.6.so（发行版 tk 包解包即得）时补进动态库/脚本库路径；系统已
      // 装 tk 或已设路径时不干扰。
      {
        const QString tkLib = QDir( m_env->rootDir() ).filePath(
            QStringLiteral( "tk-runtime/usr/lib" ) );
        if ( QFileInfo::exists( QDir( tkLib ).filePath( QStringLiteral( "libtk8.6.so" ) ) ) )
        {
          const QString oldLd = env.value( QStringLiteral( "LD_LIBRARY_PATH" ) );
          env.insert( QStringLiteral( "LD_LIBRARY_PATH" ),
                      oldLd.isEmpty() ? tkLib
                                      : tkLib + QLatin1Char( ':' ) + oldLd );
          if ( !env.contains( QStringLiteral( "TK_LIBRARY" ) ) )
            env.insert( QStringLiteral( "TK_LIBRARY" ),
                        QDir( tkLib ).filePath( QStringLiteral( "tk8.6" ) ) );
        }
      }
      // GUI 预检：MAMCL 是 tkinter 界面——缺 Tk 运行库（libtk8.6.so）时
      // startDetached 照样返回成功，进程秒退且 ImportError 只进控制台，
      // 用户侧表现为「已启动」但窗口永远不出现。预检失败如实报错并给
      // 出系统依赖安装指引，不再假成功。
      {
        QProcess check;
        check.setProgram( py );
        check.setArguments( { QStringLiteral( "-c" ), QStringLiteral( "import tkinter" ) } );
        check.setProcessEnvironment( env );
        check.start();
        if ( !check.waitForFinished( 15000 ) )
        {
          check.kill();
          finish( false, tr( "MAMCL 启动预检超时（%1）" ).arg( py ) );
          return;
        }
        if ( check.exitCode() != 0 )
        {
          const QString detail = QString::fromUtf8( check.readAllStandardError() )
                                     .section( '\n', -2 )
                                     .trimmed();
          finish( false,
                  tr( "MAMCL 需要 Tk 图形库，当前 Python 无法加载 tkinter：%1\n"
                      "请安装系统 Tk 库后重试（Arch 系：sudo pacman -S tk）。" )
                      .arg( detail ) );
          return;
        }
      }
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
      if ( !m_extractTmpDir.isEmpty() )
        QDir( m_extractTmpDir ).removeRecursively();
      m_extractTmpDir.clear();
      finish( false, tr( "MAMCL 程序包解包失败：%1" ).arg( message ) );
      return;
    }
    // zip 内含同名顶层目录 → 解出的唯一顶层目录才是 packageDir 的内容；
    // 否则（根级散落文件）整棵临时目录即 packageDir。
    QString contentRoot = m_extractTmpDir;
    if ( !m_extractTmpDir.isEmpty() )
    {
      const QStringList entries =
          QDir( m_extractTmpDir ).entryList( QDir::AllEntries | QDir::NoDotAndDotDot );
      if ( entries.size() == 1 &&
           QFileInfo( QDir( m_extractTmpDir ).filePath( entries.first() ) ).isDir() )
        contentRoot = QDir( m_extractTmpDir ).filePath( entries.first() );
    }
    if ( contentRoot.isEmpty() )
    {
      finish( false, tr( "MAMCL 程序包解包结果为空" ) );
      return;
    }
    // 整棵 rename 到位后才写旗标（半解压树不落最终位置）。
    if ( QFileInfo::exists( packageDir() ) )
      QDir( packageDir() ).removeRecursively();
    if ( !QDir().rename( contentRoot, packageDir() ) )
    {
      if ( !m_extractTmpDir.isEmpty() )
        QDir( m_extractTmpDir ).removeRecursively();
      m_extractTmpDir.clear();
      finish( false, tr( "无法落位解包结果到 %1" ).arg( packageDir() ) );
      return;
    }
    if ( !m_extractTmpDir.isEmpty() )
      QDir( m_extractTmpDir ).removeRecursively();
    m_extractTmpDir.clear();
    discardVerifiedZipCopy();
    QString markerErr;
    if ( !writeExtractMarker( &markerErr ) )
    {
      finish( false, tr( "无法写入解包旗标：%1（%2）" )
                         .arg( extractMarkerPath( packageDir() ), markerErr ) );
      return;
    }
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
    // 旗标写入校验（#731）：空旗标会让下次重复装依赖（代价低但不诚实）。
    QString markerErr;
    if ( !paleo::writeFileAtomic( depsMarkerPath(), QByteArrayLiteral( "ok\n" ), &markerErr ) )
    {
      finish( false, tr( "无法写入依赖旗标：%1（%2）" )
                         .arg( depsMarkerPath(), markerErr ) );
      return;
    }
    m_stage = Launch;
    advance();
  }
}

void MamclTool::finish( bool ok, const QString &message )
{
  discardVerifiedZipCopy();
  if ( !m_extractTmpDir.isEmpty() )
  {
    QDir( m_extractTmpDir ).removeRecursively();
    m_extractTmpDir.clear();
  }
  m_stage = Idle;
  emit busyChanged( false );
  emit launchFinished( ok, message );
}
