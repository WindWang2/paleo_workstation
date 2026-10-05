// 层：数据
#include "sgsfill.h"

#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace paleo::stratgrid
{
namespace
{

void setError(QString *error, const QString &text)
{
  if (error)
    *error = text;
}

// 重合种子（同 cell）合并取均值——口径同 geostat::detail::dedupeSamples。
struct SeedAgg
{
  double sum = 0;
  int count = 0;
  int i = 0;
  int j = 0;
  int k = 0;
};

constexpr int kMinZoneSamples = 2; // 正态得分变换 + SK 至少 2 个样本
constexpr int kMaxAxis = 1 << 20;  // sgs3 格点索引打包上限

// 射线法偶奇测试（环闭合由首尾重合或环绕数语义兜底——开放环按闭合处理）。
bool pointInRing(double px, double py, const std::vector<double> &xs,
                 const std::vector<double> &ys)
{
  bool inside = false;
  const std::size_t n = xs.size();
  for (std::size_t i = 0, j = n - 1; i < n; j = i++)
  {
    const double yi = ys[i];
    const double yj = ys[j];
    if ( ( yi > py ) != ( yj > py ) )
    {
      const double xAt = xs[j] + ( py - yj ) * ( xs[i] - xs[j] ) / ( yi - yj );
      if (px < xAt)
        inside = !inside;
    }
  }
  return inside;
}

} // namespace

std::vector<int> rasterizeZoneRings(const ZoneGrid &grid, const std::vector<ZoneRing> &rings)
{
  std::vector<int> zones(static_cast<std::size_t>(grid.ni) * grid.nj, -1);
  for (int j = 0; j < grid.nj; ++j)
  {
    for (int i = 0; i < grid.ni; ++i)
    {
      if (!grid.columnLive(i, j))
        continue;
      const double px = grid.originX + ( static_cast<double>(i) + 0.5 ) * grid.dx;
      const double py = grid.originY + ( static_cast<double>(j) + 0.5 ) * grid.dy;
      for (const ZoneRing &ring : rings)
      {
        if (ring.xs.size() < 3 || ring.xs.size() != ring.ys.size())
          continue;
        if (pointInRing(px, py, ring.xs, ring.ys))
        {
          zones[static_cast<std::size_t>(grid.columnIndex(i, j))] = ring.code;
          break; // 先命中先得（环序确定）
        }
      }
    }
  }
  return zones;
}

bool fillSgs(const ZoneGrid &grid, const std::vector<Seed> &seeds,
             const std::vector<FaultSegment> &faults, const std::vector<int> *zonePerColumn,
             const geostat::VariogramModel &model, const geostat::Sgs3Params &params,
             std::vector<PropertyVolume> *out, SgsFillMeta *meta,
             const FillProgress &progress, QString *error)
{
  if (!out || !meta)
  {
    setError(error, QStringLiteral("SGS 充填输出为空"));
    return false;
  }
  out->clear();
  *meta = SgsFillMeta{};
  if (grid.ni < 1 || grid.nj < 1 || grid.nk < 1)
  {
    setError(error, QStringLiteral("格架为空"));
    return false;
  }
  if (grid.ni >= kMaxAxis || grid.nj >= kMaxAxis || grid.nk >= kMaxAxis)
  {
    setError(error, QStringLiteral("格架轴数超 sgs3 索引上限"));
    return false;
  }
  if (!(model.range > 0.0) || model.nugget < 0.0 || model.sill < 0.0 ||
      !(model.nugget + model.sill > 0.0))
  {
    setError(error, QStringLiteral("变差模型非法：变程须为正，块金/基台非负且总基台为正"));
    return false;
  }
  if (zonePerColumn &&
      zonePerColumn->size() != static_cast<std::size_t>(grid.ni) * grid.nj)
  {
    setError(error, QStringLiteral("相带分区数组尺寸与格架不符"));
    return false;
  }

  const int R = std::clamp(params.nRealizations, 1, 64);
  geostat::Sgs3Params clamped = params;
  clamped.nRealizations = R;
  clamped.maxPoints = std::clamp(params.maxPoints, 1, 64);
  if (clamped.searchRadius < 0.0)
    clamped.searchRadius = 0.0;

  // 竖帘分块 → 连通组分（跨断块零条件泄漏的机制承担者）。
  std::vector<int> blocks;
  int blockCount = 0;
  assignColumnBlocks(grid, faults, &blocks, &blockCount);

  // 种子 → cell 聚合（同 cell 多井取均值）。
  std::unordered_map<int, SeedAgg> seedCells;
  seedCells.reserve(seeds.size());
  for (const Seed &seed : seeds)
  {
    if (!grid.inRange(seed.i, seed.j, seed.k) || !grid.columnLive(seed.i, seed.j) ||
        !std::isfinite(seed.value))
      continue;
    const int cell = grid.cellIndex(seed.i, seed.j, seed.k);
    SeedAgg &agg = seedCells[cell];
    if (agg.count == 0)
    {
      agg.i = seed.i;
      agg.j = seed.j;
      agg.k = seed.k;
    }
    agg.sum += seed.value;
    ++agg.count;
    if (agg.count > 1)
      ++meta->mergedSeedDuplicates;
  }
  meta->snappedSeedCells = static_cast<int>(seedCells.size());

  // 活柱层厚最小值（环扫剪枝的 z 步长保守界）。
  double minThickness = std::numeric_limits<double>::max();
  bool anyLive = false;
  for (int j = 0; j < grid.nj; ++j)
  {
    for (int i = 0; i < grid.ni; ++i)
    {
      if (!grid.columnLive(i, j))
        continue;
      anyLive = true;
      minThickness = std::min(minThickness, layerThickness(grid, i, j, 0));
    }
  }
  if (!anyLive)
  {
    setError(error, QStringLiteral("格架没有活柱"));
    return false;
  }
  if (!(minThickness > 0.0))
    minThickness = 1.0; // 退化护栏（buildZoneGrid 已保证正厚度，防御性）

  // 相带域枚举（升序确定）。
  std::set<int> zones;
  for (int j = 0; j < grid.nj; ++j)
    for (int i = 0; i < grid.ni; ++i)
    {
      if (!grid.columnLive(i, j))
        continue;
      zones.insert(zonePerColumn ? ( *zonePerColumn )[static_cast<std::size_t>(grid.columnIndex(i, j))]
                                 : 0);
    }
  meta->zoneCount = static_cast<int>(zones.size());

  geostat::Lattice3Steps steps;
  steps.x = std::fabs(grid.dx);
  steps.y = std::fabs(grid.dy);
  steps.zMin = minThickness;

  // 输出体（每实现一个，跨带散射后统一钉死）。
  out->assign(static_cast<std::size_t>(R), PropertyVolume{});
  const float kNan = std::numeric_limits<float>::quiet_NaN();
  for (PropertyVolume &volume : *out)
  {
    volume.grid = grid;
    volume.columnBlock = blocks;
    volume.blockCount = blockCount;
    volume.values.assign(static_cast<std::size_t>(grid.cellCount()), kNan);
  }

  int zoneIndex = 0;
  for (const int zone : zones)
  {
    // 带内种子与目标。
    std::vector<geostat::Sample3> zoneSamples;
    std::vector<geostat::Sgs3Target> zoneTargets;
    std::vector<int> targetCells;
    for (const auto &entry : seedCells)
    {
      const SeedAgg &agg = entry.second;
      const int column = grid.columnIndex(agg.i, agg.j);
      const int cellZone =
          zonePerColumn ? ( *zonePerColumn )[static_cast<std::size_t>(column)] : 0;
      if (cellZone != zone)
        continue;
      Point3 center;
      cellCenter(grid, agg.i, agg.j, agg.k, &center);
      geostat::Sample3 sample;
      sample.x = center.x;
      sample.y = center.y;
      sample.z = center.z;
      sample.value = agg.sum / static_cast<double>(agg.count);
      sample.group = blocks[static_cast<std::size_t>(column)];
      zoneSamples.push_back(sample);
    }
    for (int j = 0; j < grid.nj; ++j)
    {
      for (int i = 0; i < grid.ni; ++i)
      {
        if (!grid.columnLive(i, j))
          continue;
        const std::size_t column = static_cast<std::size_t>(grid.columnIndex(i, j));
        if (zonePerColumn && ( *zonePerColumn )[column] != zone)
          continue;
        const int group = blocks[column];
        for (int k = 0; k < grid.nk; ++k)
        {
          Point3 center;
          cellCenter(grid, i, j, k, &center);
          geostat::Sgs3Target target;
          target.ix = i;
          target.iy = j;
          target.iz = k;
          target.x = center.x;
          target.y = center.y;
          target.z = center.z;
          target.group = group;
          zoneTargets.push_back(target);
          targetCells.push_back(grid.cellIndex(i, j, k));
        }
      }
    }

    if (static_cast<int>(zoneSamples.size()) < kMinZoneSamples)
    {
      ++meta->insufficientZones;
      meta->insufficientZoneCodes.push_back(zone);
      ++zoneIndex;
      continue; // 该带保持未充填：不混全域参数（如实口径，meta 记档）
    }

    // 进度/取消：带内 f∈[0,1] 映射到总体 (zoneIndex+f)/zoneCount。
    double lastFraction = 0.0;
    geostat::Control control;
    control.progress = [&lastFraction](double f) { lastFraction = f; };
    control.cancelled = [&progress, &lastFraction, zoneIndex, &zones]() {
      if (!progress)
        return false;
      const double overall =
          ( static_cast<double>(zoneIndex) + lastFraction ) / static_cast<double>(zones.size());
      return !progress(overall);
    };

    const geostat::Sgs3Result result =
        geostat::sgs3(zoneSamples, zoneTargets, steps, model, clamped, control);
    if (result.status == geostat::Status::Cancelled)
    {
      setError(error, QStringLiteral("已取消"));
      out->clear();
      return false;
    }
    if (result.status != geostat::Status::Ok)
    {
      setError(error, QStringLiteral("序贯高斯模拟失败：%1")
                              .arg(QString::fromStdString(result.message)));
      out->clear();
      return false;
    }
    meta->solverFailures += result.solverFailures;
    for (int r = 0; r < R; ++r)
    {
      PropertyVolume &volume = ( *out )[static_cast<std::size_t>(r)];
      const std::vector<double> &realization =
          result.realizations[static_cast<std::size_t>(r)];
      for (std::size_t t = 0; t < zoneTargets.size(); ++t)
      {
        const double value = realization[t];
        if (std::isfinite(value))
          volume.values[static_cast<std::size_t>(targetCells[t])] = static_cast<float>(value);
      }
    }
    ++zoneIndex;
  }

  // 硬数据钉死：种子 cell 每实现回写种子值（float 存储精度）。
  for (PropertyVolume &volume : *out)
  {
    for (const auto &entry : seedCells)
    {
      const SeedAgg &agg = entry.second;
      volume.values[static_cast<std::size_t>(entry.first)] =
          static_cast<float>(agg.sum / static_cast<double>(agg.count));
    }
    volume.filledCells = 0;
    volume.unfilledLiveCells = 0;
    for (int j = 0; j < grid.nj; ++j)
      for (int i = 0; i < grid.ni; ++i)
      {
        if (!grid.columnLive(i, j))
          continue;
        for (int k = 0; k < grid.nk; ++k)
        {
          if (std::isfinite(volume.values[static_cast<std::size_t>(grid.cellIndex(i, j, k))]))
            ++volume.filledCells;
          else
            ++volume.unfilledLiveCells;
        }
      }
  }
  meta->realizationCount = R;

  QStringList caliber;
  caliber << QStringLiteral("序贯高斯：%1 实现，种子 %2（钉死硬数据）")
                 .arg(R)
                 .arg(meta->snappedSeedCells);
  caliber << QStringLiteral("分块 %1（竖帘阻断）").arg(blockCount);
  if (zonePerColumn)
  {
    caliber << QStringLiteral("相带分区 %1 域（各带独立统计，变差几何共享全局模型）")
                   .arg(meta->zoneCount);
    if (meta->insufficientZones > 0)
    {
      QStringList codes;
      for (int code : meta->insufficientZoneCodes)
        codes << QString::number(code);
      caliber << QStringLiteral("样本不足保持未充填的相带（<%1 种子）：%2")
                     .arg(kMinZoneSamples)
                     .arg(codes.join(QStringLiteral(",")));
    }
  }
  else
  {
    caliber << QStringLiteral("无相带输入：全域单一参数域");
  }
  if (meta->mergedSeedDuplicates > 0)
    caliber << QStringLiteral("重合种子合并 %1 次（取均值）").arg(meta->mergedSeedDuplicates);
  meta->caliber = caliber.join(QStringLiteral("；"));
  return true;
}

} // namespace paleo::stratgrid
