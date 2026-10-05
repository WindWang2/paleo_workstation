// 层：数据
#pragma once

#include "propfill.h"

#include "../geostat/sgs3.h"
#include "../geostat/variogram.h"

#include <QString>

#include <vector>

// stratgrid/sgsfill — 序贯高斯充填接入 IJK 格架（goal/prop-model-v2）。
//
// 不是新模拟核：数值机件全部来自 geostat（sgs3 = 同一序贯模拟核的
// 三维点集入口）。本文件只做编排转换：
//   * 条件点 = 粗化井柱 cell（seedsFromUpscale 的种子，重合种子合并取均值）；
//   * 序贯路径走三维网格（目标 = 活柱全部 cell，坐标取 cell 实际中心）；
//   * 断层竖帘分块（assignColumnBlocks）→ 连通组分屏障：跨断块零条件泄漏；
//   * 相带分区参数域分离：每带独立正态得分变换与样本统计、独立 sgs3 调用
//     （变差几何共享全局模型——带内样本量不足以可信拟合带内变差，如实共享
//     并在口径串标注）；带内种子 < 2 → 该带保持未充填并计数，不混全域参数；
//   * 硬数据钉死：种子 cell 在每个 realization 回写种子值（PropertyVolume
//     是 float 存储，钉死精度 = float 转换精度）。
// 无相带（zonePerColumn 空指针）= 全域单一参数域，口径如实标注。
namespace paleo::stratgrid
{

struct SgsFillMeta
{
  int zoneCount = 1;                   // 相带分区数（无相带 = 1）
  int insufficientZones = 0;           // 种子 < 2 保持未充填的带数
  std::vector<int> insufficientZoneCodes; // 对应相带码（升序）
  int mergedSeedDuplicates = 0;        // 重合种子合并次数
  int snappedSeedCells = 0;            // 钉死硬数据的 cell 数（去重后）
  int solverFailures = 0;              // sgs3 跨带累计
  int realizationCount = 1;
  QString caliber;                     // 口径串（provenance 直接带走）
};

// 相带多边形环（外环；孔洞递延——编图产物是无重叠邻接瓦片，如实口径）。
struct ZoneRing
{
  int code = 0;
  std::vector<double> xs;
  std::vector<double> ys;
};

// 环 → 柱相带码：柱心逐环偶奇测试（射线法），先命中先得（环序确定即确定）；
// 无命中 = -1（背景域，自成参数域——样本不足时如实未充填）。死柱 -1。
// 返回尺寸 ni*nj。环数或环内点数为零 → 全 -1。
std::vector<int> rasterizeZoneRings(const ZoneGrid &grid, const std::vector<ZoneRing> &rings);

// 成功 → *out 填 params.nRealizations 个属性体（各含同一 columnBlock 分块）。
// progress 返回 false 取消（失败不写半成品）。失败时 *out 保持清空。
bool fillSgs(const ZoneGrid &grid, const std::vector<Seed> &seeds,
             const std::vector<FaultSegment> &faults, const std::vector<int> *zonePerColumn,
             const geostat::VariogramModel &model, const geostat::Sgs3Params &params,
             std::vector<PropertyVolume> *out, SgsFillMeta *meta,
             const FillProgress &progress = {}, QString *error = nullptr);

} // namespace paleo::stratgrid
