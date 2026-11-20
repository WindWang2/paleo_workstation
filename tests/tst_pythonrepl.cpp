#include "../src/services/pythonenv.h"
#include "../src/services/pythonrepl.h"

#include <QSignalSpy>
#include <QtTest>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include <memory>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <csignal>
#endif
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// 方向68：PythonReplSession（实验性 REPL 桥）进程级验收——基本往返、
// exit() 正常退出、析构强制回收不留孤儿；任务 B（#960）补：start 后立即
// stop、停止后重启（中文往返）、REPL 后代的整树回收。
// Windows 上 QProcess::started 在 start() 内同步发出（Unix 经事件循环），
// 故一律「先查计数再等」，两种时序都成立。
class TestPythonRepl : public QObject
{
  Q_OBJECT
private slots:
  void initTestCase();
  void roundTrip();
  void stopExitsCleanly();
  void destroyKillsProcess();
  void sendWithoutRunIsIgnored();
  void stopImmediatelyAfterStartExitsCleanly();
  void restartAfterStopKeepsChineseRoundTrip();
  void descendantsReclaimedOnStop();
  void descendantsReclaimedOnDestructor();

private:
  static bool pidDead(qint64 pid);
  QString m_python;
};

void TestPythonRepl::initTestCase()
{
#ifdef PALEO_TEST_PYTHON
  // 方向81：configure 钉了解释器 → ctest 必须把它作为 PALEO_PYTHON 注入。
  QCOMPARE(QDir::fromNativeSeparators(qEnvironmentVariable("PALEO_PYTHON")),
           QStringLiteral(PALEO_TEST_PYTHON));
#endif
  // PALEO_PYTHON 一旦设置就必须可用且被选中——不许静默退回 PATH 发现
  //（Windows PATH 首个 python 常是 WindowsApps 商店桩）。
  if (const QString pinned = qEnvironmentVariable("PALEO_PYTHON").trimmed(); !pinned.isEmpty())
  {
    QVERIFY2(QFileInfo::exists(pinned), qPrintable(QStringLiteral("PALEO_PYTHON missing: ") + pinned));
    QCOMPARE(QDir::fromNativeSeparators(PythonEnvService::findBasePython()),
             QDir::fromNativeSeparators(pinned));
  }
  m_python = PythonEnvService::findBasePython();
  if (m_python.isEmpty())
    QSKIP("no base python on PATH");
}

void TestPythonRepl::roundTrip()
{
  PythonReplSession repl;
  QSignalSpy startedSpy(&repl, &PythonReplSession::started);
  QSignalSpy finishedSpy(&repl, &PythonReplSession::finished);
  repl.start(m_python);
  QVERIFY(startedSpy.count() == 1 || startedSpy.wait(10000));
  QVERIFY(repl.isRunning());
  QVERIFY(repl.processId() > 0);
  QString acc;
  connect(&repl, &PythonReplSession::output, this,
          [&acc](const QString &text, bool) { acc += text; });
  repl.sendLine(QStringLiteral("print(1+1)"));
  QTRY_VERIFY_WITH_TIMEOUT(acc.contains(QStringLiteral("2")), 15000);
  repl.stop();
  QVERIFY(finishedSpy.wait(15000));
  const QList<QVariant> args = finishedSpy.takeFirst();
  QCOMPARE(args.at(0).toInt(), 0);      // exit() 正常退出
  QCOMPARE(args.at(1).toBool(), false); // crashed
  QVERIFY(!repl.isRunning());
}

void TestPythonRepl::stopExitsCleanly()
{
  PythonReplSession repl;
  QSignalSpy startedSpy(&repl, &PythonReplSession::started);
  QSignalSpy finishedSpy(&repl, &PythonReplSession::finished);
  repl.start(m_python);
  QVERIFY(startedSpy.count() == 1 || startedSpy.wait(10000));
  repl.stop();
  QVERIFY(finishedSpy.wait(15000));
  QCOMPARE(finishedSpy.takeFirst().at(1).toBool(), false);
  // 幂等：重复 stop 不崩不再发信号。
  repl.stop();
  QCOMPARE(finishedSpy.count(), 0);
}

void TestPythonRepl::destroyKillsProcess()
{
#ifdef Q_OS_UNIX
  auto *repl = new PythonReplSession;
  QSignalSpy startedSpy(repl, &PythonReplSession::started);
  repl->start(m_python);
  QVERIFY(startedSpy.count() == 1 || startedSpy.wait(10000));
  const qint64 pid = repl->processId();
  QVERIFY(pid > 0);
  delete repl; // 析构须强制整组回收（不等 exit()）
  QCOMPARE(::kill(static_cast<pid_t>(pid), 0), -1);
  QCOMPARE(errno, ESRCH);
#else
  QSKIP("orphan assertion is Unix-specific");
#endif
}

void TestPythonRepl::sendWithoutRunIsIgnored()
{
  PythonReplSession repl;
  QSignalSpy startedSpy(&repl, &PythonReplSession::started);
  repl.sendLine(QStringLiteral("print(1)"));
  repl.stop(); // 未运行 = 幂等无操作
  QTest::qWait(200);
  QCOMPARE(startedSpy.count(), 0);
}

bool TestPythonRepl::pidDead(qint64 pid)
{
  if (pid <= 0)
    return true;
#ifdef Q_OS_UNIX
  return ::kill(static_cast<pid_t>(pid), 0) == -1 && errno == ESRCH;
#else
  HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                               DWORD(pid));
  if (!process)
    return GetLastError() == ERROR_INVALID_PARAMETER;
  const bool exited = WaitForSingleObject(process, 10000) == WAIT_OBJECT_0;
  CloseHandle(process);
  return exited;
#endif
}

// start 后尚未收到 started 就 stop：exit() 先落管道，解释器 exec 完成后
// 走正常退出——不允许把这段窗口变成孤儿或假失败。
void TestPythonRepl::stopImmediatelyAfterStartExitsCleanly()
{
  PythonReplSession repl;
  QSignalSpy finishedSpy(&repl, &PythonReplSession::finished);
  QSignalSpy failedSpy(&repl, &PythonReplSession::startFailed);
  repl.start(m_python);
  repl.stop(); // 不等 started（Unix 下此刻通常还没到）
  QVERIFY(finishedSpy.wait(15000) || finishedSpy.count() == 1);
  QCOMPARE(failedSpy.count(), 0); // 这是停止，不是启动失败
  QVERIFY(!repl.isRunning());
}

// 停止后立即重新开始：新会话不受旧 timer/进程信号干扰，中文输入输出正常。
void TestPythonRepl::restartAfterStopKeepsChineseRoundTrip()
{
  PythonReplSession repl;
  QSignalSpy startedSpy(&repl, &PythonReplSession::started);
  QSignalSpy finishedSpy(&repl, &PythonReplSession::finished);
  repl.start(m_python);
  QVERIFY(startedSpy.count() == 1 || startedSpy.wait(10000));
  repl.stop();
  QVERIFY(finishedSpy.wait(15000));
  // 立即重启（第二轮）：旧会话的停止状态机不得命中新会话。
  repl.start(m_python);
  QVERIFY(startedSpy.count() == 2 || startedSpy.wait(10000));
  QString acc;
  connect(&repl, &PythonReplSession::output, this,
          [&acc](const QString &text, bool) { acc += text; });
  repl.sendLine(QStringLiteral("print('中文-交互-回显')"));
  QTRY_VERIFY_WITH_TIMEOUT(acc.contains(QStringLiteral("中文-交互-回显")), 15000);
  repl.stop();
  QVERIFY(finishedSpy.wait(15000) || finishedSpy.count() == 2);
  QCOMPARE(finishedSpy.count(), 2); // 两会话各恰好一次 finished
}

// REPL 拉起的后代（subprocess 子进程）在 stop 递进里连坐回收：
// 「解释器已退」不得掩盖仍存活的后代。
void TestPythonRepl::descendantsReclaimedOnStop()
{
  PythonReplSession repl;
  QSignalSpy startedSpy(&repl, &PythonReplSession::started);
  QSignalSpy finishedSpy(&repl, &PythonReplSession::finished);
  repl.start(m_python);
  QVERIFY(startedSpy.count() == 1 || startedSpy.wait(10000));
  QString acc;
  connect(&repl, &PythonReplSession::output, this,
          [&acc](const QString &text, bool) { acc += text; });
  repl.sendLine(QStringLiteral("import subprocess,sys"));
  repl.sendLine(QStringLiteral(
      "_p = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(120)'])"));
  repl.sendLine(QStringLiteral("print('REPL_CHILD:%d' % _p.pid)"));
  QRegularExpression pattern(QStringLiteral("REPL_CHILD:(\\d+)"));
  QTRY_VERIFY_WITH_TIMEOUT(pattern.match(acc).hasMatch(), 15000);
  const qint64 childPid = pattern.match(acc).captured(1).toLongLong();
  QVERIFY(childPid > 0);
  // 忽略温和停止：子进程不随 exit()/SIGTERM 退（Windows TerminateJobObject
  // /Unix 进程组强杀档回收）。
  repl.stop();
  QVERIFY(finishedSpy.wait(15000));
  QTRY_VERIFY_WITH_TIMEOUT(pidDead(childPid), 10000);
}

// 面板关闭/服务析构：运行中的 REPL 连同后代整树回收。
void TestPythonRepl::descendantsReclaimedOnDestructor()
{
#ifdef Q_OS_UNIX
  auto *repl = new PythonReplSession;
  QSignalSpy startedSpy(repl, &PythonReplSession::started);
  repl->start(m_python);
  QVERIFY(startedSpy.count() == 1 || startedSpy.wait(10000));
  QString acc;
  connect(repl, &PythonReplSession::output, this,
          [&acc](const QString &text, bool) { acc += text; });
  repl->sendLine(QStringLiteral("import subprocess,sys"));
  repl->sendLine(QStringLiteral(
      "_p = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(120)'])"));
  repl->sendLine(QStringLiteral("print('REPL_CHILD:%d' % _p.pid)"));
  QRegularExpression pattern(QStringLiteral("REPL_CHILD:(\\d+)"));
  QTRY_VERIFY_WITH_TIMEOUT(pattern.match(acc).hasMatch(), 15000);
  const qint64 childPid = pattern.match(acc).captured(1).toLongLong();
  delete repl; // 析构强制整组回收（不等 exit()）
  QTRY_VERIFY_WITH_TIMEOUT(pidDead(childPid), 10000);
#else
  QSKIP("orphan assertion is Unix-specific in this suite; Windows lanes cover it "
        "via the job-object suite");
#endif
}

QTEST_GUILESS_MAIN(TestPythonRepl)
#include "tst_pythonrepl.moc"
