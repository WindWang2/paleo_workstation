// 层：数据
#pragma once

#include "types.h"
#include "variogram.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// geostat/sgs — 序贯高斯模拟（SGS）。
// 流程：样本正态得分变换 →（每实现）随机路径序贯访问 → 每格用
// 简单克里金（高斯域，均值 0 已知）以「静态样本 + 已模拟格」条件估计
// → N(est, var) 抽样 → 全场完成后经经验分位数反变换回原值域
//（线性内插 + 端部线性外推，直方图忠实样本分布）。
// 随机数：mt19937_64 原始 u64 → 53bit (0,1) → Box-Muller——不用
// std::normal_distribution（其序列是实现定义的，跨平台不可复现）。
// 同一 seed 两次运行逐位相等。#478 起每实现一份派生流：seed +
// r·0x9E3779B97F4A7C15（模 2⁶⁴ 确定回绕，sgsfill 按带散布种子同口径；
// r=0 种子不变），实现间零数据依赖——无进度/取消回调且 R>1 时实现级
// 并行（≤4 工作线程），结果与顺序执行逐位一致。
// 层：数据
namespace paleo::geostat
{

// P3：SGS 硬屏障隔断的纯数据输入。分量归属由物化方（workflow 层）经
// labelHardBarriers 等既有分区核产出并做稳定重编号（分量号 = 该分量在
// row-major 扫描下首次出现的格索引序），与约束行的存储顺序无关。
struct SgsBarrierInput
{
  // rows*cols 逐格域归属：>=0 连通分量号；<0 = 屏障格/域外（nodata，不模拟）。
  std::vector<int> cellComponent;
  // 与调用方传入 sgs() 的 samples 一一对齐的分量归属（去重前的原始顺序）。
  // <0 = 井落在屏障上或未归属（与 localidw 对 ambiguous 井同口径：拒绝）。
  std::vector<int> sampleComponent;
  int componentCount = 0; // >= 1；每个分量都必须有至少一个样本（无样本闭合
                          // 区与 localidw「硬隔断无井闭合区」同口径：拒绝）
  // 引擎回执透传：实际进入隔断模型的屏障稳定 ID（物化方从分区核 consumedIds
  // 原样带出，进 QC 的 constraints_consumed——数值核不猜选择列表）。
  std::vector<std::string> consumedBarrierIds;
  // 分区模型名（物化方填写，如 "grid_connectivity_v1"）——QC 记录用。
  std::string barrierModel;
};

struct SgsParams
{
  int nRealizations = 1;          // [1, 64]
  std::uint64_t seed = 42;
  int maxPoints = 16;             // SK 邻域上限（静态样本 + 已模拟格合并取最近）
  double searchRadius = 0;        // <= 0 → 全域最近 K
  // PW-130：true → 在正态得分上重拟合变差函数（同类型、各向异性沿用）；
  // false（缺省，旧行为）→ 按得分方差比例缩放基台。
  bool refitVariogramOnNormalScores = false;
  // P3：可选硬屏障隔断输入（纯值，无 QGIS/图层依赖）。物化方把既有分区核
  //（singlefactor 屏障模型 grid_connectivity_v1）的格/样本分量归属冻结成
  // 值对象传入；无值 = 原数值路径，逐位不变（golden anchor 契约）。
  std::optional<SgsBarrierInput> barrier;
};

struct SgsResult
{
  Status status = Status::InvalidInput;
  std::string message;
  std::vector<std::vector<double>> realizations; // 每实现一张场，NaN = nodata
  int finiteCells = 0;   // 最后一实现的计数（随机路径不同，各实现的 nodata
  int nodataCells = 0;   // 模式可不同——如实取末实现，不冒充全实现一致）
  int solverFailures = 0; // 跨实现累计
  int mergedDuplicates = 0;
  double sampleMean = 0;  // 原域样本统计（直方图忠实基准；有屏障时为全样本
  double sampleStd = 0;   // 口径，分量统计见 componentSampleCount/barrierNode）
  // P3 隔断诊断（无屏障输入时为零值/空，不影响既有字段含义）：
  int barrierCells = 0;                  // cellComponent < 0 的格数
  std::vector<int> componentCellCount;   // 各分量格数（按分量号）
  std::vector<int> componentSampleCount; // 各分量去重后样本数
  std::vector<std::string> consumedBarrierIds; // 引擎回执（barrier 输入透传）
  std::string barrierModel;              // 隔断模型名（无屏障为空）
};

SgsResult sgs( const std::vector<Sample> &samples, const GridSpec &grid,
               const VariogramModel &model, const SgsParams &params,
               const Control &control = {} );

} // namespace paleo::geostat
