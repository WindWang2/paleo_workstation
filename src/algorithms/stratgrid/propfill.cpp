// 层：数据
#include "propfill.h"

#include "../faultsurface/faultsurface.h"

#include <QJsonDocument>
#include <QtEndian>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <queue>

namespace paleo::stratgrid
{
namespace
{

void setError(QString *error, const QString &text)
{
  if (error)
    *error = text;
}

int orient(double ax, double ay, double bx, double by, double cx, double cy)
{
  const double v = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
  const double scale = 1.0 + std::fabs(ax) + std::fabs(ay) + std::fabs(bx) + std::fabs(by) +
                       std::fabs(cx) + std::fabs(cy);
  const double eps = 1e-9 * scale;
  if (v > eps)
    return 1;
  if (v < -eps)
    return -1;
  return 0;
}

bool onSegment(double ax, double ay, double bx, double by, double px, double py)
{
  const double eps = 1e-9 * (1.0 + std::fabs(ax) + std::fabs(bx) + std::fabs(px));
  return px >= std::min(ax, bx) - eps && px <= std::max(ax, bx) + eps &&
         py >= std::min(ay, by) - eps && py <= std::max(ay, by) + eps;
}

bool samePoint(double ax, double ay, double bx, double by)
{
  const double scale = 1.0 + std::fabs(ax) + std::fabs(ay) + std::fabs(bx) + std::fabs(by);
  const double eps = 1e-9 * scale;
  const double dx = ax - bx;
  const double dy = ay - by;
  return dx * dx + dy * dy <= eps * eps;
}

// 共线且落在线段上，但不是该线段的端点。
bool interiorPoint(double ax, double ay, double bx, double by, double px, double py)
{
  if (samePoint(ax, ay, px, py) || samePoint(bx, by, px, py))
    return false;
  return onSegment(ax, ay, bx, by, px, py);
}

// 共线区间的重叠长度。两端点重合的正长度区间算重叠；仅共享一个端点则长度为 0。
double collinearOverlapLength(double ax, double ay, double bx, double by, double cx, double cy,
                              double dx, double dy)
{
  const double abx = bx - ax;
  const double aby = by - ay;
  const double ab2 = abx * abx + aby * aby;
  if (!(ab2 > 0.0))
    return 0.0;
  const double tC = ((cx - ax) * abx + (cy - ay) * aby) / ab2;
  const double tD = ((dx - ax) * abx + (dy - ay) * aby) / ab2;
  const double lo = std::max(0.0, std::min(tC, tD));
  const double hi = std::min(1.0, std::max(tC, tD));
  if (!(hi > lo))
    return 0.0;
  return (hi - lo) * std::sqrt(ab2);
}

// 柱心连线（闭区间）与断层段：严格穿越、端点落在对方内部、或共线重叠长度 > ε 才阻断。
// 只共享端点不阻断。零长度断层不阻断。
bool segmentsCross(double ax, double ay, double bx, double by, double cx, double cy, double dx,
                   double dy)
{
  const double cdx = dx - cx;
  const double cdy = dy - cy;
  const double faultScale = 1.0 + std::fabs(cx) + std::fabs(cy) + std::fabs(dx) + std::fabs(dy);
  const double faultEps = 1e-9 * faultScale;
  if (cdx * cdx + cdy * cdy <= faultEps * faultEps)
    return false;

  const int o1 = orient(ax, ay, bx, by, cx, cy);
  const int o2 = orient(ax, ay, bx, by, dx, dy);
  const int o3 = orient(cx, cy, dx, dy, ax, ay);
  const int o4 = orient(cx, cy, dx, dy, bx, by);
  if (o1 * o2 < 0 && o3 * o4 < 0)
    return true;
  if ((o1 == 0 && interiorPoint(ax, ay, bx, by, cx, cy)) ||
      (o2 == 0 && interiorPoint(ax, ay, bx, by, dx, dy)) ||
      (o3 == 0 && interiorPoint(cx, cy, dx, dy, ax, ay)) ||
      (o4 == 0 && interiorPoint(cx, cy, dx, dy, bx, by)))
    return true;
  if (o1 == 0 && o2 == 0 && o3 == 0 && o4 == 0)
  {
    const double scale = 1.0 + std::fabs(ax) + std::fabs(ay) + std::fabs(bx) + std::fabs(by) +
                         std::fabs(cx) + std::fabs(cy) + std::fabs(dx) + std::fabs(dy);
    return collinearOverlapLength(ax, ay, bx, by, cx, cy, dx, dy) > 1e-8 * scale;
  }
  return false;
}

void columnCenter(const ZoneGrid &grid, int i, int j, double *x, double *y)
{
  *x = grid.originX + (static_cast<double>(i) + 0.5) * grid.dx;
  *y = grid.originY + (static_cast<double>(j) + 0.5) * grid.dy;
}

bool edgeBlocked(const ZoneGrid &grid, int i0, int j0, int i1, int j1,
                 const std::vector<FaultSegment> &faults)
{
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  columnCenter(grid, i0, j0, &x0, &y0);
  columnCenter(grid, i1, j1, &x1, &y1);
  for (const FaultSegment &f : faults)
  {
    if (segmentsCross(x0, y0, x1, y1, f.x0, f.y0, f.x1, f.y1))
      return true;
  }
  return false;
}

void assignBlocks(const ZoneGrid &grid, const std::vector<FaultSegment> &faults,
                  std::vector<int> *columnBlock, int *blockCount)
{
  const int n = grid.ni * grid.nj;
  columnBlock->assign(static_cast<std::size_t>(n), -1);
  int next = 0;
  const int di[4] = {-1, 1, 0, 0};
  const int dj[4] = {0, 0, -1, 1};
  for (int j = 0; j < grid.nj; ++j)
  {
    for (int i = 0; i < grid.ni; ++i)
    {
      const int start = grid.columnIndex(i, j);
      if (!grid.columnLive(i, j) || (*columnBlock)[static_cast<std::size_t>(start)] >= 0)
        continue;
      std::queue<std::pair<int, int>> q;
      q.emplace(i, j);
      (*columnBlock)[static_cast<std::size_t>(start)] = next;
      while (!q.empty())
      {
        const auto [ci, cj] = q.front();
        q.pop();
        for (int nEdge = 0; nEdge < 4; ++nEdge)
        {
          const int ni = ci + di[nEdge];
          const int nj = cj + dj[nEdge];
          if (ni < 0 || nj < 0 || ni >= grid.ni || nj >= grid.nj || !grid.columnLive(ni, nj))
            continue;
          const int idx = grid.columnIndex(ni, nj);
          if ((*columnBlock)[static_cast<std::size_t>(idx)] >= 0)
            continue;
          if (edgeBlocked(grid, ci, cj, ni, nj, faults))
            continue;
          (*columnBlock)[static_cast<std::size_t>(idx)] = next;
          q.emplace(ni, nj);
        }
      }
      ++next;
    }
  }
  *blockCount = next;
}

void trackRange(float v, bool *any, float *vmin, float *vmax)
{
  if (!std::isfinite(v))
    return;
  if (!*any)
  {
    *vmin = v;
    *vmax = v;
    *any = true;
  }
  else
  {
    *vmin = std::min(*vmin, v);
    *vmax = std::max(*vmax, v);
  }
}

} // namespace

void assignColumnBlocks(const ZoneGrid &grid, const std::vector<FaultSegment> &faults,
                        std::vector<int> *columnBlock, int *blockCount)
{
  if (!columnBlock || !blockCount)
    return;
  assignBlocks(grid, faults, columnBlock, blockCount);
}

std::vector<Seed> seedsFromUpscale(const UpscaleTable &table)
{
  std::vector<Seed> seeds;
  for (int w = 0; w < table.nWells; ++w)
  {
    for (int k = 0; k < table.nLayers; ++k)
    {
      const LayerValue &cell = table.at(w, k);
      if (!cell.hasValue || cell.columnI < 0 || cell.columnJ < 0)
        continue;
      seeds.push_back(Seed{cell.columnI, cell.columnJ, k, cell.value});
    }
  }
  return seeds;
}

bool fillIdw(const ZoneGrid &grid, const std::vector<Seed> &seeds,
             const std::vector<FaultSegment> &faults, double power, PropertyVolume *out,
             const FillProgress &progress, QString *error)
{
  if (!out)
  {
    setError(error, QStringLiteral("充填输出为空"));
    return false;
  }
  if (!(power > 0.0) || !std::isfinite(power))
  {
    setError(error, QStringLiteral("IDW 幂次须为正有限值"));
    return false;
  }
  if (grid.nk < 1 || grid.ni < 1 || grid.nj < 1)
  {
    setError(error, QStringLiteral("格架为空"));
    return false;
  }

  PropertyVolume local;
  local.grid = grid;
  assignBlocks(grid, faults, &local.columnBlock, &local.blockCount);
  const float kNan = std::numeric_limits<float>::quiet_NaN();
  local.values.assign(static_cast<std::size_t>(grid.cellCount()), kNan);

  std::vector<std::vector<Seed>> byBlock(static_cast<std::size_t>(std::max(local.blockCount, 0)));
  for (const Seed &seed : seeds)
  {
    if (!grid.columnLive(seed.i, seed.j) || seed.k < 0 || seed.k >= grid.nk ||
        !std::isfinite(seed.value))
      continue;
    const int block = local.columnBlock[static_cast<std::size_t>(grid.columnIndex(seed.i, seed.j))];
    if (block < 0)
      continue;
    byBlock[static_cast<std::size_t>(block)].push_back(seed);
  }

  if (progress && !progress(0.0))
  {
    setError(error, QStringLiteral("已取消"));
    return false;
  }

  // 垂向步长：活柱层厚 (bot−top)/nk 的柱平均。各柱内各层等厚，柱间可以不同。
  double verticalStep = 1.0;
  {
    double thickSum = 0.0;
    int nLive = 0;
    for (int j = 0; j < grid.nj; ++j)
    {
      for (int i = 0; i < grid.ni; ++i)
      {
        if (!grid.columnLive(i, j))
          continue;
        thickSum += layerThickness(grid, i, j, 0);
        ++nLive;
      }
    }
    if (nLive > 0)
      verticalStep = thickSum / static_cast<double>(nLive);
  }

  const int rows = grid.nj;
  int filled = 0;
  int unfilled = 0;
  for (int j = 0; j < grid.nj; ++j)
  {
    for (int i = 0; i < grid.ni; ++i)
    {
      if (!grid.columnLive(i, j))
        continue;
      const int block = local.columnBlock[static_cast<std::size_t>(grid.columnIndex(i, j))];
      if (block < 0 || byBlock[static_cast<std::size_t>(block)].empty())
      {
        unfilled += grid.nk;
        continue;
      }
      const std::vector<Seed> &bucket = byBlock[static_cast<std::size_t>(block)];
      for (int k = 0; k < grid.nk; ++k)
      {
        const int idx = grid.cellIndex(i, j, k);
        double exactSum = 0;
        int exactN = 0;
        double wsum = 0;
        double vsum = 0;
        for (const Seed &seed : bucket)
        {
          if (i == seed.i && j == seed.j && k == seed.k)
          {
            exactSum += seed.value;
            ++exactN;
            continue;
          }
          const double hx = static_cast<double>(i - seed.i) * grid.dx;
          const double hy = static_cast<double>(j - seed.j) * grid.dy;
          const double hz = static_cast<double>(k - seed.k) * verticalStep;
          const double d2 = hx * hx + hy * hy + hz * hz;
          if (!(d2 > 0.0))
          {
            exactSum += seed.value;
            ++exactN;
            continue;
          }
          const double w = 1.0 / std::pow(d2, power * 0.5);
          wsum += w;
          vsum += w * seed.value;
        }
        float value = kNan;
        if (exactN > 0)
          value = static_cast<float>(exactSum / static_cast<double>(exactN));
        else if (wsum > 0.0)
          value = static_cast<float>(vsum / wsum);
        local.values[static_cast<std::size_t>(idx)] = value;
        if (std::isfinite(value))
          ++filled;
        else
          ++unfilled;
      }
    }
    if (progress)
    {
      const double fraction = static_cast<double>(j + 1) / static_cast<double>(rows);
      if (!progress(fraction))
      {
        setError(error, QStringLiteral("已取消"));
        return false;
      }
    }
  }
  local.filledCells = filled;
  local.unfilledLiveCells = unfilled;
  *out = std::move(local);
  return true;
}

bool projectToSection(const PropertyVolume &volume, const SectionGeometry &section,
                      std::vector<float> *out, float *valueMin, float *valueMax, QString *error)
{
  if (!out)
  {
    setError(error, QStringLiteral("剖面投影输出为空"));
    return false;
  }
  if (section.nTraces < 1 || section.nSamples < 1 ||
      static_cast<int>(section.traceX.size()) != section.nTraces ||
      static_cast<int>(section.traceY.size()) != section.nTraces || !(section.dz != 0.0))
  {
    setError(error, QStringLiteral("剖面几何不完整"));
    return false;
  }
  const float kNan = std::numeric_limits<float>::quiet_NaN();
  out->assign(static_cast<std::size_t>(section.nTraces * section.nSamples), kNan);
  bool any = false;
  float vmin = 0, vmax = 0;
  const ZoneGrid &grid = volume.grid;
  for (int s = 0; s < section.nSamples; ++s)
  {
    const double z = section.z0 + static_cast<double>(s) * section.dz;
    for (int t = 0; t < section.nTraces; ++t)
    {
      Index3 ijk;
      if (!ijkAt(grid, section.traceX[static_cast<std::size_t>(t)],
                 section.traceY[static_cast<std::size_t>(t)], z, &ijk))
        continue;
      if (volume.values.size() != static_cast<std::size_t>(grid.cellCount()))
        continue;
      const float v = volume.values[static_cast<std::size_t>(grid.cellIndex(ijk.i, ijk.j, ijk.k))];
      (*out)[static_cast<std::size_t>(s * section.nTraces + t)] = v;
      trackRange(v, &any, &vmin, &vmax);
    }
  }
  if (valueMin)
    *valueMin = any ? vmin : 0;
  if (valueMax)
    *valueMax = any ? vmax : 0;
  return true;
}

bool fillIdw(const ZoneGrid &grid, const std::vector<Seed> &seeds, const std::vector<FaultTriangle> &mesh,
             double power, PropertyVolume *out, const FillProgress &progress, QString *error)
{
  if (!out)
  {
    setError(error, QStringLiteral("充填输出为空"));
    return false;
  }
  if (!(power > 0.0) || !std::isfinite(power))
  {
    setError(error, QStringLiteral("IDW 幂次须为正有限值"));
    return false;
  }
  if (grid.nk < 1 || grid.ni < 1 || grid.nj < 1)
  {
    setError(error, QStringLiteral("格架为空"));
    return false;
  }

  std::vector<paleo::faultsurf::Triangle3> tris;
  tris.reserve(mesh.size());
  for (const FaultTriangle &t : mesh)
    tris.push_back(paleo::faultsurf::Triangle3{t.ax, t.ay, t.az, t.bx, t.by, t.bz, t.cx, t.cy, t.cz});

  // #234 项 1：mesh 空间索引建一次，blocked 每次查询只扫线段覆盖格内的
  // 候选三角形，替代 O(|tris|) 线性扫描（网格版接线前该阶段是分钟级悬崖）。
  paleo::faultsurf::SegmentMeshIndex meshIndex;
  meshIndex.build(tris);

  const auto blocked = [&](int i0, int j0, int k0, int i1, int j1, int k1) {
    Point3 a;
    Point3 b;
    if (!cellCenter(grid, i0, j0, k0, &a) || !cellCenter(grid, i1, j1, k1, &b))
      return true;
    if (meshIndex.usable())
      return meshIndex.segmentIntersects(a.x, a.y, a.z, b.x, b.y, b.z);
    return paleo::faultsurf::segmentIntersectsTriangles(a.x, a.y, a.z, b.x, b.y, b.z, tris);
  };

  PropertyVolume local;
  local.grid = grid;
  const int nCell = grid.cellCount();
  local.cellBlock.assign(static_cast<std::size_t>(nCell), -1);
  local.columnBlock.assign(static_cast<std::size_t>(grid.ni * grid.nj), -1);
  int next = 0;
  const int di[6] = {-1, 1, 0, 0, 0, 0};
  const int dj[6] = {0, 0, -1, 1, 0, 0};
  const int dk[6] = {0, 0, 0, 0, -1, 1};
  for (int k = 0; k < grid.nk; ++k)
  {
    // #234 项 1：一阶段（断块标记 BFS，含 blocked 求交）也接进度/取消——
    // 此前仅二阶段（IDW 充填）可达，一阶段是网格版的主要耗时所在。
    if (progress && !progress(0.5 * static_cast<double>(k) / static_cast<double>(grid.nk)))
    {
      setError(error, QStringLiteral("已取消"));
      return false;
    }
    for (int j = 0; j < grid.nj; ++j)
    {
      for (int i = 0; i < grid.ni; ++i)
      {
        const int start = grid.cellIndex(i, j, k);
        if (!grid.columnLive(i, j) || local.cellBlock[static_cast<std::size_t>(start)] >= 0)
          continue;
        std::queue<Index3> q;
        q.push(Index3{i, j, k});
        local.cellBlock[static_cast<std::size_t>(start)] = next;
        while (!q.empty())
        {
          const Index3 cur = q.front();
          q.pop();
          for (int e = 0; e < 6; ++e)
          {
            const int ni = cur.i + di[e];
            const int nj = cur.j + dj[e];
            const int nk = cur.k + dk[e];
            if (!grid.inRange(ni, nj, nk) || !grid.columnLive(ni, nj))
              continue;
            const int idx = grid.cellIndex(ni, nj, nk);
            if (local.cellBlock[static_cast<std::size_t>(idx)] >= 0)
              continue;
            if (blocked(cur.i, cur.j, cur.k, ni, nj, nk))
              continue;
            local.cellBlock[static_cast<std::size_t>(idx)] = next;
            q.push(Index3{ni, nj, nk});
          }
        }
        ++next;
      }
    }
  }
  local.blockCount = next;
  for (int j = 0; j < grid.nj; ++j)
  {
    for (int i = 0; i < grid.ni; ++i)
    {
      if (!grid.columnLive(i, j))
        continue;
      local.columnBlock[static_cast<std::size_t>(grid.columnIndex(i, j))] =
          local.cellBlock[static_cast<std::size_t>(grid.cellIndex(i, j, 0))];
    }
  }

  const float kNan = std::numeric_limits<float>::quiet_NaN();
  local.values.assign(static_cast<std::size_t>(nCell), kNan);
  std::vector<std::vector<Seed>> byBlock(static_cast<std::size_t>(std::max(next, 0)));
  for (const Seed &seed : seeds)
  {
    if (!grid.columnLive(seed.i, seed.j) || seed.k < 0 || seed.k >= grid.nk || !std::isfinite(seed.value))
      continue;
    const int block = local.cellBlock[static_cast<std::size_t>(grid.cellIndex(seed.i, seed.j, seed.k))];
    if (block < 0)
      continue;
    byBlock[static_cast<std::size_t>(block)].push_back(seed);
  }

  double verticalStep = 1.0;
  {
    double thickSum = 0.0;
    int nLive = 0;
    for (int j = 0; j < grid.nj; ++j)
    {
      for (int i = 0; i < grid.ni; ++i)
      {
        if (!grid.columnLive(i, j))
          continue;
        thickSum += layerThickness(grid, i, j, 0);
        ++nLive;
      }
    }
    if (nLive > 0)
      verticalStep = thickSum / static_cast<double>(nLive);
  }

  int filled = 0;
  int unfilled = 0;
  for (int j = 0; j < grid.nj; ++j)
  {
    for (int i = 0; i < grid.ni; ++i)
    {
      if (!grid.columnLive(i, j))
        continue;
      for (int k = 0; k < grid.nk; ++k)
      {
        const int idx = grid.cellIndex(i, j, k);
        const int block = local.cellBlock[static_cast<std::size_t>(idx)];
        if (block < 0 || byBlock[static_cast<std::size_t>(block)].empty())
        {
          ++unfilled;
          continue;
        }
        const std::vector<Seed> &bucket = byBlock[static_cast<std::size_t>(block)];
        double exactSum = 0;
        int exactN = 0;
        double wsum = 0;
        double vsum = 0;
        for (const Seed &seed : bucket)
        {
          if (i == seed.i && j == seed.j && k == seed.k)
          {
            exactSum += seed.value;
            ++exactN;
            continue;
          }
          const double hx = static_cast<double>(i - seed.i) * grid.dx;
          const double hy = static_cast<double>(j - seed.j) * grid.dy;
          const double hz = static_cast<double>(k - seed.k) * verticalStep;
          const double d2 = hx * hx + hy * hy + hz * hz;
          if (!(d2 > 0.0))
          {
            exactSum += seed.value;
            ++exactN;
            continue;
          }
          const double w = 1.0 / std::pow(d2, power * 0.5);
          wsum += w;
          vsum += w * seed.value;
        }
        float value = kNan;
        if (exactN > 0)
          value = static_cast<float>(exactSum / static_cast<double>(exactN));
        else if (wsum > 0.0)
          value = static_cast<float>(vsum / wsum);
        local.values[static_cast<std::size_t>(idx)] = value;
        if (std::isfinite(value))
          ++filled;
        else
          ++unfilled;
      }
    }
    if (progress && !progress(0.5 + 0.5 * static_cast<double>(j + 1) / static_cast<double>(grid.nj)))
    {
      setError(error, QStringLiteral("已取消"));
      return false;
    }
  }
  local.filledCells = filled;
  local.unfilledLiveCells = unfilled;
  *out = std::move(local);
  return true;
}

bool extractSlice(const PropertyVolume &volume, int axis, int index, std::vector<float> *out,
                  int *width, int *height, float *valueMin, float *valueMax, QString *error)
{
  if (!out || !width || !height)
  {
    setError(error, QStringLiteral("切片输出为空"));
    return false;
  }
  const ZoneGrid &g = volume.grid;
  if (volume.values.size() != static_cast<std::size_t>(g.cellCount()))
  {
    setError(error, QStringLiteral("属性体与格架尺寸不符"));
    return false;
  }
  int w = 0, h = 0;
  if (axis == 0)
  {
    if (index < 0 || index >= g.ni)
    {
      setError(error, QStringLiteral("I 切片越界"));
      return false;
    }
    w = g.nj;
    h = g.nk;
  }
  else if (axis == 1)
  {
    if (index < 0 || index >= g.nj)
    {
      setError(error, QStringLiteral("J 切片越界"));
      return false;
    }
    w = g.ni;
    h = g.nk;
  }
  else if (axis == 2)
  {
    if (index < 0 || index >= g.nk)
    {
      setError(error, QStringLiteral("K 切片越界"));
      return false;
    }
    w = g.ni;
    h = g.nj;
  }
  else
  {
    setError(error, QStringLiteral("切片轴须为 0/1/2"));
    return false;
  }
  const float kNan = std::numeric_limits<float>::quiet_NaN();
  out->assign(static_cast<std::size_t>(w * h), kNan);
  bool any = false;
  float vmin = 0, vmax = 0;
  for (int row = 0; row < h; ++row)
  {
    for (int col = 0; col < w; ++col)
    {
      int i = 0, j = 0, k = 0;
      if (axis == 0)
      {
        i = index;
        j = col;
        k = row;
      }
      else if (axis == 1)
      {
        i = col;
        j = index;
        k = row;
      }
      else
      {
        i = col;
        j = row;
        k = index;
      }
      float v = kNan;
      if (g.columnLive(i, j))
        v = volume.values[static_cast<std::size_t>(g.cellIndex(i, j, k))];
      (*out)[static_cast<std::size_t>(row * w + col)] = v;
      trackRange(v, &any, &vmin, &vmax);
    }
  }
  *width = w;
  *height = h;
  if (valueMin)
    *valueMin = any ? vmin : 0;
  if (valueMax)
    *valueMax = any ? vmax : 0;
  return true;
}

QByteArray writePropertyBlob(const PropertyVolume &volume, const QJsonObject &provenance)
{
  static_assert(std::endian::native == std::endian::little, "PPROP1 stores little-endian floats");
  const ZoneGrid &g = volume.grid;
  const int n = g.ni * g.nj;
  if (n < 1 || g.nk < 1 || static_cast<int>(g.topZ.size()) != n ||
      static_cast<int>(g.botZ.size()) != n ||
      static_cast<int>(volume.values.size()) != g.cellCount())
    return {};
  QJsonObject obj;
  obj.insert(QStringLiteral("format"), QStringLiteral("paleo-property-volume"));
  obj.insert(QStringLiteral("version"), 1);
  obj.insert(QStringLiteral("ni"), g.ni);
  obj.insert(QStringLiteral("nj"), g.nj);
  obj.insert(QStringLiteral("nk"), g.nk);
  obj.insert(QStringLiteral("originX"), g.originX);
  obj.insert(QStringLiteral("originY"), g.originY);
  obj.insert(QStringLiteral("dx"), g.dx);
  obj.insert(QStringLiteral("dy"), g.dy);
  obj.insert(QStringLiteral("liveColumns"), g.liveColumns);
  obj.insert(QStringLiteral("filledCells"), volume.filledCells);
  obj.insert(QStringLiteral("unfilledLiveCells"), volume.unfilledLiveCells);
  obj.insert(QStringLiteral("provenance"), provenance);
  const QByteArray json = QJsonDocument(obj).toJson(QJsonDocument::Compact);
  QByteArray blob;
  blob.reserve(static_cast<int>(json.size() + 16 + (n * 2 + g.cellCount()) * 4));
  blob.append("PPROP1\n", 7);
  char len[4];
  qToLittleEndian(static_cast<quint32>(json.size()), len);
  blob.append(len, 4);
  blob.append(json);
  const auto append = [&](const std::vector<float> &values) {
    blob.append(reinterpret_cast<const char *>(values.data()),
                static_cast<int>(values.size() * sizeof(float)));
  };
  append(g.topZ);
  append(g.botZ);
  append(volume.values);
  return blob;
}

bool readPropertyBlob(const QByteArray &blob, PropertyVolume *volume, QJsonObject *provenance,
                      QString *error)
{
  static_assert(std::endian::native == std::endian::little, "PPROP1 stores little-endian floats");
  if (!volume)
  {
    setError(error, QStringLiteral("读回输出为空"));
    return false;
  }
  *volume = PropertyVolume{};
  if (blob.size() < 11 || std::memcmp(blob.constData(), "PPROP1\n", 7) != 0)
  {
    setError(error, QStringLiteral("不是属性体文件（缺少 PPROP1 头）"));
    return false;
  }
  const quint32 jsonLen = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(blob.constData() + 7));
  if (jsonLen == 0 || 11 + static_cast<int>(jsonLen) > blob.size())
  {
    setError(error, QStringLiteral("属性体头长度损坏"));
    return false;
  }
  const QByteArray json = blob.mid(11, static_cast<int>(jsonLen));
  QJsonParseError parseErr;
  const QJsonDocument doc = QJsonDocument::fromJson(json, &parseErr);
  if (parseErr.error != QJsonParseError::NoError || !doc.isObject())
  {
    setError(error, QStringLiteral("属性体头不是 JSON 对象"));
    return false;
  }
  const QJsonObject obj = doc.object();
  if (obj.value(QStringLiteral("format")).toString() != QLatin1String("paleo-property-volume") ||
      obj.value(QStringLiteral("version")).toInt() != 1)
  {
    setError(error, QStringLiteral("属性体格式版本不认识"));
    return false;
  }
  ZoneGrid grid;
  grid.ni = obj.value(QStringLiteral("ni")).toInt();
  grid.nj = obj.value(QStringLiteral("nj")).toInt();
  grid.nk = obj.value(QStringLiteral("nk")).toInt();
  grid.originX = obj.value(QStringLiteral("originX")).toDouble();
  grid.originY = obj.value(QStringLiteral("originY")).toDouble();
  grid.dx = obj.value(QStringLiteral("dx")).toDouble();
  grid.dy = obj.value(QStringLiteral("dy")).toDouble();
  // #219：读侧与写侧同闸——先把每轴钳在 [1, kMaxPropAxis]，再用 qint64 算
  // 列数/格数/所需字节，避免 int 乘法回绕让 need 变小而绕过长度闸后巨量分配。
  constexpr int kMaxPropAxis = 1 << 20;
  if (grid.ni < 1 || grid.nj < 1 || grid.nk < 1 || grid.ni > kMaxPropAxis ||
      grid.nj > kMaxPropAxis || grid.nk > kMaxPropAxis)
  {
    setError(error, QStringLiteral("属性体几何或体数据长度不符"));
    return false;
  }
  const qint64 n64 = static_cast<qint64>(grid.ni) * grid.nj;
  const qint64 cells64 = n64 * grid.nk;
  const qint64 need = 11 + static_cast<qint64>(jsonLen) +
                      (n64 * 2 + cells64) * static_cast<qint64>(sizeof(float));
  if (cells64 > std::numeric_limits<int>::max() || static_cast<qint64>(blob.size()) < need)
  {
    setError(error, QStringLiteral("属性体几何或体数据长度不符"));
    return false;
  }
  const int n = static_cast<int>(n64);
  const int cells = static_cast<int>(cells64);
  const char *cursor = blob.constData() + 11 + static_cast<int>(jsonLen);
  const auto readFloats = [&](int count, std::vector<float> *dst) {
    dst->resize(static_cast<std::size_t>(count));
    std::memcpy(dst->data(), cursor, static_cast<std::size_t>(count) * sizeof(float));
    cursor += static_cast<std::size_t>(count) * sizeof(float);
  };
  readFloats(n, &grid.topZ);
  readFloats(n, &grid.botZ);
  grid.live.assign(static_cast<std::size_t>(n), 0);
  int live = 0;
  for (int i = 0; i < n; ++i)
  {
    const float tz = grid.topZ[static_cast<std::size_t>(i)];
    const float bz = grid.botZ[static_cast<std::size_t>(i)];
    if (std::isfinite(tz) && std::isfinite(bz) &&
        static_cast<double>(bz) - static_cast<double>(tz) > kThicknessEps)
    {
      grid.live[static_cast<std::size_t>(i)] = 1;
      ++live;
    }
  }
  grid.liveColumns = live;
  grid.deadColumns = n - live;
  volume->grid = std::move(grid);
  readFloats(cells, &volume->values);
  volume->filledCells = obj.value(QStringLiteral("filledCells")).toInt();
  volume->unfilledLiveCells = obj.value(QStringLiteral("unfilledLiveCells")).toInt();
  if (provenance)
  {
    const QJsonValue prov = obj.value(QStringLiteral("provenance"));
    *provenance = prov.isObject() ? prov.toObject() : QJsonObject();
  }
  return true;
}

} // namespace paleo::stratgrid
