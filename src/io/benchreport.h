// 层：数据
#pragma once
#include <QString>
#include <QVector>

// io/ — 基准结果模型 + JSON/HTML 报告（wave/io-perf-cache D8.1/D8.6）。
// selfcheck perf 组产出 BenchResult 列表 → JSON（机器可读，回归门消费）/
// HTML（人读摘要：表格 + 预算内绿条/超预算红条）。

struct BenchResult
{
  QString name;        // 如 "las_cold_ms"（稳定 id，回归门按键比对）
  QString group;       // las / segy / pyramid / catalog / io
  double value = 0.0;  // 数值
  QString unit;        // ms / bytes / ratio
  double budget = -1.0; // >0 = 有预算阈值（超 → fail）
  bool pass = true;    // budget 存在时按 value<=budget 判；否则恒 true
  QString note;        // 环境备注（冷/热、机器无关口径说明）
};

class BenchReport
{
  public:
    static QByteArray toJson(const QVector<BenchResult> &results);
    static bool writeJsonFile(const QString &path, const QVector<BenchResult> &results);

    // D8.6：单文件 HTML 摘要（内联 CSS，无外部依赖；group 分节 + 预算条）。
    static QByteArray toHtml(const QVector<BenchResult> &results, const QString &title);
    static bool writeHtmlFile(const QString &path, const QVector<BenchResult> &results,
                              const QString &title);
};
