// 层：数据
#pragma once

#include "sgs_internal.h"
#include "types.h"
#include "variogram.h"

#include <cstdint>
#include <string>
#include <vector>

// geostat/sgs3 — 序贯高斯模拟的三维点集入口（goal/prop-model-v2）。
//
// 与 2D 栅格入口 sgs() 共享同一套序贯模拟机件（sgs_internal.h：正态得分
// 变换、Box-Muller mt19937_64 随机流、简单克里金求解、分位反变换）——
// 不是新模拟核，是同一核的三维目标面。差别只在邻域策略：
//   * 2D：规则栅格环扫（访问序是逐位结果的组成部分，冻结不动）；
//   * 3D：目标点自带格架索引 (ix,iy,iz) + 实际坐标 (x,y,z)，六面壳扩张
//     按索引空间、按实际坐标计量；并列距离由固定环访问序打破——结果
//     确定（不是严格下标全序：早断可漏掉后续壳中恰好并列的点）。
//
// 连通屏障（group）：样本与目标各带连通组分号（断层分隔的断块），条件
// 邻域只在同组分内取——跨断块零泄漏是机制不是约定。无断层场景全 0。
//
// 层：数据
namespace paleo::geostat
{

struct Sample3
{
  double x = 0;
  double y = 0;
  double z = 0;
  double value = 0;
  int group = 0; // 连通组分（断块）；负数非法
};

struct Sgs3Target
{
  int ix = 0;
  int iy = 0;
  int iz = 0;   // 格架索引（环扫邻域用）；各轴须在 [0, 2^20)
  double x = 0;
  double y = 0;
  double z = 0; // 实际坐标（变差方程组与输出用）
  int group = 0;
};

// 环扫剪枝用的索引轴度量步长（stratgrid 侧：dx、|dy|、活柱最小层厚）。
// topRelief = 活柱 top 极差（含断块错位后的）：跨柱 z 落差可抵消层厚差，
// 垂向下界按「同柱 r·zMin / 跨柱 sqrt(min(dx,|dy|)² + max(0, r·zMin−relief)²)」
// 分解（见 sgs3.cpp 剪枝注释）。平格架 relief=0 退化为三轴最小步长。
struct Lattice3Steps
{
  double x = 1;
  double y = 1;
  double zMin = 1;
  double topRelief = 0;
};

struct Sgs3Params
{
  int nRealizations = 1;          // [1, 64]
  std::uint64_t seed = 42;
  int maxPoints = 16;             // SK 邻域上限（静态样本 + 已模拟点合并取最近）
  double searchRadius = 0;        // <= 0 → 全域最近 K；> 0 时三维欧氏距离过滤
};

struct Sgs3Result
{
  Status status = Status::InvalidInput;
  std::string message;
  std::vector<std::vector<double>> realizations; // 每实现每目标一个值（输入序），NaN = nodata
  int finitePoints = 0;   // 最后一实现的计数（各实现口径一致）
  int nodataPoints = 0;
  int solverFailures = 0; // 无同组分邻域或数值失败的目标次数（跨实现累计）
  int mergedDuplicates = 0;
  double sampleMean = 0;
  double sampleStd = 0;
};

Sgs3Result sgs3( const std::vector<Sample3> &samples, const std::vector<Sgs3Target> &targets,
                 const Lattice3Steps &steps, const VariogramModel &model,
                 const Sgs3Params &params, const Control &control = {} );

} // namespace paleo::geostat
