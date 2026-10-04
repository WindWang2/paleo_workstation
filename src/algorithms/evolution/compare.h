// 层：数据
#pragma once

#include "types.h"

// algorithms/evolution — 相邻期相覆盖对比引擎（方向35）。
namespace paleo::evolution
{

// 同源校验（格网尺寸/像元/原点/坐标域）：两期输入是否可在同口径下对比。
// same=false 时 reason 给出人可读拒算原因（含两侧数值）。
DomainCheck checkSameDomain( const FaciesCoverage &earlier, const FaciesCoverage &later );

// 相邻期对比引擎：earlier（老/深）→ later（新/浅）。
// 不同格网/坐标域 → Status::InvalidInput + message（拒算不近似，不插值硬对）。
// 输出：逐相面积增减/质心位移/边界进退中位距 + 稀疏重叠矩阵 + 前缘位移矢量场。
EvolutionResult compareFacies( const FaciesCoverage &earlier, const FaciesCoverage &later,
                               const CompareOptions &options = CompareOptions(),
                               const Control &control = Control() );

} // namespace paleo::evolution
