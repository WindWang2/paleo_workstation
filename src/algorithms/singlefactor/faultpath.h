// 层：数据
#pragma once

#include "types.h"

#include <optional>
#include <span>
#include <utility>
#include <vector>

// continuous_metric.FaultPathMetric + surfer_idw 移植（haiyou-visualization 27fdb99）。
// 有限断层绕行：以图上最短路（绕过障碍的测地距离）替代「不可见即丢弃样本」。
// 标准全局 IDW：各向异性权重 + 断层路径距离，无脊线输运、无井晕、无平滑。
// 上游节点集取 shapely epsilon-buffer 外环顶点；本实现用等价的解析拐点
// （线段端点 ± 垂向偏移），位置差 O(epsilon)，不改变绕行拓扑。
// 层：数据
namespace paleo::singlefactor
{

struct FaultLine
{
  std::vector<Point2> points;
};

// 屏障线段 (first=起点, second=终点)。
using FaultSegment = std::pair<Point2, Point2>;

// a→b 射线被线段 cd 横越则阻断（允许相切：端点接触不算）。
bool segmentBlocksSight( Point2 a, Point2 b, Point2 c, Point2 d );

class FaultPathMetric
{
public:
  // ratio/angleDegrees：可选各向异性度量（沿轴按 ratio 压缩）；1/0 即欧氏。
  FaultPathMetric( std::span<const Point2> wells, std::span<const FaultLine> barriers,
                   double ratio = 1.0, double angleDegrees = 0.0 );

  // 行主序 points.size() × wells.size() 距离矩阵；被阻 pair 取绕行最短路。
  std::vector<double> distances( std::span<const Point2> points ) const;

  bool hasDetourNodes() const { return mPaths.has_value(); }

private:
  struct NodeGraph
  {
    std::vector<Point2> nodes;
    std::vector<double> allPairs; // 节点间最短路（行主序 N×N）
    std::vector<double> toWells; // 节点→井（经可见出口），行主序 N×wells
  };

  std::vector<Point2> mWells;
  std::vector<FaultSegment> mSegments;
  double mRatio = 1.0;
  double mAngleRadians = 0.0;
  std::optional<NodeGraph> mPaths;

  double metricDistance( Point2 a, Point2 b ) const;
};

struct SurferIdwOptions
{
  double power = 2;
  double anisotropyRatio = 1;
  double anisotropyAngleDegrees = 0;
  int minPoints = 1;
  int maxPoints = 0; // 0 = 全部可达样本
  std::optional<double> searchRadius; // 物理欧氏半径，独立于各向异性权重
};

struct SurferIdwResult
{
  Status status = Status::Ok;
  std::string message;
  std::vector<double> values; // NaN = 缺失
};

// 标准全局 IDW 点查询。保留观测值域；完全隔离的无井仓保持 NaN。
SurferIdwResult interpolateGlobalIdw( std::span<const Point2> points,
                                      std::span<const Point2> wellXy,
                                      std::span<const double> wellValues,
                                      const SurferIdwOptions &options,
                                      std::span<const FaultLine> barriers,
                                      const Control *control = nullptr );

} // namespace paleo::singlefactor
