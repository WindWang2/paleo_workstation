// 层：数据
#pragma once

#include "planarpoint.h"

#include <vector>

namespace paleo::wellsiting
{
using Point2 = paleo::singlefactor::Point2;

// 空洞区域的显示/量算摘要。area 用格计数；boundaryParts 是格矩形并集的
// 外环集合（GEOS unary union，阶梯轮廓；8 连通域角点相接时可能多段）。
struct HoleRegion
{
  int id = 0;
  double area = 0;        // m²（cellCount × cellArea）
  int cellCount = 0;
  double maxDistance = 0; // 空洞内最深格的距最近井距离
  Point2 deepest;         // 最大空洞圆心（最深格中心）
  std::vector<std::vector<Point2>> boundaryParts;
};
}
