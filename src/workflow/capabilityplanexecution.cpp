// 层：功能
// workflow/capabilityplanexecution.cpp — H3 v2 类型化收集执行器。
#include "capabilityplanexecution.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonDocument>

namespace paleo::capability {
namespace {

QString shaBytes(const QByteArray &bytes) {
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

} // namespace

PlanCollectedExecution executePlanCollected(const ExecuteRequest &host, const QVector<PlanNode> &nodes,
                                            const QVector<PlanEdge> &edges, const PlanExports &exports,
                                            const std::function<bool()> &cancelled) {
  PlanCollectedExecution out;
  const auto validated = validatePlanV2(nodes, edges, exports);
  if (!validated.ok) {
    out.code = validated.issues.isEmpty() ? QStringLiteral("invalid_plan") : validated.issues.first().code;
    return out;
  }
  const auto &plan = validated.plan;
  if (host.documentId.isEmpty() || host.callerScope.isEmpty() || host.clientRequestId.isEmpty() ||
      host.operationDir.isEmpty()) {
    out.code = QStringLiteral("operation_identity_required");
    return out;
  }
  // 在创建第一个子操作前核对每个字面引用（外部 operation/managed 结果），
  // 与 v1 executePlan 同一口径。
  for (const auto &entry : plan.nodes) {
    const auto raster = entry.node.inputs.value(QStringLiteral("raster_path")).toString();
    if (!raster.isEmpty() && !isIssuedResult(host.documentId, raster)) {
      out.code = QStringLiteral("untrusted_result_ref");
      return out;
    }
  }
  QHash<QString, int> byId;
  for (int i = 0; i < plan.nodes.size(); ++i)
    byId.insert(plan.nodes[i].node.id, i);
  out.order = plan.order;
  QHash<int, PlanNodeArtifact> produced;
  for (const auto &id : plan.order) {
    if (cancelled && cancelled()) {
      out.code = QStringLiteral("cancelled");
      out.failedNode = id;
      return out;
    }
    const int index = byId.value(id);
    const auto &node = plan.nodes[index].node;
    ExecuteRequest request = host;
    request.capabilityId = node.capabilityId;
    request.contractVersion = node.contractVersion;
    // 子请求身份 = 父请求 + 精确节点 ID 的稳定哈希；与 v1 完全一致，
    // 保证 v2 重放与 v1 历史子操作的身份兼容。
    const auto identity = QJsonDocument(QJsonArray{host.clientRequestId, node.id}).toJson(QJsonDocument::Compact);
    request.clientRequestId = QStringLiteral("plan-node:") + shaBytes(identity);
    request.publishIntent = node.intent;
    request.parameters = node.parameters;
    request.inputs = node.inputs;
    request.onRunning = {};
    for (const auto &edge : plan.edges) {
      if (edge.toNode != index)
        continue;
      const auto upstream = produced.value(edge.fromNode);
      if (upstream.artifactPath.isEmpty()) {
        out.code = QStringLiteral("missing_edge_value");
        out.failedNode = id;
        return out;
      }
      request.inputs.insert(edge.inputKey, upstream.artifactPath);
    }
    const auto result = execute(request);
    if (!result.ok) {
      out.code = result.code;
      out.failedNode = id;
      out.childOperations.append(QVariantMap{{"nodeId", node.id}, {"operationId", result.operationId},
                                             {"requestHash", result.requestHash}, {"failed", true}});
      return out;
    }
    const auto path = result.summary.value(QStringLiteral("result_path")).toString();
    if (path.isEmpty()) {
      // 成功回包必须携带真实工件路径；空路径不能冒充成功端口。
      out.code = QStringLiteral("missing_result_port");
      out.failedNode = id;
      out.childOperations.append(QVariantMap{{"nodeId", node.id}, {"operationId", result.operationId},
                                             {"requestHash", result.requestHash}, {"failed", true}});
      return out;
    }
    PlanNodeArtifact artifact{node.id, node.outputPort, path, result.operationId, result.requestHash};
    produced.insert(index, artifact);
    out.produced.append(artifact);
    out.childOperations.append(QVariantMap{{"nodeId", node.id}, {"operationId", result.operationId},
                                           {"requestHash", result.requestHash}});
  }
  out.ok = true;
  out.code = QStringLiteral("computed");
  return out;
}

} // namespace paleo::capability
