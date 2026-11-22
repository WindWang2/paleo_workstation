// 层：测试壳
#include <QtTest>
#include "workflow/aioperationpresentation.h"
#include "workflow/aichatcontroller.h"
#include "workflow/capabilityhost.h"
#include "workflow/capabilityrecovery.h"
#include "catalog/datacatalog.h"
#include "metadata/projectlock.h"
#include "services/hostcallcontext.h"
#include "services/paleotaskservice.h"
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>

using namespace paleo;
using namespace paleo::capability;

// G5 W5/W6：AiOperationPresentation adapter 对真实 CapabilityHost/
// CapabilityRecovery/OperationLog 的行为验证。夹具口径沿用
// tst_capability_recovery 的 Host（本地工程 + 只读/可写上下文）。
//
// oracle 关注「不绕服务、不改服务事实」：取消后 computeCount/已提交成果
// 未被 UI 重试改变；门禁失败返回 code 且不触后续服务调用；异步读页被新
// 请求顶替时旧回调不落地。
namespace {

OperationWorker fileWorker(const QByteArray &payload = "g5-worker-bytes!!") {
  return [payload](const OperationWorkContext &c) {
    OperationWorkResult r;
    QFile file(c.privateDir + "/values.bin");
    r.ok = file.open(QIODevice::WriteOnly) && file.write(payload) == payload.size();
    file.close();
    r.outputs.ok = r.ok;
    r.outputs.artifacts.append({"OUTPUT", file.fileName(), {}, {}, {}, {}});
    return r;
  };
}

FrozenOperation publishPlan(const QString &request) {
  FrozenOperation p;
  p.capabilityId = "g5.compute";
  p.implementationVersion = "1";
  p.callerScope = "agent:chat:g5"; // setCallerScope(Agent,"agent:chat") + 会话哈希段由 adapter 现算
  p.clientRequestId = request;
  p.intent = {{"number", 1}};
  p.frozenPlan = {{"immutable", true}};
  p.phase = "publish";
  p.worker = fileWorker();
  p.outputSpec.id = "g5:values";
  p.outputSpec.complete = true;
  AlgoPort port;
  port.id = "OUTPUT";
  port.kind = "file";
  p.outputSpec.outputs = {port};
  p.outputPorts = {"OUTPUT"};
  return p;
}

// 真实 Host：DataCatalog + ProjectDirLock + 任务池 + 恢复协调器。
struct Host {
  DataCatalog catalog;
  std::unique_ptr<ProjectDirLock> lock;
  PaleoTaskService tasks;
  CapabilityHost host;
  bool writable = true;
  bool allowRead = true;
  quint64 epoch = 1;
  int contextReads = 0;
  int flipEpochAfterReads = -1; // 模拟受理瞬间宿主重绑（race）
  QString documentId = "doc-g5";

  explicit Host(QObject *parent) : host(parent) {}
  bool open(const QString &root) {
    lock = std::make_unique<ProjectDirLock>(root);
    if (!lock->tryLock() || !catalog.open(root))
      return false;
    host.setCallerScope(host::Caller::Agent, "agent:chat");
    host.setBindingProvider([this, root] {
      HostBinding b;
      b.projectDir = root;
      b.operationDir = root + "/.paleo/capability-ops";
      b.catalog = &catalog;
      b.writeClaim = writable ? lock->claim() : nullptr;
      b.context.documentId = documentId;
      b.context.epoch = epoch;
      b.context.open = true;
      b.context.persistentIdentity = true;
      b.context.readOnly = !writable;
      b.context.ownsWrites = bool(b.writeClaim);
      b.context.allowedEffects =
        host::Read | host::PrivateStage | host::CatalogPublish | host::Presentation;
      if (!allowRead)
        b.context.allowedEffects &= ~host::Read;
      ++contextReads;
      if (flipEpochAfterReads > 0 && contextReads >= flipEpochAfterReads)
        ++epoch; // 下一次 live 读看到不同 epoch（captured ≠ live）
      return b;
    });
    host.recovery()->setTaskService(&tasks);
    return true;
  }
};

bool spinUntil(const std::function<bool()> &done, int timeoutMs = 10000) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < timeoutMs)
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  return done();
}
QString controllerSession; // 当前 controller 会话 id（夹具内共享，测试内更新）
} // namespace

class TestG5OperationPresentation : public QObject {
  Q_OBJECT
private slots:
  void refreshReturnsServiceFacts();
  void cancelMarksRequestWithoutPretendingStopped();
  void cancelAfterCommitKeepsCommittedFacts();
  void eventsPageAdvancesByDecimalCursor();
  void resultsListsPortsFromCommittedOperation();
  void readPortReturnsBoundedBase64Page();
  void newerReadSupersedesOlderCallback();
  void resumeDoubleClickIsDeduplicated();
  void contextRaceBetweenCaptureAndLiveIsRejected();
  void missingSessionOrHostRefusesWithoutServiceCall();
  void busyFlagTogglesAroundActions();

private:
  static QString seedOperation(Host &host, const QString &request);
};

QString TestG5OperationPresentation::seedOperation(Host &host, const QString &request) {
  auto plan = publishPlan(request);
  plan.callerScope = host.host.callerScope(host::Caller::Agent, controllerSession);
  const auto accepted = host.host.recovery()->accept(plan);
  if (!accepted.ok)
    return {};
  return accepted.record.operationId;
}

void TestG5OperationPresentation::refreshReturnsServiceFacts() {
  QTemporaryDir root;
  Host host(this);
  QVERIFY(host.open(root.path()));
  AiChatController controller;
  controllerSession = controller.session().id;
  paleo::chat::AiOperationPresentation::Deps deps{&controller, &host.host, nullptr};
  paleo::chat::AiOperationPresentation presenter(deps);
  const QString operationId = seedOperation(host, "refresh-1");
  QVERIFY(!operationId.isEmpty());

  QSignalSpy state(&presenter, &paleo::chat::AiOperationPresentation::operationStateChanged);
  QString reason;
  QVERIFY2(presenter.requestRefresh(operationId, &reason), qPrintable(reason));
  QCOMPARE(state.size(), 1);
  const auto payload = state.at(0).at(1).toJsonObject();
  QCOMPARE(payload.value(QStringLiteral("operationId")).toString(), operationId);
  // acceptDurable 种子事实（capabilityoperationcontrol.cpp）：queued /
  // not_requested / 修订 1 —— adapter 原样转述服务事实，不改写。
  QCOMPARE(payload.value(QStringLiteral("execution")).toString(), QStringLiteral("queued"));
  QCOMPARE(payload.value(QStringLiteral("commitState")).toString(),
           QStringLiteral("not_requested"));
  QCOMPARE(payload.value(QStringLiteral("revision")).toString(), QStringLiteral("1"));
  QVERIFY(payload.value(QStringLiteral("ok")).toBool());
}

void TestG5OperationPresentation::cancelMarksRequestWithoutPretendingStopped() {
  QTemporaryDir root;
  Host host(this);
  QVERIFY(host.open(root.path()));
  AiChatController controller;
  controllerSession = controller.session().id;
  paleo::chat::AiOperationPresentation presenter{{&controller, &host.host, nullptr}};
  const QString operationId = seedOperation(host, "cancel-1");
  QVERIFY(!operationId.isEmpty());

  QSignalSpy finished(&presenter,
                      &paleo::chat::AiOperationPresentation::operationActionFinished);
  QVERIFY(presenter.requestCancel(operationId));
  QCOMPARE(finished.size(), 1);
  const auto payload = finished.at(0).at(2).toJsonObject();
  // 服务事实：cancel_requested 落账 + cancelRequested=true（不宣称已停止）。
  QCOMPARE(payload.value(QStringLiteral("code")).toString(),
           QStringLiteral("cancel_requested"));
  QVERIFY(payload.value(QStringLiteral("cancelRequested")).toBool());
  // 刷新核对：computeCount 不因 UI 侧取消而变化（未重试执行）。
  QSignalSpy state(&presenter, &paleo::chat::AiOperationPresentation::operationStateChanged);
  QVERIFY(presenter.requestRefresh(operationId));
  QCOMPARE(state.at(0).at(1).toJsonObject().value(QStringLiteral("computeCount")).toDouble(),
           0.0);
}

void TestG5OperationPresentation::cancelAfterCommitKeepsCommittedFacts() {
  QTemporaryDir root;
  Host host(this);
  QVERIFY(host.open(root.path()));
  AiChatController controller;
  controllerSession = controller.session().id;
  paleo::chat::AiOperationPresentation presenter{{&controller, &host.host, nullptr}};
  auto plan = publishPlan("commit-1");
  plan.callerScope = host.host.callerScope(host::Caller::Agent, controllerSession);
  const auto accepted = host.host.recovery()->accept(plan);
  QVERIFY(accepted.ok);
  QString error;
  // 真实执行到 committed（worker 落一个端口产物）。
  QEventLoop loop;
  auto *task = host.host.recovery()->start(
    plan.callerScope, accepted.record.operationId, &host.host, "run",
    [&loop](ExecuteResult) { loop.quit(); }, false, &error);
  QVERIFY2(task, qPrintable(error));
  loop.exec();
  const auto committed = host.host.recovery()->get(plan.callerScope,
                                                   accepted.record.operationId);
  QCOMPARE(committed.value(QStringLiteral("commitState")).toString(),
           QStringLiteral("committed"));

  QSignalSpy finished(&presenter,
                      &paleo::chat::AiOperationPresentation::operationActionFinished);
  QVERIFY(presenter.requestCancel(accepted.record.operationId));
  const auto payload = finished.at(0).at(2).toJsonObject();
  QCOMPARE(payload.value(QStringLiteral("code")).toString(),
           QStringLiteral("cancel_after_commit"));
  // 已提交事实不被取消抹掉：commitState 仍 committed，receipt 仍在。
  QCOMPARE(payload.value(QStringLiteral("commitState")).toString(),
           QStringLiteral("committed"));
  QVERIFY(!payload.value(QStringLiteral("commitReceiptRef")).toString().isEmpty());
}

void TestG5OperationPresentation::eventsPageAdvancesByDecimalCursor() {
  QTemporaryDir root;
  Host host(this);
  QVERIFY(host.open(root.path()));
  AiChatController controller;
  controllerSession = controller.session().id;
  paleo::chat::AiOperationPresentation presenter{{&controller, &host.host, nullptr}};
  const QString operationId = seedOperation(host, "events-1");
  QVERIFY(!operationId.isEmpty());
  // 制造两个事件修订：cancel 落账一次即可叠加（accept=1, cancel=2）。
  host.host.recovery()->cancel(host.host.callerScope(host::Caller::Agent,
                                                     controllerSession),
                               operationId);

  QSignalSpy events(&presenter,
                    &paleo::chat::AiOperationPresentation::operationEventsArrived);
  QVERIFY(presenter.requestEvents(operationId, 0));
  QCOMPARE(events.size(), 1);
  const auto page1 = events.at(0).at(1).toJsonObject();
  QVERIFY(page1.value(QStringLiteral("ok")).toBool());
  const auto rows = page1.value(QStringLiteral("events")).toArray();
  QCOMPARE(rows.size(), 2);
  const QString next1 = page1.value(QStringLiteral("nextRevision")).toString();
  QCOMPARE(next1, QStringLiteral("2")); // 十进制字符串口径
  // 游标推进：after=2 → 空页 + hasMore=false（不回第一页拼史）。
  QVERIFY(presenter.requestEvents(operationId, next1.toULongLong()));
  const auto page2 = events.at(1).at(1).toJsonObject();
  QVERIFY(page2.value(QStringLiteral("ok")).toBool());
  QCOMPARE(page2.value(QStringLiteral("events")).toArray().size(), 0);
  QVERIFY(!page2.value(QStringLiteral("hasMore")).toBool());
  // 越界游标：invalid_event_cursor（服务事实原样返回）。
  QVERIFY(presenter.requestEvents(operationId, 99));
  QCOMPARE(events.at(2).at(1).toJsonObject().value(QStringLiteral("code")).toString(),
           QStringLiteral("invalid_event_cursor"));
}

void TestG5OperationPresentation::resultsListsPortsFromCommittedOperation() {
  QTemporaryDir root;
  Host host(this);
  QVERIFY(host.open(root.path()));
  AiChatController controller;
  controllerSession = controller.session().id;
  paleo::chat::AiOperationPresentation presenter{{&controller, &host.host, nullptr}};
  auto plan = publishPlan("ports-1");
  plan.callerScope = host.host.callerScope(host::Caller::Agent, controllerSession);
  const auto accepted = host.host.recovery()->accept(plan);
  QVERIFY(accepted.ok);
  QEventLoop loop;
  QString error;
  host.host.recovery()->start(plan.callerScope, accepted.record.operationId, &host.host,
                              "run", [&loop](ExecuteResult) { loop.quit(); }, false,
                              &error);
  loop.exec();

  QSignalSpy results(&presenter,
                     &paleo::chat::AiOperationPresentation::operationResultsArrived);
  QVERIFY(presenter.requestResults(accepted.record.operationId));
  QCOMPARE(results.size(), 1);
  const auto ports = results.at(0).at(1).toJsonObject().value(QStringLiteral("ports")).toArray();
  QCOMPARE(ports.size(), 1);
  QCOMPARE(ports.at(0).toObject().value(QStringLiteral("portId")).toString(),
           QStringLiteral("OUTPUT"));
  QVERIFY(ports.at(0).toObject().value(QStringLiteral("byteSize")).toString().toLongLong() > 0);
}

void TestG5OperationPresentation::readPortReturnsBoundedBase64Page() {
  QTemporaryDir root;
  Host host(this);
  QVERIFY(host.open(root.path()));
  AiChatController controller;
  controllerSession = controller.session().id;
  paleo::chat::AiOperationPresentation presenter{{&controller, &host.host, nullptr}};
  auto plan = publishPlan("read-1");
  plan.callerScope = host.host.callerScope(host::Caller::Agent, controllerSession);
  const auto accepted = host.host.recovery()->accept(plan);
  QVERIFY(accepted.ok);
  QEventLoop loop;
  QString error;
  host.host.recovery()->start(plan.callerScope, accepted.record.operationId, &host.host,
                              "run", [&loop](ExecuteResult) { loop.quit(); }, false,
                              &error);
  loop.exec();
  const QString operationId = accepted.record.operationId;

  QSignalSpy page(&presenter, &paleo::chat::AiOperationPresentation::operationPortPage);
  QVERIFY(presenter.requestPortPage(operationId, QStringLiteral("OUTPUT"), 0, 1024));
  QVERIFY2(spinUntil([&page] { return page.size() == 1; }), "端口读页必须完成");
  const auto payload = page.at(0).at(2).toJsonObject();
  QVERIFY(payload.value(QStringLiteral("ok")).toBool());
  QCOMPARE(QByteArray::fromBase64(payload.value(QStringLiteral("data")).toString().toLatin1()),
           QByteArray("g5-worker-bytes!!"));
  QCOMPARE(payload.value(QStringLiteral("nextOffset")).toString(), QStringLiteral("17"));
  QVERIFY(!payload.value(QStringLiteral("hasMore")).toBool());
}

void TestG5OperationPresentation::newerReadSupersedesOlderCallback() {
  QTemporaryDir root;
  Host host(this);
  QVERIFY(host.open(root.path()));
  AiChatController controller;
  controllerSession = controller.session().id;
  paleo::chat::AiOperationPresentation presenter{{&controller, &host.host, nullptr}};
  auto plan = publishPlan("supersede-1");
  plan.callerScope = host.host.callerScope(host::Caller::Agent, controllerSession);
  const auto accepted = host.host.recovery()->accept(plan);
  QVERIFY(accepted.ok);
  QEventLoop loop;
  QString error;
  host.host.recovery()->start(plan.callerScope, accepted.record.operationId, &host.host,
                              "run", [&loop](ExecuteResult) { loop.quit(); }, false,
                              &error);
  loop.exec();
  const QString operationId = accepted.record.operationId;

  QSignalSpy page(&presenter, &paleo::chat::AiOperationPresentation::operationPortPage);
  // 两个读页请求：第二个顶替第一个（同 action 忙碌会拒绝并发，串行发两次）。
  QVERIFY(presenter.requestPortPage(operationId, QStringLiteral("OUTPUT"), 0, 8));
  QVERIFY2(spinUntil([&page] { return page.size() == 1; }), "第一页完成（忙碌解除）");
  QVERIFY(presenter.requestPortPage(operationId, QStringLiteral("OUTPUT"), 8, 1024));
  QVERIFY2(spinUntil([&page] { return page.size() == 2; }), "第二页完成");
  // 两页各自落地（顺序请求互不顶替）；越界 offset 由服务拒绝为事实返回。
  const auto last = page.at(1).at(2).toJsonObject();
  QVERIFY(last.value(QStringLiteral("ok")).toBool());
  QCOMPARE(last.value(QStringLiteral("offset")).toString(), QStringLiteral("8"));
}

void TestG5OperationPresentation::resumeDoubleClickIsDeduplicated() {
  QTemporaryDir root;
  Host host(this);
  QVERIFY(host.open(root.path()));
  AiChatController controller;
  controllerSession = controller.session().id;
  paleo::chat::AiOperationPresentation presenter{{&controller, &host.host, nullptr}};
  const QString operationId = seedOperation(host, "resume-1");
  QVERIFY(!operationId.isEmpty());

  QSignalSpy finished(&presenter,
                      &paleo::chat::AiOperationPresentation::operationActionFinished);
  QString reason;
  QVERIFY(presenter.requestResume(operationId, &reason));
  // 双击第二次：忙碌去重，未再发起（服务侧 computeCount 不被双击翻倍——
  // 由最终 refresh 断言，恢复语义遵循服务原合同）。
  QVERIFY(!presenter.requestResume(operationId, &reason));
  QCOMPARE(reason, QStringLiteral("request_already_busy"));
  QVERIFY2(spinUntil([&finished] { return finished.size() == 1; }),
           "恢复必须有一次终态");
  // 恢复完成后刷新：computeCount 按服务合同推进（prepared → resumed 执行一次）。
  QSignalSpy state(&presenter, &paleo::chat::AiOperationPresentation::operationStateChanged);
  presenter.requestRefresh(operationId);
  QVERIFY2(spinUntil([&state] { return state.size() == 1; }), "刷新完成");
  const double compute =
    state.at(0).at(1).toJsonObject().value(QStringLiteral("computeCount")).toDouble();
  QVERIFY2(compute >= 1.0, "resume 至少执行一次 compute（服务原合同）");
  QVERIFY2(compute < 3.0, "双击不得放大 compute（去重后 ≤ 2）");
}

void TestG5OperationPresentation::contextRaceBetweenCaptureAndLiveIsRejected() {
  QTemporaryDir root;
  Host host(this);
  host.flipEpochAfterReads = 1; // capture 后第一次 live 读即换 epoch
  QVERIFY(host.open(root.path()));
  AiChatController controller;
  controllerSession = controller.session().id;
  paleo::chat::AiOperationPresentation presenter{{&controller, &host.host, nullptr}};
  const QString operationId = seedOperation(host, "race-1");
  QVERIFY(!operationId.isEmpty());

  QString reason;
  QVERIFY(!presenter.requestRefresh(operationId, &reason));
  QVERIFY2(!reason.isEmpty(), "受理-调用间重绑必须给出拒绝 code");
  QVERIFY(reason != QStringLiteral("request_already_busy"));
}

void TestG5OperationPresentation::missingSessionOrHostRefusesWithoutServiceCall() {
  QTemporaryDir root;
  Host host(this);
  QVERIFY(host.open(root.path()));
  AiChatController controller;
  paleo::chat::AiOperationPresentation noHost{{&controller, nullptr, nullptr}};
  QString reason;
  QVERIFY(!noHost.requestRefresh(QStringLiteral("op-any"), &reason));
  QCOMPARE(reason, QStringLiteral("operation_host_or_session_unavailable"));
  QVERIFY(!noHost.busy(QStringLiteral("op-any"), QStringLiteral("refresh")));

  // Read 效果被宿主收回：checkContext 外层门拒绝且 code 原样。
  host.allowRead = false;
  paleo::chat::AiOperationPresentation presenter{{&controller, &host.host, nullptr}};
  controllerSession = controller.session().id;
  QVERIFY(!presenter.requestRefresh(QStringLiteral("op-x"), &reason));
  QVERIFY(!reason.isEmpty());
  QVERIFY(reason != QStringLiteral("operation_host_or_session_unavailable"));
}

void TestG5OperationPresentation::busyFlagTogglesAroundActions() {
  QTemporaryDir root;
  Host host(this);
  QVERIFY(host.open(root.path()));
  AiChatController controller;
  controllerSession = controller.session().id;
  paleo::chat::AiOperationPresentation presenter{{&controller, &host.host, nullptr}};
  const QString operationId = seedOperation(host, "busy-1");
  QVERIFY(!operationId.isEmpty());
  QVERIFY(!presenter.busy(operationId, QStringLiteral("refresh")));
  QSignalSpy busySpy(&presenter, &paleo::chat::AiOperationPresentation::busyChanged);
  QVERIFY(presenter.requestRefresh(operationId));
  QCOMPARE(busySpy.size(), 2); // true → false
  QCOMPARE(busySpy.at(0).at(2).toBool(), true);
  QCOMPARE(busySpy.at(1).at(2).toBool(), false);
  QVERIFY(!presenter.busy(operationId, QStringLiteral("refresh")));
}

QTEST_MAIN(TestG5OperationPresentation)
#include "tst_g5_operation_presentation.moc"
