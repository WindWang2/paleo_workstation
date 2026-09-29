// 层：数据
#pragma once
#include <QString>
#include <QStringList>
#include <functional>

// io/ — 流式读取器（wave/io-perf-cache D7.6）。
// GeoJSON/CSV/DC.dat/井文本从此不再 readAll 整文件进内存：分块 256KB 逐行
// 产出，带进度与协作取消；GeoJSON 包围盒用增量扫描器代替 QJsonDocument 全量
// DOM（DOM 路径在 geojsonaffine 保留给需要属性/几何编辑的调用方）。
namespace Streaming
{
  // 逐行流式读取。cancel 返回 true → 停止（已产出的行有效）。
  // 返回读到的行数（含空行）；文件打不开 → -1 + error。
  // 行产出口径与 QTextStream::readLine 对齐（\n / \r\n / \r 都算行界）。
  qint64 forEachLine(const QString &path,
                     const std::function<bool(qint64 lineIndex, const QString &line)> &onLine,
                     const std::function<bool()> &cancel = nullptr,
                     const std::function<void(qint64 done, qint64 total)> &progress = nullptr,
                     QString *error = nullptr);

  // CSV/空白分隔表：逐行切列（sep 为空 → 任意空白分隔）。
  qint64 forEachRecord(const QString &path, char sep,
                       const std::function<bool(qint64 row, const QStringList &cols)> &onRow,
                       const std::function<bool()> &cancel = nullptr,
                       QString *error = nullptr);

  // GeoJSON 包围盒增量扫描：只认 "coordinates" 数值数组的首两个数（点/线/
  // 面皆适用），不建 DOM、不整读。缺 coordinates / 坏 JSON 结构 → false。
  // outBounds = [minX, minY, maxX, maxY]。
  bool geoJsonBoundsStreaming(const QString &path, double outBounds[4],
                              const std::function<bool()> &cancel = nullptr,
                              QString *error = nullptr);

  // 流式数要素/计属性键集合（预览页首屏统计用）：返回 Feature 个数；属性键
  // 集合收进 outKeys（可选）。
  qint64 geoJsonFeatureCountStreaming(const QString &path, QStringList *outKeys = nullptr,
                                      QString *error = nullptr);
} // namespace Streaming
