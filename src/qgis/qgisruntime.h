// 层：QGIS 封装
#pragma once
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QString>
#include <functional>

// P0 spine service — owns QgsApplication lifecycle and vendor prefix resolution.
// Contract: init() must be the first QGIS call in any process using QGIS APIs;
// all init-order knowledge (env vars -> prefixPath -> QgsApplication ctor) lives here.
class QgisRuntime
{
  public:
    // 默认 prefix 解析顺序（优先级总表见 resolvePrefixPath：显式 CLI 值 >
    // QGIS_PREFIX_PATH env > 本缺省序）：
    // 1. 可重定位安装（#86）：可执行文件位于 <prefix>/bin 且
    //    <prefix>/share/qgis/resources/srs.db 存在 → 用 <prefix>；
    // 2. CMake 按构建实际链接的 QGIS 位置注入的 PALEO_QGIS_PREFIX_DEFAULT
    //    （开发构建的绝对 vendor prefix，仅作兜底）；
    // 3. 无 vendor 构建回落发行版 "/usr"。
    static QString relocatablePrefixPath()
    {
      QString exeDir;
      if ( QCoreApplication::instance() )
        exeDir = QCoreApplication::applicationDirPath();
#ifdef Q_OS_LINUX
      else // main() 在构造 QApplication 之前就要 prefix
        exeDir = QFileInfo( QStringLiteral( "/proc/self/exe" ) ).canonicalFilePath().section( QLatin1Char( '/' ), 0, -2 );
#endif
      if ( exeDir.isEmpty() || QFileInfo( exeDir ).fileName() != QLatin1String( "bin" ) )
        return QString();
      const QString prefix = QDir::cleanPath( exeDir + QStringLiteral( "/.." ) );
      if ( !QFileInfo::exists( prefix + QStringLiteral( "/share/qgis/resources/srs.db" ) ) )
        return QString();
      return prefix;
    }
    static QString defaultPrefixPath()
    {
      const QString relocated = relocatablePrefixPath();
      if ( !relocated.isEmpty() )
        return relocated;
#ifdef PALEO_QGIS_PREFIX_DEFAULT
      return QStringLiteral(PALEO_QGIS_PREFIX_DEFAULT);
#else
      return QStringLiteral("/usr");
#endif
    }
    // prefix 解析的唯一事实（#483）：QGIS_PREFIX_PATH env > 显式实参 >
    // defaultPrefixPath()。initialize / paleo_mkproject / selfcheck 共用——
    // 此前各处手工复制「env 或缺省」序列，规则出现多份事实。注意显式实参
    // 不压 env：数十个测试 main 以 "/usr" 作兜底实参、依赖 CI 注入的 env
    // 覆盖它；CLI --prefix 想压 env 须在 main() 里 qputenv（见 src/app/main.cpp）。
    // 头内联与 defaultPrefixPath 同款（paleo_selfcheck 只链接口面不链
    // paleo_qgis，体外实现会断链）。
    static QString resolvePrefixPath( const QString &explicitPrefix,
                                      const QString &envValue )
    {
      if ( !envValue.isEmpty() )
        return envValue;
      if ( !explicitPrefix.isEmpty() )
        return explicitPrefix;
      return defaultPrefixPath();
    }
    // prefixPath 为空或缺省时按 resolvePrefixPath 取 env/缺省。Returns false
    // if already initialized.
    // Called after QApplication exists, before provider initialization; no UI
    // dependency here. The application may present startup progress in the hook.
    static bool initialize(const QString &prefixPath = QString(),
                           const std::function<void()> &applicationCreated = {});
    static void shutdown();
    static bool isInitialized();
    static int providerCount();          // -1 if not initialized
    static QString srsDbPath();          // empty if not initialized
    // 原生属性窗口创建前补齐默认色带；仅新增缺项，保留用户同名配色与自定义库。
    static bool ensureDefaultColorRamps(QString *error = nullptr);
    static QJsonObject coordinateRuntimeReport(); // real QGIS + GDAL EPSG/transform checks
};
