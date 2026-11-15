// 层：测试壳
#include "fixtures/pythongeneratedsdkfixture.h"
#include "workflow/aichatcontroller.h"
#include "workflow/capabilityhost.h"
#include "workflow/pythonenvironmentoperation.h"
#include "services/pythonenvironmentregistry.h"
#include "services/pythonanalysiscapabilityspec.h"
#include "services/pythongenerated.h"
#include <QtTest>
#include <QEventLoop>
#include <QHostAddress>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

using namespace paleo;
using namespace paleo::capability;
namespace {
constexpr auto Scope = "agent:pi-generated";
bool work(PaleoTaskService &tasks, std::function<QString(const std::function<bool()> &)> run, QString &code) {
  QEventLoop wait; QTimer deadline; deadline.setSingleShot(true); bool done = false;
  QObject::connect(&deadline, &QTimer::timeout, &wait, &QEventLoop::quit);
  const auto result = std::make_shared<QString>();
  auto *task = tasks.start("Pi fixture prewarm", [run, result](PaleoTask *task) { *result = run([task] { return task->cancelRequested(); }); return *result; });
  if (!task) return false;
  QObject::connect(task, &PaleoTask::finished, &wait, [&] { done = true; wait.quit(); });
  deadline.start(180000); if (!done) wait.exec();
  if (!done) { task->requestInterruption(); tasks.shutdown(15000, true); }
  code = done ? *result : QStringLiteral("prewarm_timeout"); return done && code.isEmpty();
}
class Model : public QObject {
public:
  QTcpServer server; int requests = 0; bool emitTool = true; ChatToolCall next;
  Model() {
    connect(&server, &QTcpServer::newConnection, this, [this] {
      auto *socket = server.nextPendingConnection(); auto bytes = std::make_shared<QByteArray>();
      auto served = std::make_shared<bool>(false);
      connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      connect(socket, &QTcpSocket::readyRead, socket, [this, socket, bytes, served] {
        if (*served) return; bytes->append(socket->read(128 * 1024)); const auto header = bytes->indexOf("\r\n\r\n");
        if (header < 0) return;
        const auto length = QString::fromUtf8(bytes->left(header)).section("Content-Length:", 1, 1, QString::SectionCaseInsensitiveSeps).section('\r', 0, 0).trimmed().toInt();
        if (length < 0 || length > 128 * 1024 || bytes->size() < header + 4 + length) return;
        *served = true; ++requests; const bool call = emitTool; emitTool = false;
        QJsonObject message{{"role", "assistant"}, {"content", call ? "" : "completed through host"}};
        if (call) message["tool_calls"] = QJsonArray{next.toJson()};
        const auto body = QJsonDocument(QJsonObject{{"choices", QJsonArray{QJsonObject{{"message", message}, {"finish_reason", call ? "tool_calls" : "stop"}}}}}).toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
      });
    });
  }
  bool open() { return server.listen(QHostAddress::LocalHost, 0); }
  LlmConfig config() const { return LlmConfig::fromParts(QUrl(QString("http://127.0.0.1:%1/v1").arg(server.serverPort())), "controlled-fixture", "fixture-only-key", false); }
  void call(const QString &name, const QJsonObject &arguments) { emitTool = true; next = {"provider-call", name, QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact))}; }
};
struct Fixture : python::generated_sdk_fixture::Fixture {
  PaleoTaskService tasks; std::shared_ptr<python::PythonEnvironmentRegistry> registry;
  std::unique_ptr<CapabilityHost> host; QThread *owner = QThread::currentThread();
  std::atomic_bool ownerOnly{true}, workerOffOwner{false}; std::atomic_int materializations{0};
  ~Fixture() { host.reset(); tasks.shutdown(15000, true); }
  bool open() { tasks.setMaxWorkerThreads(4); return python::generated_sdk_fixture::Fixture::open(QLatin1String(Scope)); }
  bool environment(bool reload, QString &code) {
    registry = std::make_shared<python::PythonEnvironmentRegistry>(temp.filePath("host-config/python.json"));
    const auto directory = registry; python::EnvironmentCandidate candidate;
    candidate.runtimeRoot = qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_ROOT");
    candidate.runtimeSha256 = qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_MANIFEST_SHA256");
    candidate.policy.allowGenerated = true; candidate.policy.allowTrusted = false;
    return work(tasks, [directory, candidate, reload](const auto &cancelled) {
      if (reload) {
        const auto error = directory->refresh({}, directory->beginRefresh(), cancelled); if (!error.isEmpty()) return error;
      } else {
        const auto activated = directory->activate(candidate, 0, cancelled); if (!activated.ok()) return activated.code;
      }
      const auto selected = directory->active(python::EnvironmentMode::Generated); if (!selected.ok()) return selected.code;
      const auto state = directory->probe(selected.identity.id, python::EnvironmentMode::Generated, cancelled);
      return state.value("ready").toBool() && state.value("runtimeProbeVerified").toBool() ? QString() : state.value("code").toString("probe_failed");
    }, code);
  }
  void rebuildHost() {
    host = std::make_unique<CapabilityHost>(NativeAdapterFactory{}, nullptr); host->setTaskService(&tasks);
    host->setCallerScope(host::Caller::Agent, QLatin1String(Scope));
    host->setBindingProvider([this] {
      HostBinding value; value.catalog = &catalog; value.projectDir = project; value.operationDir = project + "/.paleo/capability-ops"; value.writeClaim = lock->claim();
      value.context.documentId = plan.documentId; value.context.epoch = 1; value.context.open = true; value.context.persistentIdentity = true;
      value.context.readOnly = false; value.context.ownsWrites = true; value.context.allowedEffects = host::Read | host::PrivateStage | host::LocalExecute | host::CatalogPublish | host::Presentation;
      return value;
    });
    auto adapter = python::environmentAnalysisAdapter(python::AnalysisKind::Generated, registry,
      [this](const ExecuteRequest &request, python::AnalysisReadPlan &out, std::function<QString()> &validate) {
        if (QThread::currentThread() != owner) ownerOnly = false; python::AnalysisSelectionRequest parsed; QString code;
        if (!request.parameters.isEmpty() || QJsonObject::fromVariantMap(request.inputs) != selection ||
            !python::analysisSelectionFromJson(selection, python::AnalysisKind::Generated, parsed, &code)) return QString("fixture_selection_mismatch");
        out = plan; out.documentId = request.documentId; out.callerScope = request.callerScope; out.curves = parsed.curves;
        for (auto &curve : out.curves) for (const auto &actual : plan.curves) if (curve.curveId == actual.curveId) curve.source = actual.source;
        validate = [this] { if (QThread::currentThread() != owner) ownerOnly = false;
          return catalog.storedEntityById("generated-well").extra.value("areaModelWellId").toLongLong() == plan.curves.first().wellId ? QString() : QString("well_mapping_changed"); };
        return QString();
      });
    const auto capture = adapter.capture;
    adapter.capture = [this, capture](const auto &request, const auto &intent, QString *error) {
      auto preparation = capture(request, intent, error); const auto materialize = preparation.worker;
      if (materialize) preparation.worker = [this, materialize](const auto &cancelled, QString *error) {
        ++materializations; workerOffOwner = QThread::currentThread() != owner; return materialize(cancelled, error);
      }; return preparation;
    };
    host->registerExecutionAdapter(QLatin1String(python::GeneratedCapability), std::move(adapter));
  }
  void bind(AiChatController &controller, const Model &model) {
    const auto selected = registry->active(python::EnvironmentMode::Generated); controller.toolRunner()->setTaskService(&tasks);
    controller.toolRunner()->setPythonEnvironments(registry); AiChatToolContext context; context.catalog = &catalog;
    context.projectDir = project; context.documentId = plan.documentId; context.documentEpoch = 1; context.capabilityHost = host.get(); context.requireCapabilityHost = true;
    context.pythonRuntime = selected.runtime; context.pythonSandbox = selected.sandbox;
    context.runtimeReadiness = [directory = registry](const QString &name, const QJsonObject &arguments, bool listing) {
      if (name == QLatin1String(python::WellQualityTool) || name == QLatin1String(python::CorrelationTool)) return QString("python_execution_denied");
      if (name != QLatin1String(python::GeneratedTool)) return QString();
      const auto current = directory->active(python::EnvironmentMode::Generated); if (!current.ok()) return current.code;
      if (!listing && arguments.value("publish_intent") == "publish") return QString();
      const auto integrity = current.runtime->snapshot(); if (!integrity.ready()) return integrity.code;
      const auto state = current.sandbox->snapshot(); return state.value("ready").toBool() ? QString() : state.value("code").toString("python_sandbox_unavailable");
    };
    controller.toolRunner()->setContext(context); controller.setConfig(model.config());
    controller.setPiRuntime({true, qEnvironmentVariable("PALEO_TEST_PI_NATIVE_ROOT"), qEnvironmentVariable("PALEO_TEST_PI_NATIVE_SHA256")});
  }
  OperationLog::Record record(const AiChatController &controller, int binding = 0) const {
    const auto id = controller.session().hostToolBindings.at(binding).toObject().value("clientRequestId").toString();
    return host->recovery()->lookup(host->callerScope(host::Caller::Agent, controller.session().id), id).record;
  }
};
QJsonObject lastTool(const AiChatController &controller) {
  const auto messages = controller.messages();
  for (auto message = messages.crbegin(); message != messages.crend(); ++message)
    if (message->role == ChatRole::Tool) return QJsonDocument::fromJson(message->content.toUtf8()).object();
  return {};
}
QJsonObject page(Fixture &fixture, const QString &scope, const QString &operation, qint64 offset) {
  QObject listener; QEventLoop wait; QTimer timer; timer.setSingleShot(true); bool done = false; QJsonObject result; QString error;
  QObject::connect(&timer, &QTimer::timeout, &wait, &QEventLoop::quit);
  auto *task = fixture.host->recovery()->read(scope, operation, "REPORT", offset, 128, &listener, [&](auto value) { result = value; done = true; wait.quit(); }, &error);
  if (!task) return {{"ok", false}, {"code", error}}; timer.start(180000); if (!done) wait.exec();
  if (!done) { task->requestInterruption(); return {{"ok", false}, {"code", "test_timeout"}}; } return result;
}
}
class TestPiGeneratedOperations : public QObject {
  Q_OBJECT
  QStringList sessions;
private slots:
  void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
  void cleanupTestCase() { for (const auto &id : sessions) ChatSessionStore::remove(id, nullptr); }
  void missingHostConfigurationDoesNotGrantGeneratedOrTrustedExecution() {
    QTemporaryDir directory; python::PythonEnvironmentRegistry registry(directory.filePath("isolated/config.json"));
    QVERIFY(!registry.active(python::EnvironmentMode::Generated).ok()); QVERIFY(!registry.active(python::EnvironmentMode::Trusted).ok());
    QVERIFY(!registry.status().value("allowGenerated").toBool()); QVERIFY(!registry.status().value("allowTrusted").toBool());
  }
  void realPiManagedComputePagesRestartPublishAndStableReplay() {
    if (qEnvironmentVariable("PALEO_TEST_PI_NATIVE_ROOT").isEmpty() || qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_ROOT").isEmpty()) {
      QVERIFY2(qEnvironmentVariable("PALEO_REQUIRE_PI_EXECUTION") != "1" && qEnvironmentVariable("PALEO_REQUIRE_AGENT_PYTHON_EXECUTION") != "1", "Required Pi/Python runtime fixture missing"); return;
    }
    Fixture fixture; QVERIFY2(fixture.open(), qPrintable(fixture.error)); QString code;
    QVERIFY2(fixture.environment(false, code), qPrintable(code)); fixture.rebuildHost(); Model model; QVERIFY(model.open());
    auto controller = std::make_unique<AiChatController>(); fixture.bind(*controller, model); const auto session = controller->session().id; sessions.append(session);
    QVERIFY(!fixture.registry->status().value("allowTrusted").toBool()); QVERIFY(fixture.registry->status().value("allowGenerated").toBool());
    QJsonObject arguments{{"client_request_id", "model-must-not-own-this"}, {"binding_mode", "pinned-value"}, {"publish_intent", "compute-only"},
      {"parameters", QJsonObject{{"code", QString::fromUtf8(python::generated_sdk_fixture::sourceCode())}}}, {"inputs", fixture.selection}};
    model.call(QLatin1String(python::GeneratedTool), arguments); QSignalSpy errors(controller.get(), &AiChatController::errorOccurred);
    controller->sendUserText("compute the selected well window means"); QTRY_VERIFY_WITH_TIMEOUT(!controller->streaming(), 180000);
    QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.first().first().toString()));
    QCOMPARE(model.requests, 2); QCOMPARE(controller->session().hostToolBindings.size(), 1);
    auto computed = fixture.record(*controller); QCOMPARE(computed.computeCount, 1); QCOMPARE(computed.execution, QString("succeeded"));
    const auto frozen = computed.frozenRequest.value("plan").toObject().value("plan").toObject();
    QCOMPARE(frozen.value("codeSha256").toString(), QString::fromLatin1(QCryptographicHash::hash(python::generated_sdk_fixture::sourceCode(), QCryptographicHash::Sha256).toHex()));
    QCOMPARE(fixture.catalog.versions().size(), 1); QVERIFY(fixture.ownerOnly.load()); QVERIFY(fixture.workerOffOwner.load());
    const auto materializations = fixture.materializations.load(); model.call(QLatin1String(python::GeneratedTool), arguments);
    controller->retryLast(); QTRY_VERIFY_WITH_TIMEOUT(!controller->streaming(), 180000);
    QVERIFY(errors.isEmpty()); QCOMPARE(fixture.record(*controller).operationId, computed.operationId); QCOMPARE(fixture.materializations.load(), materializations);
    auto changed = arguments; changed["parameters"] = QJsonObject{{"code", QString::fromUtf8(python::generated_sdk_fixture::sourceCode()) + "\n# changed\n"}};
    model.call(QLatin1String(python::GeneratedTool), changed); controller->retryLast(); QTRY_VERIFY_WITH_TIMEOUT(!controller->streaming(), 180000);
    QVERIFY(!errors.isEmpty()); QVERIFY(errors.last().first().toString().contains("pi_invocation_binding_conflict"));
    QCOMPARE(fixture.record(*controller).computeCount, 1); QCOMPARE(fixture.materializations.load(), materializations);
    QTRY_VERIFY_WITH_TIMEOUT(controller->canRetry(), 180000); errors.clear();
    model.call("paleo.operation_read", {{"operation_id", computed.operationId}, {"port_id", "REPORT"}, {"offset", "0"}, {"max_bytes", 128}});
    controller->sendUserText("read the first authenticated report page"); QTRY_VERIFY_WITH_TIMEOUT(!controller->streaming(), 180000); QVERIFY(errors.isEmpty());
    const auto first = lastTool(*controller).value("data").toObject(); QVERIFY(first.value("ok").toBool());
    QByteArray report = QByteArray::fromBase64(first.value("data").toString().toLatin1()); QCOMPARE(report.size(), 128);
    bool more = first.value("hasMore").toBool(); qint64 offset = first.value("nextOffset").toString().toLongLong();
    const auto scope = fixture.host->callerScope(host::Caller::Agent, session);
    for (int index = 0; more && index < 2048; ++index) {
      const auto next = page(fixture, scope, computed.operationId, offset); QVERIFY(next.value("ok").toBool());
      const auto bytes = QByteArray::fromBase64(next.value("data").toString().toLatin1()); QVERIFY(!bytes.isEmpty() && bytes.size() <= 128); report += bytes;
      more = next.value("hasMore").toBool(); offset = next.value("nextOffset").toString().toLongLong();
    }
    QVERIFY(!more); const auto value = QJsonDocument::fromJson(report).object();
    QCOMPARE(value.value("metrics").toArray().at(0).toObject().value("value").toDouble(), 49.5);
    QCOMPARE(value.value("metrics").toArray().at(1).toObject().value("value").toDouble(), 155.5);
    QCOMPARE(value.value("codeSha256").toString(), QString::fromLatin1(QCryptographicHash::hash(python::generated_sdk_fixture::sourceCode(), QCryptographicHash::Sha256).toHex()));
    std::string reason; QVERIFY(fixture.sdk.updateCurveValue(fixture.plan.curves.first().curveId, 0, 9999, reason));
    controller.reset(); fixture.host.reset(); QVERIFY2(fixture.environment(true, code), qPrintable(code)); fixture.rebuildHost();
    controller = std::make_unique<AiChatController>(); sessions.append(controller->session().id); QVERIFY(controller->loadSession(session)); fixture.bind(*controller, model);
    QSignalSpy restartErrors(controller.get(), &AiChatController::errorOccurred);
    const QJsonObject publication{{"client_request_id", "another-model-id"}, {"binding_mode", "pinned-value"}, {"publish_intent", "publish"},
      {"parameters", QJsonObject{}}, {"inputs", QJsonObject{{"source_operation_id", computed.operationId}}}};
    model.call(QLatin1String(python::GeneratedTool), publication);
    controller->sendUserText("publish this frozen report once after host restart"); QTRY_VERIFY_WITH_TIMEOUT(!controller->streaming(), 180000);
    QVERIFY2(restartErrors.isEmpty(), restartErrors.isEmpty() ? "" : qPrintable(restartErrors.first().first().toString()));
    QCOMPARE(lastTool(*controller).value("commitState").toString(), QString("committed")); QCOMPARE(fixture.catalog.versions().size(), 2);
    model.call(QLatin1String(python::GeneratedTool), publication); controller->retryLast(); QTRY_VERIFY_WITH_TIMEOUT(!controller->streaming(), 180000);
    QVERIFY(restartErrors.isEmpty()); QCOMPARE(fixture.catalog.versions().size(), 2);
    OperationLog journal(fixture.project + "/.paleo/capability-ops", fixture.project, fixture.lock->claim()); QCOMPARE(journal.recordById(computed.operationId).computeCount, 1);
    int derived = 0; for (const auto &version : fixture.catalog.versions()) if (version.stage == "DERIVED") { ++derived; QCOMPARE(version.parentVersionIds, QStringList{"generated-raw"}); }
    QCOMPARE(derived, 1); QCOMPARE(fixture.materializations.load(), materializations); QVERIFY(fixture.ownerOnly.load());
  }
};
QTEST_GUILESS_MAIN(TestPiGeneratedOperations)
#include "tst_pigeneratedoperations.moc"
