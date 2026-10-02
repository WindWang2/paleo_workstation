// 层：数据
#pragma once

#include "types.h"

#include <string>
#include <vector>

// 层：数据
namespace paleo::singlefactor
{

double meanNearestSpacing( const std::vector<Point2> &points );
double medianNearestSpacing( const std::vector<Point2> &uniqueSorted );

bool pointInDomain( const std::vector<Polygon> &domain, Point2 point, double tolerance );

// 半像元步长栅格化 + 4 连通。屏障格为 -2。落在屏障格且无明确归属的井列入 ambiguous，
// 不静默分到某一侧。displayBuffer 不参与栅格化。
BarrierGrid labelHardBarriers( const GridSpec &grid, const std::vector<ConstraintLine> &barriers,
                               const std::vector<Sample> &samples, double tolerance,
                               const Control *control = nullptr );

// 解析自动半径。失败时返回错误文案，params 不标 autosApplied。
std::string resolveParameters( const PreparedInput &input, const GridSpec &grid,
                               ResolvedParameters *params );

struct ClusterModel
{
  std::vector<std::vector<int>> groups; // 原始行号
  std::vector<std::vector<Point2>> hulls;
  double radius = 0;
  std::vector<double> weights( const std::vector<Point2> &queries, int sampleCount ) const;
};

ClusterModel buildClusters( const std::vector<Sample> &samples, double span );

Point2 cellCenter( const GridSpec &grid, int column, int row );

bool gridBudgetOk( const GridSpec &grid, int sampleCount, std::string *error );

// 版本 17 局部工作场。无实际穿线时 unchanged，不改数值。
WorkField buildCartographicWork( const GridSpec &grid, const std::vector<double> &analysis,
                                 const std::vector<std::uint8_t> &valid,
                                 const std::vector<ConstraintLine> &stops,
                                 const std::vector<ContourPolyline> &contours,
                                 const std::vector<double> &levels, double transitionDistance );

bool segmentsCross( Point2 a, Point2 b, Point2 c, Point2 d );

} // namespace paleo::singlefactor
