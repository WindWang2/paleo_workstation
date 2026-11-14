// 层：测试壳
#include "services/pythonsandbox_windows.h"
#include <QtTest>

class TestPythonWindowsUnavailable : public QObject {
  Q_OBJECT
private slots:
  void platformCannotGrantExecution() {
    using namespace paleo::python;
    QCOMPARE(windowsSandboxReadiness().value("code").toString(), QString("python_sandbox_unavailable"));
    SandboxRequest request;
    request.policy.allowTrusted = true;
    request.policy.allowGenerated = true;
    const auto result = runWindowsSandbox(request);
    QCOMPARE(result.code, QString("python_sandbox_unavailable"));
    QVERIFY(result.output.isEmpty());
    QVERIFY(result.outputLog.isEmpty());
    QCOMPARE(result.exitCode, -1);
    QVERIFY(!result.memoryPeakComplete);
  }
};
QTEST_GUILESS_MAIN(TestPythonWindowsUnavailable)
#include "tst_python_windows_unavailable.moc"
