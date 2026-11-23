// 层：测试壳
// H4/tst_h4_operation_list_tool — paleo.operation_list 的真实 runner 链：
// 工具表可发现、严格实参档、list→得到 ID→显式 get 的生产闭环与否定参数。
// 全部经 AiChatToolRunner/CapabilityHost/CapabilityRecovery 生产入口，不直访
// 私有快照；oracle 用业务 worker 调用计数（发现自身为零）与 journal 字节。
#include <QtTest>
#include "../src/ai/chat/chatmessage.h"
#include "../src/ai/chat/domaintools.h"
#include "../src/workflow/aichattoolrunner.h"
#include "../src/workflow/capabilityhost.h"
#include "../src/catalog/datacatalog.h"
#include "../src/metadata/projectlock.h"
#include "../src/services/paleotaskservice.h"
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>

using namespace paleo;
using namespace paleo::capability;

namespace {
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString logDir(const QString &root) { return QDir(root).filePath(".paleo/capability-ops"); }

OperationWorker countingWorker(std::shared_ptr<int> calls) {
  return [calls](const OperationWorkContext &c) {
    ++*calls;
    OperationWorkResult r;
    QFile file(c.privateDir + "/values.bin");
    r.ok = file.open(QIODevice::WriteOnly) && file.write("h4-tool-bytes") == 13;
    file.close();
    r.outputs.ok = r.ok;
    r.outputs.artifacts.append({"OUTPUT", file.fileName(), {}, {}, {}, {}});
    return r;
  };
}
FrozenOperation plan(const QString &request, const OperationWorker &worker, const QString &scope) {
  FrozenOperation p;
  p.capabilityId = "h4.compute";
  p.implementationVersion = "1";
  p.callerScope = scope;
  p.clientRequestId = request;
  p.intent = {{"number", 1}};
  p.frozenPlan = {{"immutable", true}};
  p.phase = "publish";
  p.worker = worker;
  p.outputSpec.id = "h4:values";
  p.outputSpec.complete = true;
  AlgoPort port;
  port.id = "OUTPUT";
  port.kind = "file";
  p.outputSpec.outputs = {port};
  p.outputPorts = {"OUTPUT"};
  return p;
}

// 宿主 + runner 装配（生产形态：requireCapabilityHost + 可信 Agent 作用域）。
struct ToolHost {
  QTemporaryDir root;
  DataCatalog catalog;
  std::unique_ptr<ProjectDirLock> lock;
  PaleoTaskService tasks;
  CapabilityHost host;
  AiChatToolRunner runner;
  QString session = "chat-" + uuid();
  int businessRuns = 0;
  std::shared_ptr<int> calls = std::make_shared<int>(0);
  ToolHost() : host(NativeAdapterFactory([](const HostBinding &) {
    // 直通校验器：宿主 validateFrozen 恒接线（未注册 adapter 的 capability
    // 一律拒绝）；数值 worker 走 recovery runtime hooks，与生产 restore 同路径。
    CapabilityExecutionAdapter adapter;
    adapter.validateFrozen = [](const OperationLog::Record &) { return QString(); };
    adapter.restoreInputValidator = [](const OperationLog::Record &, QString *) { return OperationInputValidator{}; };
    return adapter;
  }), nullptr) {
    lock = std::make_unique<ProjectDirLock>(root.path());
    QVERIFY2(lock->tryLock(), "project lock");
    QVERIFY2(catalog.open(root.path()), "catalog open");
    host.setTaskService(&tasks);
    host.setCallerScope(paleo::host::Caller::Agent, QStringLiteral("agent:h4"));
    host.setBindingProvider([this] {
      HostBinding binding;
      binding.context.open = true;
      binding.context.documentId = QStringLiteral("doc");
      binding.context.epoch = 1;
      binding.context.persistentIdentity = true;
      binding.context.readOnly = false;
      binding.context.ownsWrites = true;
      binding.context.allowedEffects = host::Read | host::PrivateStage | host::CatalogPublish | host::Presentation;
      binding.projectDir = root.path();
      binding.operationDir = logDir(root.path());
      binding.catalog = &catalog;
      binding.writeClaim = lock->claim();
      return binding;
    });
    AiChatToolContext context;
    context.capabilityHost = &host;
    context.requireCapabilityHost = true;
    context.documentId = QStringLiteral("doc");
    context.documentEpoch = 1;
    context.projectDir = root.path();
    runner.setContext(context);
    runner.setTaskService(&tasks);
    runner.setResultSession(session);
  }
  QString scope() const { return host.callerScope(paleo::host::Caller::Agent, session); }
  bool createCommittedOperation(QString *operationId, QString *error) {
    const auto accepted = host.recovery()->accept(plan(uuid(), countingWorker(calls), scope()));
    if (!accepted.ok) {
      *error = accepted.code;
      return false;
    }
    bool done = false;
    ExecuteResult result;
    auto *task = host.recovery()->start(accepted.record.callerScope, accepted.record.operationId, &host,
        QStringLiteral("h4"), [&](ExecuteResult r) { result = r; done = true; }, false, error);
    if (!task)
      return false;
    QEventLoop loop;
    QObject::connect(task, &PaleoTask::finished, &loop, &QEventLoop::quit);
    loop.exec();
    for (int i = 0; i < 500 && !done; ++i) {
      QCoreApplication::processEvents();
      QThread::msleep(1);
    }
    if (!done || !result.ok) {
      *error = result.code;
      return false;
    }
    *operationId = accepted.record.operationId;
    return true;
  }
  bool runTool(const QString &name, const QString &arguments, bool *ok, QJsonObject *payload) {
    QSignalSpy finished(&runner, &AiChatToolRunner::toolFinished);
    runner.run({QStringLiteral("call-") + uuid(), name, arguments});
    for (int i = 0; i < 500 && finished.isEmpty(); ++i) {
      QCoreApplication::processEvents();
      QThread::msleep(1);
    }
    if (finished.isEmpty())
      return false;
    const auto record = finished.takeFirst();
    *ok = record.at(1).toBool();
    *payload = QJsonDocument::fromJson(record.at(2).toString().toUtf8()).object();
    return true;
  }
};
} // namespace

class H4OperationListTool : public QObject {
  Q_OBJECT
private slots:
  void toolDeclaredAndDispatchable();
  void listGetChainThroughRealRunner();
  void negativeArgumentsFailClosed();
  void readinessGates();
  void invalidationDropsInFlightResult();
  void emptySessionIsRefusedBeforeAnyTask();
};

void H4OperationListTool::toolDeclaredAndDispatchable() {
  bool found = false;
  for (const auto &spec : builtinAiToolSpecs()) {
    if (spec.name != QLatin1String("paleo.operation_list"))
      continue;
    found = true;
    QCOMPARE(spec.effects, unsigned(paleo::host::Read));
    QStringList required;
    for (const auto &parameter : spec.parameters)
      if (parameter.required)
        required.append(parameter.name);
    QCOMPARE(required, QStringList()); // 唯一不要求已知 operation_id（反向钉住：确实无必填项）
    const auto schema = QJsonDocument::fromJson(spec.resultSchemaJson.toUtf8()).object();
    const auto properties = schema.value(QStringLiteral("properties")).toObject();
    // 结果 schema 与服务实际返回一致：清单载荷不是单条操作，不声明 operationId。
    QVERIFY(!properties.contains(QStringLiteral("operationId")));
    for (const auto *key : {"anchorOperationId", "coverage", "minimumBytes", "note", "nextCursor"})
      QVERIFY2(properties.contains(QLatin1String(key)), key);
    const auto dispatch = dispatchAiTool(QStringLiteral("paleo.operation_list"), {});
    QCOMPARE(dispatch.status, AiToolDispatchStatus::Routed);
    // 严格外部档：合法实参通过，坏枚举/坏类型拒绝。
    QCOMPARE(spec.validateExternalArguments({{QStringLiteral("view"), QStringLiteral("relations")}}), QString());
    // 词表封闭在宽松档（validateParameters，经 dispatchAiTool 执行）；
    // 严格档管 integer/number 表示与数值域。
    QVERIFY(!spec.validateParameters({{QStringLiteral("view"), QStringLiteral("everything")}}).isEmpty());
    QVERIFY(!spec.validateExternalArguments({{QStringLiteral("page_size"), QStringLiteral("16")}}).isEmpty());
    QVERIFY(!spec.validateExternalArguments({{QStringLiteral("page_size"), 0}}).isEmpty());
    QVERIFY(!spec.validateExternalArguments({{QStringLiteral("page_size"), 33}}).isEmpty());
  }
  QVERIFY(found);
}

void H4OperationListTool::listGetChainThroughRealRunner() {
  ToolHost host;
  QString operationId, error;
  QVERIFY2(host.createCommittedOperation(&operationId, &error), qPrintable(error));
  QCOMPARE(*host.calls, 1);
  // 模型链：list（无已知 ID）→ 得到 ID → 显式 get。
  bool ok = false;
  QJsonObject payload;
  QVERIFY(host.runTool(QStringLiteral("paleo.operation_list"), QStringLiteral("{}"), &ok, &payload));
  QVERIFY2(ok, qPrintable(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact))));
  QStringList ids;
  for (const auto &entry : payload.value(QStringLiteral("operations")).toArray())
    ids.append(entry.toObject().value(QStringLiteral("operationId")).toString());
  QCOMPARE(ids, QStringList{operationId});
  QCOMPARE(payload.value(QStringLiteral("coverage")).toObject().value(QStringLiteral("sort")).toString(),
           QStringLiteral("operationId-lex@1"));
  const auto callJson = [](const QJsonObject &object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
  };
  QVERIFY(host.runTool(QStringLiteral("paleo.operation_get"),
                       callJson({{QStringLiteral("operation_id"), operationId}}), &ok, &payload));
  QVERIFY2(ok, qPrintable(payload.value(QStringLiteral("code")).toString()));
  QCOMPARE(payload.value(QStringLiteral("operationId")).toString(), operationId);
  QCOMPARE(payload.value(QStringLiteral("commitState")).toString(), QStringLiteral("committed"));
  // results：从清单拿到的 ID 解析出准确端口。
  QVERIFY(host.runTool(QStringLiteral("paleo.operation_results"),
                       callJson({{QStringLiteral("operation_id"), operationId}}), &ok, &payload));
  QVERIFY2(ok, qPrintable(payload.value(QStringLiteral("code")).toString()));
  QCOMPARE(payload.value(QStringLiteral("ports")).toArray().size(), 1);
  QCOMPARE(payload.value(QStringLiteral("ports")).toArray().at(0).toObject().value(QStringLiteral("portId")).toString(),
           QStringLiteral("OUTPUT"));
  // read：字节页读回的正是业务 worker 落盘的内容（SHA 锚定认证链路不变）。
  QVERIFY(host.runTool(QStringLiteral("paleo.operation_read"),
                       callJson({{QStringLiteral("operation_id"), operationId},
                                 {QStringLiteral("port_id"), QStringLiteral("OUTPUT")},
                                 {QStringLiteral("offset"), QStringLiteral("0")},
                                 {QStringLiteral("max_bytes"), 64}}),
                       &ok, &payload));
  QVERIFY2(ok, qPrintable(payload.value(QStringLiteral("code")).toString()));
  QCOMPARE(QByteArray::fromBase64(payload.value(QStringLiteral("data")).toString().toLatin1()),
           QByteArrayLiteral("h4-tool-bytes"));
  // 发现与查询自身零业务执行：worker 未被再次调用。
  QCOMPARE(*host.calls, 1);
  // 关系视图（真实 runner + anchor）。
  QVERIFY(host.runTool(QStringLiteral("paleo.operation_list"),
                       QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("view"), QStringLiteral("relations")},
                                                                   {QStringLiteral("anchor_operation_id"), operationId}})
                                             .toJson(QJsonDocument::Compact)),
                       &ok, &payload));
  QVERIFY2(ok, qPrintable(payload.value(QStringLiteral("code")).toString()));
  QCOMPARE(payload.value(QStringLiteral("relations")).toArray().size(), 0);
  QCOMPARE(payload.value(QStringLiteral("anchorOperationId")).toString(), operationId);
}

void H4OperationListTool::negativeArgumentsFailClosed() {
  ToolHost host;
  bool ok = true;
  QJsonObject payload;
  // 否定参数全部在工具分派/严格档/宿主准入层被拒：没有任何一个走到建任务。
  QSignalSpy added(&host.tasks, &PaleoTaskService::taskAdded);
  // 坏视图 / 越界页预算 / 文本页预算（严格档）/ 坏锚点：任务/磁盘读取前拒绝。
  QVERIFY(host.runTool(QStringLiteral("paleo.operation_list"),
                       QStringLiteral("{\"view\":\"everything\"}"), &ok, &payload));
  QVERIFY(!ok);
  // 词表外取值在 dispatch 档被拒（Disabled 路径给 error 文案）。
  QVERIFY(!payload.value(QStringLiteral("error")).toString().isEmpty());
  QVERIFY(host.runTool(QStringLiteral("paleo.operation_list"), QStringLiteral("{\"page_size\":0}"), &ok, &payload));
  QVERIFY(!ok);
  QVERIFY(host.runTool(QStringLiteral("paleo.operation_list"), QStringLiteral("{\"page_size\":\"16\"}"), &ok, &payload));
  QVERIFY(!ok);
  QVERIFY(host.runTool(QStringLiteral("paleo.operation_list"),
                       QStringLiteral("{\"view\":\"relations\"}"), &ok, &payload));
  QVERIFY(!ok);
  QCOMPARE(payload.value(QStringLiteral("code")).toString(), QStringLiteral("invalid_anchor_operation"));
  QVERIFY(host.runTool(QStringLiteral("paleo.operation_list"), QStringLiteral("{\"cursor\":\"forged\"}"), &ok, &payload));
  QVERIFY(!ok);
  QCOMPARE(payload.value(QStringLiteral("code")).toString(), QStringLiteral("cursor_invalid"));
  QCOMPARE(added.size(), 0);
}

void H4OperationListTool::emptySessionIsRefusedBeforeAnyTask() {
  ToolHost host;
  QSignalSpy added(&host.tasks, &PaleoTaskService::taskAdded);
  // 无当前聊天会话：没有可信 callerScope 可用——在取 scope 之前拒绝，不建任务。
  host.runner.setResultSession(QString());
  bool ok = true;
  QJsonObject payload;
  QVERIFY(host.runTool(QStringLiteral("paleo.operation_list"), QStringLiteral("{}"), &ok, &payload));
  QVERIFY(!ok);
  QCOMPARE(payload.value(QStringLiteral("code")).toString(),
           QStringLiteral("operation_host_or_session_unavailable"));
  QCOMPARE(added.size(), 0);
}

void H4OperationListTool::readinessGates() {
  ToolHost host;
  // 正常绑定：资格通过且出现在枚举里。
  QVERIFY(host.runner.readiness(QStringLiteral("paleo.operation_list")).ready);
  bool listed = false;
  for (const auto &spec : host.runner.availableTools())
    if (spec.name == QLatin1String("paleo.operation_list"))
      listed = true;
  QVERIFY(listed);
  // 无 TaskService：精确资格失败（扫描是异步任务）。
  AiChatToolRunner bare;
  AiChatToolContext context;
  context.capabilityHost = &host.host;
  context.requireCapabilityHost = true;
  context.documentId = QStringLiteral("doc");
  context.documentEpoch = 1;
  context.projectDir = host.root.path();
  bare.setContext(context);
  const auto ready = bare.readiness(QStringLiteral("paleo.operation_list"));
  QVERIFY(!ready.ready);
  QCOMPARE(ready.code, QStringLiteral("task_service_unavailable"));
  // 无宿主：host_unavailable。
  AiChatToolRunner orphan;
  AiChatToolContext emptyContext;
  emptyContext.requireCapabilityHost = true;
  orphan.setContext(emptyContext);
  QCOMPARE(orphan.readiness(QStringLiteral("paleo.operation_list")).code, QStringLiteral("host_unavailable"));
  // legacy 适配器上下文（未要求 CapabilityHost、无宿主）：其余 operation_*
  // 沿既有口径被广播，但 operation_list 依赖 Host/Recovery/TaskService——
  // 不经 legacy 无条件放行，既不广播也不可分派。
  AiChatToolRunner legacy;
  AiChatToolContext legacyContext;
  legacyContext.documentId = QStringLiteral("doc");
  legacy.setContext(legacyContext);
  const auto legacyReady = legacy.readiness(QStringLiteral("paleo.operation_list"));
  QVERIFY(!legacyReady.ready);
  QCOMPARE(legacyReady.code, QStringLiteral("host_unavailable"));
  bool legacyListed = false;
  for (const auto &spec : legacy.availableTools())
    if (spec.name == QLatin1String("paleo.operation_list"))
      legacyListed = true;
  QVERIFY(!legacyListed);
  // 对照：同上下文里既有 operation_get 仍按旧口径就绪（本任务不改其语义）。
  QVERIFY(legacy.readiness(QStringLiteral("paleo.operation_get")).ready);
}

void H4OperationListTool::invalidationDropsInFlightResult() {
  ToolHost host;
  QString operationId, error;
  QVERIFY2(host.createCommittedOperation(&operationId, &error), qPrintable(error));
  // taskAdded 内同步重绑（真实换 projectDir → invalidateContext）：旧摘要
  // 不投递——帧按 stale_session 单帧终态（与既有 tst_aichatcapability 口径
  // 一致），迟到业务结果不回灌，新请求句柄不被旧回调清掉。
  QTemporaryDir other;
  AiChatToolContext rebound;
  {
    AiChatToolContext original;
    original.capabilityHost = &host.host;
    original.requireCapabilityHost = true;
    original.documentId = QStringLiteral("doc");
    original.documentEpoch = 1;
    original.projectDir = host.root.path();
    host.runner.setContext(original); // 复位到已知干净绑定
    rebound = original;
    rebound.projectDir = other.path(); // 变化触发 invalidateContext
  }
  const QMetaObject::Connection added =
      QObject::connect(&host.tasks, &PaleoTaskService::taskAdded, [&](PaleoTask *) { host.runner.setContext(rebound); });
  QSignalSpy finished(&host.runner, &AiChatToolRunner::toolFinished);
  host.runner.run({QStringLiteral("call-x"), QStringLiteral("paleo.operation_list"), QStringLiteral("{}")});
  // 有界等待（不依赖固定 sleep）：终态帧经事件循环投递。
  for (int i = 0; i < 500 && finished.isEmpty(); ++i) {
    QCoreApplication::processEvents();
    QThread::msleep(1);
  }
  QObject::disconnect(added);
  // 与既有口径一致（tst_aichatcapability::contextChangeDuringTaskAddedCancelsTheTask）：
  // 重绑帧恰有一帧终态，按 stale_session 如实回灌，不投递旧摘要。
  QCOMPARE(finished.size(), 1);
  const auto record = finished.takeFirst();
  QVERIFY(!record.at(1).toBool());
  const auto stalePayload = QJsonDocument::fromJson(record.at(2).toString().toUtf8()).object();
  QCOMPARE(stalePayload.value(QStringLiteral("code")).toString(), QStringLiteral("stale_session"));
  // 恢复正常：重新绑定原上下文，list 正常完成（旧回调不清掉新请求句柄）。
  AiChatToolContext restored;
  restored.capabilityHost = &host.host;
  restored.requireCapabilityHost = true;
  restored.documentId = QStringLiteral("doc");
  restored.documentEpoch = 1;
  restored.projectDir = host.root.path();
  host.runner.setContext(restored);
  bool ok = false;
  QJsonObject payload;
  QVERIFY(host.runTool(QStringLiteral("paleo.operation_list"), QStringLiteral("{}"), &ok, &payload));
  QVERIFY2(ok, qPrintable(payload.value(QStringLiteral("code")).toString()));
  QCOMPARE(payload.value(QStringLiteral("returnedCount")).toInt(), 1);
}

QTEST_MAIN(H4OperationListTool)
#include "tst_h4_operation_list_tool.moc"
