#include "../src/services/pythonenv.h"
#include "../src/services/pythonrepl.h"

#include <QSignalSpy>
#include <QtTest>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <csignal>
#endif

// 方向68：PythonReplSession（实验性 REPL 桥）进程级验收——基本往返、
// exit() 正常退出、析构强制回收不留孤儿。
class TestPythonRepl : public QObject
{
  Q_OBJECT
private slots:
  void initTestCase();
  void roundTrip();
  void stopExitsCleanly();
  void destroyKillsProcess();
  void sendWithoutRunIsIgnored();

private:
  QString m_python;
};

void TestPythonRepl::initTestCase()
{
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
  QVERIFY(startedSpy.wait(10000));
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
  QVERIFY(startedSpy.wait(10000));
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
  QVERIFY(startedSpy.wait(10000));
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

QTEST_GUILESS_MAIN(TestPythonRepl)
#include "tst_pythonrepl.moc"
