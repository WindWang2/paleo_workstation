// 层：测试壳
#include "services/pythonsandboxprovider.h"
#include "services/pythonprocess.h"
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
}
class TestPythonSandboxReadiness:public QObject {
  Q_OBJECT
private slots:
  void ownerThreadDoesNotLaunchPlatformProbes() {
    SandboxProvider provider;QCOMPARE(provider.snapshot().value("code").toString(),QString("python_sandbox_not_probed"));
    QCOMPARE(provider.inspect().value("code").toString(),QString("python_readiness_worker_required"));
    QVERIFY(!provider.snapshot().value("ready").toBool());
  }
  void cancelledProbeDoesNotConsumeTheSharedExecutionPermit() {
    SandboxProvider provider;const auto slot=claimExecutionPermit();QVERIFY(slot);
    const auto state=completed(QtConcurrent::run([&]{return provider.inspect([]{return true;});}));
    QCOMPARE(state.value("code").toString(),QString("cancelled"));QVERIFY(!state.value("ready").toBool());
  }
  void snapshotsRemainImmediateWhileLinuxProbeQueuesAndCancels() {
#if defined(Q_OS_LINUX) && QT_VERSION >= QT_VERSION_CHECK(6,7,0)
    SandboxProvider provider;const auto slot=claimExecutionPermit();QVERIFY(slot);
    std::atomic_bool entered=false,stop=false;
    auto future=QtConcurrent::run([&]{entered=true;return provider.inspect([&]{return stop.load();});});
    QTRY_VERIFY_WITH_TIMEOUT(entered.load(),1000);
    QElapsedTimer clock;clock.start();
    for(int i=0;i<500;++i)QVERIFY(!provider.snapshot().value("ready").toBool());
    QVERIFY(clock.elapsed()<1000);stop=true;
    QCOMPARE(completed(future).value("code").toString(),QString("cancelled"));
#else
    SandboxProvider provider;QVERIFY(!provider.snapshot().value("ready").toBool());
#endif
  }
  void actualWorkerProbeReportsPlatformReadinessWithoutChangingExecutionPolicy() {
    SandboxProvider provider;const auto state=completed(QtConcurrent::run([&]{return provider.inspect();}));
    const auto code=state.value("code").toString();QVERIFY(!code.isEmpty());
    QVERIFY(code!="python_readiness_worker_required");QCOMPARE(state.value("ready").toBool(),code=="ok");
    const auto expected=qEnvironmentVariable("PALEO_EXPECT_SANDBOX_READINESS_CODE");
    if(!expected.isEmpty())QCOMPARE(code,expected);
    QCOMPARE(provider.snapshot(),state);
    SandboxRequest request;QCOMPARE(provider.run(request).code,QString("python_execution_denied"));
  }
};
QTEST_GUILESS_MAIN(TestPythonSandboxReadiness)
#include "tst_pythonsandboxreadiness.moc"
