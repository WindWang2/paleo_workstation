// 层：测试壳
#include "../src/services/pythonenv.h"
#include "../src/services/scriptprocessscope.h"

#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QtTest>

// 任务 B（#960）：ScriptProcessScope 契约验收。
// Unix：保形无操作层——调用方既有的 setsid 进程组语义不变（本套只钉住
// 契约形状，进程级行为在 tst_scriptrunner/tst_pythonrepl 的真实进程树用例
// 里验证）。Windows：创建时 Job 归属、整树强停与拒绝启动语义。
class TestScriptProcessScope : public QObject
{
  Q_OBJECT
private slots:
  void unixContractIsNoOp();
  void windowsJobOwnsTreeAtCreation();
  void windowsCreateFailureRefusesLaunch();

private:
  QString python() const;
};

QString TestScriptProcessScope::python() const
{
#ifdef Q_OS_UNIX
  return QStringLiteral("/bin/sh");
#else
  // 与其余 Python 进程级测试同一发现序（PALEO_PYTHON 注入口径一致）。
  return PythonEnvService::findBasePython();
#endif
}

void TestScriptProcessScope::unixContractIsNoOp()
{
#ifdef Q_OS_UNIX
  ScriptProcessScope scope;
  QVERIFY(scope.isValid());          // Unix 恒可用（无系统资源要申请）
  QVERIFY(scope.launchPrepared());   // configure 前/后恒 true
  QProcess proc;
  scope.configure(proc);             // 无操作：不装创建参数修饰器
  QVERIFY(scope.launchPrepared());
  QVERIFY(!scope.terminateTree());   // 树级强停交还调用方（进程组信号）
  QVERIFY(scope.treeIdle());
  QVERIFY(scope.waitForTreeIdle(1));
  QVERIFY(scope.reclaim(1));
#else
  QSKIP("Unix no-op contract is verified on Unix lanes");
#endif
}

void TestScriptProcessScope::windowsJobOwnsTreeAtCreation()
{
#ifdef Q_OS_WIN
  const QString program = python();
  if (program.isEmpty())
    QSKIP("no base python on PATH for the tree fixture");
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  ScriptProcessScope scope;
  if (!scope.isValid())
    QSKIP("job object unavailable on this host");
  QProcess proc;
  scope.configure(proc);
  proc.setProcessChannelMode(QProcess::SeparateChannels);
  // 子进程再拉一个孙进程（继承 stdout 管道）；父退后孙仍存活——整树强停
  // 必须连坐。python -c 里用 sys.executable 保同解释器。
  const QString script = QStringLiteral(
      "import subprocess,sys,time\n"
      "subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)'])\n"
      "print('SPAWNED',flush=True)\ntime.sleep(60)\n");
  QSignalSpy startedSpy(&proc, &QProcess::started);
  proc.start(program, {QStringLiteral("-c"), script});
  QVERIFY(startedSpy.count() > 0 || startedSpy.wait(15000) || proc.state() != QProcess::NotRunning);
  QVERIFY(scope.launchPrepared()); // 创建属性备齐（否则 CreateProcess 已失败）
  QVERIFY(proc.bytesAvailable() > 0 || proc.waitForReadyRead(15000));
  QVERIFY(proc.readAllStandardOutput().startsWith("SPAWNED"));
  // 主进程仍在：树内必有进程。
  QVERIFY(!scope.treeIdle());
  QVERIFY(scope.terminateTree());
  QVERIFY(scope.waitForTreeIdle(5000)); // 主进程 + 孙进程一并退出
  QVERIFY(proc.waitForFinished(15000));
  QVERIFY(scope.treeIdle());
#else
  QSKIP("creation-time job ownership requires Windows");
#endif
}

void TestScriptProcessScope::windowsCreateFailureRefusesLaunch()
{
#ifdef Q_OS_WIN
  scriptprocessscope_test::forceCreateFailure();
  const auto restore = qScopeGuard([] { scriptprocessscope_test::clearCreateFailure(); });
  ScriptProcessScope scope;
  QVERIFY(!scope.isValid());
  QProcess proc;
  scope.configure(proc); // 失败态也要 configure：修饰器以 FailedToStart 拒绝
  QSignalSpy failedSpy(&proc, &QProcess::errorOccurred);
  proc.start(python(), {QStringLiteral("-c"), QStringLiteral("print('must not run')")});
  QCOMPARE(proc.state(), QProcess::NotRunning);
  QCOMPARE(failedSpy.count(), 1);
  QCOMPARE(failedSpy.takeFirst().at(0).toInt(), int(QProcess::FailedToStart));
  QVERIFY(!scope.launchPrepared());
#else
  QSKIP("forced job-create failure requires Windows");
#endif
}

QTEST_GUILESS_MAIN(TestScriptProcessScope)
#include "tst_scriptprocessscope.moc"
