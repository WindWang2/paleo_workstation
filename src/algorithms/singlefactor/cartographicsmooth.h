// 层：数据
#pragma once

#include "types.h"

#include <vector>

// 上游移植：constrained_engine.py 制图平滑辅助函数（纯几何，无 GEOS）。
// cartographic_smooth_contours(contours, grid_step, iterations)：
// 压阶梯 → 滑动平均 → 去尖刺 → RDP → 补密 → Chaikin（自交回退）。
// 输入输出均为 (level → polylines) 的有序映射。

namespace paleo::singlefactor
{

using Polyline = std::vector<Point2>;
// (level, lines) 顺序保留输入 dict 迭代序（调用方按 level 升序传入）。
using ContourLineMap = std::vector<std::pair<double, std::vector<Polyline>>>;

bool isClosedPolyline( const Polyline &points, double gridStep );
double polylinePointDistance( Point2 a, Point2 b );
double polylinePathLength( const Polyline &points );
Polyline dedupeConsecutivePoints( const Polyline &points, double tolerance );
Polyline rdpSimplify( const Polyline &points, double tolerance );
Polyline chaikinSmoothPolyline( const Polyline &points, int iterations, double gridStep );
bool polylineSelfIntersects( const Polyline &points, double gridStep );
Polyline cartographicSmoothPolyline( const Polyline &points, double gridStep,
                                     int iterations = 4 );
ContourLineMap cartographicSmoothContours( const ContourLineMap &contours,
                                           double gridStep, int iterations = 3 );

} // namespace paleo::singlefactor
