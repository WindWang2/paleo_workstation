// 层：数据
#pragma once

#include "stratgrid.h"

#include <QString>

#include <vector>

// stratgrid/faultoffset — 断块错位（goal/prop-model-v2）。
//
// 断距矢量的 z 分量驱动网格错位：下掉侧整柱 top/bot 平移同一断距，跨断层
// 同 k 层面错位量 = 断距。连通屏障（竖帘 columnBlock）是另一层机制，由
// propfill::assignColumnBlocks 承担——几何错位与连通阻断分层（方向 45 提示词
// 口径）。无断距数据的断层在调用方保持纯竖帘（不进本模块），口径由编排层标注。
//
// 语义（钉死）：
//   * 断距沿段线性内插 throwStart→throwEnd（t 为柱心在段上的投影参数）；
//     正值 = 下掉侧 z 增大（更深；stratgrid z 向下为正），负值即逆冲上推。
//   * dropLeftSide：面向线段方向（x0→x1）的左侧为下掉侧。调用方把
//     FaultHangingSide 等盘侧语义映射到这里。
//   * 每柱取柱心最近的断层段（并列取下标小者，确定性）——一柱一断层的
//     最近主控口径，多断层不叠加。
//   * 柱心恰在断层线上（叉积≈0，边界柱两盘归属未定）不动，如实计数。
//   * 整柱 top/bot 平移同一断距：层厚保序、活/死柱不变。深度随层变化
//     的生长断层断距递延（单一垂向断距口径）。
//   * heave（x/y 向水平错动）破坏规则柱假设（ijkAt/粗化/切片全依赖
//     originX/dx 规则柱位）——递延，见 ledger 轮0 决策 1。
namespace paleo::stratgrid
{

struct FaultThrow
{
  double x0 = 0;
  double y0 = 0;
  double x1 = 0;
  double y1 = 0;
  double throwStart = 0; // 米，z 向下为正
  double throwEnd = 0;
  bool dropLeftSide = true;
};

struct FaultOffsetMeta
{
  int offsetColumns = 0;   // 实际施位移的下掉侧活柱数
  int boundaryColumns = 0; // 恰在断线上未动的活柱数
  double maxAbsThrow = 0;  // 施加断距的最大绝对值（米）
};

// 错位格架写到 *out（副本，原格架不动）；每柱位移写 dzPerColumn（ni*nj，
// 死柱/未动柱 0）。失败时 *out/*dzPerColumn 保持清空。
bool applyFaultOffset(const ZoneGrid &grid, const std::vector<FaultThrow> &throws,
                      ZoneGrid *out, std::vector<float> *dzPerColumn,
                      FaultOffsetMeta *meta = nullptr, QString *error = nullptr);

} // namespace paleo::stratgrid
