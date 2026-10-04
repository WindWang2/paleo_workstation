// 层：功能
#pragma once
#include <QString>

// workflow/boundaryeditrules.h — 相界地质语义类型的编辑规则（方向 39）。
// 纯语义判定：输入要素级事实（类型/环闭合/邻接相/渐变带），输出接受或
// 拒绝+原因。规则按冻结词表（boundarysemantics.h）三类差异化：
//   整合接触 — 边界不切割两侧相：跨边界邻接要素相代码已知且不同 → 拒绝；
//   尖灭     — 允许单边相带终止（开放端）：环不闭合可过；其余类型拒绝；
//   相变     — 允许渐变带：transition_width > 0 仅相变可携带。
// 诚实面：相代码未知不参与拒绝（中性放行，缺口归 QA 报告）；未分类不判。
// 挂点：CompositionWorkflow::saveFaciesAttributes 赋类/带域前逐要素门禁。
namespace BoundarySemantics
{

// 要素级边界事实（由调用方从图层上下文装配；单位：transitionWidth 为
// 图层长度单位，ringClosed 指环几何首尾是否重合）。
struct BoundaryEditFacts
{
  QString kind;               // 待生效的边界类型（空 = 未分类）
  bool ringClosed = true;     // 环是否闭合（尖灭允许 false）
  int selfFaciesCode = -1;    // 本要素相代码（<0 = 未知）
  bool hasAdjacent = false;   // 是否存在跨边界（共享边线段）邻接要素
  int adjacentFaciesCode = -1; // 与本要素相代码不同的邻接要素相代码（证据值；
                               // 无差异/未知时与本要素同值或 -1）
  bool adjacentFaciesDiffers = false; // 任一已知相代码的邻接要素与本要素不同
  double transitionWidth = 0; // 渐变带宽度（<=0 = 无带）
};

// 判定结果（拒绝时 reason 含冻结词面与证据值，直接面向用户）。
struct BoundaryEditVerdict
{
  bool accepted = true;
  QString reason;
};

// 赋类检查：把 kind 赋给该要素是否合法（整合接触切两侧在此拒绝）。
BoundaryEditVerdict checkKindAssignment( const BoundaryEditFacts &facts );

// 带域检查：渐变带宽度随赋类提交是否合法（渐变带仅相变可携带）。只判
// 本次提交的带宽提案；换类型不携带宽键时旧带宽数据不判（惰性——非相变
// 类目不渲染带、QA 不核查），不静默清零也不锁死类型切换。
BoundaryEditVerdict checkTransitionBand( const BoundaryEditFacts &facts );

// 环编辑检查：几何编辑后的环闭合状态在该 kind 下是否合法（尖灭开放端可过）。
BoundaryEditVerdict checkRingClosure( const BoundaryEditFacts &facts );

} // namespace BoundarySemantics
