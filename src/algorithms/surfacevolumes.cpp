// 层：数据
#include "surfacevolumes.h"

#include <cmath>
#include <limits>

namespace paleo::surfacevolumes
{
namespace
{

bool validate(int cols, int rows, double dx, double dy, QString *error)
{
  if (cols < 1 || rows < 1 || !(dx > 0.0) || !(dy > 0.0))
  {
    if (error)
      *error = QStringLiteral("grid dimensions/spacing must be positive (got %1x%2, "
                              "dx=%3, dy=%4)")
                   .arg(cols)
                   .arg(rows)
                   .arg(dx)
                   .arg(dy);
    return false;
  }
  return true;
}

void finalize(VolumeReport &r, double sumT, double sumAbsT, double minT, double maxT)
{
  r.volume = sumT * r.cellArea;
  r.absVolume = sumAbsT * r.cellArea;
  r.area = static_cast<double>(r.cells) * r.cellArea;
  if (r.cells > 0)
  {
    r.minThickness = minT;
    r.maxThickness = maxT;
    r.meanThickness = sumT / static_cast<double>(r.cells);
  }
  else
    r.minThickness = r.maxThickness = r.meanThickness =
        std::numeric_limits<double>::quiet_NaN();
}

} // namespace

bool volumeBetween(const float *top, const float *base, int cols, int rows, double dx,
                   double dy, VolumeReport *out, QString *error)
{
  if (out == nullptr)
    return false;
  if (!validate(cols, rows, dx, dy, error))
    return false;
  if (top == nullptr || base == nullptr)
  {
    if (error)
      *error = QStringLiteral("input raster pointer is null");
    return false;
  }
  VolumeReport r;
  r.cellArea = dx * dy;
  double sumT = 0, sumAbsT = 0, minT = std::numeric_limits<double>::infinity();
  double maxT = -std::numeric_limits<double>::infinity();
  const std::size_t n = static_cast<std::size_t>(rows) * cols;
  for (std::size_t i = 0; i < n; ++i)
  {
    if (std::isnan(top[i]) || std::isnan(base[i]))
    {
      ++r.nullCells;
      continue;
    }
    const double t = static_cast<double>(top[i]) - static_cast<double>(base[i]);
    ++r.cells;
    sumT += t;
    sumAbsT += std::fabs(t);
    if (t > 0)
      ++r.positiveCells;
    else if (t < 0)
      ++r.negativeCells;
    minT = std::min(minT, t);
    maxT = std::max(maxT, t);
  }
  finalize(r, sumT, sumAbsT, minT, maxT);
  *out = r;
  return true;
}

bool volumeAboveDatum(const float *z, int cols, int rows, double dx, double dy,
                      double datum, VolumeReport *out, QString *error)
{
  if (out == nullptr)
    return false;
  if (!validate(cols, rows, dx, dy, error))
    return false;
  if (z == nullptr)
  {
    if (error)
      *error = QStringLiteral("input raster pointer is null");
    return false;
  }
  if (!std::isfinite(datum))
  {
    if (error)
      *error = QStringLiteral("datum must be finite");
    return false;
  }
  VolumeReport r;
  r.cellArea = dx * dy;
  double sumT = 0, sumAbsT = 0, minT = std::numeric_limits<double>::infinity();
  double maxT = -std::numeric_limits<double>::infinity();
  const std::size_t n = static_cast<std::size_t>(rows) * cols;
  for (std::size_t i = 0; i < n; ++i)
  {
    if (std::isnan(z[i]))
    {
      ++r.nullCells;
      continue;
    }
    // 厚度取 max(z−datum, 0)：只累计正部分；min/max/mean 统计同口径
    //（负侧格计 cells/面积但不贡献体积）。
    const double t = std::max(static_cast<double>(z[i]) - datum, 0.0);
    ++r.cells;
    sumT += t;
    sumAbsT += t;
    if (t > 0)
      ++r.positiveCells;
    minT = std::min(minT, t);
    maxT = std::max(maxT, t);
  }
  finalize(r, sumT, sumAbsT, minT, maxT);
  *out = r;
  return true;
}

} // namespace paleo::surfacevolumes
