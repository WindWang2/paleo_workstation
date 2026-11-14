// 层：测试壳
#include "services/pythontrustedreadiness.h"
#include "services/pythonprocess.h"
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>
#include <atomic>
#include <chrono>
#include <future>

using namespace paleo::python;
namespace {
QJsonObject inspect(const std::shared_ptr<TrustedReadinessProvider> &provider,
                    const std::function<bool()> &cancelled={}) {
  return std::async(std::launch::async,[provider,cancelled]{return provider->inspect(cancelled);}).get();
}
std::shared_ptr<RuntimeProvider> missingRuntime(bool allow=true) {
  ExecutionPolicy policy;policy.allowTrusted=allow;
  return std::make_shared<RuntimeProvider>(RuntimeConfig{},policy);
}
RuntimeConfig realConfiguration() {
  return {qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_ROOT"),
    qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_MANIFEST_SHA256"),true};
}
}
class TestPythonTrustedReadiness:public QObject {
  Q_OBJECT
private slots:
  void snapshotAndPolicyNeverLaunchOrVerify() {
    auto runtime=missingRuntime(false);auto provider=std::make_shared<TrustedReadinessProvider>(runtime);
    QElapsedTimer clock;clock.start();const auto snapshot=provider->snapshot();QVERIFY(clock.elapsed()<100);
    QVERIFY(!snapshot.value("executionReady").toBool());QCOMPARE(snapshot.value("code").toString(),QString("python_execution_denied"));
    const auto before=runtime->snapshot().code;const auto result=inspect(provider);
    QCOMPARE(result.value("code").toString(),QString("python_execution_denied"));QCOMPARE(runtime->snapshot().code,before);
  }
  void ownerThreadInspectionIsRejected() {
    auto provider=std::make_shared<TrustedReadinessProvider>(missingRuntime());
    QCOMPARE(provider->inspect().value("code").toString(),QString("python_readiness_worker_required"));
    QVERIFY(!provider->snapshot().value("executionReady").toBool());
  }
  void wrongRuntimeAndCancelledInspectionNeverBecomeReady() {
    auto provider=std::make_shared<TrustedReadinessProvider>(missingRuntime());const auto result=inspect(provider);
    QVERIFY(!result.value("executionReady").toBool());QVERIFY(!result.value("runtimeReady").toBool());
    QCOMPARE(inspect(provider,[]{return true;}).value("code").toString(),QString("cancelled"));
    QVERIFY(!provider->snapshot().value("executionReady").toBool());
  }
  void snapshotDoesNotWaitForGlobalPermitAndQueueIsCancellable() {
    auto permit=claimExecutionPermit();QVERIFY(permit);
    auto provider=std::make_shared<TrustedReadinessProvider>(missingRuntime());std::atomic_bool cancel=false;
    auto work=std::async(std::launch::async,[&]{return provider->inspect([&]{return cancel.load();});});
    QTRY_VERIFY_WITH_TIMEOUT(provider->snapshot().value("busy").toBool(),1000);
    QElapsedTimer clock;clock.start();for(int i=0;i<500;++i)QVERIFY(!provider->snapshot().value("executionReady").toBool());
    QVERIFY(clock.elapsed()<1000);cancel=true;
    QVERIFY(work.wait_for(std::chrono::seconds(2))==std::future_status::ready);
    QCOMPARE(work.get().value("code").toString(),QString("cancelled"));
  }
  void queueDeadlineDoesNotAcquireAnExtraSlot() {
    auto permit=claimExecutionPermit();QVERIFY(permit);auto provider=std::make_shared<TrustedReadinessProvider>(missingRuntime());
    QElapsedTimer clock;clock.start();const auto result=inspect(provider);
    QCOMPARE(result.value("code").toString(),QString("python_readiness_queue_timeout"));
    QVERIFY(clock.elapsed()>=14500 && clock.elapsed()<20000);QVERIFY(!result.value("executionReady").toBool());
  }
  void realPinnedBackendWhenConfigured() {
    const auto configuration=realConfiguration();
    if(configuration.root.isEmpty()) {
      QVERIFY2(qEnvironmentVariable("PALEO_REQUIRE_AGENT_PYTHON_EXECUTION")!="1","Explicit locked runtime fixture is required");
      qInfo()<<"No runtime fixture: real backend execution is not asserted";return;
    }
    QVERIFY(!configuration.manifestSha256.isEmpty());ExecutionPolicy policy;policy.allowTrusted=true;
    auto runtime=std::make_shared<RuntimeProvider>(configuration,policy);
    auto provider=std::make_shared<TrustedReadinessProvider>(runtime);
    const auto result=inspect(provider);auto expected=qEnvironmentVariable("PALEO_EXPECT_TRUSTED_READINESS_CODE");
    if(expected.isEmpty())expected="ok";
    QVERIFY(expected=="ok" || expected=="python_linux_resource_limits_unenforced");
    QCOMPARE(result.value("code").toString(),expected);QVERIFY(result.value("runtimeReady").toBool());
    QCOMPARE(result.value("executionReady").toBool(),expected=="ok");QVERIFY(!result.contains("stdout"));
    QCOMPARE(provider->snapshot().value("code").toString(),expected);
    QCOMPARE(inspect(provider,[]{return true;}).value("code").toString(),QString("cancelled"));
    QVERIFY(!provider->snapshot().value("executionReady").toBool());
    auto wrong=configuration;wrong.manifestSha256=QString(64,'0');
    auto changed=std::make_shared<TrustedReadinessProvider>(std::make_shared<RuntimeProvider>(wrong,policy));
    QVERIFY(!inspect(changed).value("executionReady").toBool());
  }
};
QTEST_GUILESS_MAIN(TestPythonTrustedReadiness)
#include "tst_pythontrustedreadiness.moc"
