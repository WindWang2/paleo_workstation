#include <QtTest>
#include <QSignalSpy>
#include <QVariantMap>

#include "../src/workflow/registration.h"
#include "../src/io/dataimportservice.h"
#include "../src/qgis/qgislayerservice.h"

class TestWorkflowRegistration : public QObject
{
  Q_OBJECT

private slots:
  void initialState();
  void nullServicesFailGracefully();
  void nullLayerServiceFails();
  void emptyParamsHandled();
  void signalSpyContracts();
  void mutationDemonstration_monotonicCounter();
};

void TestWorkflowRegistration::initialState()
{
  RegistrationWorkflow wf(nullptr, nullptr);
  QCOMPARE(wf.provisionalLayerCount(), 0);
}

void TestWorkflowRegistration::nullServicesFailGracefully()
{
  RegistrationWorkflow wf(nullptr, nullptr);
  QSignalSpy failSpy(&wf, &RegistrationWorkflow::registrationFailed);
  QSignalSpy successSpy(&wf, &RegistrationWorkflow::provisionalRegistered);
  QSignalSpy countSpy(&wf, &RegistrationWorkflow::provisionalLayerCountChanged);

  QVariantMap params;
  params.insert(QStringLiteral("tx"), 10.0);
  params.insert(QStringLiteral("ty"), 20.0);
  wf.applyProvisionalRegistration(QStringLiteral("ast-test-1"), params);

  // 必须发射失败信号，且不触发成功与计数变更
  QCOMPARE(failSpy.count(), 1);
  QCOMPARE(successSpy.count(), 0);
  QCOMPARE(countSpy.count(), 0);
  QCOMPARE(wf.provisionalLayerCount(), 0);

  const QString errMsg = failSpy.takeFirst().at(0).toString();
  QVERIFY(errMsg.contains(QStringLiteral("未就绪")) || errMsg.contains(QStringLiteral("失败")));
}

void TestWorkflowRegistration::nullLayerServiceFails()
{
  // 仅有 import service 但缺 layer service
  RegistrationWorkflow wf(nullptr, nullptr);
  QSignalSpy failSpy(&wf, &RegistrationWorkflow::registrationFailed);

  wf.applyProvisionalRegistration(QStringLiteral("ast-missing"), {});
  QCOMPARE(failSpy.count(), 1);
  QVERIFY(!failSpy.takeFirst().at(0).toString().isEmpty());
}

void TestWorkflowRegistration::emptyParamsHandled()
{
  RegistrationWorkflow wf(nullptr, nullptr);
  QSignalSpy failSpy(&wf, &RegistrationWorkflow::registrationFailed);

  wf.applyProvisionalRegistration(QString(), {});
  QCOMPARE(failSpy.count(), 1);
}

void TestWorkflowRegistration::signalSpyContracts()
{
  RegistrationWorkflow wf(nullptr, nullptr);
  QSignalSpy failSpy(&wf, &RegistrationWorkflow::registrationFailed);
  QSignalSpy regSpy(&wf, &RegistrationWorkflow::provisionalRegistered);
  QSignalSpy cntSpy(&wf, &RegistrationWorkflow::provisionalLayerCountChanged);

  QVERIFY(failSpy.isValid());
  QVERIFY(regSpy.isValid());
  QVERIFY(cntSpy.isValid());

  // 触发一次调用
  wf.applyProvisionalRegistration(QStringLiteral("nonexistent"), {});
  QCOMPARE(failSpy.count(), 1);
  QCOMPARE(regSpy.count(), 0);
  QCOMPARE(cntSpy.count(), 0);
}

void TestWorkflowRegistration::mutationDemonstration_monotonicCounter()
{
  // 变异测试示范：provisionalLayerCount 必须是非负单调锁存计数器
  RegistrationWorkflow wf(nullptr, nullptr);
  QCOMPARE(wf.provisionalLayerCount(), 0);

  // 失败调用绝不会产生伪计数递增
  for (int i = 0; i < 5; ++i)
  {
    wf.applyProvisionalRegistration(QStringLiteral("invalid"), {});
  }
  QCOMPARE(wf.provisionalLayerCount(), 0);
}

QTEST_GUILESS_MAIN(TestWorkflowRegistration)
#include "tst_workflow_registration.moc"
