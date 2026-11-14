// 层：功能
// workflow/capabilityplan.cpp — 先验证整图，再允许宿主创建任何子操作。
#include "capabilityplan.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <cmath>
#include <limits>

namespace paleo::capability {
namespace {

constexpr qsizetype DescriptorBytes = 256 * 1024;
constexpr int DescriptorValues = 16384;
constexpr qsizetype ArrayItems = 4096;

bool fail(ValidatedPlan &plan, const QString &code, const QString &field = {}, const QString &message = {}) {
  plan.issues.append({code, field, message});
  return false;
}

const AlgoPort *portById(const QVector<AlgoPort> &ports, const QString &id) {
  for (const auto &port : ports)
    if (port.id == id)
      return &port;
  return nullptr;
}

// Bound expansion before asking QJsonDocument to allocate the encoded descriptor.
bool boundedValue(const QVariant &value, int depth, int &values, qsizetype &bytes) {
  if (depth > 16 || ++values > DescriptorValues)
    return false;
  const int type = value.userType();
  if (!value.isValid() || value.isNull())
    return true;
  if (type == QMetaType::QString) {
    if (value.toString().size() > DescriptorBytes)
      return false;
    bytes += value.toString().toUtf8().size();
  } else if (type == QMetaType::QVariantMap) {
    const auto map = value.toMap();
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
      if (it.key().size() > DescriptorBytes)
        return false;
      bytes += it.key().toUtf8().size();
      if (bytes > DescriptorBytes || !boundedValue(it.value(), depth + 1, values, bytes))
        return false;
    }
  } else if (type == QMetaType::QVariantList || type == QMetaType::QStringList) {
    const auto list = value.toList();
    if (list.size() > ArrayItems)
      return false;
    for (const auto &item : list)
      if (!boundedValue(item, depth + 1, values, bytes))
        return false;
  } else if (type == QMetaType::Double || type == QMetaType::Float) {
    if (!std::isfinite(value.toDouble()))
      return false;
  } else if (type != QMetaType::Bool && type != QMetaType::Int && type != QMetaType::UInt &&
             type != QMetaType::LongLong && type != QMetaType::ULongLong) {
    return false;
  }
  return bytes <= DescriptorBytes;
}

bool descriptorFits(const QVector<PlanNode> &nodes, const QVector<PlanEdge> &edges) {
  QJsonArray nodeValues;
  int values = 0;
  qsizetype bytes = 0;
  for (const auto &node : nodes) {
    QVariantMap object{{QStringLiteral("id"), node.id}, {QStringLiteral("capabilityId"), node.capabilityId},
                       {QStringLiteral("contractVersion"), node.contractVersion},
                       {QStringLiteral("outputPort"), node.outputPort}, {QStringLiteral("intent"), node.intent},
                       {QStringLiteral("parameters"), node.parameters}, {QStringLiteral("inputs"), node.inputs}};
    if (!boundedValue(object, 0, values, bytes))
      return false;
    nodeValues.append(QJsonObject::fromVariantMap(object));
  }
  QJsonArray edgeValues;
  for (const auto &edge : edges) {
    for (const auto &key : {edge.from, edge.fromPort, edge.to, edge.toPort})
      if (key.size() > 256)
        return false;
    edgeValues.append(QJsonArray{edge.from, edge.fromPort, edge.to, edge.toPort});
  }
  return QJsonDocument(QJsonObject{{QStringLiteral("nodes"), nodeValues}, {QStringLiteral("edges"), edgeValues}})
             .toJson(QJsonDocument::Compact).size() <= DescriptorBytes;
}

bool loadNodes(ValidatedPlan &plan, const QVector<PlanNode> &nodes, QHash<QString, int> &byId) {
  plan.nodes.reserve(nodes.size());
  for (const auto &node : nodes) {
    if (node.id.isEmpty())
      return fail(plan, QStringLiteral("empty_node"));
    if (node.id.toUtf8().size() > 256)
      return fail(plan, QStringLiteral("node_id_too_large"), node.id);
    if (byId.contains(node.id))
      return fail(plan, QStringLiteral("duplicate_node"), node.id);
    const auto capability = CapabilityRegistry::spec(node.capabilityId, node.contractVersion);
    if (capability.capabilityId.isEmpty())
      return fail(plan, CapabilityRegistry::contains(node.capabilityId)
                            ? QStringLiteral("unsupported_version") : QStringLiteral("unknown_capability"),
                  node.capabilityId);
    if (capability.exposure != CapabilityExposure::Callable)
      return fail(plan, capability.exposure == CapabilityExposure::Unavailable ? QStringLiteral("unavailable")
                                                                              : QStringLiteral("not_callable"),
                  node.capabilityId);
    const auto algorithm = AlgorithmSpecRegistry::specAtVersion(capability.algorithmId,
                                                               capability.algorithmContractVersion);
    if (algorithm.id.isEmpty() || !algorithm.complete)
      return fail(plan, QStringLiteral("unsupported_algorithm"), capability.algorithmId);
    if (!portById(algorithm.outputs, node.outputPort))
      return fail(plan, QStringLiteral("port_type"), node.outputPort, QStringLiteral("unknown selected output"));
    byId.insert(node.id, plan.nodes.size());
    plan.nodes.append({node, algorithm, {}});
  }
  return true;
}

bool loadEdges(ValidatedPlan &plan, const QVector<PlanEdge> &edges, const QHash<QString, int> &byId) {
  QSet<QPair<int, QString>> assigned;
  for (const auto &edge : edges) {
    if (!byId.contains(edge.from) || !byId.contains(edge.to))
      return fail(plan, QStringLiteral("unknown_node"), !byId.contains(edge.from) ? edge.from : edge.to);
    const int from = byId.value(edge.from);
    const int to = byId.value(edge.to);
    const auto *source = portById(plan.nodes[from].algorithm.outputs, edge.fromPort);
    const auto *target = portById(plan.nodes[to].algorithm.inputs, edge.toPort);
    if (!source || !target)
      return fail(plan, QStringLiteral("port_type"), source ? edge.toPort : edge.fromPort);
    for (const auto &prior : plan.edges)
      if (prior.fromNode == from && prior.toNode == to && prior.fromPort.id == edge.fromPort &&
          prior.toPort.id == edge.toPort)
        return fail(plan, QStringLiteral("duplicate_edge"), edge.toPort);
    const auto slot = qMakePair(to, edge.toPort);
    if (assigned.contains(slot))
      return fail(plan, QStringLiteral("input_already_assigned"), edge.toPort);
    assigned.insert(slot);
    plan.edges.append({from, to, *source, *target, {}});
  }
  return true;
}

bool orderNodes(ValidatedPlan &plan) {
  QVector<int> indegree(plan.nodes.size(), 0);
  for (const auto &edge : plan.edges)
    ++indegree[edge.toNode];
  QStringList ready;
  QHash<QString, int> byId;
  for (int i = 0; i < plan.nodes.size(); ++i) {
    const auto id = plan.nodes[i].node.id;
    byId.insert(id, i);
    if (indegree[i] == 0)
      ready.append(id);
  }
  ready.sort();
  while (!ready.isEmpty()) {
    const auto id = ready.takeFirst();
    plan.order.append(id);
    for (const auto &edge : plan.edges) {
      if (edge.fromNode != byId.value(id) || --indegree[edge.toNode] != 0)
        continue;
      ready.append(plan.nodes[edge.toNode].node.id);
      ready.sort();
    }
  }
  return plan.order.size() == plan.nodes.size() || fail(plan, QStringLiteral("cycle"));
}

bool bindAdapter(ValidatedPlan &plan, ValidatedPlanNode &entry) {
  const auto id = entry.node.capabilityId;
  if (id == QLatin1String("constraint.idw")) {
    if (entry.node.outputPort != QLatin1String("OUTPUT"))
      return fail(plan, QStringLiteral("unsupported_output_shape"), entry.node.outputPort);
    entry.inputKeys = {{QStringLiteral("INPUT"), QStringLiteral("points")},
                       {QStringLiteral("CONSTRAINTS"), QStringLiteral("lines")},
                       {QStringLiteral("LOCAL_GRID_WKT"), QStringLiteral("grid_wkt")}};
  } else if (id == QLatin1String("singlefactor.contours")) {
    entry.inputKeys = {{QStringLiteral("RASTER"), QStringLiteral("raster_path")}};
  } else {
    return fail(plan, QStringLiteral("unsupported_plan_adapter"), id,
                QStringLiteral("no single-artifact request/result bridge"));
  }
  if (!entry.node.intent.isEmpty() && entry.node.intent != QLatin1String("compute-only"))
    return fail(plan, QStringLiteral("unsupported_plan_intent"), entry.node.id);
  entry.node.intent = QStringLiteral("compute-only");
  return true;
}

bool bindEdges(ValidatedPlan &plan) {
  for (auto &edge : plan.edges) {
    const auto &source = plan.nodes[edge.fromNode];
    const auto &target = plan.nodes[edge.toNode];
    if (source.node.outputPort != edge.fromPort.id)
      return fail(plan, QStringLiteral("unselected_output"), edge.fromPort.id);
    const auto issue = checkPortCompatibility(edge.fromPort, edge.toPort);
    if (issue.code != QLatin1String("ok"))
      return fail(plan, issue.code, issue.field);
    edge.inputKey = target.inputKeys.value(edge.toPort.id);
    // The currently implemented result is a file. IDW's points/lines are inline
    // value tables, not vector file readers; matching kind alone is insufficient.
    if (target.node.capabilityId != QLatin1String("singlefactor.contours") ||
        edge.inputKey != QLatin1String("raster_path"))
      return fail(plan, QStringLiteral("unsupported_edge_shape"), edge.toPort.id);
    if (target.node.inputs.contains(edge.inputKey))
      return fail(plan, QStringLiteral("input_already_assigned"), edge.toPort.id);
  }
  return true;
}

bool checkSingleResult(ValidatedPlan &plan) {
  QSet<int> producers;
  for (const auto &edge : plan.edges)
    producers.insert(edge.fromNode);
  if (plan.nodes.size() - producers.size() != 1)
    return fail(plan, QStringLiteral("unsupported_plan_shape"), {},
                QStringLiteral("the executor returns exactly one terminal artifact"));
  return true;
}

bool isNumber(const QVariant &value) {
  const int type = value.userType();
  return type == QMetaType::Int || type == QMetaType::UInt || type == QMetaType::LongLong ||
         type == QMetaType::ULongLong || type == QMetaType::Float || type == QMetaType::Double;
}

bool checkLiteral(ValidatedPlan &plan, const AlgoPort &port, const QVariant &value) {
  if (port.kind == QLatin1String("vector")) {
    if (value.userType() != QMetaType::QVariantList)
      return fail(plan, QStringLiteral("input_type"), port.id);
    const auto rows = value.toList();
    if (rows.isEmpty() && port.required)
      return fail(plan, QStringLiteral("missing_required"), port.id);
    for (const auto &valueRow : rows) {
      if (valueRow.userType() != QMetaType::QVariantMap)
        return fail(plan, QStringLiteral("input_type"), port.id);
      const auto row = valueRow.toMap();
      if (port.geometryHint == QLatin1String("point")) {
        for (const auto &key : {QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("z")})
          if (!isNumber(row.value(key)) || !std::isfinite(row.value(key).toDouble()))
            return fail(plan, QStringLiteral("invalid_point_value"), key);
      } else if (row.value(QStringLiteral("wkt")).userType() != QMetaType::QString ||
                 row.value(QStringLiteral("wkt")).toString().isEmpty()) {
        return fail(plan, QStringLiteral("input_type"), port.id);
      }
    }
  } else if (value.userType() != QMetaType::QString || value.toString().isEmpty()) {
    return fail(plan, QStringLiteral("input_type"), port.id);
  }
  return true;
}

bool checkInputs(ValidatedPlan &plan, int index) {
  auto &entry = plan.nodes[index];
  QSet<QString> known;
  for (auto it = entry.inputKeys.constBegin(); it != entry.inputKeys.constEnd(); ++it)
    known.insert(it.value());
  if (entry.node.capabilityId == QLatin1String("constraint.idw")) {
    for (const auto &key : {QStringLiteral("points_ref"), QStringLiteral("constraints_ref"),
                            QStringLiteral("subinterval_id")})
      known.insert(key);
    const auto subinterval = entry.node.inputs.value(QStringLiteral("subinterval_id"));
    if (subinterval.userType() != QMetaType::QString || subinterval.toString().isEmpty())
      return fail(plan, QStringLiteral("missing_required"), QStringLiteral("subinterval_id"));
    for (const auto &key : {QStringLiteral("points_ref"), QStringLiteral("constraints_ref")}) {
      if (entry.node.inputs.contains(key) && entry.node.inputs.value(key).userType() != QMetaType::QString)
        return fail(plan, QStringLiteral("input_type"), key);
    }
  }
  for (auto it = entry.node.inputs.constBegin(); it != entry.node.inputs.constEnd(); ++it)
    if (!known.contains(it.key()))
      return fail(plan, QStringLiteral("unknown_input"), it.key());
  for (const auto &port : entry.algorithm.inputs) {
    const auto key = entry.inputKeys.value(port.id);
    bool suppliedByEdge = false;
    for (const auto &edge : plan.edges)
      suppliedByEdge = suppliedByEdge || (edge.toNode == index && edge.toPort.id == port.id);
    const bool literal = !key.isEmpty() && entry.node.inputs.contains(key);
    if (suppliedByEdge && literal)
      return fail(plan, QStringLiteral("input_already_assigned"), port.id);
    if (!literal && !suppliedByEdge && port.effectiveCardinalityMin() > 0)
      return fail(plan, QStringLiteral("missing_required"), port.id);
    if (port.cardinalityMax != 1 || port.effectiveCardinalityMin() > 1)
      return fail(plan, QStringLiteral("unsupported_input_cardinality"), port.id);
    if (literal && !checkLiteral(plan, port, entry.node.inputs.value(key)))
      return false;
  }
  return true;
}

bool checkParameters(ValidatedPlan &plan, ValidatedPlanNode &entry) {
  for (const auto &field : entry.algorithm.params) {
    if (!entry.node.parameters.contains(field.key))
      continue;
    const auto value = entry.node.parameters.value(field.key);
    if (field.type == AlgorithmParamField::String && value.userType() != QMetaType::QString)
      return fail(plan, QStringLiteral("type_mismatch"), field.key);
    if (field.type == AlgorithmParamField::Int) {
      const double number = value.toDouble();
      if (!isNumber(value) || std::floor(number) != number || number < std::numeric_limits<int>::min() ||
          number > std::numeric_limits<int>::max())
        return fail(plan, QStringLiteral("type_mismatch"), field.key);
    }
  }
  const auto parameters = validateAndFillParams(entry.algorithm.id, entry.algorithm.contractVersion,
                                               entry.node.parameters);
  if (!parameters.ok) {
    plan.issues += parameters.issues;
    return false;
  }
  const auto keys = entry.algorithm.paramKeys();
  for (auto it = entry.node.parameters.constBegin(); it != entry.node.parameters.constEnd(); ++it)
    if (!keys.contains(it.key()))
      return fail(plan, QStringLiteral("unknown_param"), it.key());
  entry.node.parameters = parameters.effective;
  return true;
}

} // namespace

ValidatedPlan validatePlan(const QVector<PlanNode> &nodes, const QVector<PlanEdge> &edges) {
  ValidatedPlan plan;
  if (nodes.isEmpty()) {
    fail(plan, QStringLiteral("empty_plan"));
    return plan;
  }
  if (nodes.size() > 32 || edges.size() > 64 || !descriptorFits(nodes, edges)) {
    fail(plan, QStringLiteral("plan_too_large"));
    return plan;
  }
  QHash<QString, int> byId;
  if (!loadNodes(plan, nodes, byId) || !loadEdges(plan, edges, byId) || !orderNodes(plan))
    return plan;
  for (auto &entry : plan.nodes)
    if (!bindAdapter(plan, entry))
      return plan;
  if (!bindEdges(plan))
    return plan;
  if (!checkSingleResult(plan))
    return plan;
  for (int i = 0; i < plan.nodes.size(); ++i)
    if (!checkParameters(plan, plan.nodes[i]) || !checkInputs(plan, i))
      return plan;
  plan.ok = true;
  return plan;
}

ContractIssue checkPlan(const QVector<PlanNode> &nodes, const QVector<PlanEdge> &edges) {
  const auto plan = validatePlan(nodes, edges);
  return plan.ok ? ContractIssue{QStringLiteral("ok"), {}, {}} : plan.issues.first();
}

} // namespace paleo::capability
