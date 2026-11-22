// 层：功能
// workflow/capabilityplan_outputs.h — H3 v2：显式多成果（exports）的计划值类型。
// v1 单成果契约（preparePlan/validatePlan）保持不变；本模块只为带显式
// exports 描述的 v2 计划服务。无 I/O、子操作或数值调用。
#pragma once

#include "capabilityplan.h"

namespace paleo::capability {

// 计划描述符里的版本语义：缺版本 = v1（冻结历史按原样恢复）；2 = 显式
// exports；其他值一律拒绝，不做“善意猜测”或静默降级。
inline constexpr int kPlanDescriptorV1 = 1;
inline constexpr int kPlanDescriptorV2 = 2;

// 首批上界：export 数量与单个别名长度（UTF-8 字节）。沿用 v1 的 32 节点/
// 64 边/256 KiB 描述预算，不因 v2 放宽。
inline constexpr int kPlanMaxExports = 8;
inline constexpr qsizetype kPlanMaxAliasBytes = 128;

// 一项导出：父输出的稳定端口身份（alias）+ 精确来源（nodeId + 该节点选定
// 的 outputPort）。alias 是持久端口 ID（进入 stagePlan/artifactManifest/
// receipt），必须通过 isSafePathSegment——节点 ID 本身允许 Unicode/分隔样
// 字符，绝不能拿它拼路径。
struct PlanExport {
  QString alias;
  QString nodeId;
  QString portId;
};

// 声明顺序即持久顺序：进入冻结 intent 与 requestHash，接收方不得按容器
// 枚举次序或显示名重排。
struct PlanExports {
  QVector<PlanExport> entries;
};

struct ValidatedPlanExports {
  bool ok = false;
  ValidatedPlan plan;             // 已通过共享整图预检（无单终端约束）
  PlanExports exports;            // 原样保留声明顺序
  QVector<int> nodeIndex;         // 与 exports.entries 平行：plan.nodes 下标
  QHash<QString, int> entryByAlias;
  QVector<ContractIssue> issues;
};

// 整图 + exports 闭包校验：复用 v1 拓扑/端口/参数/字面输入检查，再加
// 别名安全/唯一、精确引用、文件型端口、终端全覆盖、全节点有贡献。
// 分配任何子操作或写任何文件之前必须通过。
ValidatedPlanExports validatePlanV2(const QVector<PlanNode> &nodes, const QVector<PlanEdge> &edges,
                                    const PlanExports &exports);

// exports 描述的编解码。编码保持声明顺序；解码严格：未知字段、缺失字段、
// 未知版本都失败（未来 schema 字段不得被静默忽略后执行另一张图）。
QJsonObject planExportsJson(const PlanExports &exports);
bool planExportsFromJson(const QJsonObject &graph, PlanExports &exports, QString *code);

// 组合父 outputSpec：每个 export 一个端口，alias 为端口 ID；kind/unit/
// axis/datum/geometry/约束/required 全部复制自所选算法端口与真实产物契约，
// 不因导出改名而丢失科学语义。
AlgorithmSpec compositePlanSpec(const ValidatedPlanExports &validated);

} // namespace paleo::capability
