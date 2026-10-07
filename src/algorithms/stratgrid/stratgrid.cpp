// 层：数据
#include "stratgrid.h"
#include "../algoerrors_internal.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace paleo::stratgrid
{
namespace
{

using paleo::algo_detail::setError;

bool closeEnough(double a, double b)
{
  const double scale = 1.0 + std::max(std::fabs(a), std::fabs(b));
  return std::fabs(a - b) <= 1e-6 * scale;
}

bool sameGeometry(const SurfaceGrid &a, const SurfaceGrid &b)
{
  return a.cols == b.cols && a.rows == b.rows && closeEnough(a.originX, b.originX) &&
         closeEnough(a.originY, b.originY) && closeEnough(a.dx, b.dx) &&
         closeEnough(a.dy, b.dy);
}

} // namespace

int layerOfFraction(double s, int nk)
{
  if (nk < 1 || !std::isfinite(s) || s < 0.0 || s > 1.0)
    return -1;
  if (s >= 1.0)
    return nk - 1;
  const double scaled = s * static_cast<double>(nk);
  int k = static_cast<int>(std::floor(scaled));
  const double nearest = std::round(scaled);
  const double tol = 1e-8 * static_cast<double>(nk);
  if (std::fabs(scaled - nearest) <= tol && nearest >= 1.0 && nearest < static_cast<double>(nk))
    k = static_cast<int>(nearest);
  if (k < 0)
    k = 0;
  if (k >= nk)
    k = nk - 1;
  return k;
}

bool buildZoneGrid(const SurfaceGrid &top, const SurfaceGrid &bot, int nLayers, ZoneGrid *out,
                   QString *error)
{
  if (!out)
  {
    setError(error, QStringLiteral("输出网格为空"));
    return false;
  }
  *out = ZoneGrid{};
  if (nLayers < 1)
  {
    setError(error, QStringLiteral("层数须 ≥ 1"));
    return false;
  }
  const int n = top.cols * top.rows;
  if (top.cols < 1 || top.rows < 1 || static_cast<int>(top.z.size()) != n)
  {
    setError(error, QStringLiteral("顶面网格为空或长度不符"));
    return false;
  }
  if (static_cast<int>(bot.z.size()) != n || !sameGeometry(top, bot))
  {
    setError(error, QStringLiteral("顶底面网格几何不一致"));
    return false;
  }
  if (!(top.dx > 0.0) || top.dy == 0.0 || !std::isfinite(top.dx) || !std::isfinite(top.dy))
  {
    setError(error, QStringLiteral("像元尺寸非法（dx 须为正，dy 不得为 0）"));
    return false;
  }

  out->ni = top.cols;
  out->nj = top.rows;
  out->nk = nLayers;
  out->originX = top.originX;
  out->originY = top.originY;
  out->dx = top.dx;
  out->dy = top.dy;
  const float kNan = std::numeric_limits<float>::quiet_NaN();
  out->topZ.assign(static_cast<std::size_t>(n), kNan);
  out->botZ.assign(static_cast<std::size_t>(n), kNan);
  out->live.assign(static_cast<std::size_t>(n), 0);

  int finitePairs = 0;
  int equalPairs = 0;
  int inverted = 0;
  int live = 0;
  for (int idx = 0; idx < n; ++idx)
  {
    const float tz = top.z[static_cast<std::size_t>(idx)];
    const float bz = bot.z[static_cast<std::size_t>(idx)];
    if (!std::isfinite(tz) || !std::isfinite(bz))
      continue;
    ++finitePairs;
    out->topZ[static_cast<std::size_t>(idx)] = tz;
    out->botZ[static_cast<std::size_t>(idx)] = bz;
    const double thick = static_cast<double>(bz) - static_cast<double>(tz);
    if (std::fabs(thick) <= kThicknessEps)
    {
      ++equalPairs;
      continue;
    }
    if (thick < 0.0)
    {
      ++inverted;
      continue;
    }
    out->live[static_cast<std::size_t>(idx)] = 1;
    ++live;
  }
  out->liveColumns = live;
  out->deadColumns = n - live;
  if (live == 0)
  {
    if (finitePairs > 0 && equalPairs == finitePairs)
      setError(error, QStringLiteral("奇异面：顶底重合"));
    else if (finitePairs > 0 && inverted == finitePairs)
      setError(error, QStringLiteral("无正厚度柱：顶底颠倒（要求底面值大于顶面值）"));
    else
      setError(error, QStringLiteral("无正厚度柱：顶底重合、颠倒或全部无值"));
    *out = ZoneGrid{};
    return false;
  }
  return true;
}

double layerThickness(const ZoneGrid &grid, int i, int j, int k)
{
  if (!grid.columnLive(i, j) || k < 0 || k >= grid.nk)
    return 0.0;
  const int c = grid.columnIndex(i, j);
  return (static_cast<double>(grid.botZ[static_cast<std::size_t>(c)]) -
          static_cast<double>(grid.topZ[static_cast<std::size_t>(c)])) /
         static_cast<double>(grid.nk);
}

double cellVolume(const ZoneGrid &grid, int i, int j, int k)
{
  const double thick = layerThickness(grid, i, j, k);
  if (!(thick > 0.0))
    return 0.0;
  return grid.dx * std::fabs(grid.dy) * thick;
}

bool interfaceZ(const ZoneGrid &grid, int i, int j, int kInterface, double *zOut)
{
  if (!zOut || !grid.columnLive(i, j) || kInterface < 0 || kInterface > grid.nk)
    return false;
  const int c = grid.columnIndex(i, j);
  const double top = grid.topZ[static_cast<std::size_t>(c)];
  const double bot = grid.botZ[static_cast<std::size_t>(c)];
  *zOut = top + (static_cast<double>(kInterface) / static_cast<double>(grid.nk)) * (bot - top);
  return true;
}

bool cellCenter(const ZoneGrid &grid, int i, int j, int k, Point3 *out)
{
  if (!out || !grid.inRange(i, j, k) || !grid.columnLive(i, j))
    return false;
  const int c = grid.columnIndex(i, j);
  const double top = grid.topZ[static_cast<std::size_t>(c)];
  const double bot = grid.botZ[static_cast<std::size_t>(c)];
  const double s = (static_cast<double>(k) + 0.5) / static_cast<double>(grid.nk);
  out->x = grid.originX + (static_cast<double>(i) + 0.5) * grid.dx;
  out->y = grid.originY + (static_cast<double>(j) + 0.5) * grid.dy;
  out->z = top + s * (bot - top);
  return true;
}

bool ijkAt(const ZoneGrid &grid, double x, double y, double z, Index3 *out)
{
  if (!out || !(grid.dx > 0.0) || grid.dy == 0.0 || grid.ni < 1 || grid.nj < 1)
    return false;
  const double u = (x - grid.originX) / grid.dx;
  const double v = (y - grid.originY) / grid.dy;
  const int i = static_cast<int>(std::floor(u));
  const int j = static_cast<int>(std::floor(v));
  if (i < 0 || j < 0 || i >= grid.ni || j >= grid.nj || !grid.columnLive(i, j))
    return false;
  const int c = grid.columnIndex(i, j);
  const double top = grid.topZ[static_cast<std::size_t>(c)];
  const double bot = grid.botZ[static_cast<std::size_t>(c)];
  const double span = bot - top;
  if (!(span > kThicknessEps))
    return false;
  const int k = layerOfFraction((z - top) / span, grid.nk);
  if (k < 0)
    return false;
  out->i = i;
  out->j = j;
  out->k = k;
  return true;
}

std::vector<Index3> neighbors6(const ZoneGrid &grid, int i, int j, int k)
{
  std::vector<Index3> out;
  if (!grid.inRange(i, j, k) || !grid.columnLive(i, j))
    return out;
  const int di[6] = {-1, 1, 0, 0, 0, 0};
  const int dj[6] = {0, 0, -1, 1, 0, 0};
  const int dk[6] = {0, 0, 0, 0, -1, 1};
  out.reserve(6);
  for (int n = 0; n < 6; ++n)
  {
    const int ii = i + di[n];
    const int jj = j + dj[n];
    const int kk = k + dk[n];
    if (grid.inRange(ii, jj, kk) && grid.columnLive(ii, jj))
      out.push_back(Index3{ii, jj, kk});
  }
  return out;
}

} // namespace paleo::stratgrid
