// 层：功能
#include "aiplanpresentation.h"
#include "aichatcontroller.h"
#include "capabilityhost.h"
#include "capabilityrecovery.h"
#include "documentsession.h"
#include "../ai/chat/chatmessage.h"
#include "../ai/chat/planpreviewtool.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <limits>

namespace paleo::chat {
namespace {

// 严格值游走：深度/元素/字符串/键数预算先于容器扩张；数值必须有限；
// 只接受 string/number/bool/array/object，null/undefined 一律拒绝。
bool strictValue(const QJsonValue &value, int depth, int &values,
                 QString *code, QString *field, const QString &path) {
  if (depth > kPlanMaxDepth) {
    *code = QStringLiteral("plan_value_too_deep");
    *field = path;
    return false;
  }
  if (++values > kPlanMaxArrayItems * kPlanMaxNodes) {
    *code = QStringLiteral("plan_value_budget_exceeded");
    *field = path;
    return false;
  }
  if (value.isString()) {
    if (value.toString().size() > kPlanMaxStringValue) {
      *code = QStringLiteral("plan_string_too_large");
      *field = path;
      return false;
    }
    return true;
  }
  if (value.isDouble())
    return std::isfinite(value.toDouble()) ? true
        : (*code = QStringLiteral("plan_non_finite_number"), *field = path, false);
  if (value.isBool())
    return true;
  if (value.isArray()) {
    const auto array = value.toArray();
    if (array.size() > kPlanMaxArrayItems) {
      *code = QStringLiteral("plan_array_too_large");
      *field = path;
      return false;
    }
    for (int i = 0; i < array.size(); ++i)
      if (!strictValue(array.at(i), depth + 1, values, code, field,
                       QStringLiteral("%1.%2").arg(path).arg(i)))
        return false;
    return true;
  }
  if (value.isObject()) {
    const auto object = value.toObject();
    if (object.size() > kPlanMaxObjectKeys) {
      *code = QStringLiteral("plan_object_too_large");
      *field = path;
      return false;
    }
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
      if (it.key().size() > 64) {
        *code = QStringLiteral("plan_key_too_large");
        *field = path;
        return false;
      }
      if (!strictValue(it.value(), depth + 1, values, code, field,
                       QStringLiteral("%1.%2").arg(path, it.key())))
        return false;
    }
    return true;
  }
  *code = QStringLiteral("plan_value_type_rejected"); // null/undefined/原始二进制
  *field = path;
  return false;
}

// 封闭词表的字符串字段读取：类型必须严格匹配；空与超长按调用方语义判定。
bool stringField(const QJsonObject &object, const QString &key, int maxChars,
                 QString *out, QString *code, QString *field, const QString &path,
                 bool allowEmpty) {
  const auto value = object.value(key);
  if (value.isUndefined())
    return true; // 缺省不是解码错误（业务校验按必填判）
  if (!value.isString()) {
    *code = QStringLiteral("plan_field_type");
    *field = QStringLiteral("%1.%2").arg(path, key);
    return false;
  }
  const auto text = value.toString();
  if ((!allowEmpty && text.isEmpty()) || text.size() > maxChars) {
    *code = text.isEmpty() ? QStringLiteral("plan_field_empty")
                           : QStringLiteral("plan_string_too_large");
    *field = QStringLiteral("%1.%2").arg(path, key);
    return false;
  }
  *out = text;
  return true;
}

bool decodeNodesEdges(const QJsonArray &nodeArray, const QJsonArray &edgeArray,
                      QVector<paleo::capability::PlanNode> &nodes,
                      QVector<paleo::capability::PlanEdge> &edges,
                      QString *code, QString *field) {
  const auto fail = [&](const QString &what, const QString &where) {
    *code = what;
    *field = where;
    return false;
  };
  // 预算先于容器扩张（W1.2）：节点/边数量在分配 QVector 前判。
  if (nodeArray.size() < 1)
    return fail(QStringLiteral("plan_nodes_required"), QStringLiteral("nodes"));
  if (nodeArray.size() > kPlanMaxNodes || edgeArray.size() > kPlanMaxEdges)
    return fail(QStringLiteral("plan_too_large"),
                nodeArray.size() > kPlanMaxNodes ? QStringLiteral("nodes")
                                                 : QStringLiteral("edges"));
  int values = 0;
  nodes.reserve(int(nodeArray.size()));
  for (int i = 0; i < nodeArray.size(); ++i) {
    const auto entry = nodeArray.at(i);
    const QString path = QStringLiteral("nodes.%1").arg(i);
    if (!entry.isObject())
      return fail(QStringLiteral("plan_node_type"), path);
    const auto object = entry.toObject();
    // 封闭词表：未声明的字段（含 approved/caller/documentId/路径等一切
    // 授权语义字段）在解码层即被拒。
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
      if (it.key() != QLatin1String("id") && it.key() != QLatin1String("capability") &&
          it.key() != QLatin1String("contract_version") && it.key() != QLatin1String("output_port") &&
          it.key() != QLatin1String("intent") && it.key() != QLatin1String("parameters") &&
          it.key() != QLatin1String("inputs"))
        return fail(QStringLiteral("unknown_field"), path + QLatin1Char('.') + it.key());
    paleo::capability::PlanNode node;
    if (!stringField(object, QStringLiteral("id"), 256, &node.id, code, field, path, false))
      return false;
    if (!stringField(object, QStringLiteral("capability"), 256, &node.capabilityId, code, field,
                     path, false))
      return false;
    const auto version = object.value(QStringLiteral("contract_version"));
    if (!version.isUndefined()) {
      if (!version.isDouble() || !std::isfinite(version.toDouble()) ||
          std::floor(version.toDouble()) != version.toDouble() ||
          version.toDouble() < 1 || version.toDouble() > 64) {
        *code = QStringLiteral("plan_field_type");
        *field = path + QStringLiteral(".contract_version");
        return false;
      }
      node.contractVersion = int(version.toDouble());
    }
    if (!stringField(object, QStringLiteral("output_port"), 256, &node.outputPort, code, field,
                     path, true))
      return false;
    if (!stringField(object, QStringLiteral("intent"), 64, &node.intent, code, field, path,
                     true))
      return false;
    const auto parameters = object.value(QStringLiteral("parameters"));
    if (!parameters.isUndefined()) {
      if (!parameters.isObject()) {
        *code = QStringLiteral("plan_field_type");
        *field = path + QStringLiteral(".parameters");
        return false;
      }
      if (!strictValue(parameters, 1, values, code, field,
                       path + QStringLiteral(".parameters")))
        return false;
      node.parameters = parameters.toObject().toVariantMap();
    }
    const auto inputs = object.value(QStringLiteral("inputs"));
    if (!inputs.isUndefined()) {
      if (!inputs.isObject()) {
        *code = QStringLiteral("plan_field_type");
        *field = path + QStringLiteral(".inputs");
        return false;
      }
      if (!strictValue(inputs, 1, values, code, field, path + QStringLiteral(".inputs")))
        return false;
      node.inputs = inputs.toObject().toVariantMap();
    }
    nodes.append(std::move(node));
  }
  edges.reserve(int(edgeArray.size()));
  for (int i = 0; i < edgeArray.size(); ++i) {
    const auto entry = edgeArray.at(i);
    const QString path = QStringLiteral("edges.%1").arg(i);
    if (!entry.isObject())
      return fail(QStringLiteral("plan_edge_type"), path);
    const auto object = entry.toObject();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
      if (it.key() != QLatin1String("from") && it.key() != QLatin1String("from_port") &&
          it.key() != QLatin1String("to") && it.key() != QLatin1String("to_port"))
        return fail(QStringLiteral("unknown_field"), path + QLatin1Char('.') + it.key());
    paleo::capability::PlanEdge edge;
    for (const auto &key : {QStringLiteral("from"), QStringLiteral("from_port"),
                            QStringLiteral("to"), QStringLiteral("to_port")}) {
      QString *target = key == QLatin1String("from") ? &edge.from
          : key == QLatin1String("from_port") ? &edge.fromPort
          : key == QLatin1String("to") ? &edge.to : &edge.toPort;
      if (!stringField(object, key, 256, target, code, field, path, false))
        return false;
    }
    edges.append(std::move(edge));
  }
  return true;
}

QJsonObject normalizedNodeJson(const paleo::capability::PlanNode &node) {
  return {{QStringLiteral("id"), node.id},
          {QStringLiteral("capability"), node.capabilityId},
          {QStringLiteral("contract_version"), node.contractVersion},
          {QStringLiteral("output_port"), node.outputPort},
          {QStringLiteral("intent"), node.intent.isEmpty()
              ? QStringLiteral("compute-only") : node.intent},
          {QStringLiteral("parameters"), QJsonObject::fromVariantMap(node.parameters)},
          {QStringLiteral("inputs"), QJsonObject::fromVariantMap(node.inputs)}};
}

QVector<QJsonValue> jsonValues(const QJsonArray &array) {
  QVector<QJsonValue> values;
  values.reserve(int(array.size()));
  for (const auto &value : array)
    values.append(value);
  return values;
}

QJsonArray sortedNodeArray(const QJsonObject &graph) {
  // QJsonArray 迭代器解引用为 QJsonValueRef，无可行 swap——拷贝到
  // QVector 排序后重建（节点数 ≤32，成本可忽略）。
  auto values = jsonValues(graph.value(QStringLiteral("nodes")).toArray());
  std::stable_sort(values.begin(), values.end(),
                   [](const QJsonValue &a, const QJsonValue &b) {
                     return a.toObject().value(QStringLiteral("id")).toString() <
                            b.toObject().value(QStringLiteral("id")).toString();
                   });
  QJsonArray sorted;
  for (const auto &value : values)
    sorted.append(value);
  return sorted;
}

QJsonArray sortedEdgeArray(const QJsonObject &graph) {
  auto values = jsonValues(graph.value(QStringLiteral("edges")).toArray());
  const auto key = [](const QJsonValue &value) {
    const auto edge = value.toObject();
    return QStringList{edge.value(QStringLiteral("from")).toString(),
                       edge.value(QStringLiteral("from_port")).toString(),
                       edge.value(QStringLiteral("to")).toString(),
                       edge.value(QStringLiteral("to_port")).toString()}.join(
        QChar::fromLatin1('|'));
  };
  std::stable_sort(values.begin(), values.end(),
                   [&key](const QJsonValue &a, const QJsonValue &b) { return key(a) < key(b); });
  QJsonArray sorted;
  for (const auto &value : values)
    sorted.append(value);
  return sorted;
}

QJsonObject factsFromResult(const paleo::capability::ExecuteResult &result) {
  QJsonObject facts{{QStringLiteral("ok"), result.ok},
                    {QStringLiteral("code"), result.code},
                    {QStringLiteral("operationId"), result.operationId},
                    {QStringLiteral("execution"), result.execution},
                    {QStringLiteral("commitState"), result.commitState},
                    {QStringLiteral("presentation"), result.presentation},
                    {QStringLiteral("requestHash"), result.requestHash}};
  const auto summary = QJsonObject::fromVariantMap(result.summary);
  if (!summary.isEmpty())
    facts.insert(QStringLiteral("summary"), summary);
  return facts;
}

QString busyKey(const QString &draftId, const QString &action) {
  return draftId + QLatin1Char(':') + action;
}

} // namespace

PlanPreviewDecode decodePlanPreviewArguments(const QJsonObject &arguments) {
  PlanPreviewDecode out;
  QString code, field;
  for (auto it = arguments.constBegin(); it != arguments.constEnd(); ++it)
    if (it.key() != QLatin1String("title") && it.key() != QLatin1String("note") &&
        it.key() != QLatin1String("nodes") && it.key() != QLatin1String("edges")) {
      out.code = QStringLiteral("unknown_field");
      out.field = it.key();
      return out;
    }
  QString title, note;
  if (!stringField(arguments, QStringLiteral("title"), 120, &title, &code, &field,
                   QString(), true) ||
      !stringField(arguments, QStringLiteral("note"), 500, &note, &code, &field,
                   QString(), true)) {
    out.code = code;
    out.field = field;
    return out;
  }
  const auto nodes = arguments.value(QStringLiteral("nodes"));
  const auto edges = arguments.value(QStringLiteral("edges"));
  if (!nodes.isArray() || !edges.isArray()) {
    out.code = QStringLiteral("plan_field_type");
    out.field = nodes.isArray() ? QStringLiteral("edges") : QStringLiteral("nodes");
    return out;
  }
  QVector<paleo::capability::PlanNode> decodedNodes;
  QVector<paleo::capability::PlanEdge> decodedEdges;
  if (!decodeNodesEdges(nodes.toArray(), edges.toArray(), decodedNodes, decodedEdges,
                        &code, &field)) {
    out.code = code;
    out.field = field;
    return out;
  }
  out.ok = true;
  out.nodes = decodedNodes;
  out.edges = decodedEdges;
  out.title = title;
  out.note = note;
  return out;
}

PlanPreviewDecode decodePlanPreviewGraph(const QJsonObject &graph) {
  PlanPreviewDecode out;
  QString code, field;
  const auto nodes = graph.value(QStringLiteral("nodes"));
  const auto edges = graph.value(QStringLiteral("edges"));
  if (!nodes.isArray() || !edges.isArray()) {
    out.code = QStringLiteral("plan_field_type");
    out.field = nodes.isArray() ? QStringLiteral("edges") : QStringLiteral("nodes");
    return out;
  }
  QVector<paleo::capability::PlanNode> decodedNodes;
  QVector<paleo::capability::PlanEdge> decodedEdges;
  if (!decodeNodesEdges(nodes.toArray(), edges.toArray(), decodedNodes, decodedEdges,
                        &code, &field)) {
    out.code = code;
    out.field = field;
    return out;
  }
  out.ok = true;
  out.nodes = decodedNodes;
  out.edges = decodedEdges;
  return out;
}

QJsonObject planPreviewPayload(const paleo::capability::ValidatedPlan &plan,
                               const QVector<paleo::capability::PlanNode> &nodes,
                               const QVector<paleo::capability::PlanEdge> &edges,
                               const QString &title, const QString &note) {
  QJsonObject payload{{QStringLiteral("contract"), QLatin1String(kPlanPreviewContract)},
                      {QStringLiteral("tool"), QLatin1String(kPlanPreviewTool)},
                      {QStringLiteral("ok"), plan.ok},
                      {QStringLiteral("status"), QLatin1String(kPlanPreviewNotExecuted)}};
  if (!title.isEmpty())
    payload.insert(QStringLiteral("title"), title);
  if (!note.isEmpty())
    payload.insert(QStringLiteral("note"), note);
  // 规范化图回显：预检通过时用验证器归一结果（含 effective 参数），
  // 失败时回显解码归一值（参数为原样提议值）。
  QJsonArray nodeValues;
  QJsonObject effective;
  if (plan.ok) {
    for (const auto &entry : plan.nodes) {
      nodeValues.append(normalizedNodeJson(entry.node));
      effective.insert(entry.node.id, QJsonObject::fromVariantMap(entry.node.parameters));
    }
    payload.insert(QStringLiteral("effective_parameters"), effective);
    QJsonArray order;
    for (const auto &id : plan.order)
      order.append(id);
    payload.insert(QStringLiteral("order"), order);
    payload.insert(QStringLiteral("terminal"), plan.order.isEmpty()
        ? QString() : plan.order.last());
  } else {
    for (const auto &node : nodes)
      nodeValues.append(normalizedNodeJson(node));
  }
  QJsonArray edgeValues;
  for (const auto &edge : edges)
    edgeValues.append(QJsonObject{{QStringLiteral("from"), edge.from},
                                  {QStringLiteral("from_port"), edge.fromPort},
                                  {QStringLiteral("to"), edge.to},
                                  {QStringLiteral("to_port"), edge.toPort}});
  const QJsonObject graph{{QStringLiteral("nodes"), nodeValues},
                          {QStringLiteral("edges"), edgeValues}};
  payload.insert(QStringLiteral("graph"), graph);
  payload.insert(QStringLiteral("digest"), planGraphDigest(graph));
  if (!plan.issues.isEmpty()) {
    QJsonArray issues;
    for (const auto &issue : plan.issues) {
      QJsonObject entry{{QStringLiteral("code"), issue.code}};
      if (!issue.field.isEmpty())
        entry.insert(QStringLiteral("field"), issue.field);
      if (!issue.message.isEmpty())
        entry.insert(QStringLiteral("message"), issue.message);
      issues.append(entry);
    }
    payload.insert(QStringLiteral("issues"), issues);
  }
  return payload;
}

QString planGraphDigest(const QJsonObject &normalizedGraph) {
  const QJsonObject canonical{{QStringLiteral("nodes"), sortedNodeArray(normalizedGraph)},
                              {QStringLiteral("edges"), sortedEdgeArray(normalizedGraph)}};
  const auto bytes = QJsonDocument(canonical).toJson(QJsonDocument::Compact);
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString planExecutionRequestId(const QString &documentId, const QString &graphDigest) {
  const auto bytes = QStringLiteral("paleo.plan-exec.v1|%1|%2").arg(documentId, graphDigest)
                         .toUtf8();
  return QStringLiteral("plan:") +
         QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

// ---- 适配器 ----

AiPlanPresentation::AiPlanPresentation(const Deps &deps, QObject *parent)
  : QObject(parent), m_deps(deps) {
  const auto invalidate = [this] {
    ++m_generation;
    // 换工程/换会话/宿主销毁：既有草案的可执行资格失效，但草案内容与
    // 已发生的操作事实保留（W5.3：失效不是删除用户输入）。
    invalidateEligibility(QStringLiteral("context_changed"));
    emit contextChanged();
  };
  if (m_deps.document)
    connect(m_deps.document, &paleo::document::DocumentSession::sessionChanged, this, invalidate);
  if (m_deps.chat)
    connect(m_deps.chat, &AiChatController::sessionChanged, this, invalidate);
  if (m_deps.host)
    connect(m_deps.host, &QObject::destroyed, this, invalidate);
}

void AiPlanPresentation::invalidateEligibility(const QString &reason) {
  for (auto it = m_drafts.begin(); it != m_drafts.end(); ++it)
    if (it->executionEligible) {
      it->executionEligible = false;
      it->ineligibleReason = reason;
      emit draftChanged(it->id);
    }
}

AiPlanPresentation::Draft *AiPlanPresentation::findDraft(const QString &draftId) {
  const auto it = m_drafts.find(draftId);
  return it == m_drafts.end() ? nullptr : &it.value();
}

const AiPlanPresentation::Draft *AiPlanPresentation::findDraft(const QString &draftId) const {
  return const_cast<AiPlanPresentation *>(this)->findDraft(draftId);
}

QString AiPlanPresentation::guardError(const Guard &guard) const {
  if (QThread::currentThread() != thread())
    return QStringLiteral("owner_thread_required");
  if (!m_deps.chat || !m_deps.host || guard.generation != m_generation ||
      m_deps.chat->session().id != guard.chatSessionId)
    return QStringLiteral("chat_context_changed");
  return paleo::host::checkContext(m_deps.host->context(), guard.capturedContext,
                                   paleo::host::Read);
}

bool AiPlanPresentation::hostReady(QString *code) const {
  if (!m_deps.chat) {
    if (code) *code = QStringLiteral("chat_controller_unavailable");
    return false;
  }
  if (m_deps.chat->session().id.isEmpty()) {
    if (code) *code = QStringLiteral("chat_session_unavailable");
    return false;
  }
  if (!m_deps.host || !m_deps.host->recovery()) {
    if (code) *code = QStringLiteral("plan_host_or_session_unavailable");
    return false;
  }
  const auto context = m_deps.host->context();
  if (!context.open) {
    if (code) *code = QStringLiteral("document_unavailable");
    return false;
  }
  return true;
}

bool AiPlanPresentation::beginGuarded(const QString &draftId, const QString &action,
                                      Guard &guard, QString *reason) {
  if (!hostReady(reason))
    return false;
  guard.chatSessionId = m_deps.chat->session().id;
  if (busy(draftId, action)) {
    if (reason) *reason = QStringLiteral("request_already_busy");
    return false;
  }
  guard.generation = m_generation;
  guard.capturedContext = m_deps.host->context();
  const QPointer<AiPlanPresentation> self(this);
  m_busy.insert(busyKey(draftId, action));
  emit busyChanged(draftId, action, true);
  if (!self)
    return false;
  const auto gate = guardError(guard);
  if (!gate.isEmpty()) {
    if (reason) *reason = gate;
    releaseBusy(draftId, action);
    return false;
  }
  return true;
}

void AiPlanPresentation::releaseBusy(const QString &draftId, const QString &action) {
  if (m_busy.remove(busyKey(draftId, action)))
    emit busyChanged(draftId, action, false);
}

bool AiPlanPresentation::busy(const QString &draftId, const QString &action) const {
  return m_busy.contains(busyKey(draftId, action));
}

QString AiPlanPresentation::openProposal(const QJsonObject &payload,
                                         const QString &sourceToolCallId, QString *reason) {
  if (payload.value(QStringLiteral("contract")).toString() != QLatin1String(kPlanPreviewContract)) {
    if (reason) *reason = QStringLiteral("plan_contract_mismatch");
    return {};
  }
  const auto graph = payload.value(QStringLiteral("graph")).toObject();
  if (graph.value(QStringLiteral("nodes")).toArray().isEmpty()) {
    // 缺完整图（截断 envelope/纯错误帧）：只读摘要路径，不拼凑可执行草案。
    if (reason) *reason = QStringLiteral("plan_graph_unavailable");
    return {};
  }
  const auto digest = payload.value(QStringLiteral("digest")).toString();
  if (!digest.isEmpty() && digest != planGraphDigest(graph)) {
    if (reason) *reason = QStringLiteral("plan_digest_mismatch");
    return {};
  }
  for (auto it = m_drafts.begin(); it != m_drafts.end(); ++it)
    if (it->sourceToolCallId == sourceToolCallId) {
      // 同一提案来源重开：保留草案（含用户修订），只刷新原始载荷。
      it->originPayload = payload;
      it->originGraph = graph;
      emit draftChanged(it->id);
      return it->id;
    }
  Draft draft;
  draft.id = QStringLiteral("plan-%1-%2").arg(QString::number(++m_draftSequence),
                                              digest.left(12));
  draft.sourceToolCallId = sourceToolCallId;
  draft.originPayload = payload;
  draft.originGraph = graph;
  draft.currentGraph = graph;
  draft.title = payload.value(QStringLiteral("title")).toString();
  draft.note = payload.value(QStringLiteral("note")).toString();
  draft.lastPayload = payload;
  draft.preflightDone = true;
  draft.preflightValid = payload.value(QStringLiteral("ok")).toBool();
  // 打开不绑定执行资格：计算前必须在当前上下文重新预检（W5.2/W5.3）。
  draft.executionEligible = false;
  draft.ineligibleReason = QStringLiteral("preflight_required");
  const QString id = draft.id;
  m_draftOrder.append(id);
  m_drafts.insert(id, std::move(draft));
  emit draftChanged(id);
  return id;
}

AiPlanPresentation::DraftView AiPlanPresentation::draft(const QString &draftId) const {
  DraftView view;
  const auto *draft = findDraft(draftId);
  if (!draft)
    return view; // 未知 id：draftId 留空，调用方守卫可判别
  view.draftId = draftId;
  view.sourceToolCallId = draft->sourceToolCallId;
  view.revision = draft->revision;
  view.preflightValid = draft->preflightValid;
  view.preflightDone = draft->preflightDone;
  view.lastPayload = draft->lastPayload;
  view.originGraph = draft->originGraph;
  view.currentGraph = draft->currentGraph;
  view.title = draft->title;
  view.note = draft->note;
  view.executionEligible = draft->executionEligible;
  view.ineligibleReason = draft->ineligibleReason;
  view.clientRequestId = draft->clientRequestId;
  if (view.clientRequestId.isEmpty() && !draft->graphDigest.isEmpty() &&
      !draft->boundContext.documentId.isEmpty())
    view.clientRequestId = planExecutionRequestId(draft->boundContext.documentId,
                                                  draft->graphDigest);
  view.operationId = draft->operationId;
  view.execution = draft->execution;
  view.commitState = draft->commitState;
  view.presentation = draft->presentation;
  view.requestHash = draft->requestHash;
  view.acceptedNote = draft->acceptedNote;
  return view;
}

QStringList AiPlanPresentation::draftIds() const { return m_draftOrder; }

bool AiPlanPresentation::updateDraftGraph(const QString &draftId, const QJsonObject &graph,
                                          QString *reason) {
  auto *draft = findDraft(draftId);
  if (!draft) {
    if (reason) *reason = QStringLiteral("plan_draft_unknown");
    return false;
  }
  const auto decoded = decodePlanPreviewGraph(graph);
  if (!decoded.ok) {
    if (reason) {
      *reason = decoded.code;
    }
    return false;
  }
  // 有界编辑预算：与模型工具参数同一 16 KiB 量级（不放大既有预算）。
  if (QJsonDocument(graph).toJson(QJsonDocument::Compact).size() >
      ChatToolCall::kMaxArgumentBytes) {
    if (reason) *reason = QStringLiteral("plan_edit_budget_exceeded");
    return false;
  }
  draft->currentGraph = graph;
  ++draft->revision;
  draft->preflightDone = false; // 旧预检不再描述当前修订
  if (draft->executionEligible) {
    draft->executionEligible = false;
    draft->ineligibleReason = QStringLiteral("graph_edited");
  }
  emit draftChanged(draftId);
  return true;
}

bool AiPlanPresentation::requestPreflight(const QString &draftId, QString *reason) {
  auto *draft = findDraft(draftId);
  if (!draft) {
    if (reason) *reason = QStringLiteral("plan_draft_unknown");
    return false;
  }
  Guard guard;
  const QString action = QStringLiteral("preflight");
  if (!beginGuarded(draftId, action, guard, reason))
    return false;
  const auto release = [this, draftId, action] { releaseBusy(draftId, action); };
  // 门通过后再解码/校验（纯值，无 I/O）；失败保留草案与准确错误。
  const auto decoded = decodePlanPreviewGraph(draft->currentGraph);
  if (!decoded.ok) {
    if (reason) *reason = decoded.code;
    release();
    return false;
  }
  const auto plan = paleo::capability::validatePlan(decoded.nodes, decoded.edges);
  const auto payload = planPreviewPayload(plan, decoded.nodes, decoded.edges,
                                          draft->title, draft->note);
  draft->lastPayload = payload;
  draft->preflightDone = true;
  draft->preflightValid = plan.ok;
  draft->graphDigest = payload.value(QStringLiteral("digest")).toString();
  // 显式绑定当前上下文（W5.3）：预检即采纳。
  draft->chatSessionId = guard.chatSessionId;
  draft->boundContext = guard.capturedContext;
  draft->executionEligible = plan.ok;
  draft->ineligibleReason = plan.ok ? QString() : QStringLiteral("preflight_failed");
  emit preflightFinished(draftId, payload);
  emit draftChanged(draftId);
  release();
  return true;
}

bool AiPlanPresentation::computeAllowed(const QString &draftId, QString *reason) const {
  const auto *draft = findDraft(draftId);
  if (!draft) {
    if (reason) *reason = QStringLiteral("plan_draft_unknown");
    return false;
  }
  if (!draft->preflightDone || !draft->preflightValid) {
    if (reason) *reason = draft->preflightDone ? QStringLiteral("preflight_failed")
                                               : QStringLiteral("preflight_required");
    return false;
  }
  if (!draft->executionEligible) {
    if (reason) *reason = draft->ineligibleReason.isEmpty()
        ? QStringLiteral("context_changed") : draft->ineligibleReason;
    return false;
  }
  QString code;
  if (!hostReady(&code)) {
    if (reason) *reason = code;
    return false;
  }
  if (busy(draftId, QStringLiteral("compute"))) {
    if (reason) *reason = QStringLiteral("request_already_busy");
    return false;
  }
  return true;
}

bool AiPlanPresentation::requestCompute(const QString &draftId, QString *reason) {
  using namespace paleo::capability;
  if (!computeAllowed(draftId, reason))
    return false;
  auto *draft = findDraft(draftId);
  // W5.2：门比对的是「预检采纳时绑定的上下文」与 live——不是动作时刻的
  // 现值自比（否则切工程后旧资格静默换绑到新文档）。
  Guard guard;
  guard.chatSessionId = draft->chatSessionId;
  guard.capturedContext = draft->boundContext;
  guard.generation = m_generation;
  const QString action = QStringLiteral("compute");
  m_busy.insert(busyKey(draftId, action));
  emit busyChanged(draftId, action, true);
  const auto finishBusy = [this, draftId, action] { releaseBusy(draftId, action); };
  const auto gate = guardError(guard);
  if (!gate.isEmpty()) {
    if (reason) *reason = gate;
    finishBusy();
    return false;
  }
  // 会话必须在预检后未变且仍非空（guardError 已核）；documentId 以宿主 live 值为准。
  const auto documentId = m_deps.host->context().documentId;
  // 重新解码当前修订并整图预检（冻结图来自本次结果；不信任 UI 缓存值）。
  const auto decoded = decodePlanPreviewGraph(draft->currentGraph);
  if (!decoded.ok) {
    if (reason) *reason = decoded.code;
    finishBusy();
    return false;
  }
  const auto plan = validatePlan(decoded.nodes, decoded.edges);
  if (!plan.ok) {
    if (reason) *reason = plan.issues.isEmpty()
        ? QStringLiteral("preflight_failed") : plan.issues.first().code;
    finishBusy();
    return false;
  }
  ExecuteRequest request;
  request.publishIntent = QStringLiteral("compute-only");
  request.documentId = documentId;
  request.clientRequestId = planExecutionRequestId(documentId, draft->graphDigest);
  draft->clientRequestId = request.clientRequestId;
  ++m_prepareAttempts;
  const auto prepared = m_deps.host->preparePlan(request, decoded.nodes, decoded.edges,
                                                 paleo::host::Caller::Agent,
                                                 guard.chatSessionId);
  if (!prepared.ok) {
    // 真实错误码（idempotency_conflict/prepared_capacity_exceeded/
    // prepared_expired/effect 门等）原样上抛；草案保留，不自动换 ID 重试。
    if (reason) *reason = prepared.code;
    finishBusy();
    return false;
  }
  // 同一已采纳草案的重复请求：preparePlan 幂等重放会发 code=replay 的
  // ticket；start 走既有恢复链，不重复受理。
  const quint64 execGeneration = ++draft->execGeneration;
  const QPointer<AiPlanPresentation> self(this);
  ++m_startAttempts;
  QString error;
  auto *task = m_deps.host->start(prepared.handle, request, this,
                                  tr("计算计划（私有成果）"),
                                  [this, self, execGeneration, draftId,
                                   finishBusy](ExecuteResult result) {
                                    if (!self)
                                      return;
                                    // 晚结果只更新其对应草案/请求（W5.5）；已
                                    // 受理的操作事实不因显示失败被改写。
                                    auto *draft = findDraft(draftId);
                                    if (!draft || draft->execGeneration != execGeneration)
                                      return;
                                    const auto facts = factsFromResult(result);
                                    draft->operationId = result.operationId;
                                    draft->execution = result.execution;
                                    draft->commitState = result.commitState;
                                    draft->presentation = result.presentation;
                                    draft->requestHash = result.requestHash;
                                    draft->acceptedNote.clear();
                                    emit computeFinished(draftId, facts);
                                    emit draftChanged(draftId);
                                    finishBusy();
                                  },
                                  &error);
  if (!task) {
    // W6.2：Host::start 先耐久 accept 再请求调度——空 task/启动错误不能推断
    // 「从未受理」。作为本次显式执行动作的结果处理，沿原请求身份 lookup
    // 确认事实（不是历史卡自动查询）。
    const auto scope = m_deps.host->callerScope(paleo::host::Caller::Agent,
                                                guard.chatSessionId);
    const auto prior = m_deps.host->recovery()->lookup(scope, request.clientRequestId);
    if (prior.code == QLatin1String("found")) {
      draft->operationId = prior.record.operationId;
      draft->execution = prior.record.execution;
      draft->commitState = prior.record.commitState;
      draft->presentation = prior.record.presentation;
      draft->acceptedNote = tr("已受理（调度暂不可用：%1）").arg(error);
      emit computeFinished(draftId, QJsonObject{
          {QStringLiteral("ok"), false},
          {QStringLiteral("code"), error},
          {QStringLiteral("operationId"), prior.record.operationId},
          {QStringLiteral("execution"), prior.record.execution},
          {QStringLiteral("commitState"), prior.record.commitState},
          {QStringLiteral("accepted"), true}});
      emit draftChanged(draftId);
    } else {
      // 查询无权/失败/确实未受理：受理状态待确认（保留请求身份与已知 ID）。
      draft->acceptedNote = tr("受理状态待确认（%1）").arg(error);
      emit computeFinished(draftId, QJsonObject{
          {QStringLiteral("ok"), false},
          {QStringLiteral("code"), error},
          {QStringLiteral("accepted"), false},
          {QStringLiteral("pendingConfirmation"), true}});
      emit draftChanged(draftId);
    }
    if (reason) *reason = error;
    finishBusy();
    return false;
  }
  return true;
}

bool AiPlanPresentation::requestOriginalOperation(const QString &draftId, QString *reason) {
  auto *draft = findDraft(draftId);
  if (!draft) {
    if (reason) *reason = QStringLiteral("plan_draft_unknown");
    return false;
  }
  Guard guard;
  const QString action = QStringLiteral("original");
  if (!beginGuarded(draftId, action, guard, reason))
    return false;
  const auto scope = m_deps.host->callerScope(paleo::host::Caller::Agent,
                                              guard.chatSessionId);
  auto *service = m_deps.host->recovery();
  // 已知 operationId 直接 get；否则按稳定请求身份 lookup（确定性重建，
  // 不需要 operation_list）。二者都只读原操作，不新起计算；来源不足或
  // 原操作不存在时如实报告不可恢复，不自动发起新计算冒充恢复。
  QJsonObject record;
  if (!draft->operationId.isEmpty())
    record = service->get(scope, draft->operationId);
  if (record.value(QStringLiteral("ok")).toBool() != true ||
      record.value(QStringLiteral("operationId")).toString().isEmpty()) {
    if (draft->graphDigest.isEmpty() || guard.capturedContext.documentId.isEmpty()) {
      emit originalOperationResolved(draftId, QJsonObject{
          {QStringLiteral("ok"), false},
          {QStringLiteral("code"), QStringLiteral("plan_identity_unavailable")}});
      releaseBusy(draftId, action);
      return true;
    }
    const auto requestId = planExecutionRequestId(guard.capturedContext.documentId,
                                                  draft->graphDigest);
    const auto prior = service->lookup(scope, requestId);
    if (prior.code != QLatin1String("found")) {
      emit originalOperationResolved(draftId, QJsonObject{
          {QStringLiteral("ok"), false},
          {QStringLiteral("code"), prior.code.isEmpty()
              ? QStringLiteral("operation_not_found") : prior.code}});
      releaseBusy(draftId, action);
      return true;
    }
    draft->operationId = prior.record.operationId;
    record = service->get(scope, prior.record.operationId);
  }
  emit originalOperationResolved(draftId, record);
  releaseBusy(draftId, action);
  return true;
}

} // namespace paleo::chat
