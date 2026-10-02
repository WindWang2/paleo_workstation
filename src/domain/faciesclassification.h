// 层：数据
#pragma once
#include "crossplotsamples.h"
#include <QPointF>
#include <QVariantMap>
#include <cstdint>

namespace paleo::crossplot {
enum class Classifier { KMeans, Gmm, Hull, Box };
struct ClassificationOptions {
  Classifier method = Classifier::KMeans;
  int k = 8, maxIterations = 100, manualClass = 0;
  std::uint64_t seed = 42;
  bool standardize = true;
  Axes axes;
  QVector<QPointF> selection;
};
struct ClassColor {
  int red = 0, green = 0, blue = 0;
};
// Distinct categorical data symbols, not UI chrome or geological facies claims.
inline ClassColor classColor(int code) {
  constexpr ClassColor palette[] = {
      {166, 206, 227}, {31, 120, 180}, {178, 223, 138}, {51, 160, 44},
      {251, 154, 153}, {227, 26, 28},  {253, 191, 111}, {255, 127, 0},
      {202, 178, 214}, {106, 61, 154}, {255, 255, 153}, {177, 89, 40}};
  return palette[unsigned(code) % 12];
}
struct Classification {
  bool ok = false, cancelled = false;
  QString error;
  std::vector<int> labels;
  std::vector<double> confidence, squaredDistance;
  QVariantMap provenance;
  QVector<qint64> counts;
};
struct WellInterval {
  QString wellId;
  double top = 0, base = 0, meanConfidence = 0;
  int classId = -1, sampleCount = 0;
};
struct FaciesProduct {
  bool ok = false;
  QString error, path, assetId, versionId, layerId;
  QVector<qint64> counts;
};
} // namespace paleo::crossplot
Q_DECLARE_METATYPE(paleo::crossplot::ClassificationOptions)
