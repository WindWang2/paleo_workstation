// 层：数据
#include "objectmodel.h"
#include "../algoerrors_internal.h"

#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <random>
#include <vector>

namespace paleo::stratgrid
{
namespace
{

using paleo::algo_detail::setError;

struct ChannelPath
{
  double ax = 0;
  double ay = 0;
  double dx = 0; // 单位走向向量
  double dy = 0;
  double px = 0; // 单位左法向
  double py = 0;
  double length = 0;
  double amplitude = 0;

  // 中线参数点。
  void point(double t, double *x, double *y) const
  {
    const double bend = amplitude * std::sin( 2.0 * std::numbers::pi * t );
    *x = ax + t * length * dx + bend * px;
    *y = ay + t * length * dy + bend * py;
  }
};

double pointToSegmentDistance2(double px, double py, double ax, double ay, double bx, double by)
{
  const double ex = bx - ax;
  const double ey = by - ay;
  const double len2 = ex * ex + ey * ey;
  double t = 0;
  if (len2 > 0)
    t = std::clamp(( ( px - ax ) * ex + ( py - ay ) * ey ) / len2, 0.0, 1.0 );
  const double qx = ax + t * ex;
  const double qy = ay + t * ey;
  return ( px - qx ) * ( px - qx ) + ( py - qy ) * ( py - qy );
}

// 柱心到河道中线折线（kSegments 段）的最小距离²。
double channelDistance2(const ChannelPath &path, double px, double py)
{
  constexpr int kSegments = 512;
  double minDist2 = std::numeric_limits<double>::max();
  double prevX = 0;
  double prevY = 0;
  path.point(0.0, &prevX, &prevY);
  for (int s = 1; s <= kSegments; ++s)
  {
    double x = 0;
    double y = 0;
    path.point(static_cast<double>(s) / kSegments, &x, &y);
    minDist2 = std::min(minDist2, pointToSegmentDistance2(px, py, prevX, prevY, x, y));
    prevX = x;
    prevY = y;
  }
  return minDist2;
}

// 柱心到椭圆：归一化坐标（长轴 length、短轴 width）的模 ≤ 1 即命中；
// 返回归一化距离（>1 在外）。
double ellipseNormalizedDistance(double px, double py, double cx, double cy, double ux,
                                 double uy, double vx, double vy, double halfLength,
                                 double halfWidth)
{
  const double along = ( ( px - cx ) * ux + ( py - cy ) * uy ) / std::max(halfLength, 1e-9);
  const double across = ( ( px - cx ) * vx + ( py - cy ) * vy ) / std::max(halfWidth, 1e-9);
  return std::hypot(along, across);
}

void recountVolume(PropertyVolume *volume)
{
  volume->filledCells = 0;
  volume->unfilledLiveCells = 0;
  for (int j = 0; j < volume->grid.nj; ++j)
    for (int i = 0; i < volume->grid.ni; ++i)
    {
      if (!volume->grid.columnLive(i, j))
        continue;
      for (int k = 0; k < volume->grid.nk; ++k)
      {
        if (std::isfinite(volume->values[static_cast<std::size_t>(volume->grid.cellIndex(i, j, k))]))
          ++volume->filledCells;
        else
          ++volume->unfilledLiveCells;
      }
    }
}

} // namespace

bool placeObjects(const ZoneGrid &grid, const std::vector<int> *zonePerColumn,
                  std::uint64_t seed, const std::vector<ObjectSpec> &specs,
                  PropertyVolume *out, ObjectModelMeta *meta, QString *error)
{
  if (!out || !meta)
  {
    setError(error, QStringLiteral("对象建模输出为空"));
    return false;
  }
  *meta = ObjectModelMeta{};
  if (grid.ni < 1 || grid.nj < 1 || grid.nk < 1)
  {
    setError(error, QStringLiteral("格架为空"));
    return false;
  }
  if (zonePerColumn &&
      zonePerColumn->size() != static_cast<std::size_t>(grid.ni) * grid.nj)
  {
    setError(error, QStringLiteral("相带分区数组尺寸与格架不符"));
    return false;
  }
  const double mapDiagonal = std::hypot(static_cast<double>(grid.ni) * std::fabs(grid.dx),
                                        static_cast<double>(grid.nj) * std::fabs(grid.dy));
  for (const ObjectSpec &spec : specs)
  {
    const double params[6] = {spec.azimuthDeg, spec.length, spec.width,
                              spec.thickness, spec.curvature, spec.verticalFrac};
    for (double v : params)
    {
      if (!std::isfinite(v))
      {
        setError(error, QStringLiteral("对象几何参数非有限值"));
        return false;
      }
    }
    if (!(spec.width > 0) || !(spec.thickness > 0) || spec.count < 1)
    {
      setError(error, QStringLiteral("对象宽度/厚度须为正，数量须 ≥ 1"));
      return false;
    }
    if (spec.verticalFrac < 0.0 || spec.verticalFrac > 1.0)
    {
      setError(error, QStringLiteral("垂向锚定系数须在 [0,1]"));
      return false;
    }
  }

  // 播种候选柱（限制相带内）；全部 spec 预先收集——失败（空相带）发生在
  // 任何输出写入之前，*out 保持调用前状态（与 faultoffset/sgsfill 契约一致）。
  std::vector<std::pair<int, int>> allLive;
  for (int j = 0; j < grid.nj; ++j)
    for (int i = 0; i < grid.ni; ++i)
      if (grid.columnLive(i, j))
        allLive.emplace_back(i, j);
  std::vector<std::vector<std::pair<int, int>>> candidatesPerSpec;
  candidatesPerSpec.reserve(specs.size());
  for (const ObjectSpec &spec : specs)
  {
    std::vector<std::pair<int, int>> candidates;
    if (spec.zoneCode < 0 || !zonePerColumn)
    {
      candidates = allLive;
    }
    else
    {
      for (const auto &col : allLive)
      {
        const int zone = ( *zonePerColumn )[static_cast<std::size_t>(grid.columnIndex(col.first, col.second))];
        if (zone == spec.zoneCode)
          candidates.push_back(col);
      }
    }
    if (candidates.empty())
    {
      setError(error, QStringLiteral("相带 %1 没有可播种的活柱").arg(spec.zoneCode));
      return false;
    }
    candidatesPerSpec.push_back(std::move(candidates));
  }

  std::mt19937_64 rng(seed);
  out->grid = grid;
  out->values.assign(static_cast<std::size_t>(grid.cellCount()),
                     std::numeric_limits<float>::quiet_NaN());
  out->columnBlock.clear(); // 对象指示场不携带竖帘分块——连通口径属背景场
  out->blockCount = 0;
  std::vector<std::uint8_t> hit(static_cast<std::size_t>(grid.cellCount()), 0);

  for (std::size_t specIndex = 0; specIndex < specs.size(); ++specIndex)
  {
    const ObjectSpec &spec = specs[specIndex];
    const std::vector<std::pair<int, int>> &candidates = candidatesPerSpec[specIndex];

    const double azimuthRad = spec.azimuthDeg * std::numbers::pi / 180.0;
    const double dirX = std::sin(azimuthRad);
    const double dirY = std::cos(azimuthRad);
    const double perpX = -dirY; // 左法向
    const double perpY = dirX;
    const double length = spec.length > 0 ? spec.length : 0.8 * mapDiagonal;

    for (int n = 0; n < spec.count; ++n)
    {
      const auto [ci, cj] = candidates[rng() % candidates.size()];
      const double centerX = grid.originX + ( static_cast<double>(ci) + 0.5 ) * grid.dx;
      const double centerY = grid.originY + ( static_cast<double>(cj) + 0.5 ) * grid.dy;

      ObjectPlacementRecord record;
      record.type = spec.type;
      record.centerX = centerX;
      record.centerY = centerY;
      record.azimuthDeg = spec.azimuthDeg;
      record.length = length;
      record.width = spec.width;
      record.thickness = spec.thickness;
      record.curvature = spec.type == ObjectType::Channel ? spec.curvature : 0.0;
      record.verticalFrac = spec.verticalFrac;
      record.value = spec.value;
      record.zoneCode = spec.zoneCode;

      const ChannelPath path{centerX, centerY, dirX, dirY, perpX, perpY, length,
                             record.curvature};
      const double halfWidth = 0.5 * spec.width;

      // 对象包围盒（曲率振幅计入）：盒外柱直接跳过——全柱 512 段折线
      // 精算是 O(柱×段)，没有预筛在 200×200 网格 × 200 对象下不可用。
      double boxMinX = 0, boxMaxX = 0, boxMinY = 0, boxMaxY = 0;
      if (spec.type == ObjectType::Channel)
      {
        boxMinX = boxMaxX = centerX;
        boxMinY = boxMaxY = centerY;
        double prevX = 0;
        double prevY = 0;
        path.point(0.0, &prevX, &prevY);
        for (int s = 1; s <= 64; ++s) // 包围盒 64 采样足够（正弦单峰间隔）
        {
          double x = 0;
          double y = 0;
          path.point(static_cast<double>(s) / 64, &x, &y);
          boxMinX = std::min(boxMinX, x);
          boxMaxX = std::max(boxMaxX, x);
          boxMinY = std::min(boxMinY, y);
          boxMaxY = std::max(boxMaxY, y);
        }
        // 64 采样对正弦的欠覆盖补偿（垂度 ≈ A·(2π/64)²/8 ≈ 0.0077A）：
        // 盒再外扩该量，避免窄带内的真实命中柱被静默跳过。
        const double sagitta = 0.008 * std::fabs(record.curvature) + halfWidth;
        boxMinX -= sagitta;
        boxMaxX += sagitta;
        boxMinY -= sagitta;
        boxMaxY += sagitta;
      }
      else
      {
        const double ex = std::fabs(dirX) * 0.5 * length + std::fabs(perpX) * halfWidth;
        const double ey = std::fabs(dirY) * 0.5 * length + std::fabs(perpY) * halfWidth;
        boxMinX = centerX - ex;
        boxMaxX = centerX + ex;
        boxMinY = centerY - ey;
        boxMaxY = centerY + ey;
      }
      for (int j = 0; j < grid.nj; ++j)
      {
        for (int i = 0; i < grid.ni; ++i)
        {
          const double px = grid.originX + ( static_cast<double>(i) + 0.5 ) * grid.dx;
          const double py = grid.originY + ( static_cast<double>(j) + 0.5 ) * grid.dy;
          if (px < boxMinX || px > boxMaxX || py < boxMinY || py > boxMaxY)
            continue; // 包围盒快筛（dy 可负，中心判据与符号无关）
          if (!grid.columnLive(i, j))
            continue;
          const double d2 = spec.type == ObjectType::Channel
                                ? channelDistance2(path, px, py)
                                : 0.0;
          const double ellipseD = spec.type == ObjectType::PointBar
                                      ? ellipseNormalizedDistance(px, py, centerX, centerY,
                                                                 dirX, dirY, perpX, perpY,
                                                                 0.5 * length, halfWidth)
                                      : 0.0;
          const bool inMap = spec.type == ObjectType::Channel
                                 ? d2 <= halfWidth * halfWidth
                                 : ellipseD <= 1.0;
          if (!inMap)
            continue;

          // 垂向：层位坐标 s ∈ [verticalFrac − thickness/柱厚, verticalFrac]。
          const std::size_t column = static_cast<std::size_t>(grid.columnIndex(i, j));
          const double thickness = grid.botZ[column] - grid.topZ[column];
          const double tFrac = thickness > 0 ? std::clamp(spec.thickness / thickness, 0.0, 1.0) : 1.0;
          const double sLo = std::max(0.0, spec.verticalFrac - tFrac);
          const double sHi = std::min(1.0, spec.verticalFrac);
          for (int k = 0; k < grid.nk; ++k)
          {
            const double s = ( static_cast<double>(k) + 0.5 ) / grid.nk;
            if (s < sLo || s > sHi)
              continue;
            const std::size_t cell = static_cast<std::size_t>(grid.cellIndex(i, j, k));
            if (hit[cell])
              ++meta->overlapCells;
            hit[cell] = 1;
            out->values[cell] = static_cast<float>(spec.value);
            ++record.cells;
          }
        }
      }
      meta->placements.push_back(record);
    }
  }
  meta->objectCells = static_cast<int>(std::count(hit.begin(), hit.end(), std::uint8_t(1)));
  recountVolume(out);

  QStringList caliber;
  caliber << QStringLiteral("对象建模：%1 个对象，命中 %2 cell（重叠 %3）")
                 .arg(meta->placements.size())
                 .arg(meta->objectCells)
                 .arg(meta->overlapCells);
  caliber << QStringLiteral("对象优先：对象 cell 硬覆盖背景场（IDW/SGS）值");
  if (zonePerColumn)
    caliber << QStringLiteral("相带内播种（几何出带不裁剪）");
  else
    caliber << QStringLiteral("无相带输入：全域播种");
  caliber << QStringLiteral("种子 %1（mt19937_64 可复现）").arg(seed);
  meta->caliber = caliber.join(QStringLiteral("；"));
  return true;
}

bool applyObjectOverride(const PropertyVolume &objects, PropertyVolume *background,
                         QString *error)
{
  if (!background)
  {
    setError(error, QStringLiteral("背景属性体为空"));
    return false;
  }
  if (objects.values.size() != background->values.size() ||
      objects.grid.ni != background->grid.ni || objects.grid.nj != background->grid.nj ||
      objects.grid.nk != background->grid.nk)
  {
    setError(error, QStringLiteral("对象场与背景场网格不一致"));
    return false;
  }
  for (std::size_t cell = 0; cell < objects.values.size(); ++cell)
  {
    if (std::isfinite(objects.values[cell]))
      background->values[cell] = objects.values[cell];
  }
  recountVolume(background);
  return true;
}

} // namespace paleo::stratgrid
