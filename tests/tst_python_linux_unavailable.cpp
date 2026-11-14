// 层：测试壳
#include "services/pythonsandbox_linux.h"
#include <QTest>
class TestLinuxSandboxUnavailable:public QObject {
  Q_OBJECT
private slots:
  void platformOrQtCannotExecuteGeneratedCode() {
    paleo::python::SandboxRequest request;request.policy.allowGenerated=true;
    const auto result=paleo::python::runLinuxSandbox(request);
#ifdef Q_OS_LINUX
    QCOMPARE(result.code,QString("python_sandbox_qt_unsupported"));
#else
    QCOMPARE(result.code,QString("python_sandbox_platform_unavailable"));
#endif
    QVERIFY(result.output.isEmpty());QVERIFY(result.outputLog.isEmpty());
    QCOMPARE(result.exitCode,-1);QCOMPARE(result.cpuMicros,0);QCOMPARE(result.peakMemoryBytes,0);
  }
};
QTEST_GUILESS_MAIN(TestLinuxSandboxUnavailable)
#include "tst_python_linux_unavailable.moc"
