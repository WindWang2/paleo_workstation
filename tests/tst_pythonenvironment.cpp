// 层：测试壳
#include "workflow/aichattoolrunner.h"
#include "ai/chat/domaintools.h"
#include "services/pythonruntime.h"
#include <QtTest>
#include <QJsonDocument>

class TestPythonEnvironment : public QObject {
  Q_OBJECT
private slots:
  void statusIsReadOnlyAndCannotAcceptExecutableArguments() {
    const auto spec=aiToolSpec("paleo.python_environment");QVERIFY(!spec.name.isEmpty());
    QCOMPARE(spec.effects,unsigned(paleo::host::Read));QVERIFY(spec.validateParameters({}).isEmpty());
    QVERIFY(!spec.validateParameters({{"code","open('outside','w')"}}).isEmpty());
    QVERIFY(!spec.validateParameters({{"interpreter","untrusted"}}).isEmpty());
    QCOMPARE(dispatchAiTool(spec.name,{}).status,AiToolDispatchStatus::Routed);
  }
  void missingEnvironmentIsAnAsynchronousStatus() {
    PaleoTaskService tasks;AiChatToolRunner runner;runner.setTaskService(&tasks);
    QSignalSpy done(&runner,&AiChatToolRunner::toolFinished);
    runner.run({"status","paleo.python_environment","{}"});QCOMPARE(done.size(),0);
    QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,5000);
    const auto payload=QJsonDocument::fromJson(done.first().at(2).toString().toUtf8()).object();
    QCOMPARE(payload.value("code").toString(),QString("python_environment_disabled"));
    QVERIFY(!payload.value("ready").toBool());QVERIFY(!payload.value("generatedCodeAvailable").toBool());
    QVERIFY(!payload.contains("root"));QVERIFY(!payload.contains("interpreter"));
  }
  void runtimeAnchorIsHostOwned() {
    PaleoTaskService tasks;AiChatToolRunner runner;runner.setTaskService(&tasks);
    AiChatToolRunner::Context context;
    context.pythonRuntime=std::make_shared<paleo::python::RuntimeProvider>(paleo::python::RuntimeConfig{"/not-a-runtime",{},true});
    runner.setContext(context);QSignalSpy done(&runner,&AiChatToolRunner::toolFinished);
    runner.run({"anchor","paleo.python_environment","{}"});QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,5000);
    const auto payload=QJsonDocument::fromJson(done.first().at(2).toString().toUtf8()).object();
    QCOMPARE(payload.value("code").toString(),QString("python_manifest_anchor_required"));
    QVERIFY(!payload.value("ready").toBool());
  }
  void reentrantDeletionAtTaskAddedDoesNotPublish() {
    PaleoTaskService tasks;auto *runner=new AiChatToolRunner;runner->setTaskService(&tasks);
    QPointer<AiChatToolRunner> guard(runner);
    connect(&tasks,&PaleoTaskService::taskAdded,&tasks,[&]{delete runner;runner=nullptr;});
    runner->run({"delete","paleo.python_environment","{}"});
    QTRY_VERIFY_WITH_TIMEOUT(!guard,5000);QTRY_COMPARE_WITH_TIMEOUT(tasks.runningCount(),0,5000);
  }
};
QTEST_GUILESS_MAIN(TestPythonEnvironment)
#include "tst_pythonenvironment.moc"
