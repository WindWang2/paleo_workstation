// 层：功能
#pragma once

#include "domain/constraintsemantics.h"

#include <qgsgeometry.h>

#include <QVariantMap>
#include <QVector>

// 约束使用计划（P1-A）：store 行（ConstraintWorkflow::loadConstraints 的 QVariantMap）
// → 逐条/逐部件 ConstraintUseDecision。UI 计数、生成门、QC/血缘的「计划」
// 一侧都从这里取，不再数 type 字符串。几何按 store CRS（本地工程网格）解释；
// domain 也必须在同一 CRS。
namespace paleo::singlefactor
{

struct ConstraintUsePlan
{
  QVector<ConstraintUseDecision> decisions;
  // 与 decisions 逐项对齐的部件几何（store CRS 单部件折线；非线/空几何为 null）。
  // 网格化工作流按 decision 取实际消费的部件，不再另行拆分/按 type 过滤。
  QVector<QgsGeometry> partGeometries;
  // 与方法无关的「可用」口径：启用 + 参数合法 + 几何有效。
  int availableBarriers = 0;   // 有效语义 hard_barrier（完全阻断）
  int availableDirections = 0; // 有效语义 direction_guide
  int availableSoft = 0;       // interpretive_boundary
  int availableCartographic = 0; // contour_stop / cartographic_detour
  int invalid = 0;             // 参数非法或几何无效的部件/行
  int disabled = 0;
  int consumed = 0;            // 当前方法实际数值消费
  int blocking = 0;            // 当前方法会因此整次拒绝
  QString capabilityNote;
  QVariantMap summary;         // summarizeConstraintDecisions
};

ConstraintUsePlan planConstraintUse( const QVector<QVariantMap> &rows, const ConstraintMethodContext &context );

// 约束就绪状态（单一事实来源）：UI 生成门、提示与 workflow 共用。
// 约束线是可选的插值解释条件，不是普通插值的前置条件；只有当前方法会因约束
// 整次拒绝（plan.blocking>0）时才阻止运行。
enum class ConstraintReadinessState
{
  NoneRunnable,    // 1. 没有约束，允许正常计算
  Consumed,        // 2. 有约束，当前方法会使用
  PresentUnused,   // 3. 有约束，但当前方法不使用
  InvalidExcluded, // 4. 只有无效约束，不参与计算（不阻止运行）
  RequiredUnmet,   // 5. 当前方法要求的约束不满足（参数越界会被引擎整次拒绝）
};

struct ConstraintReadiness
{
  ConstraintReadinessState state = ConstraintReadinessState::NoneRunnable;
  bool runAllowed = true;
  QString message; // 中文一句话说明
};

ConstraintReadiness assessConstraintReadiness( const ConstraintUsePlan &plan );

} // namespace paleo::singlefactor
