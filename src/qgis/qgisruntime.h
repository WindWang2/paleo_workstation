// 层：QGIS 封装
#pragma once
#include <QString>

// P0 spine service — owns QgsApplication lifecycle and vendor prefix resolution.
// Contract: init() must be the first QGIS call in any process using QGIS APIs;
// all init-order knowledge (env vars -> prefixPath -> QgsApplication ctor) lives here.
class QgisRuntime
{
  public:
    // prefixPath: vendor prefix or distro root ("/usr" on Arch). Returns false if already initialized.
    static bool initialize(const QString &prefixPath);
    static void shutdown();
    static bool isInitialized();
    static int providerCount();          // -1 if not initialized
    static QString srsDbPath();          // empty if not initialized
};
