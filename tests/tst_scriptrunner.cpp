#include "../src/services/pythonenv.h"
#include "../src/services/scriptrunner.h"
#include "perfbudget_relax.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QtTest>

#include <memory>

#include <utility>

#include <QTemporaryDir>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <csignal>
#endif
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// 方向68：ScriptRunnerService 进程级验收——退出码/流式/argv 透传/超时/
// 取消（terminate→kill 递进）/并发闸（缺省 ≤2、第 3 个排队）/无孤儿。
class TestScriptRunner : public QObject
{
  Q_OBJECT
private slots:
  void initTestCase();
  void runsToCompletionExitZero();
  void exitCodePropagated();
  void streamsStdoutAndStderr();
  void argvPassthrough();
  void utf8IoRoundTrip();
  void unbrokenFloodOutputStaysBounded();
  void timeoutTerminatesRun();
  void cancelTerminatesRun();
  void cancelEscalatesToKill();
  void gateQueuesThirdRun();
  void queuedCancelFinishesWithoutStart();
  void startFailureIsHonest();
  void failedToStartReportsCrashed();
  void noOrphanAfterCancel();
  void treeDescendantsReclaimedWhenMainExitsFirst();
  void treeDescendantsReclaimedOnCancel();
  void treeDescendantsReclaimedOnTimeout();
  void treeDescendantsReclaimedOnDestructor();

private:
  static QString fixture(const QString &name);
  // #960：跨平台 pid 存活判定（句柄/信号而非「进程列表里没有」）。
  static bool pidDead(qint64 pid);
  static QList<qint64> treePidsFromSentinel(const QString &sentinel);
  QString m_python;
};

QString TestScriptRunner::fixture(const QString &name)
{
  return QStringLiteral(SCRIPT_FIXTURE_DIR) + QLatin1Char('/') + name;
}

void TestScriptRunner::initTestCase()
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
  QVERIFY(QFileInfo(fixture(QStringLiteral("echo_args.py"))).isFile());
  m_python = PythonEnvService::findBasePython();
  if (m_python.isEmpty())
    QSKIP("no base python on PATH");
  // 诊断：CI 上实际选中的解释器（Windows 由 PATH 次序决定）。
  qInfo("base python: %s", qPrintable(QDir::toNativeSeparators(m_python)));
}

void TestScriptRunner::runsToCompletionExitZero()
{
  ScriptRunnerService svc;
  QSignalSpy finishedSpy(&svc, &ScriptRunnerService::runFinished);
  const qint64 id = svc.start({fixture(QStringLiteral("echo_args.py")), {}, {}, m_python, 0});
  QVERIFY(id > 0);
  QVERIFY(finishedSpy.wait(30000));
  const QList<QVariant> args = finishedSpy.takeFirst();
  QCOMPARE(args.at(0).toLongLong(), id);
  QCOMPARE(args.at(1).toString(), fixture(QStringLiteral("echo_args.py")));
  QCOMPARE(args.at(2).toInt(), 0);      // exitCode
  QCOMPARE(args.at(3).toBool(), false); // crashed
  QCOMPARE(args.at(4).toBool(), false); // cancelled
  QCOMPARE(args.at(5).toBool(), false); // timedOut
  QCOMPARE(svc.runningCount(), 0);
}

void TestScriptRunner::exitCodePropagated()
{
  ScriptRunnerService svc;
  QSignalSpy finishedSpy(&svc, &ScriptRunnerService::runFinished);
  QStringList lines;
  connect(&svc, &ScriptRunnerService::runOutput, this,
          [&lines](qint64, const QString &line, bool) { lines.append(line); });
  const qint64 id =
      svc.start({fixture(QStringLiteral("fail.py")), {}, {}, m_python, 0});
  QVERIFY(id > 0);
  QVERIFY(finishedSpy.wait(30000));
  const QList<QVariant> args = finishedSpy.takeFirst();
  QCOMPARE(args.at(2).toInt(), 3);      // fail.py 里 sys.exit(3)
  QCOMPARE(args.at(3).toBool(), false); // 非零退出 ≠ crashed
  QVERIFY(lines.contains(QStringLiteral("FAIL_STDOUT")));
  QVERIFY(lines.contains(QStringLiteral("FAIL_STDERR")));
}

void TestScriptRunner::streamsStdoutAndStderr()
{
  ScriptRunnerService svc;
  QSignalSpy finishedSpy(&svc, &ScriptRunnerService::runFinished);
  QStringList outLines, errLines;
  bool sawOutputBeforeFinish = false;
  connect(&svc, &ScriptRunnerService::runOutput, this,
          [&](qint64, const QString &line, bool stderrChannel) {
            sawOutputBeforeFinish = true; // finished 必然在其后（同一线程顺序）
            (stderrChannel ? errLines : outLines).append(line);
          });
  const qint64 id =
      svc.start({fixture(QStringLiteral("stream.py")), {}, {}, m_python, 0});
  QVERIFY(id > 0);
  QVERIFY(finishedSpy.wait(30000));
  // 解释器自身可能往 stderr 写与夹具无关的行（Windows CI 上实测 stderr 多
  // 出 1 行）：只核对夹具标记行的内容与顺序，其余行回显供诊断、不判红。
  const auto fixtureLines = [](const QStringList &lines, const QString &prefix) {
    QStringList matched, extra;
    for (const QString &line : lines)
      (line.startsWith(prefix) ? matched : extra).append(line);
    return std::make_pair(matched, extra);
  };
  const auto [outMatched, outExtra] = fixtureLines(outLines, QStringLiteral("OUT_LINE_"));
  const auto [errMatched, errExtra] = fixtureLines(errLines, QStringLiteral("ERR_LINE_"));
  if (!outExtra.isEmpty() || !errExtra.isEmpty())
    qInfo("non-fixture lines: stdout=[%s] stderr=[%s]",
          qPrintable(outExtra.join(QLatin1Char('|'))),
          qPrintable(errExtra.join(QLatin1Char('|'))));
  const QStringList expectedOut = {QStringLiteral("OUT_LINE_0"), QStringLiteral("OUT_LINE_1"),
                                   QStringLiteral("OUT_LINE_2")};
  const QStringList expectedErr = {QStringLiteral("ERR_LINE_0"), QStringLiteral("ERR_LINE_1"),
                                   QStringLiteral("ERR_LINE_2")};
  QVERIFY2(outMatched == expectedOut, qPrintable(outLines.join(QLatin1Char('|'))));
  QVERIFY2(errMatched == expectedErr, qPrintable(errLines.join(QLatin1Char('|'))));
  QVERIFY(sawOutputBeforeFinish);
}

void TestScriptRunner::argvPassthrough()
{
  ScriptRunnerService svc;
  QSignalSpy finishedSpy(&svc, &ScriptRunnerService::runFinished);
  QStringList lines;
  connect(&svc, &ScriptRunnerService::runOutput, this,
          [&lines](qint64, const QString &line, bool) { lines.append(line); });
  // 含空格的参数必须作为一个整体透传（不经 shell 分词）。
  const QStringList argv = {QStringLiteral("plain"), QStringLiteral("with space"),
                            QStringLiteral("--flag=value")};
  const qint64 id =
      svc.start({fixture(QStringLiteral("echo_args.py")), argv, {}, m_python, 0});
  QVERIFY(id > 0);
  QVERIFY(finishedSpy.wait(30000));
  QVERIFY(lines.contains(QStringLiteral("ARG:plain")));
  QVERIFY(lines.contains(QStringLiteral("ARG:with space")));
  QVERIFY(lines.contains(QStringLiteral("ARG:--flag=value")));
}

void TestScriptRunner::utf8IoRoundTrip()
{
  // #479：子进程 IO 统一 UTF-8。宿主进程被按进非 UTF-8 locale（LC_ALL=C 且
  // 关闭 PEP538 强转 / UTF-8 模式）后，子进程 Python 的管道编码退化为
  // ASCII——修复前夹具的中文 print 直接 UnicodeEncodeError，行丢失；修复后
  // 服务侧 PYTHONIOENCODING=utf-8 钉死编码，中文原样回传。POSIX-only。
#ifdef Q_OS_UNIX
  const struct EnvGuard
  {
    const QByteArray lcAll = qgetenv("LC_ALL");
    const QByteArray coerce = qgetenv("PYTHONCOERCECLOCALE");
    const QByteArray utf8 = qgetenv("PYTHONUTF8");
    const QByteArray ioEnc = qgetenv("PYTHONIOENCODING");
    EnvGuard()
    {
      qputenv("LC_ALL", "C");
      qputenv("PYTHONCOERCECLOCALE", "0");
      qputenv("PYTHONUTF8", "0");
      qunsetenv("PYTHONIOENCODING"); // 不许外层环境替服务兜底
    }
    ~EnvGuard()
    {
      qputenv("LC_ALL", lcAll);
      qputenv("PYTHONCOERCECLOCALE", coerce);
      qputenv("PYTHONUTF8", utf8);
      qputenv("PYTHONIOENCODING", ioEnc);
    }
  } envGuard;
  ScriptRunnerService svc;
  QSignalSpy finishedSpy(&svc, &ScriptRunnerService::runFinished);
  QStringList lines;
  connect(&svc, &ScriptRunnerService::runOutput, this,
          [&lines](qint64, const QString &line, bool) { lines.append(line); });
  const qint64 id =
      svc.start({fixture(QStringLiteral("utf8_echo.py")), {}, {}, m_python, 0});
  QVERIFY(id > 0);
  QVERIFY(finishedSpy.wait(30000));
  QCOMPARE(finishedSpy.takeFirst().at(2).toInt(), 0); // 非 UnicodeEncodeError 退出
  QVERIFY2(lines.contains(QStringLiteral("中文-数据-管线")),
           qPrintable(lines.join(QLatin1Char('|'))));
#else
  QSKIP("locale coercion fixture is Unix-specific");
#endif
}

void TestScriptRunner::unbrokenFloodOutputStaysBounded()
{
  // #479：无换行的巨量输出不得无上限积攒半行缓冲——超上限（1 MiB）即按
  // 强制行冲刷。夹具一次性写 2 MiB 无换行：修复前收尾冲刷出单条 2 MiB 行，
  // 修复后恒按 ≤1 MiB 切块，总量不丢。
  ScriptRunnerService svc;
  QSignalSpy finishedSpy(&svc, &ScriptRunnerService::runFinished);
  qint64 maxLineChars = 0, totalChars = 0;
  connect(&svc, &ScriptRunnerService::runOutput, this,
          [&](qint64, const QString &line, bool stderrChannel) {
            // 只计 stdout：Windows 托管 Python 会往 stderr 打 54 字符的
            // "Could not find platform independent libraries <prefix>"，
            // 混进总量即 2097206 ≠ 2 MiB（windows job 恒红）。
            if (stderrChannel)
              return;
            maxLineChars = qMax(maxLineChars, qint64(line.size()));
            totalChars += line.size();
          });
  const qint64 id =
      svc.start({fixture(QStringLiteral("flood.py")), {}, {}, m_python, 0});
  QVERIFY(id > 0);
  QVERIFY(finishedSpy.wait(30000));
  QCOMPARE(finishedSpy.takeFirst().at(2).toInt(), 0);
  const qint64 expected = 2 * 1024 * 1024; // 'X' ASCII：QString 尺寸 == 字节数
  QCOMPARE(totalChars, expected);          // 不丢字节
  QVERIFY2(maxLineChars <= 1024 * 1024,    // 单行恒有界
           qPrintable(QStringLiteral("max line chars: %1").arg(maxLineChars)));
  QVERIFY(maxLineChars == 1024 * 1024);    // 确实发生了强制切块
}

void TestScriptRunner::timeoutTerminatesRun()
{
  ScriptRunnerService svc;
  QSignalSpy finishedSpy(&svc, &ScriptRunnerService::runFinished);
  QElapsedTimer timer;
  timer.start();
  const qint64 id = svc.start({fixture(QStringLiteral("sleep.py")),
                               {QStringLiteral("60")}, {}, m_python, 1500});
  QVERIFY(id > 0);
  QVERIFY(finishedSpy.wait(30000));
  const QList<QVariant> args = finishedSpy.takeFirst();
  QCOMPARE(args.at(4).toBool(), false); // cancelled
  QCOMPARE(args.at(5).toBool(), true);  // timedOut
  QVERIFY2(timer.elapsed() < paleo::perfbudget::relaxedBudgetMs(20000),
           "timeout escalation took too long");
}

void TestScriptRunner::cancelTerminatesRun()
{
  ScriptRunnerService svc;
  QSignalSpy startedSpy(&svc, &ScriptRunnerService::runStarted);
  QSignalSpy finishedSpy(&svc, &ScriptRunnerService::runFinished);
  QElapsedTimer timer;
  timer.start();
  const qint64 id = svc.start(
      {fixture(QStringLiteral("sleep.py")), {QStringLiteral("60")}, {}, m_python, 0});
  QVERIFY(id > 0);
  // runStarted 在 start() 内同步发出——先查计数再等，两种时序都成立。
  QVERIFY(startedSpy.count() == 1 || startedSpy.wait(10000));
  QTRY_VERIFY_WITH_TIMEOUT(svc.processId(id) > 0, 10000);
  svc.cancel(id);
  QVERIFY(finishedSpy.wait(30000));
  const QList<QVariant> args = finishedSpy.takeFirst();
  QCOMPARE(args.at(4).toBool(), true);  // cancelled
  QCOMPARE(args.at(5).toBool(), false); // timedOut
  QVERIFY2(timer.elapsed() < paleo::perfbudget::relaxedBudgetMs(20000),
           "cancel escalation took too long");
}

void TestScriptRunner::cancelEscalatesToKill()
{
  // ignore_term.py 吃掉 SIGTERM——kill 递进必须把它了结（Windows 上
  // TerminateProcess 本就不可忽略，同一条断言两端成立）。
  ScriptRunnerService svc;
  QSignalSpy startedSpy(&svc, &ScriptRunnerService::runStarted);
  QSignalSpy finishedSpy(&svc, &ScriptRunnerService::runFinished);
  QStringList lines;
  connect(&svc, &ScriptRunnerService::runOutput, this,
          [&lines](qint64, const QString &line, bool) { lines.append(line); });
  QElapsedTimer timer;
  timer.start();
  const qint64 id = svc.start(
      {fixture(QStringLiteral("ignore_term.py")), {}, {}, m_python, 0});
  QVERIFY(id > 0);
  QVERIFY(startedSpy.count() == 1 || startedSpy.wait(10000));
  QTRY_VERIFY_WITH_TIMEOUT(lines.contains(QStringLiteral("TERM_IGNORED")), 10000);
  svc.cancel(id);
  QVERIFY(finishedSpy.wait(30000));
  QCOMPARE(finishedSpy.takeFirst().at(4).toBool(), true); // cancelled
  // 优雅窗口 2s + 强杀；留出进程启动开销后仍须远小于「忽略 SIGTERM 就
  // 收不掉」的情形。
  QVERIFY2(timer.elapsed() < paleo::perfbudget::relaxedBudgetMs(15000),
           "kill escalation took too long");
}

void TestScriptRunner::gateQueuesThirdRun()
{
  ScriptRunnerService svc; // 缺省闸 = 2
  QCOMPARE(svc.maxConcurrent(), 2);
  QSignalSpy queuedSpy(&svc, &ScriptRunnerService::runQueued);
  QSignalSpy finishedSpy(&svc, &ScriptRunnerService::runFinished);
  QList<qint64> startedOrder, finishedOrder;
  connect(&svc, &ScriptRunnerService::runStarted, this,
          [&startedOrder](qint64 id, const QString &) { startedOrder.append(id); });
  connect(&svc, &ScriptRunnerService::runFinished, this,
          [&finishedOrder](qint64 id, const QString &, int, bool, bool, bool) {
            finishedOrder.append(id);
          });
  const qint64 id1 = svc.start(
      {fixture(QStringLiteral("sleep.py")), {QStringLiteral("5")}, {}, m_python, 0});
  const qint64 id2 = svc.start(
      {fixture(QStringLiteral("sleep.py")), {QStringLiteral("5")}, {}, m_python, 0});
  const qint64 id3 =
      svc.start({fixture(QStringLiteral("echo_args.py")), {}, {}, m_python, 0});
  QVERIFY(id1 > 0 && id2 > 0 && id3 > 0);
  QCOMPARE(svc.runningCount(), 2);
  QCOMPARE(svc.queuedCount(), 1);
  QCOMPARE(queuedSpy.count(), 1);
  QCOMPARE(queuedSpy.takeFirst().at(0).toLongLong(), id3);
  QVERIFY(svc.isQueued(id3));
  // 第 3 个必须等闸内空位：id3 的 runStarted 到来时已有运行了结。
  bool thirdStartedAfterSlotFreed = false;
  connect(&svc, &ScriptRunnerService::runStarted, this,
          [&](qint64 id, const QString &) {
            if (id == id3)
              thirdStartedAfterSlotFreed =
                  finishedOrder.contains(id1) || finishedOrder.contains(id2);
          });
  while (finishedSpy.count() < 3)
    QVERIFY(finishedSpy.wait(30000));
  QCOMPARE(startedOrder.count(), 3);
  QVERIFY(startedOrder.indexOf(id3) > startedOrder.indexOf(id2));
  QVERIFY(thirdStartedAfterSlotFreed);
  QCOMPARE(svc.runningCount(), 0);
  QCOMPARE(svc.queuedCount(), 0);
}

void TestScriptRunner::queuedCancelFinishesWithoutStart()
{
  ScriptRunnerService svc;
  QSignalSpy startedSpy(&svc, &ScriptRunnerService::runStarted);
  QSignalSpy finishedSpy(&svc, &ScriptRunnerService::runFinished);
  const qint64 id1 = svc.start(
      {fixture(QStringLiteral("sleep.py")), {QStringLiteral("30")}, {}, m_python, 0});
  const qint64 id2 = svc.start(
      {fixture(QStringLiteral("sleep.py")), {QStringLiteral("30")}, {}, m_python, 0});
  const qint64 id3 =
      svc.start({fixture(QStringLiteral("echo_args.py")), {}, {}, m_python, 0});
  QVERIFY(svc.isQueued(id3));
  svc.cancel(id3);
  // 排队项取消：runFinished(cancelled=true) 同步到来，且永无 runStarted。
  QVERIFY(finishedSpy.count() == 1 || finishedSpy.wait(10000));
  const QList<QVariant> args = finishedSpy.takeFirst();
  QCOMPARE(args.at(0).toLongLong(), id3);
  QCOMPARE(args.at(4).toBool(), true);
  QCOMPARE(startedSpy.count(), 2); // 只有 id1/id2
  QVERIFY(!svc.isQueued(id3));
  svc.cancel(id1);
  svc.cancel(id2);
  while (finishedSpy.count() < 2)
    QVERIFY(finishedSpy.wait(30000));
}

void TestScriptRunner::startFailureIsHonest()
{
  ScriptRunnerService svc;
  QString error;
  const qint64 id = svc.start(
      {QStringLiteral("/nonexistent/nope.py"), {}, {}, m_python, 0}, &error);
  QCOMPARE(id, qint64(0));
  QVERIFY(error.contains(QStringLiteral("nope.py")));
  // 解释器覆盖为不可执行路径同样前置拒绝。
  error.clear();
  const qint64 id2 =
      svc.start({fixture(QStringLiteral("echo_args.py")), {}, {},
                 QStringLiteral("/nonexistent/python"), 0},
                &error);
  QCOMPARE(id2, qint64(0));
  QVERIFY(!error.isEmpty());
}

void TestScriptRunner::failedToStartReportsCrashed()
{
  // 目录有可执行位（x=可进入），能过前置 isExecutable 校验但 exec 必然
  // 失败——覆盖运行时 FailedToStart 路径（crashed=true, exitCode=-1）。
  ScriptRunnerService svc;
  QSignalSpy finishedSpy(&svc, &ScriptRunnerService::runFinished);
  const qint64 id = svc.start(
      {fixture(QStringLiteral("echo_args.py")), {}, {}, QDir::tempPath(), 0});
  QVERIFY(id > 0);
  QVERIFY(finishedSpy.wait(30000));
  const QList<QVariant> args = finishedSpy.takeFirst();
  QCOMPARE(args.at(2).toInt(), -1);
  QCOMPARE(args.at(3).toBool(), true); // crashed
}

void TestScriptRunner::noOrphanAfterCancel()
{
#ifdef Q_OS_UNIX
  ScriptRunnerService svc;
  QSignalSpy startedSpy(&svc, &ScriptRunnerService::runStarted);
  QSignalSpy finishedSpy(&svc, &ScriptRunnerService::runFinished);
  const qint64 id = svc.start(
      {fixture(QStringLiteral("sleep.py")), {QStringLiteral("60")}, {}, m_python, 0});
  QVERIFY(id > 0);
  QVERIFY(startedSpy.count() == 1 || startedSpy.wait(10000));
  qint64 pid = 0;
  QTRY_VERIFY_WITH_TIMEOUT((pid = svc.processId(id)) > 0, 10000);
  svc.cancel(id);
  QVERIFY(finishedSpy.wait(30000));
  // 进程已回收：kill(pid, 0) 须 ESRCH（僵尸也已被 QProcess 收割）。
  QCOMPARE(::kill(static_cast<pid_t>(pid), 0), -1);
  QCOMPARE(errno, ESRCH);
#else
  QSKIP("orphan assertion is Unix-specific");
#endif
}

// —— #960 进程树回收（父/子/孙真实进程夹具 spawn_tree.py）——
// pid 存活：Unix 用 kill(pid,0)（僵尸也算存活，等 init 收尸后 ESRCH）；
// Windows 用进程句柄等待退出（不以 PID 不存在冒充退出证明）。
bool TestScriptRunner::pidDead(qint64 pid)
{
  if (pid <= 0)
    return true;
#ifdef Q_OS_UNIX
  return ::kill(static_cast<pid_t>(pid), 0) == -1 && errno == ESRCH;
#else
  HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                               DWORD(pid));
  if (!process)
    return GetLastError() == ERROR_INVALID_PARAMETER; // PID 已不存在
  const bool exited = WaitForSingleObject(process, 10000) == WAIT_OBJECT_0;
  CloseHandle(process);
  return exited;
#endif
}

QList<qint64> TestScriptRunner::treePidsFromSentinel(const QString &sentinel)
{
  QList<qint64> pids;
  QFile file(sentinel);
  if (file.open(QIODevice::ReadOnly))
  {
    const auto parts = QString::fromUtf8(file.readAll()).split(QChar(' '),
                                                               Qt::SkipEmptyParts);
    for (const auto &part : parts)
      pids.append(part.toLongLong());
  }
  return pids;
}

// 主进程先退（parent 模式父进程等 sentinel 后立即退出）、子孙仍存活并持有
// 输出管道：runFinished 到来即本次运行整树已了结——子孙全部退出、文件锁
// （sentinel 句柄）随进程释放。
void TestScriptRunner::treeDescendantsReclaimedWhenMainExitsFirst()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString sentinel = dir.filePath(QStringLiteral("tree.sentinel"));
  ScriptRunnerService svc;
  QSignalSpy finishedSpy(&svc, &ScriptRunnerService::runFinished);
  const qint64 id = svc.start(
      {fixture(QStringLiteral("spawn_tree.py")),
       {sentinel, QStringLiteral("parent")}, dir.path(), m_python, 0});
  QVERIFY(id > 0);
  QVERIFY(finishedSpy.wait(30000));
  const QList<qint64> pids = treePidsFromSentinel(sentinel);
  QCOMPARE(pids.size(), 2); // child + grand
  for (const qint64 pid : pids)
    QTRY_VERIFY2_WITH_TIMEOUT(pidDead(pid), qPrintable(QString::number(pid)), 8000);
  // 文件锁释放：可删除 sentinel（Windows 上仍被持有则删除失败）。
  QVERIFY(QFile::remove(sentinel));
}

// 取消：树建立后 cancel——主/子/孙整树回收，runFinished(cancelled=true)
// 只发一次，并发运行的其他任务不受连坐。
void TestScriptRunner::treeDescendantsReclaimedOnCancel()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString sentinelA = dir.filePath(QStringLiteral("a.sentinel"));
  const QString sentinelB = dir.filePath(QStringLiteral("b.sentinel"));
  ScriptRunnerService svc;
  QSignalSpy finishedSpy(&svc, &ScriptRunnerService::runFinished);
  QStringList lines;
  connect(&svc, &ScriptRunnerService::runOutput, this,
          [&lines](qint64, const QString &line, bool) { lines.append(line); });
  const qint64 idA = svc.start(
      {fixture(QStringLiteral("spawn_tree.py")), {sentinelA, QStringLiteral("hold")},
       dir.path(), m_python, 0});
  const qint64 idB = svc.start(
      {fixture(QStringLiteral("spawn_tree.py")), {sentinelB, QStringLiteral("hold")},
       dir.path(), m_python, 0});
  QVERIFY(idA > 0 && idB > 0);
  // 两棵树都建好（各打印一条 TREE_CHILD 行）再取消 A。
  const auto treeChildLines = [&lines] {
    int count = 0;
    for (const QString &line : lines)
      if (line.startsWith(QStringLiteral("TREE_CHILD:")))
        ++count;
    return count;
  };
  QTRY_VERIFY_WITH_TIMEOUT(treeChildLines() >= 2, 30000);
  svc.cancel(idA);
  QVERIFY(finishedSpy.wait(30000));
  const QList<QVariant> argsA = finishedSpy.takeFirst();
  QCOMPARE(argsA.at(0).toLongLong(), idA);
  QCOMPARE(argsA.at(4).toBool(), true); // cancelled
  for (const qint64 pid : treePidsFromSentinel(sentinelA))
    QTRY_VERIFY2_WITH_TIMEOUT(pidDead(pid), qPrintable(QString::number(pid)), 8000);
  // B 不受连坐：仍在运行，随后正常取消并整树回收。
  QVERIFY(svc.isRunning(idB));
  svc.cancel(idB);
  QTRY_VERIFY_WITH_TIMEOUT(finishedSpy.count() >= 1, 30000);
  for (const qint64 pid : treePidsFromSentinel(sentinelB))
    QTRY_VERIFY2_WITH_TIMEOUT(pidDead(pid), qPrintable(QString::number(pid)), 8000);
}

// 超时优先标记：超时递进同样回收整树。
void TestScriptRunner::treeDescendantsReclaimedOnTimeout()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString sentinel = dir.filePath(QStringLiteral("t.sentinel"));
  ScriptRunnerService svc;
  QSignalSpy finishedSpy(&svc, &ScriptRunnerService::runFinished);
  const qint64 id = svc.start(
      {fixture(QStringLiteral("spawn_tree.py")), {sentinel, QStringLiteral("hold")},
       dir.path(), m_python, 1500});
  QVERIFY(id > 0);
  QVERIFY(finishedSpy.wait(30000));
  const QList<QVariant> args = finishedSpy.takeFirst();
  QCOMPARE(args.at(4).toBool(), false); // cancelled
  QCOMPARE(args.at(5).toBool(), true);  // timedOut
  for (const qint64 pid : treePidsFromSentinel(sentinel))
    QTRY_VERIFY2_WITH_TIMEOUT(pidDead(pid), qPrintable(QString::number(pid)), 8000);
}

// 服务析构：运行中的树随析构整组回收，不依赖事件循环继续派发。
void TestScriptRunner::treeDescendantsReclaimedOnDestructor()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString sentinel = dir.filePath(QStringLiteral("d.sentinel"));
  QStringList lines;
  std::unique_ptr<ScriptRunnerService> svc = std::make_unique<ScriptRunnerService>();
  connect(svc.get(), &ScriptRunnerService::runOutput, this,
          [&lines](qint64, const QString &line, bool) { lines.append(line); });
  const qint64 id = svc->start(
      {fixture(QStringLiteral("spawn_tree.py")), {sentinel, QStringLiteral("hold")},
       dir.path(), m_python, 0});
  QVERIFY(id > 0);
  const auto childReported = [&lines] {
    for (const QString &line : lines)
      if (line.startsWith(QStringLiteral("TREE_CHILD:")))
        return true;
    return false;
  };
  QTRY_VERIFY_WITH_TIMEOUT(childReported(), 30000);
  QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(sentinel), 30000);
  const auto pids = treePidsFromSentinel(sentinel);
  QVERIFY(pids.size() == 2); // child + grand
  svc.reset(); // 析构不等 exit()——整树强制回收
  for (const qint64 pid : pids)
    QTRY_VERIFY2_WITH_TIMEOUT(pidDead(pid), qPrintable(QString::number(pid)), 8000);
}

QTEST_GUILESS_MAIN(TestScriptRunner)
#include "tst_scriptrunner.moc"
