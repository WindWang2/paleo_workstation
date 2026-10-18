// 层：数据
#include "seismicclusterlabels.h"
#include <QMap>
#include <QRandomGenerator>
#include <QSet>
#include <QObject>
#include <algorithm>
#include <cmath>

SeismicClusterResult labelSeismicClusters(
    SeismicClusterResult grid, const QVector<SeismicClusterWell> &wells,
    quint32 seed, const std::function<bool()> &cancel) {
  if (!grid.error.isEmpty()) return grid;
  grid.clusterMapping.clear(); grid.supportingWells.clear(); grid.wellVersionIds.clear();
  if (grid.columns <= 0 || grid.rows <= 0 || grid.extent.isEmpty() ||
      qint64(grid.columns) * grid.rows != grid.cells.size()) {
    grid.error = QObject::tr("聚类网格尺寸或范围无效"); return grid;
  }
  QMap<int, QMap<int, int>> votes;
  for (const int cluster : grid.cells)
    if (cluster != -9999) votes[cluster];
  QSet<int> observed;
  QSet<QString> seen;
  for (const auto &well : wells) {
    if (cancel && cancel()) { grid.error = QObject::tr("已取消"); return grid; }
    if (well.id.isEmpty() || seen.contains(well.id) || well.code <= 0 || well.code > 32767 ||
        !std::isfinite(well.x) || !std::isfinite(well.y)) continue;
    seen.insert(well.id);
    observed.insert(well.code);
    const double col = (well.x - grid.extent.left()) * grid.columns / grid.extent.width();
    const double row = (grid.extent.bottom() - well.y) * grid.rows / grid.extent.height();
    int cluster = -9999;
    if (col >= 0 && col < grid.columns && row >= 0 && row < grid.rows)
      cluster = grid.cells[int(std::floor(row)) * grid.columns + int(std::floor(col))];
    if (cluster != -9999) ++votes[cluster][well.code];
    grid.supportingWells << QVariantMap{{"id", well.id}, {"version_id", well.versionId},
        {"facies_code", well.code}, {"cluster_id", cluster}, {"voted", cluster != -9999}};
    if (!well.versionId.isEmpty() && !grid.wellVersionIds.contains(well.versionId))
      grid.wellVersionIds << well.versionId;
  }
  if (observed.isEmpty()) {
    grid.error = QObject::tr("当前层位没有有效测井相预测，无法标定地震聚类"); return grid;
  }
  auto allowed = observed.values();
  std::sort(allowed.begin(), allowed.end());
  QRandomGenerator random(seed);
  QMap<int, int> labels;
  for (auto it = votes.cbegin(); it != votes.cend(); ++it) {
    int chosen = 0, most = 0, total = 0;
    QVariantList counts;
    for (auto v = it->cbegin(); v != it->cend(); ++v) {
      if (v.value() > most) { most = v.value(); chosen = v.key(); }
      total += v.value();
      counts << QVariantMap{{"facies_code", v.key()}, {"count", v.value()}};
    }
    if (!total) chosen = allowed[random.bounded(quint32(allowed.size()))];
    labels[it.key()] = chosen;
    grid.clusterMapping << QVariantMap{{"cluster_id", it.key()}, {"facies_code", chosen},
        {"vote_count", total}, {"votes", counts},
        {"rule", total ? "well_majority_lowest_code_tie" : "seeded_random_observed_well_facies"}};
  }
  for (int i = 0; i < grid.cells.size(); ++i) {
    if ((i % 4096) == 0 && cancel && cancel()) { grid.error = QObject::tr("已取消"); return grid; }
    if (grid.cells[i] != -9999) grid.cells[i] = labels.value(grid.cells[i]);
  }
  return grid;
}
