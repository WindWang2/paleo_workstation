// 层：QGIS 封装
#include "composeattribution.h"
#include <QHash>
#include <memory>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>
#include <qgsgeometry.h>
#include <qgsmaplayer.h>
#include <qgspointxy.h>
#include <qgsrasterblock.h>
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
  QString alignment;
  qint64 cells = 0;
  double valueMin = std::numeric_limits<double>::max();
  double valueMax = std::numeric_limits<double>::lowest();
  double valueSum = 0;
  QHash<int, qint64> codeCells;
  double nearestMax = 0;
};

struct CellGrid {
  QgsRectangle extent;
  int cols = 0, rows = 0;
};

// 网格来源裁决：优先保存时冻结的网格（点输入 compose 的历史 bug 是重放
// 按错误口径重建网格）；无记录时按 composeRaster 规则逐分支镜像重建。
struct GridInfo {
  CellGrid grid;
  QString source;        // frozen | reconstructed
  bool usable = false;
  bool definedByRaster = false;
  QString reason;        // usable=false 时的如实原因
};

GridInfo gridOf(const QList<ResolvedInput> &inputs, const FrozenGrid &frozen,
                QString *error) {
  GridInfo out;
  if (frozen.valid()) {
    out.grid = {frozen.extent, frozen.cols, frozen.rows};
    out.source = QStringLiteral("frozen");
    out.usable = true;
    return out;
  }
  out.source = QStringLiteral("reconstructed");
  // 与 composeRaster 逐分支镜像：首个声明为栅格的输入定网格——缺失/打不开
  // 时不能跳过取下一个栅格（那会换一张网格，静默失真）。排在定网格栅格
  // 之前的已从目录删除输入类型不可知（它可能就是定网格栅格），同样不能重建。
  for (const auto &input : inputs) {
    if (input.catalogMissing) {
      out.reason = QStringLiteral("冻结网格无法重建：输入 %1 类型未知（已从目录删除）")
                       .arg(input.layerId);
      return out;
    }
    if (!input.raster)
      continue;
    auto *r = (input.missing || !input.layer)
                  ? nullptr
                  : qobject_cast<QgsRasterLayer *>(input.layer);
    // D-04（full-audit-20261021）：退化范围栅格（宽/高为 0 的 geotransform）
    // 与另两条网格来源路径同口径拒绝——下游 replayAt/replay 按 extent 宽高
    // 除算像元，cw/ch 为 0 时产出 inf/UB 而非如实拒算。
    if (!r || !r->isValid() || !r->dataProvider() || r->width() <= 0 ||
        r->height() <= 0 || r->extent().isEmpty()) {
      out.reason = QStringLiteral("冻结网格无法重建：首个栅格输入缺失或退化");
      return out;
    }
    out.grid = {r->extent(), r->width(), r->height()};
    out.usable = true;
    out.definedByRaster = true;
    return out;
  }
  bool extentSet = false;
  for (const auto &input : inputs) {
    if (input.missing || !input.layer || !input.layer->isValid()) {
      out.reason = QStringLiteral("冻结网格无法重建：输入 %1 缺失")
                       .arg(input.layerId);
      return out;
    }
    if (!extentSet) {
      out.grid.extent = input.layer->extent();
      extentSet = true;
    } else {
      out.grid.extent.combineExtentWith(input.layer->extent());
    }
  }
  if (!extentSet || out.grid.extent.isEmpty()) {
    out.reason = QStringLiteral("无法重建编图网格（无可用输入范围）");
    if (error)
      *error = out.reason;
    return out;
  }
  // composeRaster 只在退化范围时 grow；历史 gridOf 无条件 grow 是点输入
  // compose 网格错位的根因。
  if (out.grid.extent.width() < 1 || out.grid.extent.height() < 1)
    out.grid.extent.grow(100);
  out.grid.cols = out.grid.rows = 64;
  out.usable = true;
  return out;
}

QgsPointXY cellCenter(const CellGrid &g, int x, int y) {
  return {g.extent.xMinimum() + (x + .5) * g.extent.width() / g.cols,
          g.extent.yMaximum() - (y + .5) * g.extent.height() / g.rows};
}

// 采样窗口 → 块读取。窗口超过上限时退回逐像元 sample()（与重放结果等价、
// 更慢但无界内存保护）。fallback 记 sampled_fallback 口径。
constexpr qint64 kMaxBlockCells = 1048576;

struct RasterRead {
  bool fallback = false;
  QgsRasterDataProvider *provider = nullptr;
  std::unique_ptr<QgsRasterBlock> block;
  int originX = 0, originY = 0;
  bool valueAt(int x, int y, const QgsPointXY &p, double *out) const {
    if (fallback || !block) {
      bool ok = false;
      const double v = provider->sample(p, 1, &ok);
      if (!ok || !std::isfinite(v))
        return false;
      *out = v;
      return true;
    }
    const int i = (y - originY) * block->width() + (x - originX);
    if (block->isNoData(i))
      return false;
    const double v = block->value(i);
    if (!std::isfinite(v))
      return false;
    *out = v;
    return true;
  }
};

std::vector<RasterRead> rasterReads(const QList<ResolvedInput> &inputs,
                                int x0, int y0, int x1, int y1,
                                const CellGrid &grid) {
  std::vector<RasterRead> reads(inputs.size());
  const qint64 w = x1 - x0 + 1, h = y1 - y0 + 1;
  const bool tooBig = w * h > kMaxBlockCells;
  // 窗口外沿用精确像元边界（行主序，北起）。
  const double cw = grid.extent.width() / grid.cols;
  const double ch = grid.extent.height() / grid.rows;
  const QgsRectangle window(grid.extent.xMinimum() + x0 * cw,
                            grid.extent.yMaximum() - (y1 + 1) * ch,
                            grid.extent.xMinimum() + (x1 + 1) * cw,
                            grid.extent.yMaximum() - y0 * ch);
  for (int i = 0; i < inputs.size(); ++i) {
    const auto &input = inputs[i];
    if (input.missing || !input.layer || !input.layer->isValid())
      continue;
    auto *r = qobject_cast<QgsRasterLayer *>(input.layer);
    if (!r || !r->dataProvider())
      continue;
    RasterRead &read = reads[i];
    read.provider = r->dataProvider();
    read.originX = x0;
    read.originY = y0;
    if (tooBig) {
      read.fallback = true;
      continue;
    }
    read.block.reset(read.provider->block(1, window, int(w), int(h)));
    if (!read.block || !read.block->isValid()) {
      read.block.reset();
      read.fallback = true;
    }
  }
  return reads;
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
// rasterReads 非空时栅格经块/窗口读取（cellX/Y 为像元行列）；否则按 p 取样。
CellOutcome decideAt(const QList<ResolvedInput> &inputs, const QList<int> &codes,
                     const QList<double> &thresholds, const QgsPointXY &p,
                     int cellX, int cellY,
                     const std::vector<RasterRead> *reads,
                     QHash<int, PointIndex> *pointIndexes) {
  CellOutcome out;
  for (int i = 0; i < inputs.size(); ++i) {
    const auto &input = inputs[i];
    if (input.missing || !input.layer || !input.layer->isValid())
      continue;
    if (auto *r = qobject_cast<QgsRasterLayer *>(input.layer)) {
      if (!r->dataProvider())
        continue;
      double v = 0;
      bool ok = false;
      if (reads && (*reads)[i].provider)
        ok = (*reads)[i].valueAt(cellX, cellY, p, &v);
      else {
        bool sampled = false;
        v = r->dataProvider()->sample(p, 1, &sampled);
        ok = sampled;
      }
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

// 输入栅格与编图网格的对齐口径。grid_defining：范围与宽高逐位同网格
//（composeRaster 取首个栅格本身）；aligned：同像元大小且原点偏移为整像元；
// 否则 block() 按网格重采样 → resampled。
QString alignmentFor(const CellGrid &g, const QgsRasterLayer *r) {
  if (!r || r->width() <= 0 || r->height() <= 0)
    return QStringLiteral("unavailable");
  const QgsRectangle e = r->extent();
  const double gx = g.extent.width() / g.cols, gy = g.extent.height() / g.rows;
  const double rx = e.width() / r->width(), ry = e.height() / r->height();
  const double m =
      std::max({1.0, std::abs(g.extent.width()), std::abs(g.extent.height())});
  if (r->width() == g.cols && r->height() == g.rows &&
      std::abs(e.xMinimum() - g.extent.xMinimum()) <= 1e-9 * m &&
      std::abs(e.yMinimum() - g.extent.yMinimum()) <= 1e-9 * m &&
      std::abs(e.xMaximum() - g.extent.xMaximum()) <= 1e-9 * m &&
      std::abs(e.yMaximum() - g.extent.yMaximum()) <= 1e-9 * m)
    return QStringLiteral("grid_defining");
  if (std::abs(rx - gx) <= 1e-6 * gx && std::abs(ry - gy) <= 1e-6 * gy) {
    const double ox = (e.xMinimum() - g.extent.xMinimum()) / gx;
    const double oy = (e.yMaximum() - g.extent.yMaximum()) / gy;
    if (std::abs(ox - std::round(ox)) <= 1e-6 &&
        std::abs(oy - std::round(oy)) <= 1e-6)
      return QStringLiteral("aligned");
  }
  return QStringLiteral("resampled");
}

// 逐输入口径 + 整体保真度。exact 需要：网格来自冻结记录（或可由首个可用
// 栅格完整重建）且每个至少赢过一个像元的栅格输入与网格严格同口径、无人
// 退回逐点取样；其余一律 approximate 并如实给出理由。
struct Fidelity {
  QString value; // exact | approximate
  QStringList reasons;
};
Fidelity fidelityOf(const GridInfo &grid,
                    const QVector<QString> &alignments,
                    const QList<ResolvedInput> &inputs,
                    const std::function<qint64(int)> &wonCells) {
  Fidelity f{QStringLiteral("exact"), {}};
  if (!grid.usable) {
    f.value = QStringLiteral("approximate");
    f.reasons << QStringLiteral("编图网格不可用，未能按网格口径重放");
    return f;
  }
  if (grid.source != QLatin1String("frozen") && !grid.definedByRaster) {
    f.value = QStringLiteral("approximate");
    f.reasons << QStringLiteral("旧成果无冻结网格记录，已按输入合并范围重建");
  }
  for (int i = 0; i < inputs.size(); ++i) {
    if (!inputs[i].raster || wonCells(i) <= 0)
      continue;
    const auto &a = alignments.value(i);
    if (a == QLatin1String("resampled")) {
      f.value = QStringLiteral("approximate");
      f.reasons << QStringLiteral("输入 %1 与编图网格不对齐，按重采样取值")
                       .arg(inputs[i].layerId);
    } else if (a == QLatin1String("sampled_fallback")) {
      f.value = QStringLiteral("approximate");
      f.reasons << QStringLiteral("输入 %1 采样窗口过大，退化为逐点取样")
                       .arg(inputs[i].layerId);
    }
  }
  return f;
}

QVariantMap statsToMap(const InputStats &s, qint64 total) {
  QVariantMap m;
  m.insert(QStringLiteral("layer_id"), s.input->layerId);
  m.insert(QStringLiteral("version_id"), s.input->versionId);
  m.insert(QStringLiteral("asset_id"), s.input->assetId);
  m.insert(QStringLiteral("title"), s.input->title);
  m.insert(QStringLiteral("priority"), s.input->priority);
  m.insert(QStringLiteral("decision"), s.decision);
  m.insert(QStringLiteral("alignment"), s.alignment);
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

FrozenGrid frozenGridFromExtra(const QVariantMap &composedExtra) {
  FrozenGrid out;
  const auto g = composedExtra.value(QStringLiteral("compose_grid")).toMap();
  const auto e = g.value(QStringLiteral("extent")).toList();
  if (e.size() != 4)
    return out;
  out.extent = QgsRectangle(e[0].toDouble(), e[1].toDouble(), e[2].toDouble(),
                            e[3].toDouble());
  out.cols = g.value(QStringLiteral("cols")).toInt();
  out.rows = g.value(QStringLiteral("rows")).toInt();
  if (!out.valid())
    return {};
  return out;
}

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

QList<int> codeList(const QVariantList &facies) {
  QList<int> codes;
  for (const auto &f : facies)
    codes << f.toMap().value("code").toInt();
  return codes;
}

// 每输入口径向量：缺失记 missing，点输入记 categorical_nearest，栅格按
// alignmentFor；块读取退回逐点的记 sampled_fallback。
QVector<QString> alignmentsOf(const QList<ResolvedInput> &inputs,
                              const GridInfo &grid,
                              const std::vector<RasterRead> *reads) {
  QVector<QString> out(inputs.size());
  for (int i = 0; i < inputs.size(); ++i) {
    const auto &input = inputs[i];
    if (input.missing || !input.layer || !input.layer->isValid()) {
      out[i] = QStringLiteral("missing");
      continue;
    }
    if (auto *r = qobject_cast<QgsRasterLayer *>(input.layer)) {
      if (!grid.usable) {
        out[i] = QStringLiteral("unavailable");
      } else if (reads && (*reads)[i].provider && (*reads)[i].fallback) {
        out[i] = QStringLiteral("sampled_fallback");
      } else {
        out[i] = alignmentFor(grid.grid, r);
      }
    } else if (qobject_cast<QgsVectorLayer *>(input.layer)) {
      out[i] = QStringLiteral("categorical_nearest");
    } else {
      out[i] = QStringLiteral("unavailable");
    }
  }
  return out;
}
} // namespace

QVariantMap replayAt(const QList<ResolvedInput> &inputs,
                     const QVariantList &facies, const QList<double> &thresholds,
                     const QPointF &point, QString *error,
                     const FrozenGrid &frozen) {
  const auto codes = codeList(facies);
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
  const auto grid = gridOf(inputs, frozen, error);
  QVariantMap out{{QStringLiteral("x"), point.x()},
                  {QStringLiteral("y"), point.y()}};
  QgsPointXY samplePoint(point);
  int cellX = -1, cellY = -1;
  std::vector<RasterRead> reads;
  QVector<QString> alignments;
  if (grid.usable) {
    // X4：与 composeRaster 同口径——单点落到像元后按像元中心决策。
    const double cw = grid.grid.extent.width() / grid.grid.cols;
    const double ch = grid.grid.extent.height() / grid.grid.rows;
    cellX = int(std::floor((point.x() - grid.grid.extent.xMinimum()) / cw));
    cellY = int(std::floor((grid.grid.extent.yMaximum() - point.y()) / ch));
    if (cellX < 0 || cellX >= grid.grid.cols || cellY < 0 ||
        cellY >= grid.grid.rows) {
      out.insert(QStringLiteral("status"), QStringLiteral("unresolved"));
      out.insert(QStringLiteral("message"),
                 QStringLiteral("位置在编图网格之外"));
      return out;
    }
    samplePoint = cellCenter(grid.grid, cellX, cellY);
    out.insert(QStringLiteral("cell"),
               QVariantMap{{QStringLiteral("col"), cellX},
                           {QStringLiteral("row"), cellY},
                           {QStringLiteral("x"), samplePoint.x()},
                           {QStringLiteral("y"), samplePoint.y()}});
    out.insert(QStringLiteral("query_point"),
               QVariantMap{{QStringLiteral("x"), point.x()},
                           {QStringLiteral("y"), point.y()}});
    reads = rasterReads(inputs, cellX, cellY, cellX, cellY, grid.grid);
    alignments = alignmentsOf(inputs, grid, &reads);
  } else {
    alignments = alignmentsOf(inputs, grid, nullptr);
  }
  QHash<int, PointIndex> pointIndexes;
  const auto outcome =
      decideAt(inputs, codes, thresholds, samplePoint, cellX, cellY,
               reads.empty() ? nullptr : &reads, &pointIndexes);
  const auto fidelity = fidelityOf(
      grid, alignments, inputs,
      [&outcome](int i) -> qint64 { return i == outcome.winner ? 1 : 0; });
  out.insert(QStringLiteral("grid_source"),
             grid.usable ? grid.source : QStringLiteral("unavailable"));
  out.insert(QStringLiteral("fidelity"), fidelity.value);
  if (!fidelity.reasons.isEmpty())
    out.insert(QStringLiteral("fidelity_reasons"),
               QVariantList(fidelity.reasons.cbegin(), fidelity.reasons.cend()));
  if (outcome.winner < 0) {
    out.insert(QStringLiteral("status"), QStringLiteral("unresolved"));
    if (outcome.rejectedCode)
      out.insert(QStringLiteral("message"),
                 QStringLiteral("该位置的相编码不在当前相分类中"));
    else
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
                   QString *error, const FrozenGrid &frozen) {
  if (request.usePoint) {
    auto out = replayAt(inputs, facies, thresholds, request.point, error, frozen);
    out.insert(QStringLiteral("facies_code"), request.faciesCode);
    return out;
  }
  const auto codes = codeList(facies);
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
  const auto grid = gridOf(inputs, frozen, error);
  if (!grid.usable)
    unresolved << QVariantMap{{QStringLiteral("reason"), grid.reason}};
  QVariantMap out{{QStringLiteral("algorithm"),
                   QStringLiteral("priority-first-valid; categorical-nearest; "
                                  "threshold-classification")},
                  {QStringLiteral("unresolved"), unresolved}};
  out.insert(QStringLiteral("grid_source"),
             grid.usable ? grid.source : QStringLiteral("unavailable"));
  if (!unresolved.isEmpty() && !grid.usable) {
    out.insert(QStringLiteral("status"), QStringLiteral("unresolved"));
    out.insert(QStringLiteral("message"),
               QStringLiteral("输入版本缺失且无法重建网格，归因不可用"));
    return out;
  }
  if (!grid.usable) {
    out.insert(QStringLiteral("status"), QStringLiteral("unresolved"));
    out.insert(QStringLiteral("message"), grid.reason);
    return out;
  }
  // 图斑范围 → 网格 bbox → 行主序等步长抽样（上限保护 UI 点击）。
  QgsRectangle probe = grid.grid.extent;
  if (!request.polygon.isNull() && !request.polygon.isEmpty()) {
    probe = request.polygon.boundingBox();
    probe = probe.intersect(grid.grid.extent);
    if (probe.isEmpty()) {
      out.insert(QStringLiteral("status"), QStringLiteral("unresolved"));
      out.insert(QStringLiteral("message"),
                 QStringLiteral("图斑范围在编图网格之外"));
      return out;
    }
  }
  int x0 = std::max(0, int(std::floor((probe.xMinimum() - grid.grid.extent.xMinimum()) /
                                      grid.grid.extent.width() * grid.grid.cols)));
  int x1 = std::min(grid.grid.cols - 1,
                    int(std::floor((probe.xMaximum() - grid.grid.extent.xMinimum()) /
                                   grid.grid.extent.width() * grid.grid.cols)));
  int y0 = std::max(0, int(std::floor((grid.grid.extent.yMaximum() - probe.yMaximum()) /
                                      grid.grid.extent.height() * grid.grid.rows)));
  int y1 = std::min(grid.grid.rows - 1,
                    int(std::floor((grid.grid.extent.yMaximum() - probe.yMinimum()) /
                                   grid.grid.extent.height() * grid.grid.rows)));
  const qint64 bboxCells = qint64(x1 - x0 + 1) * (y1 - y0 + 1);
  int stride = 1;
  while (bboxCells / (qint64(stride) * stride) > request.maxSampledCells)
    ++stride;
  const bool contains = !request.polygon.isNull() && !request.polygon.isEmpty();
  auto reads = rasterReads(inputs, x0, y0, x1, y1, grid.grid);
  const auto alignments = alignmentsOf(inputs, grid, &reads);
  QHash<int, PointIndex> pointIndexes;
  QVector<InputStats> stats(inputs.size());
  for (int i = 0; i < inputs.size(); ++i) {
    stats[i].input = &inputs[i];
    stats[i].alignment = alignments[i];
  }
  QHash<int, qint64> codeCells;
  qint64 sampled = 0, decided = 0, inGeometry = 0, rejectedCodeCells = 0;
  for (int y = y0; y <= y1; y += stride) {
    for (int x = x0; x <= x1; x += stride) {
      const auto p = cellCenter(grid.grid, x, y);
      if (contains && !request.polygon.contains(p.x(), p.y()))
        continue;
      ++sampled;
      const auto outcome = decideAt(inputs, codes, thresholds, p, x, y, &reads,
                                    &pointIndexes);
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
    if (s.cells > 0 || s.alignment != QLatin1String("missing"))
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
             QVariantMap{{QStringLiteral("cols"), grid.grid.cols},
                         {QStringLiteral("rows"), grid.grid.rows},
                         {QStringLiteral("extent"),
                          QVariantList{grid.grid.extent.xMinimum(),
                                       grid.grid.extent.yMinimum(),
                                       grid.grid.extent.xMaximum(),
                                       grid.grid.extent.yMaximum()}}});
  const auto fidelity =
      fidelityOf(grid, alignments, inputs,
                 [&stats](int i) { return stats.value(i).cells; });
  out.insert(QStringLiteral("fidelity"), fidelity.value);
  if (!fidelity.reasons.isEmpty())
    out.insert(QStringLiteral("fidelity_reasons"),
               QVariantList(fidelity.reasons.cbegin(), fidelity.reasons.cend()));
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
                      const QList<double> &thresholds, qint64 maxCells,
                      const FrozenGrid &frozen) {
  GridReplay out;
  const auto codes = codeList(facies);
  if (codes.isEmpty())
    return out;
  if (!thresholdsValid(inputs, codes, thresholds))
    return out; // 冻结参数疑似损坏：宁可不重放，不越界读 codes[]。
  const auto grid = gridOf(inputs, frozen, nullptr);
  if (!grid.usable)
    return out;
  if (qint64(grid.grid.cols) * grid.grid.rows > maxCells)
    return out; // 有界：超限由调用方退化为潜在影响。
  out.gridSource = grid.source;
  out.cols = grid.grid.cols;
  out.rows = grid.grid.rows;
  out.extent = grid.grid.extent;
  out.codes.resize(grid.grid.cols * grid.grid.rows);
  out.winners.resize(grid.grid.cols * grid.grid.rows);
  out.codes.fill(-9999);
  out.winners.fill(-1);
  // 批量读：栅格输入每个只读一次 block（与 composeRaster 同法、同口径），
  // 而不是逐像元 sample()——影响分析的大网格在 UI 线程同步执行，逐点
  // 取样是分钟级冻结。相点输入仍走空间索引最近邻。
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
        r->dataProvider()->block(1, grid.grid.extent, grid.grid.cols,
                                 grid.grid.rows));
    if (block && block->isValid()) {
      rasterIndexByInput.insert(i, rasterBlocks.size());
      rasterBlocks << std::move(block);
    }
  }
  QHash<int, PointIndex> pointIndexes;
  QVector<qint64> wonCells(inputs.size(), 0);
  for (int y = 0; y < grid.grid.rows; ++y) {
    for (int x = 0; x < grid.grid.cols; ++x) {
      const int i = y * grid.grid.cols + x;
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
          ++wonCells[n];
          continue;
        }
        const auto outcome = decideAtPoint(inputs, codes, n,
                                           cellCenter(grid.grid, x, y),
                                           &pointIndexes);
        if (outcome.winner >= 0) {
          out.winners[i] = outcome.winner;
          out.codes[i] = outcome.code;
          ++wonCells[outcome.winner];
        }
      }
    }
  }
  out.alignments.resize(inputs.size());
  for (int i = 0; i < inputs.size(); ++i) {
    const auto &input = inputs[i];
    if (input.missing || !input.layer || !input.layer->isValid())
      out.alignments[i] = QStringLiteral("missing");
    else if (auto *r = qobject_cast<QgsRasterLayer *>(input.layer))
      out.alignments[i] = rasterIndexByInput.contains(i)
                              ? alignmentFor(grid.grid, r)
                              : QStringLiteral("unavailable");
    else
      out.alignments[i] = QStringLiteral("categorical_nearest");
  }
  const auto fidelity = fidelityOf(grid, out.alignments, inputs,
                                   [&wonCells](int i) { return wonCells[i]; });
  out.fidelity = fidelity.value;
  out.fidelityReasons = fidelity.reasons;
  out.valid = true;
  return out;
}
} // namespace paleo::attribution
