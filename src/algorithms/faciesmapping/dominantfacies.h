// 层：数据
#pragma once

#include "../singlefactor/types.h"

#include <QVariantMap>

#include <map>
#include <string>
#include <vector>

// faciesmapping/dominantfacies — 优势相分析（goal/facies-automapping 阶段1）。
// 纯计算：井点相代码柱（层位×井的深度区间观测）→ 优势相/频率统计表 + 覆盖率。
// 无 QtWidgets、无 QGIS 图层、无文件发布。诚实语义：证据不足不赋相
//（dominantCode = -1），覆盖率/平局如实记录进诊断字段。

namespace paleo::faciesmapping
{

using singlefactor::Control;
using singlefactor::Status;

// 一段深度上的相观测。faciesCode < 0 = 未定/缺失（厚度只进覆盖率分母侧，
// 不进任何相的频率）。confidence ∈ [0,1] 仅加权优势得分，不改厚度记账。
struct FaciesInterval
{
  double top = 0;
  double bottom = 0;
  int faciesCode = -1;
  double confidence = 1.0;
};

// 一口井 × 一个层位的相柱：[top, bottom] 为层位井段，intervals 为解释相段。
struct WellFaciesColumn
{
  std::string wellId;
  double x = 0;
  double y = 0;
  double top = 0;
  double bottom = 0;
  std::vector<FaciesInterval> intervals;
};

// 单相频率条目（按 thickness 降序输出）。
struct FaciesFrequency
{
  int faciesCode = -1;
  double thickness = 0;  // 该相在层段内的累计厚度（重叠段取最上，不双算）
  double fraction = 0;   // thickness / 已分类总厚度 ∈ (0,1]
  double score = 0;      // Σ thickness×confidence —— 优势裁决用
};

// 统计行（层位×井粒度，与输入井一一对应、保序）。
struct DominantFaciesRow
{
  std::string wellId;
  double x = 0;
  double y = 0;
  int dominantCode = -1;  // -1 = 证据不足（无分类厚度或门槛未过）
  double dominance = 0;   // 优势相 score 占总 score 比 ∈ [0,1]
  double coverage = 0;    // 已分类厚度 / 层段厚度 ∈ [0,1]；层段厚度<=0 → 0
  double classifiedThickness = 0;
  double horizonThickness = 0;
  bool tie = false;       // 优势并列（score 差在容差内）——如实标记，不造确定性
  std::vector<FaciesFrequency> frequencies;
};

struct DominantFaciesOptions
{
  // 行级门槛：coverage 或 dominance 低于门槛 → dominantCode = -1（统计照记）。
  double minCoverage = 0;
  double minDominance = 0;
};

struct DominantFaciesResult
{
  Status status = Status::InvalidInput;
  std::string message;
  std::vector<DominantFaciesRow> rows;
  QVariantMap diagnostics;
};

// wells 为空 → InvalidInput。区间越出层段范围按层段裁剪；区间乱序/重叠按
//「最上优先」吸收（cursor 扫描，重叠厚度不双算）——策略写进诊断。
DominantFaciesResult computeDominantFacies( const std::vector<WellFaciesColumn> &wells,
                                            const DominantFaciesOptions &options = {},
                                            const Control *control = nullptr );

} // namespace paleo::faciesmapping
