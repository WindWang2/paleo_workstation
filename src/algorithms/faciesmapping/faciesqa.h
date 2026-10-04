// 层：数据
#pragma once

#include "candidateboundaries.h"

#include <QVariantMap>

#include <string>
#include <vector>

// faciesmapping/faciesqa — 编图一致性 QA 检测器（goal/facies-automapping
// 阶段4）。输入草稿相图单元 + 井点 + 硬约束线，输出逐条可定位问题
//（regionIds + 定位点 + 度量值）。检测器只报告不修改；报告可复现
//（同输入同输出，无随机/时间因素）。

namespace paleo::faciesmapping
{

enum class FaciesQaIssueType
{
  UnclosedRing,        // 环首尾不闭合（或点数不足）
  Overlap,             // 单元两两重叠
  SmallIsland,         // 面积小于可配阈值的孤岛
  ConstraintConflict,  // 单元边界穿越硬约束线
  NoWellCoverage,      // 单元内（或缓冲半径内）无井点
  // ---- 方向 39：按相界类型（boundaryKind）核查 ----
  PinchoutTipDangling, // 尖灭开放端无落位（悬空——距最近可落位边界超容差）
  TransitionBandMissing, // 相变单元无渐变范围（transitionWidth <= 0）
  ConformableCutFacies  // 整合接触边界切割两侧相（共享边两侧相代码不同）
};

inline const char *faciesQaIssueName( FaciesQaIssueType type )
{
  switch ( type )
  {
    case FaciesQaIssueType::UnclosedRing:
      return "unclosed_ring";
    case FaciesQaIssueType::Overlap:
      return "overlap";
    case FaciesQaIssueType::SmallIsland:
      return "small_island";
    case FaciesQaIssueType::ConstraintConflict:
      return "constraint_conflict";
    case FaciesQaIssueType::NoWellCoverage:
      return "no_well_coverage";
    case FaciesQaIssueType::PinchoutTipDangling:
      return "pinchout_tip_dangling";
    case FaciesQaIssueType::TransitionBandMissing:
      return "transition_band_missing";
    case FaciesQaIssueType::ConformableCutFacies:
      return "conformable_cuts_facies";
  }
  return "unknown";
}

// 待检编图单元（草稿相图的每个面）。
struct FaciesMapUnit
{
  std::string regionId;
  int faciesCode = -1;
  Polygon geometry;
  double area = 0; // <=0 → 由检测器现算
  // 方向 39：相界地质语义类型（boundarysemantics 词面 id；空 = 未分类，
  // 不参与按类型核查——draft 路径零行为变化）。
  std::string boundaryKind;
  double transitionWidth = 0; // 相变渐变带宽（<=0 = 无带）
};

// 硬约束线（如断层/隔挡线）：单元边界不得穿越。
struct QaConstraintLine
{
  std::string id;
  std::vector<Point2> points;
};

struct FaciesQaOptions
{
  double minIslandArea = 0;        // <=0 → 孤岛检测器关闭
  double wellCoverageRadius = 0;   // 0 = 井点须落在面内；>0 = 允许缓冲半径
  double overlapTolerance = 1e-9;  // 交集面积 <= 该值不算重叠
  double ringClosureTolerance = 1e-9; // 首尾距离 > 该值判不闭合
  double pinchoutTipTolerance = 0; // <=0 → 尖灭端点落位检测关闭；>0 = 开放端
                                   // 距最近可落位边界（他单元边界/约束线）上限
};

struct FaciesQaIssue
{
  FaciesQaIssueType type = FaciesQaIssueType::Overlap;
  std::vector<std::string> regionIds;  // 涉及单元（重叠类为两个）
  std::vector<std::string> relatedIds; // 相关约束线 id（交叉冲突用）
  double metric = 0;                   // 面积/距离/间隙（NoWellCoverage 无井系统时 = -1）
  Point2 location{ 0, 0 };             // 定位点（重叠取交面上一点，环取缺口中点）
};

struct FaciesQaResult
{
  Status status = Status::InvalidInput;
  std::string message;
  std::vector<FaciesQaIssue> issues;
  QVariantMap diagnostics; // 各类型计数 + 检测器开关状态
};

struct QaWellPoint
{
  double x = 0;
  double y = 0;
};

FaciesQaResult runFaciesQa( const std::vector<FaciesMapUnit> &units,
                            const std::vector<QaWellPoint> &wells,
                            const std::vector<QaConstraintLine> &constraints,
                            const FaciesQaOptions &options = {},
                            const Control *control = nullptr );

} // namespace paleo::faciesmapping
