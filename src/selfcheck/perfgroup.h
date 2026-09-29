// 层：测试壳
#pragma once
#include <QString>
#include <QVector>

struct BenchResult;

// selfcheck perf 组（wave/io-perf-cache D8.1）：纯数据层基准，无 QGIS 初始化。
// 输出 JSON（stdout + 可选 --json <path>），带预算阈值的项超限 → 退出码 1。
// 全部夹具由 PerfFixtures 现场生成（D8.5），不依赖本地真实工程。
namespace PerfGroup
{
  QVector<BenchResult> runAll(const QString &workDir);
  int runMain(const QStringList &args); // 处理 --json <path>；返回退出码
} // namespace PerfGroup
