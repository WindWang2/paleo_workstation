// 层：数据
#pragma once

#include "types.h"
#include "variogram.h"

#include <cstdint>
#include <string>
#include <vector>

// geostat/sgs — 序贯高斯模拟（SGS）。
// 流程：样本正态得分变换 →（每实现）随机路径序贯访问 → 每格用
// 简单克里金（高斯域，均值 0 已知）以「静态样本 + 已模拟格」条件估计
// → N(est, var) 抽样 → 全场完成后经经验分位数反变换回原值域
//（线性内插 + 端部线性外推，直方图忠实样本分布）。
// 随机数：mt19937_64 原始 u64 → 53bit (0,1) → Box-Muller——不用
// std::normal_distribution（其序列是实现定义的，跨平台不可复现）。
// 同一 seed 两次运行逐位相等；各实现共享同一条随机流（先 shuffle 后
// 逐格抽样，顺序确定）。
// 层：数据
namespace paleo::geostat
{

struct SgsParams
{
  int nRealizations = 1;          // [1, 64]
  std::uint64_t seed = 42;
  int maxPoints = 16;             // SK 邻域上限（静态样本 + 已模拟格合并取最近）
  double searchRadius = 0;        // <= 0 → 全域最近 K
};

struct SgsResult
{
  Status status = Status::InvalidInput;
  std::string message;
  std::vector<std::vector<double>> realizations; // 每实现一张场，NaN = nodata
  int finiteCells = 0;   // 最后一实现的计数（各实现口径一致）
  int nodataCells = 0;
  int solverFailures = 0;
  int mergedDuplicates = 0;
  double sampleMean = 0;  // 原域样本统计（直方图忠实基准）
  double sampleStd = 0;
};

SgsResult sgs( const std::vector<Sample> &samples, const GridSpec &grid,
               const VariogramModel &model, const SgsParams &params,
               const Control &control = {} );

} // namespace paleo::geostat
