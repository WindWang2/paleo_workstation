// 层：数据
#pragma once

#include "types.h"

#include <vector>

// contour_avoidance 移植（haiyou-visualization 27fdb99，几何核子集）。
// 制图绕行：等值线绕断线缓冲的几何处理——只改折线几何，不估厚度、不连接
// 不同的源等值线；局部绕行保不住拓扑（越出缓冲/与他线相交）时，原线在
// 缓冲处截断。相对上游的差异：无高斯圆角、无尖端桥接、无场值复核
// （本侧调用方未提供标量场），语义口径为「沿缓冲边改道，否则截断」。
// 层：数据
namespace paleo::singlefactor
{

struct ContourAvoidResult
{
  Status status = Status::Ok;
  std::string message;
  std::vector<ContourPolyline> contours; // 改线后的等值线（级别原样保留）
  int rerouted = 0; // 成功绕行的穿线区间数
  int truncated = 0; // 在缓冲处截断的等值线数
};

// wall 为制图绕行线；width 为缓冲半宽；step 为网格步长（车道余量基准）。
ContourAvoidResult avoidContourBuffers( const std::vector<ContourPolyline> &contours,
                                        const std::vector<Point2> &wall, double width,
                                        double step );

} // namespace paleo::singlefactor
