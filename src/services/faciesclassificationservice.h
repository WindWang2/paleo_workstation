// 层：数据
#pragma once
#include "algorithms/cluster/cluster.h"
#include "domain/faciesclassification.h"
namespace paleo::crossplot {
class FaciesClassificationService {
public:
  static Classification classify(const SampleSet &,
                                 const ClassificationOptions &,
                                 const cluster::Control & = {},
                                 const std::vector<int> &previous = {});
  static QVector<WellInterval> intervals(const SampleSet &,
                                         const Classification &);
  // File writes must be called inside PaleoProjectStore::enqueueWrite by the
  // workflow.
  static bool writeRaster(const QString &, const SampleSet &,
                          const Classification &, QString *error);
  static bool writeIntervals(const QString &, const SampleSet &,
                             const Classification &, QString *error);
};
} // namespace paleo::crossplot
