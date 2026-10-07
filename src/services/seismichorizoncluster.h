// 层：数据
#pragma once
#include <QRectF>
#include <QString>
#include <QVector>
#include <functional>

enum class SeismicHorizonSource { TimeRaster, Scatter };

struct SeismicClusterResult {
  QVector<int> cells;
  QRectF extent;
  int columns = 64, rows = 64;
  int validCells = 0;
  QString error;
};

// 当前层位 TWT 窗内的真实反射特征 + 确定性聚类；解释类别映射为 Mock。
// Scatter 直接用 SMI 原始点的 Inline/Crossline/TWT 提取，输出保持原测网分辨率。
// 不外推层位，不填补测网外/缺道数据。调用方把工作放在任务线程。
SeismicClusterResult clusterSeismicHorizon(
    const QString &seismicPath, const QString &horizonPath,
    const QString &cacheDir, const QVector<int> &faciesCodes,
    const std::function<void(int)> &progress = {},
    const std::function<bool()> &cancel = {},
    SeismicHorizonSource source = SeismicHorizonSource::TimeRaster);
