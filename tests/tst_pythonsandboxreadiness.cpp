// 层：测试壳
#include "services/pythonsandboxprovider.h"
#include <QtTest>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QEventLoop>
#include <QElapsedTimer>
#include <atomic>

using namespace paleo::python;
namespace {
template<class T> T completed(QFuture<T> future) {
  QFutureWatcher<T> watcher;QEventLoop loop;
  QObject::connect(&watcher,&QFutureWatcherBase::finished,&loop,&QEventLoop::quit);
  watcher.setFuture(future);if(!watcher.isFinished())loop.exec();return watcher.result();
}
std::shared_ptr<RuntimeProvider> configuredRuntime(bool generated=true,bool trusted=false) {
  ExecutionPolicy policy;policy.allowGenerated=generated;policy.allowTrusted=trusted;
  const auto root=qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_ROOT");
  return std::make_shared<RuntimeProvider>(RuntimeConfig{root,qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_MANIFEST_SHA256"),!root.isEmpty()},policy);
}
}
class TestPythonSandboxReadiness:public QObject {
  Q_OBJECT
private slots:
  void ownerThreadDoesNotLaunchPlatformProbesOrExecution() {
    SandboxProvider provider(configuredRuntime());QCOMPARE(provider.snapshot().value("code").toString(),QString("python_sandbox_not_probed"));
    QCOMPARE(provider.inspect().value("code").toString(),QString("python_readiness_worker_required"));
    QCOMPARE(provider.run({}).code,QString("python_execution_worker_required"));QVERIFY(!provider.snapshot().value("ready").toBool());
  }
  void nullRuntimeAndTrustedOnlyPolicyCannotAdvertiseGeneratedExecution() {
    SandboxProvider absent(nullptr);const auto missing=completed(QtConcurrent::run([&]{return absent.inspect();}));
    QCOMPARE(missing.value("code").toString(),QString("python_environment_disabled"));QVERIFY(!missing.value("ready").toBool());
    const auto runtime=configuredRuntime(false,true);SandboxProvider provider(runtime);const auto original=runtime->policy().json();
    const auto state=completed(QtConcurrent::run([&]{return provider.inspect();}));
    QCOMPARE(state.value("code").toString(),QString("python_execution_denied"));QVERIFY(!state.value("ready").toBool());
    QVERIFY(!state.value("runtimeProbeVerified").toBool());QCOMPARE(runtime->policy().json(),original);
    SandboxRequest request;request.policy.allowGenerated=true;
    QCOMPARE(completed(QtConcurrent::run([&]{return provider.run(request);})).code,QString("python_execution_denied"));
  }
  void cancelledProbeDoesNotConsumeTheSharedExecutionPermit() {
    SandboxProvider provider(configuredRuntime());const auto slot=claimExecutionPermit();QVERIFY(slot);
    const auto state=completed(QtConcurrent::run([&]{return provider.inspect([]{return true;});}));
    QCOMPARE(state.value("code").toString(),QString("cancelled"));QVERIFY(!state.value("ready").toBool());
  }
  void snapshotsRemainImmediateWhileProbeQueuesAndCancels() {
    SandboxProvider provider(configuredRuntime());const auto slot=claimExecutionPermit();QVERIFY(slot);
    std::atomic_bool entered=false,stop=false;
    auto future=QtConcurrent::run([&]{entered=true;return provider.inspect([&]{return stop.load();});});
    QTRY_VERIFY_WITH_TIMEOUT(entered.load(),1000);QElapsedTimer clock;clock.start();
    for(int i=0;i<500;++i)QVERIFY(!provider.snapshot().value("ready").toBool());
    QVERIFY(clock.elapsed()<1000);stop=true;const auto state=completed(future);const auto code=state.value("code").toString();
    if(code=="python_sandbox_qt_unsupported" || code=="python_sandbox_unavailable") {QVERIFY(!state.value("ready").toBool());return;}
    QCOMPARE(code,QString("cancelled"));
  }
  void changedRequestPolicyIsRejectedBeforeBackendDispatch() {
    const auto runtime=configuredRuntime();SandboxProvider provider(runtime);SandboxRequest request;
    request.policy=runtime->policy();--request.policy.wallSeconds;
    QCOMPARE(completed(QtConcurrent::run([&]{return provider.run(request);})).code,QString("python_policy_changed"));
    QVERIFY(!provider.snapshot().value("ready").toBool());
  }
  void actualWorkerRequiresTheCurrentPinnedRuntimeAndStrongNumericalProof() {
    const auto runtime=configuredRuntime();SandboxProvider provider(runtime);const auto original=runtime->policy().json();
    const auto state=completed(QtConcurrent::run([&]{return provider.inspect();}));const auto code=state.value("code").toString();
    QVERIFY(!code.isEmpty());QVERIFY(code!="python_readiness_worker_required");QCOMPARE(runtime->policy().json(),original);
    QCOMPARE(state.value("ready").toBool(),code=="ok");QCOMPARE(provider.snapshot(),state);
    const auto expected=qEnvironmentVariable("PALEO_EXPECT_SANDBOX_READINESS_CODE");if(!expected.isEmpty())QCOMPARE(code,expected);
    else if(qEnvironmentVariable("PALEO_REQUIRE_AGENT_PYTHON_EXECUTION")=="1")QVERIFY2(state.value("ready").toBool(),qPrintable(code));
    if(qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_ROOT").isEmpty()) {
      QVERIFY(!state.value("ready").toBool());QVERIFY(!state.value("runtimeProbeVerified").toBool());return;
    }
    if(!state.value("ready").toBool()){QVERIFY(!state.value("runtimeProbeVerified").toBool());return;}
    QCOMPARE(state.value("probeContract").toString(),QString("paleo.python-sandbox-readiness.v1"));
    QVERIFY(state.value("runtimeProbeVerified").toBool());QCOMPARE(state.value("requiresRuntimeProbe"),QJsonValue(false));
    QCOMPARE(state.value("runtimeManifestSha256").toString(),runtime->snapshot().runtime->manifestSha256);
    QCOMPARE(state.value("probeCodeSha256").toString().size(),64);QCOMPARE(state.value("policy"),QJsonValue(original));
    QVERIFY(state.value("cpuMicros").toString().toLongLong()>0);QVERIFY(state.value("observedPeakMemoryBytes").toString().toLongLong()>0);
    QVERIFY(state.value("hostSnapshotReservedBytes").toString().toLongLong()>0);
    QCOMPARE(state.value("childMemoryLimitBytes").toString().toLongLong(),runtime->policy().memoryBytes);
    auto different=std::make_shared<VerifiedRuntime>(*runtime->snapshot().runtime);different->manifestSha256=QString(64,'a');
    SandboxRequest request;request.policy=runtime->policy();request.runtime=different;
    QCOMPARE(completed(QtConcurrent::run([&]{return provider.run(request);})).code,QString("python_runtime_changed"));
  }
};
QTEST_GUILESS_MAIN(TestPythonSandboxReadiness)
#include "tst_pythonsandboxreadiness.moc"
