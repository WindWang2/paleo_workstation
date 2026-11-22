// 层：测试壳
#include <QtTest>
#include <qgsapplication.h>
#include "../src/catalog/datacatalog.h"
#include "../src/metadata/projectlock.h"
#include "../src/services/hostcallcontext.h"
#include "../src/services/paleotaskservice.h"
#include "../src/workflow/aichatcontroller.h"
#include "../src/workflow/aiplanpresentation.h"
#include "../src/workflow/capabilityhost.h"
#include "../src/workflow/capabilityrecovery.h"
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>

using namespace paleo;
using namespace paleo::capability;

// H5 W5–W8：AiPlanPresentation 对真实 CapabilityHost/Recovery/OperationLog
// 的行为验证。夹具口径沿用 tst_capability_recovery_native（本地真实工程 +
// 可写上下文 + 任务池），计算链走真实 IDW→contour 数值 worker。
//
// oracle 关注「不绕服务、不改服务事实」：
//   · 纯预检/打开/编辑/历史渲染路径 prepare/start 计数为零；
//   · 显式计算经 preparePlan→start 原链，compute-only 不进正式发布，
//     catalog 版本数不变、真实终端产物存在；
//   · 重复请求（同图身份）复用原 operation 不重算（computeCount 不增）；
//   · 改图产生新请求身份，原操作保留；
//   · accept 后调度失败：沿原请求 lookup 确认 queued 事实，不换 ID 不重算；
//   · 上下文/会话失效：拒绝执行且计数不增，草案保留可读。
namespace {

struct Host {
  QTemporaryDir root{QDir::temp().filePath(QStringLiteral("h5-XXXXXX"))};
  DataCatalog catalog;
  ProjectDirLock lock{root.path()};
  PaleoTaskService tasks;
  CapabilityHost service;
  bool writable = true;
  quint64 epoch = 1;
  bool init() {
    if (!lock.tryLock() || !catalog.open(root.path()))
      return false;
    service.setTaskService(&tasks);
    service.setCallerScope(host::Caller::Agent, "agent");
    service.setBindingProvider([this] {
      HostBinding b;
      b.projectDir = root.path();
      b.operationDir = root.path() + "/.paleo/capability-ops";
      b.catalog = &catalog;
      b.writeClaim = writable ? lock.claim() : nullptr;
      b.context.documentId = "doc-h5";
      b.context.epoch = epoch;
      b.context.open = true;
      b.context.persistentIdentity = true;
      b.context.readOnly = !writable;
      b.context.ownsWrites = bool(b.writeClaim);
      b.context.allowedEffects =
        host::Read | host::PrivateStage | host::CatalogPublish | host::Presentation;
      return b;
    });
    return true;
  }
};

QJsonObject validTwoStepGraph() {
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
                                               {"subinterval_id", "S1"}}}};
  const QJsonObject contour{{"id", "contours"},
                            {"capability", "singlefactor.contours"},
                            {"contract_version", 1},
                            {"output_port", "LINES"},
                            {"intent", "compute-only"},
                            {"parameters", QJsonObject{{"CONTOUR_LEVEL", 5.0}}},
                            {"inputs", QJsonObject{}}};
  return QJsonObject{{"nodes", QJsonArray{idw, contour}},
                     {"edges", QJsonArray{QJsonObject{{"from", "idw"}, {"from_port", "OUTPUT"},
                                                      {"to", "contours"}, {"to_port", "RASTER"}}}}};
}

// 与 runPlanPreview 同一实现的提案载荷（工具入口在 tst_h5_plan_preview 覆盖）。
// 夹具解码失败时载荷 ok=false，下游断言会给出可定位失败。
QJsonObject previewPayloadFor(const QJsonObject &graph) {
  const auto decoded = paleo::chat::decodePlanPreviewGraph(graph);
  const auto plan = validatePlan(decoded.nodes, decoded.edges);
  return paleo::chat::planPreviewPayload(plan, decoded.nodes, decoded.edges, {}, {});
}

} // namespace

class TestH5PlanPresentation : public QObject {
  Q_OBJECT
private slots:
  void openAndPreflightNeverTouchTheHost();
  void computeRunsRealTwoStepChainComputeOnly();
  void repeatedRequestReplaysOriginalOperation();
  void editedGraphGetsNewIdentityAndOldOperationSurvives();
  void acceptThenScheduleFailureConfirmsQueuedOperation();
  void contextInvalidationRefusesComputeWithoutNewAttempts();
  void readOnlyProjectRefusesComputeBeforeStart();
  void originalOperationIsResolvableAfterAdapterRebuild();
  void lateResultDoesNotOverrideNewerRevision();

private:
  static void editCellSize(QJsonObject &graph, double value);
};

void TestH5PlanPresentation::editCellSize(QJsonObject &graph, double value) {
  auto nodes = graph.value(QStringLiteral("nodes")).toArray();
  auto idw = nodes.at(0).toObject();
  auto parameters = idw.value(QStringLiteral("parameters")).toObject();
  parameters.insert(QStringLiteral("CELL_SIZE"), value);
  idw.insert(QStringLiteral("parameters"), parameters);
  nodes.replace(0, idw);
  graph.insert(QStringLiteral("nodes"), nodes);
}

void TestH5PlanPresentation::openAndPreflightNeverTouchTheHost() {
  Host host;
  QVERIFY(host.init());
  AiChatController controller;
  paleo::chat::AiPlanPresentation presenter{{&controller, &host.service, nullptr}};
  const auto payload = previewPayloadFor(validTwoStepGraph());
  QString reason;
  const QString draftId = presenter.openProposal(payload, "call-open-1", &reason);
  QVERIFY2(!draftId.isEmpty(), qPrintable(reason));
  // 打开提案不构成执行资格。
  QString why;
  QVERIFY(!presenter.computeAllowed(draftId, &why));
  QCOMPARE(why, QStringLiteral("preflight_required"));
  // 显式重新预检：绑定当前上下文，通过后可计算。
  QVERIFY2(presenter.requestPreflight(draftId, &reason), qPrintable(reason));
  QVERIFY(presenter.computeAllowed(draftId));
  // 全程零宿主执行链调用（纯预检 oracle）。
  QCOMPARE(presenter.prepareAttempts(), 0u);
  QCOMPARE(presenter.startAttempts(), 0u);
  QCOMPARE(host.catalog.versions().size(), 0);
  // 同一 toolCallId 重开复用草案（不重复建草案）。
  QCOMPARE(presenter.openProposal(payload, "call-open-1"), draftId);
  QCOMPARE(presenter.draftIds().size(), 1);
  // 打开缺图载荷（截断 envelope 形态）：拒绝且给出可证实理由。
  QJsonObject truncated = payload;
  truncated.remove(QStringLiteral("graph"));
  QVERIFY(presenter.openProposal(truncated, "call-open-2", &reason).isEmpty());
  QCOMPARE(reason, QStringLiteral("plan_graph_unavailable"));
}

void TestH5PlanPresentation::computeRunsRealTwoStepChainComputeOnly() {
  Host host;
  QVERIFY(host.init());
  AiChatController controller;
  paleo::chat::AiPlanPresentation presenter{{&controller, &host.service, nullptr}};
  auto graph = validTwoStepGraph();
  const auto payload = previewPayloadFor(graph);
  QString reason;
  const QString draftId = presenter.openProposal(payload, "call-compute-1", &reason);
  QVERIFY(!draftId.isEmpty());
  QVERIFY(presenter.requestPreflight(draftId, &reason));

  QSignalSpy finished(&presenter, &paleo::chat::AiPlanPresentation::computeFinished);
  QVERIFY2(presenter.requestCompute(draftId, &reason), qPrintable(reason));
  QCOMPARE(presenter.prepareAttempts(), 1u);
  QCOMPARE(presenter.startAttempts(), 1u);
  // 双击/同步重入：busy 拒绝，不产生第二次 prepare/start。
  QString busyReason;
  QVERIFY(!presenter.requestCompute(draftId, &busyReason));
  QCOMPARE(busyReason, QStringLiteral("request_already_busy"));
  QCOMPARE(presenter.prepareAttempts(), 1u);
  QVERIFY2(finished.wait(30000), "真实 IDW→contour worker 必须完成");
  const auto facts = finished.at(0).at(1).toJsonObject();
  QVERIFY2(facts.value(QStringLiteral("ok")).toBool(),
           qPrintable(facts.value(QStringLiteral("code")).toString()));
  // 四阶段诚实：预检→ticket→耐久受理→计算终态；commitState=not_requested
  //（compute-only 不进正式发布）。
  QCOMPARE(facts.value(QStringLiteral("commitState")).toString(), QStringLiteral("not_requested"));
  const auto summary = facts.value(QStringLiteral("summary")).toObject();
  QCOMPARE(summary.value(QStringLiteral("child_operations")).toArray().size(), 2);
  QCOMPARE(summary.value(QStringLiteral("plan_order")).toArray().size(), 2);
  const QString operationId = facts.value(QStringLiteral("operationId")).toString();
  QVERIFY(!operationId.isEmpty());

  // 真实终端私有成果存在（操作目录下产物），catalog 无新正式版本/无地图副作用。
  QCOMPARE(host.catalog.versions().size(), 0);
  const auto record = host.service.recovery()->get(
    host.service.callerScope(host::Caller::Agent, controller.session().id), operationId);
  QCOMPARE(record.value(QStringLiteral("ok")).toBool(), true);
  QCOMPARE(record.value(QStringLiteral("computeCount")).toInt(), 1);
  const auto results = host.service.recovery()->results(
    host.service.callerScope(host::Caller::Agent, controller.session().id), operationId);
  QVERIFY(results.value(QStringLiteral("ok")).toBool());
  QVERIFY(results.value(QStringLiteral("ports")).toArray().size() >= 1);

  // 草案视图携带原操作事实与稳定请求身份。
  const auto view = presenter.draft(draftId);
  QCOMPARE(view.operationId, operationId);
  QVERIFY(!view.clientRequestId.isEmpty());
}

void TestH5PlanPresentation::repeatedRequestReplaysOriginalOperation() {
  Host host;
  QVERIFY(host.init());
  AiChatController controller;
  paleo::chat::AiPlanPresentation presenter{{&controller, &host.service, nullptr}};
  const auto payload = previewPayloadFor(validTwoStepGraph());
  QString reason;
  const QString draftId = presenter.openProposal(payload, "call-replay-1", &reason);
  QVERIFY(presenter.requestPreflight(draftId, &reason));
  QSignalSpy finished(&presenter, &paleo::chat::AiPlanPresentation::computeFinished);
  QVERIFY(presenter.requestCompute(draftId, &reason));
  QVERIFY(finished.wait(30000));
  const QString firstOperation = finished.at(0).at(1).toJsonObject()
                                     .value(QStringLiteral("operationId")).toString();
  QVERIFY(!firstOperation.isEmpty());

  // 同一已采纳草案的重复显式计算：同一请求身份 → 宿主幂等重放原 operation。
  QVERIFY(presenter.requestPreflight(draftId, &reason)); // 重新预检不换身份
  QVERIFY(presenter.requestCompute(draftId, &reason));
  QVERIFY(finished.wait(30000));
  QCOMPARE(finished.at(1).at(1).toJsonObject().value(QStringLiteral("operationId")).toString(),
           firstOperation);
  // computeCount 不因重复 UI 动作增长。
  const auto record = host.service.recovery()->get(
    host.service.callerScope(host::Caller::Agent, controller.session().id), firstOperation);
  QCOMPARE(record.value(QStringLiteral("computeCount")).toInt(), 1);
  QCOMPARE(presenter.prepareAttempts(), 2u);
  QCOMPARE(presenter.startAttempts(), 2u);
}

void TestH5PlanPresentation::editedGraphGetsNewIdentityAndOldOperationSurvives() {
  Host host;
  QVERIFY(host.init());
  AiChatController controller;
  paleo::chat::AiPlanPresentation presenter{{&controller, &host.service, nullptr}};
  auto graph = validTwoStepGraph();
  QString reason;
  const QString draftId = presenter.openProposal(previewPayloadFor(graph), "call-edit-1", &reason);
  QVERIFY(presenter.requestPreflight(draftId, &reason));
  const auto firstRequest = presenter.draft(draftId).clientRequestId;
  QVERIFY(!firstRequest.isEmpty());
  QSignalSpy finished(&presenter, &paleo::chat::AiPlanPresentation::computeFinished);
  QVERIFY(presenter.requestCompute(draftId, &reason));
  QVERIFY(finished.wait(30000));
  const QString firstOperation = finished.at(0).at(1).toJsonObject()
                                     .value(QStringLiteral("operationId")).toString();

  // 编辑 → 新修订 → 执行资格失效（需重新预检）→ 新请求身份。
  editCellSize(graph, 10.0);
  QVERIFY2(presenter.updateDraftGraph(draftId, graph, &reason), qPrintable(reason));
  QCOMPARE(presenter.draft(draftId).revision, 1);
  QString why;
  QVERIFY(!presenter.computeAllowed(draftId, &why));
  QCOMPARE(why, QStringLiteral("graph_edited"));
  QVERIFY(presenter.requestPreflight(draftId, &reason));
  const auto secondRequest = presenter.draft(draftId).clientRequestId;
  QVERIFY(secondRequest != firstRequest);
  QVERIFY(presenter.requestCompute(draftId, &reason));
  QVERIFY(finished.wait(30000));
  const QString secondOperation = finished.at(1).at(1).toJsonObject()
                                      .value(QStringLiteral("operationId")).toString();
  QVERIFY(secondOperation != firstOperation);
  // 原操作保留且事实未变。
  const auto record = host.service.recovery()->get(
    host.service.callerScope(host::Caller::Agent, controller.session().id), firstOperation);
  QCOMPARE(record.value(QStringLiteral("ok")).toBool(), true);
  QCOMPARE(record.value(QStringLiteral("computeCount")).toInt(), 1);
}

void TestH5PlanPresentation::acceptThenScheduleFailureConfirmsQueuedOperation() {
  Host host;
  QVERIFY(host.init());
  AiChatController controller;
  paleo::chat::AiPlanPresentation presenter{{&controller, &host.service, nullptr}};
  const auto payload = previewPayloadFor(validTwoStepGraph());
  QString reason;
  const QString draftId = presenter.openProposal(payload, "call-accept-fail", &reason);
  QVERIFY(presenter.requestPreflight(draftId, &reason));

  // Host::start 先耐久 accept 再请求调度：摘掉任务池模拟调度返回 nullptr。
  host.service.setTaskService(nullptr);
  QSignalSpy finished(&presenter, &paleo::chat::AiPlanPresentation::computeFinished);
  QVERIFY2(!presenter.requestCompute(draftId, &reason), "调度失败必须如实返回 false");
  QCOMPARE(reason, QStringLiteral("task_service_unavailable"));
  QCOMPARE(presenter.prepareAttempts(), 1u);
  QCOMPARE(presenter.startAttempts(), 1u);
  QCOMPARE(finished.size(), 1);
  const auto facts = finished.at(0).at(1).toJsonObject();
  // 空 task 不能证明未受理：沿原请求 lookup 确认已受理的 queued 原操作。
  QVERIFY(facts.value(QStringLiteral("accepted")).toBool());
  const QString operationId = facts.value(QStringLiteral("operationId")).toString();
  QVERIFY(!operationId.isEmpty());
  const auto view = presenter.draft(draftId);
  QCOMPARE(view.operationId, operationId);
  QVERIFY(!view.acceptedNote.isEmpty());

  // 确认/查询不增加 computeCount；原操作可查且不新建。
  host.service.setTaskService(&host.tasks);
  const auto scope = host.service.callerScope(host::Caller::Agent, controller.session().id);
  const auto record = host.service.recovery()->get(scope, operationId);
  QCOMPARE(record.value(QStringLiteral("ok")).toBool(), true);
  QCOMPARE(record.value(QStringLiteral("computeCount")).toInt(), 0);

  // 显式「查看原操作」在合法上下文内沿原请求定位（非历史卡自动查询）。
  QSignalSpy resolved(&presenter, &paleo::chat::AiPlanPresentation::originalOperationResolved);
  QVERIFY2(presenter.requestOriginalOperation(draftId, &reason), qPrintable(reason));
  QCOMPARE(resolved.size(), 1);
  QCOMPARE(resolved.at(0).at(1).toJsonObject().value(QStringLiteral("operationId")).toString(),
           operationId);
}

void TestH5PlanPresentation::contextInvalidationRefusesComputeWithoutNewAttempts() {
  Host host;
  QVERIFY(host.init());
  AiChatController controller;
  paleo::chat::AiPlanPresentation presenter{{&controller, &host.service, nullptr}};
  const auto payload = previewPayloadFor(validTwoStepGraph());
  QString reason;
  const QString draftId = presenter.openProposal(payload, "call-invalidate", &reason);
  QVERIFY(presenter.requestPreflight(draftId, &reason));
  QVERIFY(presenter.computeAllowed(draftId));

  // 切工程（epoch 变化，无 UI 信号）：受理时的 Read 门比对 captured≠live →
  // stale_session 拒绝；prepare/start 计数不增。
  ++host.epoch;
  QString why;
  QVERIFY(!presenter.requestCompute(draftId, &why));
  QCOMPARE(why, QStringLiteral("stale_session"));
  QCOMPARE(presenter.prepareAttempts(), 0u);
  QCOMPARE(presenter.startAttempts(), 0u);
  // 草案仍可读（失效不删除用户输入）；重新预检显式绑定当前上下文后恢复资格。
  QVERIFY(!presenter.draft(draftId).currentGraph.isEmpty());
  QVERIFY2(presenter.requestPreflight(draftId, &reason), qPrintable(reason));
  QVERIFY(presenter.computeAllowed(draftId));
}

void TestH5PlanPresentation::readOnlyProjectRefusesComputeBeforeStart() {
  Host host;
  QVERIFY(host.init());
  AiChatController controller;
  paleo::chat::AiPlanPresentation presenter{{&controller, &host.service, nullptr}};
  const auto payload = previewPayloadFor(validTwoStepGraph());
  QString reason;
  const QString draftId = presenter.openProposal(payload, "call-readonly", &reason);
  QVERIFY(presenter.requestPreflight(draftId, &reason));
  host.writable = false;
  ++host.epoch; // 触发宿主重绑（read-only 生效）
  // 重新预检绑定新上下文后，compute-only 的 PrivateStage 被 read-only 拒绝。
  QVERIFY(presenter.requestPreflight(draftId, &reason));
  QVERIFY(!presenter.requestCompute(draftId, &reason));
  QCOMPARE(reason, QStringLiteral("project_read_only"));
  QCOMPARE(presenter.startAttempts(), 0u); // 所有拒绝路径无新 start
}

void TestH5PlanPresentation::originalOperationIsResolvableAfterAdapterRebuild() {
  Host host;
  QVERIFY(host.init());
  AiChatController controller;
  paleo::chat::AiPlanPresentation presenter{{&controller, &host.service, nullptr}};
  const auto payload = previewPayloadFor(validTwoStepGraph());
  QString reason;
  const QString draftId = presenter.openProposal(payload, "call-reopen", &reason);
  QVERIFY(presenter.requestPreflight(draftId, &reason));
  QSignalSpy finished(&presenter, &paleo::chat::AiPlanPresentation::computeFinished);
  QVERIFY(presenter.requestCompute(draftId, &reason));
  QVERIFY(finished.wait(30000));
  const QString operationId = finished.at(0).at(1).toJsonObject()
                                  .value(QStringLiteral("operationId")).toString();
  QVERIFY(!operationId.isEmpty());

  // 重启/重开面板形态：新 adapter 实例 + 同一会话 + 同一提案载荷 →
  // 显式「查看原操作」按稳定请求身份 lookup 定位原操作，不自动重算。
  paleo::chat::AiPlanPresentation reopened{{&controller, &host.service, nullptr}};
  const QString newDraft = reopened.openProposal(payload, "call-reopen", &reason);
  QVERIFY(!newDraft.isEmpty());
  QVERIFY(reopened.requestPreflight(newDraft, &reason));
  QSignalSpy resolved(&reopened, &paleo::chat::AiPlanPresentation::originalOperationResolved);
  QVERIFY2(reopened.requestOriginalOperation(newDraft, &reason), qPrintable(reason));
  QCOMPARE(resolved.size(), 1);
  QCOMPARE(resolved.at(0).at(1).toJsonObject().value(QStringLiteral("operationId")).toString(),
           operationId);
  QCOMPARE(reopened.prepareAttempts(), 0u);
  QCOMPARE(reopened.startAttempts(), 0u);
  // computeCount 不增长（仅查看/确认）。
  const auto record = host.service.recovery()->get(
    host.service.callerScope(host::Caller::Agent, controller.session().id), operationId);
  QCOMPARE(record.value(QStringLiteral("computeCount")).toInt(), 1);
}

void TestH5PlanPresentation::lateResultDoesNotOverrideNewerRevision() {
  Host host;
  QVERIFY(host.init());
  AiChatController controller;
  paleo::chat::AiPlanPresentation presenter{{&controller, &host.service, nullptr}};
  auto graph = validTwoStepGraph();
  QString reason;
  const QString draftId = presenter.openProposal(previewPayloadFor(graph), "call-late", &reason);
  QVERIFY(presenter.requestPreflight(draftId, &reason));
  QSignalSpy finished(&presenter, &paleo::chat::AiPlanPresentation::computeFinished);
  QVERIFY(presenter.requestCompute(draftId, &reason));
  // 计算在途时编辑图（新修订）：晚到的结果属于旧请求——操作事实保留可见，
  // 但不得把新修订标记为已执行（执行资格仍失效，修订不被回滚）。
  editCellSize(graph, 12.5);
  QVERIFY(presenter.updateDraftGraph(draftId, graph, &reason));
  const int revisionsAtEdit = presenter.draft(draftId).revision;
  QVERIFY(finished.wait(30000));
  const auto view = presenter.draft(draftId);
  QCOMPARE(view.revision, revisionsAtEdit);
  QVERIFY(!view.executionEligible);
  QVERIFY(!view.operationId.isEmpty()); // 旧请求的操作事实保留（W7.5）
}

int main(int argc, char **argv) {
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH"), true);
  app.initQgis();
  TestH5PlanPresentation test;
  const auto code = QTest::qExec(&test, argc, argv);
  QgsApplication::exitQgis();
  return code;
}
#include "tst_h5_plan_presentation.moc"
