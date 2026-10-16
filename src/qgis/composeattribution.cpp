// 层：QGIS 封装
#include "composeattribution.h"
#include "../catalog/datacatalog.h"
#include <QHash>
#include <memory>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>
#include <qgsgeometry.h>
#include <qgsmaplayer.h>
#include <qgspointxy.h>
#include <qgsrasterdataprovider.h>
#include <qgsrasterlayer.h>
#include <qgsspatialindex.h>
#include <qgsvectorlayer.h>

namespace paleo::attribution {
namespace {
struct PointIndex {
  // 与 composeRaster 的相点分支同构：质心 + 跳过空 facies_code，绝不插值。
  // QgsSpatialIndex 默认不存几何，最近邻距离用自存质心按欧氏距离计算。
  QgsSpatialIndex index;
  QHash<QgsFeatureId, int> codes;
  QHash<QgsFeatureId, QgsPointXY> centroids;
  bool built = false;
};

// 每输入的采样期聚合状态。
struct InputStats {
  const ResolvedInput *input = nullptr;
  QString decision;
  qint64 cells = 0;
  double valueMin = std::numeric_limits<double>::max();
  double valueMax = std::numeric_limits<double>::lowest();
  double valueSum = 0;
  QHash<int, qint64> codeCells;
  double nearestMax = 0;
};

struct Grid {
  QgsRectangle extent;
  int cols = 64, rows = 64;
  bool valid = false;
};

// composeRaster 的网格规则：首个栅格输入定义网格；无栅格时合并范围 64×64。
Grid gridOf(const QList<ResolvedInput> &inputs) {
  Grid g;
  for (const auto &input : inputs) {
    if (input.missing || !input.layer)
      continue;
    if (auto *r = qobject_cast<const QgsRasterLayer *>(input.layer)) {
      if (!r->isValid() || !r->dataProvider())
        continue;
      g.extent = r->extent();
      g.cols = r->width();
      g.rows = r->height();
      g.valid = true;
      return g;
    }
  }
  for (const auto &input : inputs) {
    if (input.missing || !input.layer || !input.layer->isValid())
      continue;
    g.extent.combineExtentWith(input.layer->extent());
  }
  if (g.extent.isEmpty() || g.extent.width() < 1 || g.extent.height() < 1)
    return {};
  g.extent.grow(100); // 与 composeRaster 的无栅格 fallback 同口径
  g.cols = g.rows = 64;
  g.valid = true;
  return g;
}

QgsPointXY cellCenter(const Grid &g, int x, int y) {
  return {g.extent.xMinimum() + (x + .5) * g.extent.width() / g.cols,
          g.extent.yMaximum() - (y + .5) * g.extent.height() / g.rows};
}

// 单点重放：按优先级找到第一个给出有效值的输入。返回胜出下标（-1 = 无）。
struct CellOutcome {
  int winner = -1;
  int code = -9999;
  double value = 0;
  QString decision;
  double distance = -1;
  bool rejectedCode = false; // 有效值但编码不在当前相分类中
};
CellOutcome decideAt(const QList<ResolvedInput> &inputs, const QList<int> &codes,
                     const QList<double> &thresholds, const QgsPointXY &p,
                     QHash<int, PointIndex> *pointIndexes) {
  CellOutcome out;
  for (int i = 0; i < inputs.size(); ++i) {
    const auto &input = inputs[i];
    if (input.missing || !input.layer || !input.layer->isValid())
      continue;
    if (auto *r = qobject_cast<QgsRasterLayer *>(input.layer)) {
      auto *provider = r->dataProvider();
      if (!provider)
        continue;
      bool ok = false;
      const double v = provider->sample(p, 1, &ok);
      if (!ok || !std::isfinite(v))
        continue;
      int code;
      if (input.continuous) {
        code = codes[static_cast<int>(std::upper_bound(thresholds.cbegin(),
                                                       thresholds.cend(), v) -
                                     thresholds.cbegin())];
        out.decision = QStringLiteral("threshold_classification");
      } else {
        code = static_cast<int>(std::round(v));
        if (std::abs(v - code) > 1e-6 || !codes.contains(code)) {
          // 未定义编码：运行时 composeRaster 会整幅拒收；如实上报（可能
          // 指示冻结数据与当前相分类 schema 不一致）。
          out.rejectedCode = true;
          return out;
        }
        out.decision = QStringLiteral("priority_first_valid");
      }
      out.winner = i;
      out.code = code;
      out.value = v;
      return out;
    }
    if (auto *v = qobject_cast<QgsVectorLayer *>(input.layer)) {
      auto &pi = (*pointIndexes)[i];
      if (!pi.built) {
        QgsFeature f;
        auto it = v->getFeatures();
        QgsFeatureId id = 0;
        while (it.nextFeature(f)) {
          if (f.attribute("facies_code").isNull() || f.geometry().isEmpty())
            continue;
          const auto centroid = f.geometry().centroid();
          QgsFeature indexed(++id);
          indexed.setGeometry(centroid);
          pi.index.addFeature(indexed);
          pi.codes.insert(id, f.attribute("facies_code").toInt());
          pi.centroids.insert(id, centroid.asPoint());
        }
        pi.built = true;
      }
      if (pi.codes.isEmpty())
        continue;
      const auto nearest = pi.index.nearestNeighbor(p, 1);
      if (nearest.isEmpty())
        continue;
      const auto id = *std::min_element(nearest.cbegin(), nearest.cend());
      const int code = pi.codes.value(id, -9999);
      if (!codes.contains(code))
        continue;
      out.winner = i;
      out.code = code;
      out.decision = QStringLiteral("categorical_nearest");
      out.value = code;
      const auto &c = pi.centroids.value(id);
      out.distance = std::hypot(p.x() - c.x(), p.y() - c.y());
      return out;
    }
  }
  return out;
}

// 只对指定（点输入）下标做最近邻决策——批量重放按输入顺序求值时用。
CellOutcome decideAtPoint(const QList<ResolvedInput> &inputs,
                          const QList<int> &codes, int inputIndex,
                          const QgsPointXY &p, QHash<int, PointIndex> *pointIndexes) {
  CellOutcome out;
  const auto &input = inputs[inputIndex];
  auto *v = qobject_cast<QgsVectorLayer *>(input.layer);
  if (!v)
    return out;
  auto &pi = (*pointIndexes)[inputIndex];
  if (!pi.built) {
    QgsFeature f;
    auto it = v->getFeatures();
    QgsFeatureId id = 0;
    while (it.nextFeature(f)) {
      if (f.attribute("facies_code").isNull() || f.geometry().isEmpty())
        continue;
      QgsFeature indexed(++id);
      indexed.setGeometry(f.geometry().centroid());
      pi.index.addFeature(indexed);
      pi.codes.insert(id, f.attribute("facies_code").toInt());
      pi.centroids.insert(id, indexed.geometry().asPoint());
    }
    pi.built = true;
  }
  if (pi.codes.isEmpty())
    return out;
  const auto nearest = pi.index.nearestNeighbor(p, 1);
  if (nearest.isEmpty())
    return out;
  const auto id = *std::min_element(nearest.cbegin(), nearest.cend());
  const int code = pi.codes.value(id, -9999);
  if (!codes.contains(code))
    return out;
  out.winner = inputIndex;
  out.code = code;
  out.decision = QStringLiteral("categorical_nearest");
  out.value = code;
  const auto &c = pi.centroids.value(id);
  out.distance = std::hypot(p.x() - c.x(), p.y() - c.y());
  return out;
}

QVariantMap statsToMap(const InputStats &s, qint64 total) {
  QVariantMap m;
  m.insert(QStringLiteral("layer_id"), s.input->layerId);
  m.insert(QStringLiteral("version_id"), s.input->versionId);
  m.insert(QStringLiteral("asset_id"), s.input->assetId);
  m.insert(QStringLiteral("title"), s.input->title);
  m.insert(QStringLiteral("priority"), s.input->priority);
  m.insert(QStringLiteral("decision"), s.decision);
  m.insert(QStringLiteral("cells"), QVariant::fromValue(s.cells));
  m.insert(QStringLiteral("fraction"),
           total > 0 ? double(s.cells) / double(total) : 0.0);
  if (s.cells > 0) {
    m.insert(QStringLiteral("value_min"), s.valueMin);
    m.insert(QStringLiteral("value_max"), s.valueMax);
    m.insert(QStringLiteral("value_mean"), s.valueSum / double(s.cells));
  }
  if (s.decision == QLatin1String("categorical_nearest") && s.cells > 0)
    m.insert(QStringLiteral("nearest_distance_max"), s.nearestMax);
  QVariantList codes;
  for (auto it = s.codeCells.cbegin(); it != s.codeCells.cend(); ++it)
    codes << QVariantMap{{QStringLiteral("code"), it.key()},
                         {QStringLiteral("cells"), QVariant::fromValue(it.value())}};
  std::sort(codes.begin(), codes.end(),
            [](const QVariant &a, const QVariant &b) {
              return a.toMap().value("cells").toLongLong() >
                     b.toMap().value("cells").toLongLong();
            });
  m.insert(QStringLiteral("code_distribution"), codes);
  return m;
}
} // namespace

namespace {
// 与 composeRaster 同口径的阈值校验（连续输入需要 N-1 个严格递增阈值）；
// 冻结 extra 被改坏时宁可拒绝重放也不用越界索引读 codes[]。
bool thresholdsValid(const QList<ResolvedInput> &inputs, const QList<int> &codes,
                     const QList<double> &thresholds) {
  bool continuous = false;
  for (const auto &input : inputs)
    continuous |= input.continuous;
  if (!continuous)
    return true;
  if (thresholds.size() != codes.size() - 1)
    return false;
  for (int i = 1; i < thresholds.size(); ++i)
    if (thresholds[i] <= thresholds[i - 1])
      return false;
  return true;
}
} // namespace

QVariantMap replayAt(const QList<ResolvedInput> &inputs,
                     const QVariantList &facies, const QList<double> &thresholds,
                     const QPointF &point, QString *error) {
  QList<int> codes;
  for (const auto &f : facies)
    codes << f.toMap().value("code").toInt();
  if (codes.isEmpty()) {
    if (error)
      *error = QStringLiteral("相分类为空，无法重放");
    return {};
  }
  if (!thresholdsValid(inputs, codes, thresholds)) {
    if (error)
      *error = QStringLiteral("分相阈值与相分类不匹配（需 N-1 个严格递增），冻结参数疑似损坏");
    return {};
  }
  QHash<int, PointIndex> pointIndexes;
  const auto outcome =
      decideAt(inputs, codes, thresholds, QgsPointXY(point), &pointIndexes);
  QVariantMap out{{QStringLiteral("x"), point.x()},
                  {QStringLiteral("y"), point.y()}};
  if (outcome.winner < 0) {
    out.insert(QStringLiteral("status"), QStringLiteral("unresolved"));
    out.insert(QStringLiteral("message"),
               QStringLiteral("该位置没有输入给出有效值（nodata 或越界）"));
    return out;
  }
  const auto &input = inputs[outcome.winner];
  out.insert(QStringLiteral("status"), QStringLiteral("ok"));
  out.insert(QStringLiteral("code"), outcome.code);
  out.insert(QStringLiteral("value"), outcome.value);
  out.insert(QStringLiteral("decision"), outcome.decision);
  if (outcome.decision == QLatin1String("threshold_classification") &&
      !thresholds.isEmpty()) {
    // 命中的阈值区间（左开右闭），如实反映当时的分类条件。
    const auto upper = std::upper_bound(thresholds.cbegin(), thresholds.cend(),
                                        outcome.value);
    const int bucket = int(upper - thresholds.cbegin());
    const double lower =
        bucket > 0 ? thresholds[bucket - 1] : -std::numeric_limits<double>::infinity();
    const double hi = bucket < thresholds.size()
                          ? thresholds[bucket]
                          : std::numeric_limits<double>::infinity();
    out.insert(QStringLiteral("threshold_band"),
               QVariantList{lower, hi});
    out.insert(QStringLiteral("thresholds"), QVariantList(thresholds.cbegin(),
                                                          thresholds.cend()));
  }
  if (outcome.decision == QLatin1String("categorical_nearest"))
    out.insert(QStringLiteral("nearest_distance"), outcome.distance);
  out.insert(QStringLiteral("winning_input"),
             QVariantMap{{QStringLiteral("layer_id"), input.layerId},
                         {QStringLiteral("version_id"), input.versionId},
                         {QStringLiteral("priority"), input.priority}});
  return out;
}

QVariantMap replay(const QList<ResolvedInput> &inputs, const QVariantList &facies,
                   const QList<double> &thresholds, const ReplayRequest &request,
                   QString *error) {
  if (request.usePoint) {
    auto out = replayAt(inputs, facies, thresholds, request.point, error);
    out.insert(QStringLiteral("facies_code"), request.faciesCode);
    return out;
  }
  QList<int> codes;
  for (const auto &f : facies)
    codes << f.toMap().value("code").toInt();
  if (codes.isEmpty()) {
    if (error)
      *error = QStringLiteral("相分类为空，无法重放");
    return {};
  }
  if (!thresholdsValid(inputs, codes, thresholds)) {
    if (error)
      *error = QStringLiteral("分相阈值与相分类不匹配（需 N-1 个严格递增），冻结参数疑似损坏");
    return {};
  }
  QVariantList unresolved;
  for (const auto &input : inputs) {
    if (input.missing)
      unresolved << QVariantMap{{QStringLiteral("layer_id"), input.layerId},
                                {QStringLiteral("version_id"), input.versionId},
                                {QStringLiteral("reason"), input.missingReason}};
    else if (!input.layer || !input.layer->isValid())
      unresolved << QVariantMap{{QStringLiteral("layer_id"), input.layerId},
                                {QStringLiteral("version_id"), input.versionId},
                                {QStringLiteral("reason"),
                                 QStringLiteral("冻结输入无法按保存时状态打开")}};
  }
  const auto grid = gridOf(inputs);
  if (!grid.valid) {
    unresolved << QVariantMap{{QStringLiteral("reason"),
                               QStringLiteral("无法确定编图网格（缺少栅格输入）")}};
  }
  QVariantMap out{{QStringLiteral("algorithm"),
                   QStringLiteral("priority-first-valid; categorical-nearest; "
                                  "threshold-classification")},
                  {QStringLiteral("unresolved"), unresolved}};
  if (!unresolved.isEmpty() && !grid.valid) {
    out.insert(QStringLiteral("status"), QStringLiteral("unresolved"));
    out.insert(QStringLiteral("message"),
               QStringLiteral("输入版本缺失且无法重建网格，归因不可用"));
    return out;
  }
  // 图斑范围 → 网格 bbox → 行主序等步长抽样（上限保护 UI 点击）。
  QgsRectangle probe = grid.extent;
  if (!request.polygon.isNull() && !request.polygon.isEmpty()) {
    probe = request.polygon.boundingBox();
    probe = probe.intersect(grid.extent);
    if (probe.isEmpty()) {
      out.insert(QStringLiteral("status"), QStringLiteral("unresolved"));
      out.insert(QStringLiteral("message"),
                 QStringLiteral("图斑范围在编图网格之外"));
      return out;
    }
  }
  int x0 = std::max(0, int(std::floor((probe.xMinimum() - grid.extent.xMinimum()) /
                                      grid.extent.width() * grid.cols)));
  int x1 = std::min(grid.cols - 1,
                    int(std::floor((probe.xMaximum() - grid.extent.xMinimum()) /
                                   grid.extent.width() * grid.cols)));
  int y0 = std::max(0, int(std::floor((grid.extent.yMaximum() - probe.yMaximum()) /
                                      grid.extent.height() * grid.rows)));
  int y1 = std::min(grid.rows - 1,
                    int(std::floor((grid.extent.yMaximum() - probe.yMinimum()) /
                                   grid.extent.height() * grid.rows)));
  const qint64 bboxCells = qint64(x1 - x0 + 1) * (y1 - y0 + 1);
  int stride = 1;
  while (bboxCells / (qint64(stride) * stride) > request.maxSampledCells)
    ++stride;
  const bool contains = !request.polygon.isNull() && !request.polygon.isEmpty();
  QHash<int, PointIndex> pointIndexes;
  QVector<InputStats> stats(inputs.size());
  for (int i = 0; i < inputs.size(); ++i)
    stats[i].input = &inputs[i];
  QHash<int, qint64> codeCells;
  qint64 sampled = 0, decided = 0, inGeometry = 0, rejectedCodeCells = 0;
  for (int y = y0; y <= y1; y += stride) {
    for (int x = x0; x <= x1; x += stride) {
      const auto p = cellCenter(grid, x, y);
      if (contains && !request.polygon.contains(p.x(), p.y()))
        continue;
      ++sampled;
      const auto outcome = decideAt(inputs, codes, thresholds, p, &pointIndexes);
      if (outcome.rejectedCode) {
        ++rejectedCodeCells;
        continue;
      }
      if (outcome.winner < 0)
        continue;
      ++decided;
      ++inGeometry;
      auto &s = stats[outcome.winner];
      if (s.decision.isEmpty())
        s.decision = outcome.decision;
      ++s.cells;
      s.valueMin = std::min(s.valueMin, outcome.value);
      s.valueMax = std::max(s.valueMax, outcome.value);
      s.valueSum += outcome.value;
      if (outcome.decision == QLatin1String("categorical_nearest"))
        s.nearestMax = std::max(s.nearestMax, outcome.distance);
      ++s.codeCells[outcome.code];
      ++codeCells[outcome.code];
    }
  }
  QVariantList inputRows;
  for (const auto &s : stats)
    if (s.cells > 0)
      inputRows << statsToMap(s, decided);
  QVariantList distribution;
  for (auto it = codeCells.cbegin(); it != codeCells.cend(); ++it)
    distribution << QVariantMap{{QStringLiteral("code"), it.key()},
                                {QStringLiteral("cells"),
                                 QVariant::fromValue(it.value())},
                                {QStringLiteral("fraction"),
                                 decided > 0 ? double(it.value()) / double(decided)
                                             : 0.0}};
  std::sort(distribution.begin(), distribution.end(),
            [](const QVariant &a, const QVariant &b) {
              return a.toMap().value("cells").toLongLong() >
                     b.toMap().value("cells").toLongLong();
            });
  out.insert(QStringLiteral("status"), QStringLiteral("ok"));
  out.insert(QStringLiteral("grid"),
             QVariantMap{{QStringLiteral("cols"), grid.cols},
                         {QStringLiteral("rows"), grid.rows},
                         {QStringLiteral("extent"),
                          QVariantList{grid.extent.xMinimum(), grid.extent.yMinimum(),
                                       grid.extent.xMaximum(), grid.extent.yMaximum()}}});
  out.insert(QStringLiteral("sampled_cells"), QVariant::fromValue(sampled));
  out.insert(QStringLiteral("decided_cells"), QVariant::fromValue(decided));
  out.insert(QStringLiteral("bbox_cells"), QVariant::fromValue(bboxCells));
  out.insert(QStringLiteral("sampling"),
             stride > 1 ? QStringLiteral("strided:%1").arg(stride)
                        : QStringLiteral("full"));
  out.insert(QStringLiteral("inputs"), inputRows);
  out.insert(QStringLiteral("code_distribution"), distribution);
  out.insert(QStringLiteral("facies_code"), request.faciesCode);
  if (rejectedCodeCells > 0)
    unresolved << QVariantMap{
        {QStringLiteral("reason"),
         QStringLiteral("采样中 %1 个像元的相编码不在当前相分类中（运行时"
                        "会整幅拒收；冻结数据与当前 schema 可能不一致）")
             .arg(rejectedCodeCells)}};
  if (request.faciesCode != 0 && decided > 0) {
    const auto matching = codeCells.value(request.faciesCode, 0);
    out.insert(QStringLiteral("consistent_fraction"),
               double(matching) / double(decided));
  }
  if (sampled == 0) {
    out.insert(QStringLiteral("status"), QStringLiteral("unresolved"));
    out.insert(QStringLiteral("message"),
               QStringLiteral("图斑内没有落入编图网格的采样点"));
  } else if (decided == 0) {
    out.insert(QStringLiteral("status"), QStringLiteral("unresolved"));
    out.insert(QStringLiteral("message"),
               QStringLiteral("图斑范围内所有采样点均为 nodata"));
  } else if (!unresolved.isEmpty()) {
    // 部分输入缺失：已用可打开的冻结输入重放，缺失者如实列出。
    out.insert(QStringLiteral("message"),
               QStringLiteral("部分输入缺失，重放结果仅覆盖可用的冻结输入"));
  }
  return out;
}
GridReplay replayGrid(const QList<ResolvedInput> &inputs,
                      const QVariantList &facies,
                      const QList<double> &thresholds, qint64 maxCells) {
  GridReplay out;
  QList<int> codes;
  for (const auto &f : facies)
    codes << f.toMap().value("code").toInt();
  if (codes.isEmpty())
    return out;
  if (!thresholdsValid(inputs, codes, thresholds))
    return out; // 冻结参数疑似损坏：宁可不重放，不越界读 codes[]。
  const auto grid = gridOf(inputs);
  if (!grid.valid)
    return out;
  if (qint64(grid.cols) * grid.rows > maxCells)
    return out; // 有界：超限由调用方退化为潜在影响。
  out.cols = grid.cols;
  out.rows = grid.rows;
  out.extent = grid.extent;
  out.codes.resize(grid.cols * grid.rows);
  out.winners.resize(grid.cols * grid.rows);
  out.codes.fill(-9999);
  out.winners.fill(-1);
  // 批量读：栅格输入每个只读一次 block（与 composeRaster 同法、同口径），
  // 而不是逐像元 sample()——影响分析的大网格在 UI 线程同步执行，逐点
  // 取样是分钟级冻结。相点输入仍走空间索引最近邻。
  struct RasterBlock {
    int inputIndex = -1;
    bool continuous = false;
    std::unique_ptr<QgsRasterBlock> block;
  };
  QVector<std::shared_ptr<QgsRasterBlock>> rasterBlocks;
  QHash<int, int> rasterIndexByInput; // 输入序 → rasterBlocks 下标
  for (int i = 0; i < inputs.size(); ++i) {
    const auto &input = inputs[i];
    if (input.missing || !input.layer || !input.layer->isValid())
      continue;
    auto *r = qobject_cast<QgsRasterLayer *>(input.layer);
    if (!r || !r->dataProvider())
      continue;
    // provider->block 返回裸指针，直接托管（无拷贝构造）。
    std::shared_ptr<QgsRasterBlock> block(
        r->dataProvider()->block(1, grid.extent, grid.cols, grid.rows));
    if (block && block->isValid()) {
      rasterIndexByInput.insert(i, rasterBlocks.size());
      rasterBlocks << std::move(block);
    }
  }
  QHash<int, PointIndex> pointIndexes;
  for (int y = 0; y < grid.rows; ++y) {
    for (int x = 0; x < grid.cols; ++x) {
      const int i = y * grid.cols + x;
      for (int n = 0; n < inputs.size() && out.winners[i] < 0; ++n) {
        const auto &input = inputs[n];
        if (input.missing || !input.layer || !input.layer->isValid())
          continue;
        if (auto *r = qobject_cast<QgsRasterLayer *>(input.layer)) {
          if (!r->dataProvider())
            continue;
          const int blockIndex = rasterIndexByInput.value(n, -1);
          if (blockIndex < 0)
            continue;
          const auto &block = rasterBlocks[static_cast<int>(blockIndex)];
          if (block->isNoData(i))
            continue;
          const double v = block->value(i);
          if (!std::isfinite(v))
            continue;
          int code;
          if (input.continuous) {
            code = codes[static_cast<int>(std::upper_bound(thresholds.cbegin(),
                                                           thresholds.cend(), v) -
                                         thresholds.cbegin())];
          } else {
            code = static_cast<int>(std::round(v));
            if (std::abs(v - code) > 1e-6 || !codes.contains(code)) {
              // 与 decideAt/replay 同口径：未定义编码的像元不判给任何输入
              //（运行时 composeRaster 会整幅拒收；schema 可能已变化）。
              break;
            }
          }
          out.winners[i] = n;
          out.codes[i] = code;
          continue;
        }
        const auto outcome =
            decideAtPoint(inputs, codes, n, cellCenter(grid, x, y), &pointIndexes);
        if (outcome.winner >= 0) {
          out.winners[i] = outcome.winner;
          out.codes[i] = outcome.code;
        }
      }
    }
  }
  out.valid = true;
  return out;
}
} // namespace paleo::attribution
