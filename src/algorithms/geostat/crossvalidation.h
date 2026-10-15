// 层：算法（地质统计留一交叉验证）
#pragma once

#include "kriging.h" // Sample

#include <QString>
#include <QVariantMap>

#include <cstddef>
#include <functional>
#include <vector>

namespace paleo::geostat
{

// 样本数超过上限时按固定步长抽检（stride = ceil(n / 上限)），如实记录。
constexpr std::size_t kMaxCvSamples = 2000;

struct CvPrediction
{
  bool ok = false;
  double estimate = 0;
  double variance = 0;
};

using CvPredictFn = std::function<CvPrediction( double x, double y, const std::vector<Sample> &rest )>;

// 逐井剔除（连同坐标重合样本），用 predict 在该井位估值；返回 QC cross_validation 节点
//（status/estimator/scheme/samples_total/stride/attempted/estimated/failed/coincident_dropped/
// mean_error/rmse/mae/mean_standardized_squared_error/standardized_count）。
// cancelled() 为真时置 *wasCancelled 并返回空表。
QVariantMap leaveOneOut( const std::vector<Sample> &samples, const QString &estimator, const CvPredictFn &predict,
                         const std::function<bool()> &cancelled, bool *wasCancelled );

} // namespace paleo::geostat
