// 层：数据
#pragma once
#include "crossplotsamples.h"
#include <QPointF>
#include <QStringList>
#include <QVariantMap>
#include <cstdint>

namespace paleo::crossplot {
// 新增分类器必须追加在末尾（UI combo 索引耦合），严禁插入中间。
enum class Classifier { KMeans, Gmm, Hull, Box, Som, Lda, Qda, Knn };
struct TrainingSet {
  // 平行 SampleSet 行的类码（-1 = 未标注）。类码即 classNames 下标；
  // 井段无落库相名字段可用（勘察结论），类名一律来自自由词套索。
  std::vector<int> labels;
  QStringList classNames; // 类码 → 自由词名
  QVariantMap provenance; // 至少含 labelProvenance = "free_text_lasso"
};
struct ClassificationOptions {
  Classifier method = Classifier::KMeans;
  int k = 8, maxIterations = 100, manualClass = 0;
  std::uint64_t seed = 42;
  bool standardize = true;
  Axes axes;
  QVector<QPointF> selection;
  int somWidth = 4, somHeight = 4;            // Som 原型网格（簇数 = 面积）
  int knnNeighbors = 5;                       // Knn 近邻数（训练时夹取到标注行数）
  int cvFolds = 5;                            // 监督训练分层交叉验证折数
  std::size_t emChunkBudgetBytes = 268435456; // GMM EM 责任度缓冲预算；0 = 全量路径
  double confidenceMaskThreshold = 0.5;       // 低置信掩膜阈值（[0,1]）
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
  // 置信度伴生三件套的受管落位与图层（无监督/监督同样落；井层段路径为空）。
  QString confidencePath, confidenceAssetId, confidenceVersionId, confidenceLayerId;
  QString maskedPath, maskedAssetId, maskedVersionId, maskedLayerId;
  QVector<qint64> counts;
};
} // namespace paleo::crossplot
Q_DECLARE_METATYPE(paleo::crossplot::ClassificationOptions)
