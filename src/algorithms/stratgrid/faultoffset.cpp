// 层：数据
#include "faultoffset.h"
#include "../algoerrors_internal.h"

#include <QString>

#include <algorithm>
#include <cmath>
#include <limits>

namespace paleo::stratgrid
{
namespace
{

using paleo::algo_detail::setError;

// 柱心到段的最近点参数 t（钳到 [0,1]）与距离²。零长度段返回 false。
bool projectOnSegment(double px, double py, const FaultThrow &fault, double *t, double *dist2)
{
  const double dx = fault.x1 - fault.x0;
  const double dy = fault.y1 - fault.y0;
  const double len2 = dx * dx + dy * dy;
  const double scale = 1.0 + std::fabs(fault.x0) + std::fabs(fault.y0) +
                       std::fabs(fault.x1) + std::fabs(fault.y1);
  if (len2 <= (1e-12 * scale) * (1e-12 * scale))
    return false;
  double s = ((px - fault.x0) * dx + (py - fault.y0) * dy) / len2;
  s = std::clamp(s, 0.0, 1.0);
  const double qx = fault.x0 + s * dx;
  const double qy = fault.y0 + s * dy;
  *t = s;
  *dist2 = (px - qx) * (px - qx) + (py - qy) * (py - qy);
  return true;
}

} // namespace

bool applyFaultOffset(const ZoneGrid &grid, const std::vector<FaultThrow> &throws,
                      ZoneGrid *out, std::vector<float> *dzPerColumn,
                      FaultOffsetMeta *meta, QString *error)
{
  if (!out || !dzPerColumn)
  {
    setError(error, QStringLiteral("断块错位输出为空"));
    return false;
  }
  if (grid.ni < 1 || grid.nj < 1 || grid.nk < 1)
  {
    setError(error, QStringLiteral("格架为空"));
    return false;
  }
  if (grid.topZ.size() != static_cast<std::size_t>(grid.ni) * grid.nj ||
      grid.botZ.size() != grid.topZ.size() || grid.live.size() != grid.topZ.size())
  {
    setError(error, QStringLiteral("格架柱数组尺寸不一致"));
    return false;
  }
  for (const FaultThrow &fault : throws)
  {
    const double coords[6] = {fault.x0, fault.y0, fault.x1, fault.y1, fault.throwStart,
                              fault.throwEnd};
    for (double coord : coords)
    {
      if (!std::isfinite(coord))
      {
        setError(error, QStringLiteral("断层断距段坐标或断距非有限值"));
        return false;
      }
    }
  }
  // 别名安全：全部在局部副本上算，成功后一次性写回（out 与 grid 同对象
  // 是合法用法——编排层原地错位）。
  ZoneGrid result = grid;
  std::vector<float> dz(grid.topZ.size(), 0.0f);
  FaultOffsetMeta local;
  const double kOnLineEps = 1e-9;

  for (int j = 0; j < grid.nj; ++j)
  {
    for (int i = 0; i < grid.ni; ++i)
    {
      const std::size_t column = static_cast<std::size_t>(grid.columnIndex(i, j));
      if (!grid.columnLive(i, j))
        continue;
      const double px = grid.originX + (static_cast<double>(i) + 0.5) * grid.dx;
      const double py = grid.originY + (static_cast<double>(j) + 0.5) * grid.dy;

      int best = -1;
      double bestT = 0;
      double bestDist2 = std::numeric_limits<double>::max();
      for (std::size_t f = 0; f < throws.size(); ++f)
      {
        double t = 0;
        double dist2 = 0;
        if (!projectOnSegment(px, py, throws[f], &t, &dist2))
          continue;
        if (dist2 < bestDist2) // 严格小于：并列保留下标小者（确定性）
        {
          bestDist2 = dist2;
          bestT = t;
          best = static_cast<int>(f);
        }
      }
      if (best < 0)
        continue;

      const FaultThrow &fault = throws[static_cast<std::size_t>(best)];
      const double ex = fault.x1 - fault.x0;
      const double ey = fault.y1 - fault.y0;
      const double cross = ex * (py - fault.y0) - ey * (px - fault.x0);
      const double crossScale =
          1.0 + std::fabs(ex) + std::fabs(ey) + std::fabs(px - fault.x0) + std::fabs(py - fault.y0);
      if (std::fabs(cross) <= kOnLineEps * crossScale)
      {
        ++local.boundaryColumns; // 边界柱：两盘归属未定，不动（如实计数）
        continue;
      }
      const bool leftSide = cross > 0.0;
      if (leftSide != fault.dropLeftSide)
        continue;
      const double throwZ = fault.throwStart + (fault.throwEnd - fault.throwStart) * bestT;
      if (throwZ == 0.0)
        continue;
      result.topZ[column] =
          static_cast<float>(static_cast<double>(result.topZ[column]) + throwZ);
      result.botZ[column] =
          static_cast<float>(static_cast<double>(result.botZ[column]) + throwZ);
      dz[column] = static_cast<float>(throwZ);
      ++local.offsetColumns;
      local.maxAbsThrow = std::max(local.maxAbsThrow, std::fabs(throwZ));
    }
  }

  *out = std::move(result);
  *dzPerColumn = std::move(dz);
  if (meta)
    *meta = local;
  return true;
}

} // namespace paleo::stratgrid
