// 层：功能
// workflow/capabilityplan_outputs.cpp — H3 v2 exports 闭包校验与组合 outputSpec。
#include "capabilityplan_outputs.h"

#include "../catalog/datacatalog.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

namespace paleo::capability {
namespace {

bool fail(ValidatedPlanExports &out, const QString &code, const QString &field = {}) {
  out.issues.append({code, field, {}});
  return false;
}

bool fileKind(const QString &kind) {
  return kind == QLatin1String("raster") || kind == QLatin1String("vector") ||
         kind == QLatin1String("file") || kind == QLatin1String("table");
}

} // namespace

QJsonObject planExportsJson(const PlanExports &exports) {
  QJsonArray rows;
  for (const auto &entry : exports.entries)
    rows.append(QJsonObject{{"alias", entry.alias}, {"nodeId", entry.nodeId}, {"portId", entry.portId}});
  return {{"exports", rows}};
}

bool planExportsFromJson(const QJsonObject &graph, PlanExports &exports, QString *code) {
  const auto reject = [code](const QString &why) {
    if (code)
      *code = why;
    return false;
  };
  if (graph.value("version") != QJsonValue(kPlanDescriptorV2))
    return reject(graph.contains("version") ? QStringLiteral("unsupported_plan_version")
                                            : QStringLiteral("missing_plan_version"));
  if (!graph.contains("exports"))
    return reject(QStringLiteral("missing_export_field"));
  if (graph.size() != 4) // nodes/edges/version/exports —— 未知字段一律拒绝
    return reject(QStringLiteral("unknown_plan_field"));
  const auto rows = graph.value("exports").toArray();
  if (rows.isEmpty() || rows.size() > kPlanMaxExports)
    return reject(QStringLiteral("invalid_export_count"));
  QSet<QString> aliases, sources;
  for (const auto &value : rows) {
    const auto row = value.toObject();
    if (row.size() != 3 || !row.contains("alias") || !row.contains("nodeId") || !row.contains("portId"))
      return reject(QStringLiteral("unknown_export_field"));
    PlanExport entry{row.value("alias").toString(), row.value("nodeId").toString(), row.value("portId").toString()};
    if (entry.alias.isEmpty() || entry.nodeId.isEmpty() || entry.portId.isEmpty())
      return reject(QStringLiteral("invalid_export_entry"));
    if (!DataCatalog::isSafePathSegment(entry.alias) || entry.alias.toUtf8().size() > kPlanMaxAliasBytes)
      return reject(QStringLiteral("invalid_export_alias"));
    if (aliases.contains(entry.alias))
      return reject(QStringLiteral("duplicate_export_alias"));
    aliases.insert(entry.alias);
    const auto source = entry.nodeId + QLatin1Char('/') + entry.portId;
    if (sources.contains(source))
      return reject(QStringLiteral("duplicate_export_source"));
    sources.insert(source);
    exports.entries.append(entry);
  }
  return true;
}

ValidatedPlanExports validatePlanV2(const QVector<PlanNode> &nodes, const QVector<PlanEdge> &edges,
                                    const PlanExports &exports) {
  ValidatedPlanExports out;
  out.plan = validatePlanGraph(nodes, edges);
  if (!out.plan.ok) {
    out.issues = out.plan.issues;
    return out;
  }
  out.exports = exports;
  const auto invalid = [&](const QString &code, const QString &field = {}) {
    fail(out, code, field);
    out.ok = false;
    return out;
  };
  if (exports.entries.isEmpty())
    return invalid(QStringLiteral("missing_exports"));
  if (exports.entries.size() > kPlanMaxExports)
    return invalid(QStringLiteral("plan_too_large"));
  QHash<QString, int> byId;
  for (int i = 0; i < out.plan.nodes.size(); ++i)
    byId.insert(out.plan.nodes[i].node.id, i);
  QSet<QString> sources;
  for (const auto &entry : exports.entries) {
    if (entry.alias.isEmpty() || !DataCatalog::isSafePathSegment(entry.alias) ||
        entry.alias.toUtf8().size() > kPlanMaxAliasBytes)
      return invalid(QStringLiteral("invalid_export_alias"), entry.alias);
    if (out.entryByAlias.contains(entry.alias))
      return invalid(QStringLiteral("duplicate_export_alias"), entry.alias);
    const auto found = byId.value(entry.nodeId, -1);
    if (found < 0)
      return invalid(QStringLiteral("unknown_node"), entry.nodeId);
    const auto &node = out.plan.nodes[found].node;
    if (entry.portId != node.outputPort)
      return invalid(QStringLiteral("unselected_output"), entry.portId);
    const auto source = entry.nodeId + QLatin1Char('/') + entry.portId;
    if (sources.contains(source))
      return invalid(QStringLiteral("duplicate_export_source"), entry.alias);
    sources.insert(source);
    for (const auto &port : out.plan.nodes[found].algorithm.outputs)
      if (port.id == entry.portId && !fileKind(port.kind))
        return invalid(QStringLiteral("unsupported_export_kind"), entry.portId);
    out.entryByAlias.insert(entry.alias, out.nodeIndex.size());
    out.nodeIndex.append(found);
  }
  // 导出闭包：每个终端节点（无下游）必须被显式导出（missing_export），每个
  // 执行节点都要对某个导出有贡献（uncontributing_node）。DAG 上两条规则
  // 经由“逆向可达被导出节点”统一判定：未覆盖的终端报 missing_export，
  // 未覆盖的中间节点报 uncontributing_node。
  QSet<int> hasDownstream;
  for (const auto &edge : out.plan.edges)
    hasDownstream.insert(edge.fromNode);
  QVector<int> stack(out.nodeIndex.cbegin(), out.nodeIndex.cend());
  QSet<int> contributing;
  while (!stack.isEmpty()) {
    const int current = stack.takeLast();
    if (contributing.contains(current))
      continue;
    contributing.insert(current);
    for (const auto &edge : out.plan.edges)
      if (edge.toNode == current && !contributing.contains(edge.fromNode))
        stack.append(edge.fromNode);
  }
  for (int i = 0; i < out.plan.nodes.size(); ++i) {
    if (contributing.contains(i))
      continue;
    const auto code = hasDownstream.contains(i) ? QStringLiteral("uncontributing_node")
                                                : QStringLiteral("missing_export");
    return invalid(code, out.plan.nodes[i].node.id);
  }
  out.ok = true;
  return out;
}

AlgorithmSpec compositePlanSpec(const ValidatedPlanExports &validated) {
  AlgorithmSpec spec;
  spec.id = QStringLiteral("workflow.plan");
  spec.displayName = QStringLiteral("Workflow plan (explicit exports)");
  spec.engine = QStringLiteral("native_cpp");
  spec.complete = true;
  spec.contractVersion = 1;
  spec.implementationVersion = QStringLiteral("2");
  for (int i = 0; i < validated.exports.entries.size(); ++i) {
    const auto &entry = validated.exports.entries.at(i);
    const auto &node = validated.plan.nodes.at(validated.nodeIndex.at(i));
    for (const auto &port : node.algorithm.outputs) {
      if (port.id != entry.portId)
        continue;
      AlgoPort composite = port;
      composite.id = entry.alias; // 父端口身份 = 稳定别名；kind/unit/axis/datum/
                                  // geometry/约束原样保留，不因导出改名而丢失。
      composite.required = true;
      composite.hidden = false;
      composite.cardinalityMin = 1;
      composite.cardinalityMax = 1;
      composite.role.clear();
      spec.outputs.append(composite);
      break;
    }
  }
  return spec;
}

} // namespace paleo::capability
