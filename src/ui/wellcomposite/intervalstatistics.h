// 层：视图
#pragma once

#include <QString>
#include <QVector>

#include "domain/wellcompositemodel.h"

// ui/wellcomposite/intervalstatistics — D2.2 橡皮筋区间统计
//
// 给定深度区间与井数据，产出：各曲线 min/max/mean/std（区间内样本）、岩性
// 厚度占比、标志层计数；报告可导出 TSV（一键复制）。

namespace WellComposite
{

struct CurveIntervalStats
{
  QString name;
  QString unit;
  int sampleCount = 0;
  double min = 0.0;
  double max = 0.0;
  double mean = 0.0;
  double stdDev = 0.0; // 样本标准差（n-1；n<2 为 0）
};

struct LithoProportion
{
  QString name;
  double thickness = 0.0;   // 区间内净厚度（跨界区间按截断计）
  double fraction = 0.0;    // 占区间岩性总厚度比例 [0,1]
};

struct IntervalStatsReport
{
  double top = 0.0;
  double bottom = 0.0;
  QVector<CurveIntervalStats> curves;
  QVector<LithoProportion> lithology;
  int markerCount = 0;

  bool isValid() const { return bottom > top; }

  // D2.2 一键复制 TSV（Excel/粘贴板友好）
  QString toTsv() const;
};

// 主计算入口：curves 取 continuous + discrete 并集；lithology/marker 取井数据。
// 区间边界按半开 [top, bottom) 采样。
IntervalStatsReport computeIntervalStats(double top, double bottom,
                                         const QVector<CurveData> &continuousCurves,
                                         const QVector<CurveData> &discreteCurves,
                                         const QVector<LithologyInterval> &lithology,
                                         const QVector<QPair<double, QString>> &markers);

// 便捷重载（直接吃 ComprehensiveWellData）
IntervalStatsReport computeIntervalStats(double top, double bottom, const ComprehensiveWellData &data);

} // namespace WellComposite
