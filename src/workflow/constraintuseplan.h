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

} // namespace paleo::singlefactor
