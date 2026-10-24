// 层：数据
#pragma once
#include <QRectF>
#include <QString>
#include <QVector>
#include <QVariantList>
#include <QStringList>
#include <functional>

enum class SeismicHorizonSource { TimeRaster, Scatter };

struct SeismicClusterResult {
  QVector<int> cells;
  QRectF extent;
  int columns = 64, rows = 64;
  int validCells = 0;
  QString crsWkt;
  QString error;
  // 覆盖缩水的如实留痕（B-06）：读取失败的 inline 整条缺席，与「本来没有」
  // 不可区分——调用方应把 warnings 呈现给用户/QC。
  QStringList warnings;
  QVariantList clusterMapping, supportingWells;
  QStringList wellVersionIds;
};

// 当前层位 TWT 窗内的真实反射特征 + 确定性聚类；解释类别映射为 Mock。
// Scatter 直接用 SMI 原始点的 Inline/Crossline/TWT 提取，逐道核对实际 XY；
// 输出按实际 XY 装入同尺寸北向上网格，同像元取距格心最近的原始点。
// 不外推层位，不填补测网外/缺道数据。调用方把工作放在任务线程。
SeismicClusterResult clusterSeismicHorizon(
    const QString &seismicPath, const QString &horizonPath,
    const QString &cacheDir, const QVector<int> &faciesCodes,
    const std::function<void(int)> &progress = {},
    const std::function<bool()> &cancel = {},
    SeismicHorizonSource source = SeismicHorizonSource::TimeRaster);
