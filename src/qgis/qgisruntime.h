// 层：QGIS 封装
#pragma once
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>

// P0 spine service — owns QgsApplication lifecycle and vendor prefix resolution.
// Contract: init() must be the first QGIS call in any process using QGIS APIs;
// all init-order knowledge (env vars -> prefixPath -> QgsApplication ctor) lives here.
class QgisRuntime
{
  public:
    // 默认 prefix 解析顺序（QGIS_PREFIX_PATH env 仍可在调用点之上覆盖）：
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
    // prefixPath 为空时取 defaultPrefixPath()。Returns false if already initialized.
    static bool initialize(const QString &prefixPath = QString());
    static void shutdown();
    static bool isInitialized();
    static int providerCount();          // -1 if not initialized
    static QString srsDbPath();          // empty if not initialized
};
