// 层：QGIS 封装
#pragma once
#include <QString>

// P0 spine service — owns QgsApplication lifecycle and vendor prefix resolution.
// Contract: init() must be the first QGIS call in any process using QGIS APIs;
// all init-order knowledge (env vars -> prefixPath -> QgsApplication ctor) lives here.
class QgisRuntime
{
  public:
    // 默认 prefix：CMake 按构建实际链接的 QGIS 位置注入
    // PALEO_QGIS_PREFIX_DEFAULT（vendor prefix）；无 vendor 构建回落发行版
    // "/usr"。QGIS_PREFIX_PATH env 仍可在调用点之上覆盖（launcher/兜底）。
    static QString defaultPrefixPath()
    {
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
