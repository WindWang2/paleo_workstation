// 层：测试壳
#include "fixtures/pythonenvironmentfixture.h"
#include "services/paleotaskservice.h"
#include "workflow/capabilityrecovery.h"
#include "catalog/datacatalog.h"
#include "metadata/projectlock.h"
#include <QEventLoop>
#include <QTimer>
#include <QtTest>

using namespace paleo;
using namespace paleo::python;
using namespace paleo::capability;
namespace {
// These are real Catalog/recovery control workers, not substitutes for the
// numerical templates or strong backends. They exercise authority/commit races.
struct Host {
  QTemporaryDir project;
  DataCatalog catalog;
  ProjectDirLock lock{project.path()};
  PaleoTaskService tasks;
  CapabilityRecovery recovery;
  std::shared_ptr<PythonEnvironmentRegistry> registry;
  EnvironmentSelection identity;
  ~Host() { tasks.shutdown(15000, true); }
  bool open() {
    if (!lock.tryLock() || !catalog.open(project.path())) return false;
    recovery.setTaskService(&tasks);
    recovery.setHostProvider([this] {
      RecoveryHost host; host.projectDir = project.path(); host.operationDir = project.path() + "/.paleo/capability-ops";
      host.catalog = &catalog; host.writeClaim = lock.claim(); host.context.documentId = "doc";
      host.context.open = true; host.context.persistentIdentity = true; host.context.epoch = 1;
      host.context.readOnly = false; host.context.ownsWrites = true; host.context.allowedEffects = 127;
      host.validateFrozen = [this](const OperationLog::Record &) {
        return registry->resolve(identity.identity.id, identity.identity.runtimeSha256, identity.identity.policy.json(), EnvironmentMode::Generated).code;
      };
      return host;
    });
    return true;
  }
  FrozenOperation plan(const QString &id) const {
    FrozenOperation plan; plan.capabilityId = "test.python-environment"; plan.implementationVersion = "1";
    plan.callerScope = "agent:test"; plan.clientRequestId = id; plan.intent = {{"id", id}};
    plan.phase = "publish"; plan.requiredEffects = host::LocalExecute;
    plan.outputSpec.id = "test:python-environment"; plan.outputSpec.complete = true;
    AlgoPort output; output.id = "OUTPUT"; output.kind = "file"; plan.outputSpec.outputs = {output}; plan.outputPorts = {"OUTPUT"};
    return plan;
  }
};
bool run(Host &host, const QString &id, ExecuteResult &result, QString &error, bool resume = false) {
  QObject subscriber; QEventLoop loop; QTimer timer; timer.setSingleShot(true); bool finished = false;
  QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
  auto *task = host.recovery.start("agent:test", id, &subscriber, "Environment authority", [&](ExecuteResult value) {
    result = std::move(value); finished = true; loop.quit();
  }, resume, &error);
  if (!task) return false;
  timer.start(10000); if (!finished) loop.exec(); if (!finished) task->requestInterruption(); return finished;
}
OperationWorkResult output(const OperationWorkContext &context) {
  OperationWorkResult result; const auto path = context.privateDir + "/OUTPUT.bin";
  result.ok = python_environment_fixture::write(path, "constant control fixture"); result.outputs.ok = result.ok;
  if (result.ok) result.outputs.artifacts.append({"OUTPUT", path, {}, {}, {}, {}});
  return result;
}
} // namespace
class TestPythonEnvironmentOperation : public QObject {
  Q_OBJECT
private slots:
  void revocationAtPublishingReentrancyBlocksCommitThenReconcilesWithoutRecompute() {
    python_environment_fixture::Fixture fixture(QFINDTESTDATA("../vendor/agent-python/templates"));
    Host host; host.registry = std::make_shared<PythonEnvironmentRegistry>(fixture.configurationPath());
    const auto candidate = fixture.candidate("runtime"); QVERIFY(host.registry->activate(candidate, 0).ok());
    host.identity = host.registry->active(EnvironmentMode::Generated); QVERIFY(host.identity.ok()); QVERIFY(host.open());
    auto computations = std::make_shared<std::atomic_int>(0); auto plan = host.plan("revoke-publishing");
    plan.worker = [computations](const auto &context) { ++*computations; return output(context); };
    const auto accepted = host.recovery.accept(plan); QVERIFY2(accepted.ok, qPrintable(accepted.code));
    host.recovery.setCheckpointHook([&](const QString &point) { if (point == "publishing") {
      host.registry->denyInMemory(host.identity.identity.id); QVERIFY(host.registry->revoke(host.identity.identity.id, 1).ok());
    }});
    ExecuteResult result; QString error; QVERIFY(run(host, accepted.record.operationId, result, error));
    QVERIFY(!result.ok); QCOMPARE(result.code, QString("python_environment_identity_revoked"));
    QCOMPARE(host.catalog.versions().size(), 0); QCOMPARE(computations->load(), 1);
    const auto fact = host.recovery.get(plan.callerScope, accepted.record.operationId);
    QCOMPARE(fact.value("commitState").toString(), QString("not_requested"));
    QVERIFY(!fact.value("cancelRequested").toBool());
    QVERIFY(host.registry->activate(candidate, 2).ok()); host.recovery.setCheckpointHook({});
    QVERIFY2(run(host, accepted.record.operationId, result, error, true), qPrintable(error));
    QVERIFY2(result.ok, qPrintable(result.code)); QCOMPARE(host.catalog.versions().size(), 1); QCOMPARE(computations->load(), 1);
    host.registry->denyInMemory(); QVERIFY(host.registry->disable(3).ok());
    QVERIFY(run(host, accepted.record.operationId, result, error, true));
    QVERIFY(result.ok); QCOMPARE(result.commitState, QString("committed"));
    QCOMPARE(host.catalog.versions().size(), 1); QCOMPARE(computations->load(), 1);
  }
  void cooperativeAuthorizationInterruptionDoesNotPersistAUserCancellation() {
    python_environment_fixture::Fixture fixture(QFINDTESTDATA("../vendor/agent-python/templates"));
    Host host; host.registry = std::make_shared<PythonEnvironmentRegistry>(fixture.configurationPath());
    QVERIFY(host.registry->activate(fixture.candidate("runtime"), 0).ok());
    host.identity = host.registry->active(EnvironmentMode::Generated); QVERIFY(host.open());
    auto plan = host.plan("host-interruption"); plan.phase = "compute-only";
    plan.worker = [](const OperationWorkContext &context) {
      context.requestInterruption(); OperationWorkResult result; result.code = "cancelled"; return result;
    };
    const auto accepted = host.recovery.accept(plan); QVERIFY(accepted.ok); ExecuteResult result; QString error;
    QVERIFY(run(host, accepted.record.operationId, result, error)); QVERIFY(!result.ok);
    const auto saved = host.recovery.get(plan.callerScope, accepted.record.operationId);
    QCOMPARE(saved.value("execution").toString(), QString("interrupted")); QVERIFY(!saved.value("cancelRequested").toBool());
    QCOMPARE(host.catalog.versions().size(), 0);
  }
};
QTEST_GUILESS_MAIN(TestPythonEnvironmentOperation)
#include "tst_pythonenvironmentoperation.moc"
