// 层：数据
#pragma once

#include "types.h"

#include <string>
#include <vector>

// geostat/faultpath — 断层绕距（FaultPathMetric）：绕障碍多边形的测地
// 距离场，供「断层两侧不互用邻域」的距离语义。
// 算法取舍（写明）：障碍多边形栅格化（边界半像元步进标记 + 内部
// even-odd 扫描线填充，holes 挖空）+ 8 邻接 Dijkstra（边权各向异性：
// 横 pixelWidth、纵 |pixelHeight|、对角 hypot；对角步两侧正交格任一被挡
// 即禁行——防斜墙角接格缝隙穿越，issue #292）。快速行进（解 eikonal）
// 精度略高但均匀介质下 8 邻接 Dijkstra 与库内 distancetransform 先例
// 同口径，误差上界为斜边方向的 chamfer 偏差（~8%），简单可验。
// 输出：地图单位的测地距离；屏障格与不可达格 NaN（计数如实）。
// 层：数据
namespace paleo::geostat
{

struct BarrierRing
{
  std::vector<Point2> points;
};

struct BarrierPolygon
{
  BarrierRing exterior;
  std::vector<BarrierRing> holes;
};

struct FaultPathResult
{
  Status status = Status::InvalidInput;
  std::string message;
  std::vector<double> distance; // row-major，NaN = 屏障/不可达
  int reachedCells = 0;
  int barrierCells = 0;
  int unreachableCells = 0;
  int sourceColumn = -1;
  int sourceRow = -1;
};

// 从 (sourceX, sourceY) 出发的绕障测地距离场。源点落在屏障格时向
// 邻域螺旋搜自由格（≤5 格）；无自由格 → InvalidInput。
FaultPathResult faultPathMetric( const GridSpec &grid,
                                 const std::vector<BarrierPolygon> &faultPolygons,
                                 double sourceX, double sourceY,
                                 const Control &control = {} );

} // namespace paleo::geostat
