// 层：数据
#pragma once
#include "algorithms/cluster/cluster.h"
#include "domain/faciesclassification.h"
namespace paleo::crossplot {
class FaciesClassificationService {
public:
  // 全样本 Welford mean/sd 原地标准化——FaciesTrainingService::train 与
  // classify（KMeans/GMM/SOM）共用同一口径（全样本统计量，不按标注子集
  // 重估）；两处禁止各写一份，改这里即同步改训练侧。取消经 cancelled 带回。
  static bool standardizeMatrix(cluster::Matrix &, const cluster::Control &,
                                std::vector<double> &mean,
                                std::vector<double> &sd, bool *cancelled);
  static Classification classify(const SampleSet &,
                                 const ClassificationOptions &,
                                 const cluster::Control & = {},
                                 const Classification &previous = {});
  static QVector<WellInterval> intervals(const SampleSet &,
                                         const Classification &);
  // File writes must be called inside PaleoProjectStore::enqueueWrite by the
  // workflow.
  static bool writeRaster(const QString &, const SampleSet &,
                          const Classification &, QString *error);
  static bool writeIntervals(const QString &, const SampleSet &,
                             const Classification &, QString *error);
  // 置信度伴生栅格：GDT_Float32、同分类栅格网格、nodata=-9999、未采样像素
  // 留 nodata；metadata key PALEO_PROVENANCE（compact JSON provenance）。
  static bool writeConfidenceRaster(const QString &, const SampleSet &,
                                    const Classification &, QString *error);
  // 低置信掩膜栅格：Byte、同网格/同 palette/同 PALEO_CLASSIFICATION metadata
  // 口径；confidence < threshold 的像素写 255（nodata/透明），其余写类码。
  static bool writeMaskedRaster(const QString &, const SampleSet &,
                                const Classification &, double threshold,
                                QString *error);
};
} // namespace paleo::crossplot
