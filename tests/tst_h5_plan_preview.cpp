// 层：测试壳
#include <QtTest>
#include "../src/ai/chat/domaintools.h"
#include "../src/ai/chat/chatmessage.h"
#include "../src/ai/chat/planpreviewtool.h"
#include "../src/workflow/aiplanpresentation.h"
#include "../src/workflow/aichattoolrunner.h"
#include "../src/workflow/capabilityplan.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>

using namespace paleo::capability;

// H5 W1/W2：paleo.workflow_plan_preview 纯预检提案工具的严格边界。
//
// oracle 区分「正确与错误实现」：
//   · 有界严格解码在业务执行前拒绝不可信形状（未知字段/深度/非有限数/
//     超预算/错类型），错误码 + JSON 路径可定位；
//   · 复用唯一 v1 预检（validatePlan），正常/失败同一契约、issues 只含
//     验证器真实返回值、effective 参数来自 AlgorithmSpec 而非手抄；
//   · digest 稳定（与节点顺序无关），改图换身份；
//   · runner 入口纯路径零宿主依赖（无 host 也可执行 decode+validate）。
namespace {

QJsonObject twoStepArguments() {
  QJsonArray points;
  for (int i = 0; i < 4; ++i)
    points.append(QJsonObject{{"x", (i % 2) * 100.0}, {"y", (i / 2) * 100.0},
                              {"z", (i / 2) * 10.0}, {"well_id", QString::number(i)}});
  const QJsonObject idw{{"id", "idw"},
                        {"capability", "constraint.idw"},
                        {"contract_version", 1},
                        {"output_port", "OUTPUT"},
                        {"intent", "compute-only"},
                        {"parameters", QJsonObject{{"FIELD", "z"}, {"CELL_SIZE", 25.0}}},
                        {"inputs", QJsonObject{{"points", points},
                                               {"subinterval_id", "S1"}}};
  const QJsonObject contour{{"id", "contours"},
                            {"capability", "singlefactor.contours"},
                            {"contract_version", 1},
                            {"output_port", "LINES"},
                            {"parameters", QJsonObject{{"CONTOUR_LEVEL", 5.0}}},
                            {"inputs", QJsonObject{}}};
  return QJsonObject{{"title", "两口井的构造图"},
                     {"nodes", QJsonArray{idw, contour}},
                     {"edges", QJsonArray{QJsonObject{{"from", "idw"}, {"from_port", "OUTPUT"},
                                                      {"to", "contours"}, {"to_port", "RASTER"}}}}};
}

QJsonArray nodeArray(const QJsonObject &arguments) {
  return arguments.value(QStringLiteral("nodes")).toArray();
}
} // namespace

class TestH5PlanPreview : public QObject {
  Q_OBJECT
private slots:
  void validTwoStepProposalDecodesAndValidates();
  void payloadCarriesBoundedContractAndHonestState();
  void unknownFieldsAreRejectedBeforeValidation();
  void authorityApingFieldsAreRejected();
  void budgetDepthAndTypeMatrix();
  void digestIsOrderInsensitiveAndEditSensitive();
  void requestIdIsDeterministicPerDocumentAndGraph();
  void toolTableDeclaresAndRoutesWithoutAutoExecution();
  void runnerExecutesPurePreviewWithoutHost();
  void runnerRejectsDecodeFailureWithCodeAndField();
  void oversizedEchoStaysBounded();

private:
  static QJsonObject previewResult(const QJsonObject &arguments, bool *ok);
};

QJsonObject TestH5PlanPreview::previewResult(const QJsonObject &arguments, bool *ok) {
  const auto decoded = paleo::chat::decodePlanPreviewArguments(arguments);
  if (!decoded.ok) {
    *ok = false;
    QJsonObject payload{{"code", decoded.code}};
    if (!decoded.field.isEmpty())
      payload.insert(QStringLiteral("field"), decoded.field);
    return payload;
  }
  const auto plan = validatePlan(decoded.nodes, decoded.edges);
  *ok = true;
  return paleo::chat::planPreviewPayload(plan, decoded.nodes, decoded.edges,
                                         decoded.title, decoded.note);
}

void TestH5PlanPreview::validTwoStepProposalDecodesAndValidates() {
  bool ok = false;
  const auto payload = previewResult(twoStepArguments(), &ok);
  QVERIFY(ok);
  QCOMPARE(payload.value(QStringLiteral("contract")).toString(),
           QStringLiteral("paleo.plan-preview.v1"));
  QVERIFY(payload.value(QStringLiteral("ok")).toBool());
  QCOMPARE(payload.value(QStringLiteral("status")).toString(),
           QStringLiteral("not_executed"));
  // 顺序与终端来自验证器，不在 UI 侧手抄。
  QCOMPARE(payload.value(QStringLiteral("order")).toArray().first().toString(),
           QStringLiteral("idw"));
  QCOMPARE(payload.value(QStringLiteral("terminal")).toString(), QStringLiteral("contours"));
  const auto effective = payload.value(QStringLiteral("effective_parameters")).toObject()
                             .value(QStringLiteral("idw")).toObject();
  QCOMPARE(effective.value(QStringLiteral("FIELD")).toString(), QStringLiteral("z"));
  QCOMPARE(effective.value(QStringLiteral("CELL_SIZE")).toDouble(), 25.0);
}

void TestH5PlanPreview::payloadCarriesBoundedContractAndHonestState() {
  // 下游未知参数：失败与成功同一契约、状态恒 not_executed、issues 只含
  // 验证器真实返回的首个问题（不伪造完整问题集合）。
  auto arguments = twoStepArguments();
  auto nodes = nodeArray(arguments);
  auto contour = nodes.at(1).toObject();
  auto parameters = contour.value(QStringLiteral("parameters")).toObject();
  parameters.insert(QStringLiteral("DEPTH_LIMIT"), 1.0); // 未声明参数
  contour.insert(QStringLiteral("parameters"), parameters);
  nodes.replace(1, contour);
  arguments.insert(QStringLiteral("nodes"), nodes);
  bool ok = false;
  const auto payload = previewResult(arguments, &ok);
  QVERIFY(ok);
  QVERIFY(!payload.value(QStringLiteral("ok")).toBool());
  QCOMPARE(payload.value(QStringLiteral("status")).toString(),
           QStringLiteral("not_executed"));
  const auto issues = payload.value(QStringLiteral("issues")).toArray();
  QCOMPARE(issues.size(), 1);
  QCOMPARE(issues.at(0).toObject().value(QStringLiteral("code")).toString(),
           QStringLiteral("unknown_param"));
  QVERIFY(!issues.at(0).toObject().value(QStringLiteral("field")).toString().isEmpty());
  // 无效图仍回显规范化图（可定位修正），但无 order/terminal。
  QCOMPARE(payload.value(QStringLiteral("graph")).toObject()
               .value(QStringLiteral("nodes")).toArray().size(), 2);
  QVERIFY(!payload.contains(QStringLiteral("order")));
  QVERIFY(!payload.contains(QStringLiteral("terminal")));
}

void TestH5PlanPreview::unknownFieldsAreRejectedBeforeValidation() {
  auto arguments = twoStepArguments();
  arguments.insert(QStringLiteral("surprise"), true);
  auto decoded = paleo::chat::decodePlanPreviewArguments(arguments);
  QVERIFY(!decoded.ok);
  QCOMPARE(decoded.code, QStringLiteral("unknown_field"));
  QCOMPARE(decoded.field, QStringLiteral("surprise"));

  arguments = twoStepArguments();
  auto nodes = nodeArray(arguments);
  auto idw = nodes.at(0).toObject();
  idw.insert(QStringLiteral("mystery_port"), 7);
  nodes.replace(0, idw);
  arguments.insert(QStringLiteral("nodes"), nodes);
  decoded = paleo::chat::decodePlanPreviewArguments(arguments);
  QVERIFY(!decoded.ok);
  QCOMPARE(decoded.code, QStringLiteral("unknown_field"));
  QCOMPARE(decoded.field, QStringLiteral("nodes.0.mystery_port"));

  arguments = twoStepArguments();
  auto edges = arguments.value(QStringLiteral("edges")).toArray();
  auto edge = edges.at(0).toObject();
  edge.insert(QStringLiteral("weight"), 0.5);
  edges.replace(0, edge);
  arguments.insert(QStringLiteral("edges"), edges);
  decoded = paleo::chat::decodePlanPreviewArguments(arguments);
  QVERIFY(!decoded.ok);
  QCOMPARE(decoded.field, QStringLiteral("edges.0.weight"));
}

void TestH5PlanPreview::authorityApingFieldsAreRejected() {
  // approved/caller/documentId/路径等授权语义字段一律在解码层被拒
  //（封闭词表），不能进入执行授权。
  const QStringList authorityKeys = {QStringLiteral("approved"), QStringLiteral("caller"),
                                     QStringLiteral("callerScope"), QStringLiteral("documentId"),
                                     QStringLiteral("projectDir"), QStringLiteral("operationDir"),
                                     QStringLiteral("writeClaim"), QStringLiteral("publishIntent")};
  for (const auto &key : authorityKeys) {
    auto arguments = twoStepArguments();
    auto nodes = nodeArray(arguments);
    auto idw = nodes.at(0).toObject();
    idw.insert(key, QStringLiteral("model-claims-this"));
    nodes.replace(0, idw);
    arguments.insert(QStringLiteral("nodes"), nodes);
    const auto decoded = paleo::chat::decodePlanPreviewArguments(arguments);
    QVERIFY2(!decoded.ok, qPrintable(key));
    QCOMPARE(decoded.code, QStringLiteral("unknown_field"));
  }
}

void TestH5PlanPreview::budgetDepthAndTypeMatrix() {
  // 顶层类型错误。
  auto decoded = paleo::chat::decodePlanPreviewArguments(
    QJsonObject{{"nodes", QStringLiteral("all of them")}, {"edges", QJsonArray{}}});
  QVERIFY(!decoded.ok);
  QCOMPARE(decoded.code, QStringLiteral("plan_field_type"));

  // 空节点表。
  decoded = paleo::chat::decodePlanPreviewArguments({{"nodes", QJsonArray{}},
                                                     {"edges", QJsonArray{}}});
  QVERIFY(!decoded.ok);
  QCOMPARE(decoded.code, QStringLiteral("plan_nodes_required"));

  // 33 节点超 32 上限（分配前拒绝；后端上限不被放大）。
  QJsonArray many;
  const auto templateNode = twoStepArguments().value(QStringLiteral("nodes")).toArray().at(0);
  for (int i = 0; i < 33; ++i)
    many.append(templateNode);
  decoded = paleo::chat::decodePlanPreviewArguments({{"nodes", many}, {"edges", QJsonArray{}}});
  QVERIFY(!decoded.ok);
  QCOMPARE(decoded.code, QStringLiteral("plan_too_large"));

  // 深度超限。
  auto arguments = twoStepArguments();
  auto nodes = nodeArray(arguments);
  auto idw = nodes.at(0).toObject();
  QJsonObject deep;
  for (int i = 0; i < 12; ++i) {
    QJsonObject next;
    next.insert(QStringLiteral("v"), deep);
    deep = next;
  }
  auto parameters = idw.value(QStringLiteral("parameters")).toObject();
  parameters.insert(QStringLiteral("FIELD"), deep);
  idw.insert(QStringLiteral("parameters"), parameters);
  nodes.replace(0, idw);
  arguments.insert(QStringLiteral("nodes"), nodes);
  decoded = paleo::chat::decodePlanPreviewArguments(arguments);
  QVERIFY(!decoded.ok);
  QCOMPARE(decoded.code, QStringLiteral("plan_value_too_deep"));

  // 非有限数值（1e999 解析为 inf）。
  arguments = twoStepArguments();
  nodes = nodeArray(arguments);
  idw = nodes.at(0).toObject();
  parameters = idw.value(QStringLiteral("parameters")).toObject();
  parameters.insert(QStringLiteral("CELL_SIZE"),
                   QJsonDocument::fromJson("{\"x\":1e999}").object().value(QStringLiteral("x")));
  idw.insert(QStringLiteral("parameters"), parameters);
  nodes.replace(0, idw);
  arguments.insert(QStringLiteral("nodes"), nodes);
  decoded = paleo::chat::decodePlanPreviewArguments(arguments);
  QVERIFY(!decoded.ok);
  QCOMPARE(decoded.code, QStringLiteral("plan_non_finite_number"));

  // contract_version 必须是原生整数。
  arguments = twoStepArguments();
  nodes = nodeArray(arguments);
  idw = nodes.at(0).toObject();
  idw.insert(QStringLiteral("contract_version"), QStringLiteral("1"));
  nodes.replace(0, idw);
  arguments.insert(QStringLiteral("nodes"), nodes);
  decoded = paleo::chat::decodePlanPreviewArguments(arguments);
  QVERIFY(!decoded.ok);
  QCOMPARE(decoded.code, QStringLiteral("plan_field_type"));
  QCOMPARE(decoded.field, QStringLiteral("nodes.0.contract_version"));

  // 空 id。
  arguments = twoStepArguments();
  nodes = nodeArray(arguments);
  idw = nodes.at(0).toObject();
  idw.insert(QStringLiteral("id"), QString());
  nodes.replace(0, idw);
  arguments.insert(QStringLiteral("nodes"), nodes);
  decoded = paleo::chat::decodePlanPreviewArguments(arguments);
  QVERIFY(!decoded.ok);
  QCOMPARE(decoded.code, QStringLiteral("plan_field_empty"));

  // 单个字符串超预算。
  arguments = twoStepArguments();
  arguments.insert(QStringLiteral("title"), QString(121, QChar::fromLatin1('a')));
  decoded = paleo::chat::decodePlanPreviewArguments(arguments);
  QVERIFY(!decoded.ok);
  QCOMPARE(decoded.code, QStringLiteral("plan_string_too_large"));

  // null 值拒绝（不允许 undefined/null 进参数面）。
  arguments = twoStepArguments();
  nodes = nodeArray(arguments);
  idw = nodes.at(0).toObject();
  parameters = idw.value(QStringLiteral("parameters")).toObject();
  parameters.insert(QStringLiteral("FIELD"), QJsonValue());
  idw.insert(QStringLiteral("parameters"), parameters);
  nodes.replace(0, idw);
  arguments.insert(QStringLiteral("nodes"), nodes);
  decoded = paleo::chat::decodePlanPreviewArguments(arguments);
  QVERIFY(!decoded.ok);
  QCOMPARE(decoded.code, QStringLiteral("plan_value_type_rejected"));
}

void TestH5PlanPreview::digestIsOrderInsensitiveAndEditSensitive() {
  bool ok = false;
  const auto payload = previewResult(twoStepArguments(), &ok);
  QVERIFY(ok);
  const auto graph = payload.value(QStringLiteral("graph")).toObject();
  const auto digest = payload.value(QStringLiteral("digest")).toString();
  QVERIFY(!digest.isEmpty());
  QCOMPARE(digest, paleo::chat::planGraphDigest(graph));

  // 节点/边顺序无关。
  auto reordered = graph;
  auto nodes = reordered.value(QStringLiteral("nodes")).toArray();
  const auto first = nodes.takeAt(0);
  nodes.append(first);
  reordered.insert(QStringLiteral("nodes"), nodes);
  QCOMPARE(paleo::chat::planGraphDigest(reordered), digest);

  // 参数变化 → 新身份。
  auto edited = graph;
  nodes = edited.value(QStringLiteral("nodes")).toArray();
  auto idw = nodes.at(0).toObject();
  auto parameters = idw.value(QStringLiteral("parameters")).toObject();
  parameters.insert(QStringLiteral("CELL_SIZE"), 50.0);
  idw.insert(QStringLiteral("parameters"), parameters);
  nodes.replace(0, idw);
  edited.insert(QStringLiteral("nodes"), nodes);
  QVERIFY(paleo::chat::planGraphDigest(edited) != digest);
}

void TestH5PlanPreview::requestIdIsDeterministicPerDocumentAndGraph() {
  const auto first = paleo::chat::planExecutionRequestId(QStringLiteral("doc-a"),
                                                         QStringLiteral("dig-1"));
  QCOMPARE(first, paleo::chat::planExecutionRequestId(QStringLiteral("doc-a"),
                                                      QStringLiteral("dig-1")));
  QVERIFY(first != paleo::chat::planExecutionRequestId(QStringLiteral("doc-b"),
                                                       QStringLiteral("dig-1")));
  QVERIFY(first != paleo::chat::planExecutionRequestId(QStringLiteral("doc-a"),
                                                       QStringLiteral("dig-2")));
  QVERIFY(first.startsWith(QStringLiteral("plan:")));
}

void TestH5PlanPreview::toolTableDeclaresAndRoutesWithoutAutoExecution() {
  const auto spec = aiToolSpec(QStringLiteral("paleo.workflow_plan_preview"));
  QVERIFY(!spec.name.isEmpty());
  // 不挂在 isAiCapabilityTool（那族经 runChatTool 自动执行能力调用）。
  QVERIFY(!isAiCapabilityTool(QStringLiteral("paleo.workflow_plan_preview")));
  const auto dispatch = dispatchAiTool(QStringLiteral("paleo.workflow_plan_preview"),
                                       twoStepArguments());
  QCOMPARE(dispatch.status, AiToolDispatchStatus::Routed);
  QVERIFY(!dispatch.target.isEmpty());
  // 参数类型不合格（nodes 不是数组）→ Disabled + 原因。
  const auto bad = dispatchAiTool(QStringLiteral("paleo.workflow_plan_preview"),
                                  QJsonObject{{"nodes", 3}, {"edges", QJsonArray{}}});
  QCOMPARE(bad.status, AiToolDispatchStatus::Disabled);
  // 严格外部档通过（合法形状），工具效果面 = 只读。
  QString field;
  QVERIFY(spec.validateExternalArguments(twoStepArguments(), &field).isEmpty());
  QCOMPARE(spec.effectsForArguments({}), unsigned(paleo::host::Read));
}

void TestH5PlanPreview::runnerExecutesPurePreviewWithoutHost() {
  AiChatToolRunner runner; // 无宿主/工程绑定：纯预检路径仍可达
  runner.setContext(AiChatToolRunner::Context{});
  QSignalSpy finished(&runner, &AiChatToolRunner::toolFinished);
  ChatToolCall call;
  call.id = QStringLiteral("call-preview-1");
  call.name = QStringLiteral("paleo.workflow_plan_preview");
  call.argumentsJson = QString::fromUtf8(
    QJsonDocument(twoStepArguments()).toJson(QJsonDocument::Compact));
  runner.run(call);
  QVERIFY2(finished.wait(5000), "纯预检必须异步回报");
  QCOMPARE(finished.size(), 1);
  QCOMPARE(finished.at(0).at(0).value<ChatToolCall>().id, call.id);
  QVERIFY(finished.at(0).at(1).toBool());
  const auto payload = QJsonDocument::fromJson(
                         finished.at(0).at(2).toString().toUtf8()).object();
  QCOMPARE(payload.value(QStringLiteral("contract")).toString(),
           QStringLiteral("paleo.plan-preview.v1"));
  QVERIFY(payload.value(QStringLiteral("ok")).toBool());
  QVERIFY(!payload.value(QStringLiteral("digest")).toString().isEmpty());
}

void TestH5PlanPreview::runnerRejectsDecodeFailureWithCodeAndField() {
  AiChatToolRunner runner;
  runner.setContext(AiChatToolRunner::Context{});
  QSignalSpy finished(&runner, &AiChatToolRunner::toolFinished);
  ChatToolCall call;
  call.id = QStringLiteral("call-preview-bad");
  call.name = QStringLiteral("paleo.workflow_plan_preview");
  auto arguments = twoStepArguments();
  arguments.insert(QStringLiteral("approved"), true);
  call.argumentsJson = QString::fromUtf8(
    QJsonDocument(arguments).toJson(QJsonDocument::Compact));
  runner.run(call);
  QVERIFY(finished.wait(5000));
  QVERIFY(!finished.at(0).at(1).toBool());
  const auto payload = QJsonDocument::fromJson(
                         finished.at(0).at(2).toString().toUtf8()).object();
  QCOMPARE(payload.value(QStringLiteral("contract")).toString(),
           QStringLiteral("paleo.plan-preview.v1"));
  QCOMPARE(payload.value(QStringLiteral("code")).toString(), QStringLiteral("unknown_field"));
  QCOMPARE(payload.value(QStringLiteral("field")).toString(), QStringLiteral("approved"));
  QCOMPARE(payload.value(QStringLiteral("status")).toString(), QStringLiteral("not_executed"));
}

void TestH5PlanPreview::oversizedEchoStaysBounded() {
  // 边界：16 KiB 级参数（含接近上限的点表）→ 回显契约仍有界（≤64 KiB）。
  auto arguments = twoStepArguments();
  QJsonArray points;
  for (int i = 0; i < 200; ++i)
    points.append(QJsonObject{{"x", double(i)}, {"y", double(i % 7)},
                              {"z", double(i % 13)}, {"well_id", QStringLiteral("w%1").arg(i)}});
  auto nodes = nodeArray(arguments);
  auto idw = nodes.at(0).toObject();
  auto inputs = idw.value(QStringLiteral("inputs")).toObject();
  inputs.insert(QStringLiteral("points"), points);
  idw.insert(QStringLiteral("inputs"), inputs);
  nodes.replace(0, idw);
  arguments.insert(QStringLiteral("nodes"), nodes);
  const auto raw = QJsonDocument(arguments).toJson(QJsonDocument::Compact);
  QVERIFY(raw.size() <= 16 * 1024);
  bool ok = false;
  const auto payload = previewResult(arguments, &ok);
  QVERIFY(ok);
  QVERIFY(QJsonDocument(payload).toJson(QJsonDocument::Compact).size() <= 64 * 1024);
}

QTEST_MAIN(TestH5PlanPreview)
#include "tst_h5_plan_preview.moc"
