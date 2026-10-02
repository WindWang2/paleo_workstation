// 层：数据
#pragma once

#include <QString>

#include <cstdint>
#include <vector>

// stratgrid — 地层格架核（纯数值，goal/property-modeling）。
//
// 地层坐标：柱 (i,j) 上 s = (z − top) / (bot − top)，s=0 在顶面、s=1 在底面。
// 第 k 层（0..nk-1）占 s ∈ [k/nk, (k+1)/nk)；末层含 s=1。界面点归更深一层。
// 深度轴约定：活柱要求 bot > top（值向下增大，TVD/TWT 同号）。顶底重合的柱
// 是死柱；整幅没有任何正厚度柱时如实拒绝（全幅重合 → 「奇异面」）。
// NaN 像元 → 死柱，不补值。像元原点是列 0 / 行 0 的角点，中心在半像元处；
// dy 可为负（北向上栅格，行号增加 y 减小）。

namespace paleo::stratgrid
{

constexpr double kThicknessEps = 1e-4;

struct SurfaceGrid
{
  int cols = 0;
  int rows = 0;
  double originX = 0;
  double originY = 0;
  double dx = 1;
  double dy = 1;
  std::vector<float> z; // row-major，rows*cols；NaN = 无值
};

struct Index3
{
  int i = 0;
  int j = 0;
  int k = 0;
};

struct Point3
{
  double x = 0;
  double y = 0;
  double z = 0;
};

struct ZoneGrid
{
  int ni = 0;
  int nj = 0;
  int nk = 0;
  double originX = 0;
  double originY = 0;
  double dx = 1;
  double dy = 1;
  std::vector<float> topZ; // ni*nj
  std::vector<float> botZ;
  std::vector<std::uint8_t> live; // 1 = 活柱
  int liveColumns = 0;
  int deadColumns = 0;

  int columnIndex(int i, int j) const { return j * ni + i; }
  // 体素下标：k 最慢、j 次之、i 最快。(k * nj + j) * ni + i
  int cellIndex(int i, int j, int k) const { return (k * nj + j) * ni + i; }
  int cellCount() const { return ni * nj * nk; }
  bool inRange(int i, int j, int k) const
  {
    return i >= 0 && j >= 0 && k >= 0 && i < ni && j < nj && k < nk;
  }
  bool columnLive(int i, int j) const
  {
    if (i < 0 || j < 0 || i >= ni || j >= nj || live.empty())
      return false;
    return live[static_cast<std::size_t>(columnIndex(i, j))] != 0;
  }
};

// s ∈ [0,1] → 层号；界外返回 -1。界面（s = k/nk，k≥1）归更深一层。
int layerOfFraction(double s, int nk);

// 失败时 *out 保持清空。error 可空。
bool buildZoneGrid(const SurfaceGrid &top, const SurfaceGrid &bot, int nLayers,
                   ZoneGrid *out, QString *error = nullptr);

double cellVolume(const ZoneGrid &grid, int i, int j, int k);
double layerThickness(const ZoneGrid &grid, int i, int j, int k);
// kInterface = 0..nk（含底界面）。
bool interfaceZ(const ZoneGrid &grid, int i, int j, int kInterface, double *zOut);
bool cellCenter(const ZoneGrid &grid, int i, int j, int k, Point3 *out);
// 活柱内的点 → (i,j,k)；死柱 / 网格外 / 层段外 → false。
bool ijkAt(const ZoneGrid &grid, double x, double y, double z, Index3 *out);
// 6 邻接，只含活柱。死单元或越界查询返回空。顺序：±i、±j、±k。
std::vector<Index3> neighbors6(const ZoneGrid &grid, int i, int j, int k);

} // namespace paleo::stratgrid
