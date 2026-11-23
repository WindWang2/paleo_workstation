// 层：功能
// workflow/capabilitynativeplan_v2.cpp — H3 v2：显式 exports 的整图冻结与恢复。
// 冻结描述符带 version=2 与有序 exports；恢复严格解码（未知字段/版本拒绝）、
// 重新整图校验，执行走类型化收集器，父工件按 alias 汇成组合输出集。
#include "capabilitynativeoperation.h"
#include "capabilityplanexecution.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

namespace paleo::capability {
namespace {

QJsonObject nodeJson(const PlanNode &node) {
  return QJsonObject{{"id", node.id},
                     {"capability", node.capabilityId},
                     {"version", node.contractVersion},
                     {"outputPort", node.outputPort},
                     {"intent", node.intent},
                     {"parameters", QJsonObject::fromVariantMap(node.parameters)},
                     {"inputs", QJsonObject::fromVariantMap(node.inputs)}};
}

QJsonObject descriptorV2(const QVector<PlanNode> &nodes, const QVector<PlanEdge> &edges,
                         const PlanExports &exports) {
  QJsonArray nodeRows, edgeRows;
  for (const auto &node : nodes)
    nodeRows.append(nodeJson(node));
  for (const auto &edge : edges)
    edgeRows.append(QJsonObject{{"from", edge.from}, {"fromPort", edge.fromPort}, {"to", edge.to},
                                {"toPort", edge.toPort}});
  return QJsonObject{{"nodes", nodeRows},
                     {"edges", edgeRows},
                     {"version", kPlanDescriptorV2},
                     {"exports", planExportsJson(exports).value("exports")}};
}

// 严格解码：节点/边对象只允许已知字段——未来 schema 字段不得被静默忽略后
// 执行另一张图（与 planExportsFromJson 的 graph 级严格性同一口径）。
bool decodeStrict(const QJsonObject &graph, QVector<PlanNode> &nodes, QVector<PlanEdge> &edges) {
  const auto nodeRows = graph.value("nodes").toArray();
  const auto edgeRows = graph.value("edges").toArray();
  if (nodeRows.isEmpty() || nodeRows.size() > 32 || edgeRows.size() > 64)
    return false;
  for (const auto &value : nodeRows) {
    const auto row = value.toObject();
    if (row.size() != 7 || !row.contains("id") || !row.contains("capability") || !row.contains("version") ||
        !row.contains("outputPort") || !row.contains("intent") || !row.contains("parameters") ||
        !row.contains("inputs"))
      return false;
    nodes.append({row.value("id").toString(), row.value("capability").toString(), row.value("version").toInt(),
                  row.value("outputPort").toString(), row.value("intent").toString(),
                  row.value("parameters").toObject().toVariantMap(), row.value("inputs").toObject().toVariantMap()});
  }
  for (const auto &value : edgeRows) {
    const auto row = value.toObject();
    if (row.size() != 4)
      return false;
    edges.append({row.value("from").toString(), row.value("fromPort").toString(), row.value("to").toString(),
                  row.value("toPort").toString()});
  }
  return true;
}

} // namespace

QJsonObject nativePlanIntentV2(const ExecuteRequest &request, const QVector<PlanNode> &nodes,
                               const QVector<PlanEdge> &edges, const PlanExports &exports) {
  return {{"host", nativeIntent(request)}, {"graph", descriptorV2(nodes, edges, exports)}};
}

FrozenOperation freezeNativePlanV2(const ExecuteRequest &request, const QVector<PlanNode> &nodes,
                                   const QVector<PlanEdge> &edges, const PlanExports &exports, QString *error) {
  const auto validated = validatePlanV2(nodes, edges, exports);
  if (!validated.ok) {
    if (error)
      *error = validated.issues.isEmpty() ? QStringLiteral("invalid_plan") : validated.issues.first().code;
    return {};
  }
  FrozenOperation plan;
  plan.capabilityId = QStringLiteral("workflow.plan");
  plan.implementationVersion = QStringLiteral("2");
  plan.callerScope = request.callerScope;
  plan.clientRequestId = request.clientRequestId;
  plan.intent = nativePlanIntentV2(request, nodes, edges, exports);
  QVector<PlanNode> normalized;
  normalized.reserve(validated.plan.nodes.size());
  for (const auto &entry : validated.plan.nodes)
    normalized.append(entry.node);
  auto frozenHost = request;
  frozenHost.capabilityId = QStringLiteral("workflow.plan");
  plan.frozenPlan = {{"host", nativeFrozenRequest(frozenHost)}, {"graph", descriptorV2(normalized, edges, exports)}};
  plan.phase = request.publishIntent.isEmpty() ? QStringLiteral("compute-only") : request.publishIntent;
  // 组合父 outputSpec：所有选中 exports 都成为父端口（targets/manifest 同源）。
  plan.outputSpec = compositePlanSpec(validated);
  for (const auto &entry : validated.exports.entries)
    plan.outputPorts.append(entry.alias);
  plan.worker = restoreNativePlanV2(plan.frozenPlan, error);
  return plan;
}

OperationWorker restoreNativePlanV2(const QJsonObject &plan, QString *error) {
  const auto fail = [error](const QString &code) {
    if (error)
      *error = code;
    return OperationWorker{};
  };
  ExecuteRequest host;
  if (!nativeRequestFromJson(plan.value("host").toObject(), host))
    return fail(QStringLiteral("invalid_frozen_plan"));
  const auto graph = plan.value("graph").toObject();
  PlanExports exports;
  QString code;
  if (!planExportsFromJson(graph, exports, &code))
    return fail(code);
  QVector<PlanNode> nodes;
  QVector<PlanEdge> edges;
  if (!decodeStrict(graph, nodes, edges))
    return fail(QStringLiteral("invalid_frozen_plan"));
  const auto validated = validatePlanV2(nodes, edges, exports);
  if (!validated.ok)
    return fail(validated.issues.isEmpty() ? QStringLiteral("invalid_plan") : validated.issues.first().code);
  const auto &topology = validated.plan;
  // 逐导出的父工件来源与组内依赖：上游被导出端口以 parentPortIds（同组父
  // 版本）表达，未导出的上游保留为描述性 provenance（extra.planUpstream）。
  QHash<QString, QString> aliasByNode;
  for (int i = 0; i < validated.exports.entries.size(); ++i)
    aliasByNode.insert(topology.nodes.at(validated.nodeIndex.at(i)).node.id, validated.exports.entries.at(i).alias);
  return [host, nodes, edges, exports, aliasByNode](const OperationWorkContext &context) mutable {
    OperationWorkResult out;
    auto request = host;
    request.operationDir = context.privateDir;
    request.clientRequestId = context.attemptId;
    request.projectDir = context.projectDir;
    request.publishIntent = QStringLiteral("compute-only");
    request.cancelled = context.cancelled;
    request.catalog = nullptr;
    // 外部字面 raster 引用（operation_result/raster_path）沿用既有受信解析：
    // 图内边产物由当前私有尝试授权，外部引用必须经已验证的来源校验。
    for (auto &node : nodes)
      if (node.capabilityId == QLatin1String("singlefactor.contours") && node.inputs.contains("raster_path")) {
        auto literal = request;
        literal.inputs = node.inputs;
        const auto literalCode = verifyNativeRasterInput(literal);
        if (!literalCode.isEmpty()) {
          out.code = literalCode;
          return out;
        }
        node.inputs = literal.inputs;
      }
    const auto collected = executePlanCollected(request, nodes, edges, exports, context.cancelled);
    out.ok = collected.ok;
    out.code = collected.code;
    out.summary.insert("plan_order", QJsonArray::fromStringList(collected.order));
    out.summary.insert("plan_version", 2);
    QJsonArray children;
    for (const auto &entry : collected.childOperations) {
      auto child = QJsonObject::fromVariantMap(entry.toMap());
      child.insert("parentOperationId", context.operationId);
      children.append(child);
    }
    if (!children.isEmpty())
      out.summary.insert("child_operations", children);
    if (!collected.failedNode.isEmpty())
      out.summary.insert("failed_node", collected.failedNode);
    QJsonArray exportRows;
    QHash<QString, QString> artifactPathByAlias;
    for (const auto &entry : exports.entries) {
      for (const auto &produced : collected.produced)
        if (produced.nodeId == entry.nodeId && produced.portId == entry.portId)
          artifactPathByAlias.insert(entry.alias, produced.artifactPath);
    }
    if (collected.ok) {
      for (const auto &entry : exports.entries) {
        const auto path = artifactPathByAlias.value(entry.alias);
        if (path.isEmpty()) {
          out.ok = false;
          out.code = QStringLiteral("missing_export_artifact");
          return out;
        }
        exportRows.append(QJsonObject{{"alias", entry.alias}, {"nodeId", entry.nodeId}, {"portId", entry.portId}});
      }
    }
    if (!exportRows.isEmpty())
      out.summary.insert("exports", exportRows);
    if (!out.ok)
      return out;
    // 父输出集：每个 export 一个成员；直接上游中被导出的端口以 parentPortIds
    // 关联（commitSet 解析为同组父版本），未导出上游留在描述性 extra。
    for (const auto &entry : exports.entries) {
      AlgorithmOutputArtifact artifact;
      artifact.portId = entry.alias;
      artifact.absolutePath = artifactPathByAlias.value(entry.alias);
      QStringList parentAliases;
      QVariantList upstream;
      for (const auto &edge : edges) {
        if (edge.to != entry.nodeId)
          continue;
        QVariantMap producer{{"nodeId", edge.from}, {"portId", edge.fromPort}};
        const auto upstreamAlias = aliasByNode.value(edge.from);
        if (!upstreamAlias.isEmpty()) {
          producer.insert("alias", upstreamAlias);
          if (!parentAliases.contains(upstreamAlias))
            parentAliases.append(upstreamAlias);
        }
        upstream.append(producer);
      }
      artifact.parentPortIds = parentAliases;
      QVariantMap extra{{"planNodeId", entry.nodeId}, {"planNodePort", entry.portId}};
      if (!upstream.isEmpty())
        extra.insert("planUpstream", upstream);
      artifact.extra = extra;
      out.outputs.artifacts.append(artifact);
    }
    out.outputs.ok = true;
    return out;
  };
}

} // namespace paleo::capability
