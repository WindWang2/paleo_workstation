// 层：功能
// workflow/capabilityplan.h — 完整预检的值计划；不携带执行许可或宿主指针。
#pragma once

#include "../services/algorithmcontract.h"
#include "../services/capabilityspec.h"

namespace paleo::capability {

struct PlanNode {
  QString id;
  QString capabilityId;
  int contractVersion = 1;
  QString outputPort;
  QString intent; // 首批组合只接受 compute-only，空值规范化为 compute-only。
  QVariantMap parameters;
  QVariantMap inputs;
};

struct PlanEdge {
  QString from;
  QString fromPort;
  QString to;
  QString toPort;
};

struct ValidatedPlanNode {
  PlanNode node;
  AlgorithmSpec algorithm;
  QHash<QString, QString> inputKeys; // AlgorithmSpec 端口 -> 实际执行请求字段。
};

struct ValidatedPlanEdge {
  int fromNode = -1;
  int toNode = -1;
  AlgoPort fromPort;
  AlgoPort toPort;
  QString inputKey;
};

struct ValidatedPlan {
  bool ok = false;
  QVector<ValidatedPlanNode> nodes;
  QVector<ValidatedPlanEdge> edges;
  QStringList order;
  QVector<ContractIssue> issues;
};

// 无 I/O、子操作或数值调用。执行入口重新调用，不能拿外部修改过的 ok 当许可。
ValidatedPlan validatePlan(const QVector<PlanNode> &nodes, const QVector<PlanEdge> &edges);
// 兼容调用方的错误接口；message 不再编码拓扑顺序。
ContractIssue checkPlan(const QVector<PlanNode> &nodes, const QVector<PlanEdge> &edges);

} // namespace paleo::capability
