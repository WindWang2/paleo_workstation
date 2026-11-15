// 层：测试壳
#include "workflow/pythonenvironmentcontroller.h"
#include "fixtures/pythonenvironmentfixture.h"
#include "services/paleotaskservice.h"
#include "services/pythondelivery_files.h"
#include <QtTest>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSemaphore>
#include <QThread>
#include <thread>

using namespace paleo::python;
namespace {
struct Fixture : python_environment_fixture::Fixture {
  PaleoTaskService tasks;
  std::shared_ptr<PythonEnvironmentRegistry> registry;
  explicit Fixture(const QString &templates) : python_environment_fixture::Fixture(templates),
      registry(std::make_shared<PythonEnvironmentRegistry>(configurationPath())) {}
  ~Fixture() { tasks.shutdown(10000, true); }
};
QJsonObject form(const EnvironmentCandidate &candidate) {
  return {{"runtimeRoot", candidate.runtimeRoot}, {"runtimeManifestSha256", candidate.runtimeSha256},
    {"scriptsRoot", candidate.scriptsRoot}, {"scriptsManifestSha256", candidate.scriptsSha256},
    {"allowTrusted", candidate.policy.allowTrusted}, {"allowGenerated", candidate.policy.allowGenerated}};
}
} // namespace

class TestPythonEnvironmentController : public QObject {
  Q_OBJECT
  LegacyPythonEnvironment m_previousLegacy;
private slots:
  void initTestCase() {
    m_previousLegacy = captureLegacyPythonEnvironment();
    for (auto it = m_previousLegacy.begin(); it != m_previousLegacy.end(); ++it) qunsetenv(it.key().toLatin1().constData());
  }
  void cleanupTestCase() {
    for (auto it = m_previousLegacy.begin(); it != m_previousLegacy.end(); ++it) qputenv(it.key().toLatin1().constData(), it.value().toUtf8());
  }
  void loadedPolicyIsPreservedAndProbeDoesNotReplaceDraftRevision() {
    Fixture f(QFINDTESTDATA("../vendor/agent-python/templates")); auto first = f.candidate("first", "fixture", false, false);
    first.policy.cpuSeconds = 3; first.policy.wallSeconds = 5; first.policy.memoryBytes = 128 * 1024 * 1024;
    QVERIFY(f.registry->activate(first, 0).ok()); PythonEnvironmentController controller(f.registry, &f.tasks);
    QSignalSpy forms(&controller, &PythonEnvironmentController::configurationChanged);
    QSignalSpy messages(&controller, &PythonEnvironmentController::messageChanged);
    controller.reload(); QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000); QCOMPARE(forms.size(), 1);
    auto edited = forms.first().first().toJsonObject(); edited["allowGenerated"] = true;
    controller.activate(edited); QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
    const auto approved = f.registry->active(EnvironmentMode::Generated); QVERIFY2(approved.ok(), qPrintable(approved.code));
    QCOMPARE(approved.identity.policy.cpuSeconds, 3); QCOMPARE(approved.identity.policy.wallSeconds, 5);
    QCOMPARE(approved.identity.policy.memoryBytes, qint64(128 * 1024 * 1024));
    auto next = f.candidate("next", "changed", false, false); QVERIFY(f.registry->activate(next, 2).ok());
    const auto before = forms.size(); controller.probe(); QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
    QCOMPARE(forms.size(), before); // Integrity-only disabled probe: no interpreter is started.
    controller.activate(edited); QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
    QCOMPARE(f.registry->configurationSnapshot().value("revision").toString(), QString("3"));
    QCOMPARE(f.registry->active(EnvironmentMode::Generated).identity.runtimeSha256, next.runtimeSha256);
    QVERIFY(messages.last().first().toString().contains(QStringLiteral("当前输入仍保留")));
  }
  void taskAddedCanDestroyOwnerBeforeDispatchWithoutActivating() {
    Fixture f(QFINDTESTDATA("../vendor/agent-python/templates")); const auto first = f.candidate("first");
    QVERIFY(f.registry->activate(first, 0).ok());
    auto *controller = new PythonEnvironmentController(f.registry, &f.tasks); QPointer<PythonEnvironmentController> owner(controller);
    controller->reload(); QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 10000);
    connect(&f.tasks, &PaleoTaskService::taskAdded, this, [&](PaleoTask *) { if (owner) delete owner.data(); });
    controller->activate(form(f.candidate("next", "changed"))); QVERIFY(!owner);
    QTRY_COMPARE_WITH_TIMEOUT(f.tasks.runningCount(), 0, 10000);
    QCOMPARE(f.registry->configurationSnapshot().value("revision").toString(), QString("1"));
  }
  void taskAddedDisableSupersedesOldActivationAndDoesNotResurrect() {
    Fixture f(QFINDTESTDATA("../vendor/agent-python/templates")); QVERIFY(f.registry->activate(f.candidate("first"), 0).ok());
    PythonEnvironmentController controller(f.registry, &f.tasks); controller.reload(); QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
    bool denied = false;
    connect(&f.tasks, &PaleoTaskService::taskAdded, this, [&](PaleoTask *) { if (!denied) { denied = true; controller.disable(); } });
    controller.activate(form(f.candidate("next", "changed"))); QVERIFY(denied);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy() && f.tasks.runningCount() == 0, 10000);
    QVERIFY(f.registry->status().value("emergencyDenied").toBool()); QVERIFY(!f.registry->status().value("enabled").toBool());
    QCOMPARE(f.registry->configurationSnapshot().value("approved").toArray().size(), 1);
  }
  void destroyedTaskServiceDuringAdmissionDoesNotLeaveBusyController() {
    Fixture f(QFINDTESTDATA("../vendor/agent-python/templates")); QVERIFY(f.registry->activate(f.candidate("first"), 0).ok());
    auto *service = new PaleoTaskService; PythonEnvironmentController controller(f.registry, service);
    controller.reload(); QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
    connect(service, &PaleoTaskService::taskAdded, this, [&](PaleoTask *) { delete service; service = nullptr; });
    controller.activate(form(f.candidate("next", "changed"))); QVERIFY(!controller.busy());
    QVERIFY(!service); QCOMPARE(f.registry->configurationSnapshot().value("revision").toString(), QString("1"));
    controller.reload(); QVERIFY(!controller.busy());
  }
  void legacySourceIsProjectedWithoutInventingManagedAnchors() {
    Fixture f(QFINDTESTDATA("../vendor/agent-python/templates")); const auto candidate = f.candidate("runtime", "fixture", false, false);
    const auto cleanup = qScopeGuard([] { for (const auto key : {"PALEO_AGENT_PYTHON_ROOT", "PALEO_AGENT_PYTHON_MANIFEST_SHA256", "PALEO_AGENT_PYTHON_SCRIPTS_ROOT"}) qunsetenv(key); });
    qputenv("PALEO_AGENT_PYTHON_ROOT", candidate.runtimeRoot.toUtf8()); qputenv("PALEO_AGENT_PYTHON_MANIFEST_SHA256", candidate.runtimeSha256.toUtf8());
    qputenv("PALEO_AGENT_PYTHON_SCRIPTS_ROOT", QFINDTESTDATA("../vendor/agent-python/templates").toUtf8());
    PythonEnvironmentController controller(f.registry, &f.tasks);
    QSignalSpy forms(&controller, &PythonEnvironmentController::configurationChanged), projections(&controller, &PythonEnvironmentController::diagnosticsChanged);
    controller.reload(); QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000); QCOMPARE(forms.size(), 1);
    QCOMPARE(projections.last().first().toJsonObject().value("sourceLabel").toString(), QStringLiteral("兼容环境变量"));
    QVERIFY(forms.first().first().toJsonObject().value("scriptsManifestSha256").toString().isEmpty());
    QCOMPARE(controller.redactedDiagnostics().value("status").toObject().value("source").toString(), QString("legacy-env"));
    QVERIFY(!QFileInfo::exists(f.configurationPath()));
  }
  void busySignalCanCancelOrDeleteBeforeTaskAdmission_data() {
    QTest::addColumn<bool>("deleteOwner"); QTest::newRow("cancel") << false; QTest::newRow("delete") << true;
  }
  void busySignalCanCancelOrDeleteBeforeTaskAdmission() {
    QFETCH(bool, deleteOwner); Fixture f(QFINDTESTDATA("../vendor/agent-python/templates"));
    QVERIFY(f.registry->activate(f.candidate("first"), 0).ok());
    auto *controller = new PythonEnvironmentController(f.registry, &f.tasks); QPointer<PythonEnvironmentController> owner(controller);
    controller->reload(); QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 10000); bool once = false;
    connect(controller, &PythonEnvironmentController::busyChanged, this, [&](bool busy) {
      if (busy && !once) { once = true; if (deleteOwner) delete owner.data(); else owner->cancel(); }
    });
    controller->activate(form(f.candidate("next", "changed")));
    QTRY_VERIFY_WITH_TIMEOUT((!owner || !owner->busy()) && f.tasks.runningCount() == 0, 10000);
    QCOMPARE(f.registry->configurationSnapshot().value("revision").toString(), QString("1"));
    delete owner.data();
  }
  void completionReentryDropsOldFormAndMessage() {
    Fixture f(QFINDTESTDATA("../vendor/agent-python/templates")); QVERIFY(f.registry->activate(f.candidate("first"), 0).ok());
    PythonEnvironmentController controller(f.registry, &f.tasks); controller.reload(); QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
    QSignalSpy forms(&controller, &PythonEnvironmentController::configurationChanged), messages(&controller, &PythonEnvironmentController::messageChanged);
    bool once = false; connect(&controller, &PythonEnvironmentController::busyChanged, this, [&](bool busy) {
      if (!busy && !once) { once = true; controller.reload(); }
    });
    controller.activate(form(f.candidate("next", "changed"))); QTRY_VERIFY_WITH_TIMEOUT(once && !controller.busy(), 10000);
    QCOMPARE(forms.size(), 1); QCOMPARE(messages.size(), 0);
    QCOMPARE(forms.first().first().toJsonObject().value("runtimeManifestSha256").toString(), f.registry->active(EnvironmentMode::Generated).identity.runtimeSha256);
  }
  void acceptedDenySurvivesWindowDeletionAndProjectSwitch_data() {
    QTest::addColumn<bool>("revoke"); QTest::addColumn<int>("closeHook");
    for (bool revoke : {false, true}) for (int hook = 0; hook < 3; ++hook)
      QTest::newRow(qPrintable(QString(revoke ? "revoke-" : "disable-") + QString::number(hook))) << revoke << hook;
  }
  void acceptedDenySurvivesWindowDeletionAndProjectSwitch() {
    QFETCH(bool, revoke); QFETCH(int, closeHook); Fixture f(QFINDTESTDATA("../vendor/agent-python/templates")); f.tasks.setMaxWorkerThreads(1);
    QVERIFY(f.registry->activate(f.candidate("runtime"), 0).ok()); const auto identity = f.registry->active(EnvironmentMode::Generated);
    auto *controller = new PythonEnvironmentController(f.registry, &f.tasks); controller->reload(); QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 10000);
    QSemaphore entered, release; const auto cleanup = qScopeGuard([&] { release.release(); f.tasks.shutdown(10000, true); });
    f.tasks.start("document blocker", [&](PaleoTask *) { entered.release(); release.acquire(); return QString(); });
    QVERIFY(entered.tryAcquire(1, 3000));
    QPointer<PythonEnvironmentController> owner(controller);
    if (closeHook == 1) connect(&f.tasks, &PaleoTaskService::taskAdded, this, [&](PaleoTask *) { if (owner) delete owner.data(); });
    if (closeHook == 2) connect(controller, &PythonEnvironmentController::busyChanged, this, [&](bool busy) { if (busy && owner) delete owner.data(); });
    if (revoke) controller->revoke(identity.identity.id); else controller->disable();
    QVERIFY(identity.interrupted->load() || identity.globalInterruption->load());
    const QPointer<PaleoTask> deny = f.tasks.tasks().last(); QCOMPARE(deny->scope(), PaleoTask::Scope::Application);
    delete owner.data(); f.tasks.beginNewSession(); QVERIFY(!deny->cancelRequested()); release.release();
    QTRY_VERIFY_WITH_TIMEOUT(deny && !deny->running(), 10000); QCOMPARE(deny->state(), PaleoTask::State::Succeeded);
    const auto stored = PythonEnvironmentStore(f.configurationPath()).load(); QVERIFY(stored.ok());
    if (revoke) QVERIFY(stored.configuration.revoked.contains(identity.identity.id)); else QVERIFY(!stored.configuration.enabled);
  }
  void failedOrUnavailablePersistenceLeavesHonestInMemoryDenyAndCanRetry() {
    Fixture f(QFINDTESTDATA("../vendor/agent-python/templates")); QVERIFY(f.registry->activate(f.candidate("runtime"), 0).ok());
    PythonEnvironmentController unavailable(f.registry, nullptr); QSignalSpy unavailableMessages(&unavailable, &PythonEnvironmentController::messageChanged);
    unavailable.disable(); unavailable.disable(); QVERIFY(!unavailable.busy()); QCOMPARE(unavailableMessages.size(), 2);
    QVERIFY(f.registry->status().value("inMemoryDeny").toBool()); QVERIFY(f.registry->status().value("enabled").toBool());
    PythonEnvironmentController controller(f.registry, &f.tasks); QSignalSpy messages(&controller, &PythonEnvironmentController::messageChanged);
    { delivery::StoreLock lock(f.configurationPath() + ".lock"); QVERIFY(lock.locked()); controller.disable();
      QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000); }
    QVERIFY(f.registry->status().value("enabled").toBool()); QVERIFY(messages.last().first().toString().contains(QStringLiteral("尚未保存")));
    controller.disable(); QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000); QVERIFY(!f.registry->status().value("enabled").toBool());
  }
  void exportUsesFixedFieldsAndPreservesActualEnforcementWithoutSecrets() {
    const QString secret = "SENSITIVE_SOURCE_AND_PROVIDER_KEY";
    const auto redacted = redactedPythonEnvironmentStatus({{"code", "ok"}, {"threadCountEnforced", false}, {"cpuParallelismLimit", 1},
      {"memoryPeakComplete", false}, {"observedPeakMemoryBytes", "4096"}, {"hostSnapshotReservedBytes", "8192"},
      {"root", "C:/private"}, {"request", secret}, {"sourceCode", secret}, {"environment", QJsonObject{{"token", secret}}},
      {"policy", QJsonObject{{"memoryBytes", 512}, {"providerKey", secret}, {"runtimeRoot", "C:/private"}}}});
    QVERIFY(redacted.contains("threadCountEnforced")); QVERIFY(!redacted.value("threadCountEnforced").toBool());
    QCOMPARE(redacted.value("cpuParallelismLimit").toInt(), 1); QCOMPARE(redacted.value("observedPeakMemoryBytes").toString(), QString("4096"));
    QVERIFY(!QJsonDocument(redacted).toJson().contains(secret.toUtf8())); QVERIFY(!QJsonDocument(redacted).toJson().contains("private"));
    Fixture f(QFINDTESTDATA("../vendor/agent-python/templates")); QVERIFY(f.registry->activate(f.candidate("runtime"), 0).ok());
    PythonEnvironmentController controller(f.registry, &f.tasks); const auto path = f.temporary.filePath("diagnostics.json");
    controller.exportDiagnostics(path); QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
    const auto bytes = python_environment_fixture::read(path); QVERIFY(!bytes.isEmpty()); QVERIFY(!bytes.contains(f.temporary.path().toUtf8()));
    QCOMPARE(QJsonDocument::fromJson(bytes).object().value("schema").toString(), QString("paleo.python-environment-diagnostics.v1"));
  }
  void diagnosticsNeverWaitForRuntimeVerification() {
    Fixture f(QFINDTESTDATA("../vendor/agent-python/templates")); QVERIFY(f.registry->activate(f.candidate("runtime"), 0).ok());
    PythonEnvironmentController controller(f.registry, &f.tasks); std::atomic_bool entered{false}, release{false};
    const auto runtime = f.registry->active(EnvironmentMode::Generated).runtime;
    const auto revision = f.registry->beginRefresh(); std::thread verifier([&] { f.registry->refresh({}, revision, [&] {
      if (runtime->snapshot().code == "python_environment_verifying" && !entered.exchange(true)) {
        QElapsedTimer clock; clock.start(); while (!release.load() && clock.elapsed() < 3000) QThread::msleep(1);
      }
      return false;
    }); });
    const auto cleanup = qScopeGuard([&] { release = true; verifier.join(); }); QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 3000);
    QElapsedTimer clock; clock.start(); QVERIFY(!controller.redactedDiagnostics().isEmpty()); QVERIFY(clock.elapsed() < 500);
  }
  void realBackendProbeSeparatesIndependentModes_data() {
    QTest::addColumn<bool>("generated"); QTest::newRow("trusted-only") << false; QTest::newRow("generated-only") << true;
  }
  void realBackendProbeSeparatesIndependentModes() {
    QFETCH(bool, generated); const auto root = qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_ROOT");
    if (root.isEmpty()) { QVERIFY2(qEnvironmentVariable("PALEO_REQUIRE_AGENT_PYTHON_EXECUTION") != "1", "Required actual runtime missing"); return; }
    Fixture f(QFINDTESTDATA("../vendor/agent-python/templates")); EnvironmentCandidate candidate;
    candidate.runtimeRoot = root; candidate.runtimeSha256 = qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_MANIFEST_SHA256");
    candidate.policy.allowGenerated = generated; candidate.policy.allowTrusted = !generated;
    if (!generated) { candidate.scriptsRoot = f.scriptsRoot; candidate.scriptsSha256 = f.scriptsSha; }
    const auto approved = f.registry->activate(candidate, 0); QVERIFY2(approved.ok(), qPrintable(approved.code));
    PythonEnvironmentController controller(f.registry, &f.tasks); QSignalSpy forms(&controller, &PythonEnvironmentController::configurationChanged);
    QSignalSpy projections(&controller, &PythonEnvironmentController::diagnosticsChanged);
    controller.reload(); QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 180000); QCOMPARE(forms.size(), 1);
    controller.probe(); QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 180000); QCOMPARE(forms.size(), 1);
    const auto projection = projections.last().first().toJsonObject();
    QCOMPARE(projection.value(generated ? "generated" : "trusted").toString(), QString("ready"));
    QCOMPARE(projection.value(generated ? "trusted" : "generated").toString(), QString("disabled"));
    const auto proof = controller.redactedDiagnostics().value(generated ? "generated" : "trusted").toObject();
    QVERIFY2(proof.value("ready").toBool(), qPrintable(QJsonDocument(proof).toJson()));
    if (generated) { QVERIFY(proof.value("runtimeProbeVerified").toBool()); QVERIFY(!candidate.policy.allowTrusted); }
  }
};
QTEST_GUILESS_MAIN(TestPythonEnvironmentController)
#include "tst_pythonenvironmentcontroller.moc"
