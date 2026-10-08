// 层：测试壳
#pragma once
#include <QHash>
#include <QString>
#include <QStringList>

// tests/fixtures — mkproject 夹具工厂（方向 78：manifest 参数化 + 测试消费
// 闭环）。QProcess 驱动 paleo_mkproject --manifest mini.json --out <tmpdir>
// 走生产导入路径全链（io 解析 → catalog 登记 → 工程文件落盘），配套 catalog
// 摘要断言 helper——synthetic 夹具（PerfFixtures）盖不到的面。
//
// 环境口径：QProcess 默认继承父进程环境——ctest 的 XDG 沙箱/QGIS_PREFIX_PATH
// 与 paleo-dev 的树内 TEMP/TMP 同监子进程（沙箱监狱下 QTemporaryDir 可用，
// 方向 72 口径）；无需额外注入。
namespace MkProjectFixture
{
  // mini 数据集位置（编译定义注入；tools/reference/mkproject/mini/）。
  QString miniManifestPath();
  QString miniDir();
  // paleo_mkproject 可执行件路径（编译定义 $<TARGET_FILE:> 注入）。
  QString mkprojectBinary();

  struct RunResult
  {
    bool ran = false; // 进程起得来且等到了退出
    int exitCode = -1;
    QString standardOutput;
    QString errorText; // QProcess 错误/超时诊断
    QString projectDir;
    QString qgzPath;
    QString paleoPath;
  };

  // 跑 paleo_mkproject --manifest <mini>/manifest.json --out <outDir>。
  // outDir 建好再启动（QTemporaryDir 由调用方持有生命周期）。
  RunResult buildMiniProject(const QString &outDir, int timeoutMs = 180000);

  // 对产出的工程目录直读 catalog（DataCatalog::open）并给摘要。
  struct CatalogSummary
  {
    bool opened = false;
    QString openError;
    int wells = 0;
    QStringList wellNames;                 // 排序后
    QHash<QString, int> entitiesByType;
    QHash<QString, int> linkRoles;         // role → 已决+未决总计数
    int unresolvedLinks = 0;
    int assets = 0;
    // 井名 → role → 链接数（只数该井已决链接）。
    QHash<QString, QHash<QString, int>> wellRoles;
    // 井名 → coordinateStatus（georeference 应用面）。
    QHash<QString, QString> wellCoordinateStatus;
  };
  CatalogSummary openCatalogSummary(const QString &projectDir);
} // namespace MkProjectFixture
