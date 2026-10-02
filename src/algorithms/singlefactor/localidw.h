// 层：数据
#pragma once

#include "types.h"

#include <span>

// 层：数据
namespace paleo::singlefactor
{

// 连续局部方向 IDW。调用前必须 resolveParameters，且 autosApplied 为真。
// 不分配像元×井或像元×方向采样矩阵；查询按批处理。
// 硬屏障使用 grid_connectivity_v1，不计算 FaultPathMetric 绕行距离。
QueryResult evaluateAt( const PreparedInput &input, std::span<const Point2> queryPoints,
                        const ResolvedParameters &parameters, const Control &control );

SurfaceResult evaluateLocalIdw( const PreparedInput &input, const GridSpec &grid,
                                const ResolvedParameters &parameters, const Control &control );

} // namespace paleo::singlefactor
