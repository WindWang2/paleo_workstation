// 层：功能
// workflow/capabilityplanexecution.h — H3 v2：按 nodeId/portId 收集的类型化执行。
// 不再用“最后一个节点的 ExecuteResult”代表整图；失败保留有界节点状态。
#pragma once

#include "capabilitygateway.h"
#include "capabilityplan_outputs.h"

namespace paleo::capability {

// 单个节点执行后的真实工件（该节点选定的 outputPort）。
struct PlanNodeArtifact {
  QString nodeId;
  QString portId;
  QString artifactPath;
  QString childOperationId;
  QString childRequestHash;
};

struct PlanCollectedExecution {
  bool ok = false;
  QString code;
  QString failedNode;             // 空表示没有节点失败（取消/预检失败等）
  QStringList order;              // 实际稳定拓扑顺序
  QVector<PlanNodeArtifact> produced; // 已成功节点（失败时为成功前缀，有界）
  QVariantList childOperations;   // {nodeId, operationId, requestHash}
};

// 在既有稳定拓扑顺序上执行 v2 计划：每个节点一次执行（同一上游分叉到
// 两个 contour 时只算一次），边消费精确生产者端口，子请求身份由父尝试与
// 节点身份稳定派生。整图校验（含 exports 闭包）在本入口内重新执行，
// 不能拿外部修改过的 ok 当许可。
PlanCollectedExecution executePlanCollected(const ExecuteRequest &host, const QVector<PlanNode> &nodes,
                                            const QVector<PlanEdge> &edges, const PlanExports &exports,
                                            const std::function<bool()> &cancelled);

} // namespace paleo::capability
