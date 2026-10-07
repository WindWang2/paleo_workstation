// 层：数据
#include "scriptrunner.h"

#include "pythonenv.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>

#ifdef Q_OS_UNIX
#include <csignal>
#include <unistd.h>
#endif

namespace
{
  // 取消/超时的优雅退出窗口：terminate（Unix=进程组 SIGTERM）后给脚本
  // finally/atexit 清理机会，仍未退出再 kill（先例 stratigraphicwebsession）。
  constexpr int kKillGraceMs = 2000;
} // namespace

struct ScriptRunnerService::Run
{
  qint64 id = 0;
  RunRequest request;
  QProcess *proc = nullptr;
  QTimer *timeoutTimer = nullptr;
  QTimer *killTimer = nullptr;
  QByteArray outBuffer; // 行组包残留（跨 readyRead 的半行）
  QByteArray errBuffer;
  bool cancelRequested = false;
  bool timedOut = false;
  bool finishing = false; // finishRun 收尾中（cancel 此时无意义且会谎报状态）
  qint64 pid = 0;
};

ScriptRunnerService::ScriptRunnerService(QObject *parent) : QObject(parent) {}

ScriptRunnerService::~ScriptRunnerService()
{
  m_shuttingDown = true;
  // 排队项直接丢弃（宿主退出，不再发信号）；运行项强制整组回收。
  qDeleteAll(m_queue);
  m_queue.clear();
  for (Run *run : std::as_const(m_running))
  {
    if (run->timeoutTimer)
    {
      run->timeoutTimer->stop();
      delete run->timeoutTimer;
      run->timeoutTimer = nullptr;
    }
    if (run->killTimer)
    {
      run->killTimer->stop();
      delete run->killTimer;
      run->killTimer = nullptr;
    }
    if (run->proc)
    {
      run->proc->disconnect(this);
#ifdef Q_OS_UNIX
      if (run->pid > 0)
        ::kill(-run->pid, SIGKILL);
#endif
      run->proc->kill();
      run->proc->waitForFinished(1000);
      delete run->proc;
      run->proc = nullptr;
    }
    delete run;
  }
  m_running.clear();
}

qint64 ScriptRunnerService::start(const RunRequest &request, QString *error)
{
  const auto fail = [error](const QString &reason) -> qint64 {
    if (error)
      *error = reason;
    return 0;
  };
  if (m_shuttingDown)
    return fail(tr("脚本运行服务正在关闭"));
  QString scriptPath = request.scriptPath;
  if (QDir(scriptPath).isRelative() && !request.workingDir.isEmpty())
    scriptPath = QDir(request.workingDir).absoluteFilePath(scriptPath);
  const QFileInfo scriptInfo(scriptPath);
  if (scriptPath.isEmpty() || !scriptInfo.isFile())
    return fail(tr("脚本文件不存在：%1").arg(request.scriptPath));
  QString interpreter = request.interpreter;
  if (interpreter.isEmpty())
    interpreter = PythonEnvService::findBasePython();
  if (interpreter.isEmpty())
    return fail(tr("未找到 Python 解释器——请安装 Python 3，或设置 PALEO_PYTHON "
                   "环境变量指向解释器"));
  if (!QFileInfo(interpreter).isExecutable())
    return fail(tr("Python 解释器不可执行：%1").arg(interpreter));

  auto *run = new Run;
  run->id = m_nextId++;
  run->request = request;
  run->request.scriptPath = scriptInfo.absoluteFilePath();
  run->request.interpreter = interpreter;
  if (run->request.workingDir.isEmpty())
    run->request.workingDir = scriptInfo.absolutePath();

  if (m_running.size() >= m_maxConcurrent)
  {
    m_queue.append(run);
    updateBusy();
    emit runQueued(run->id, run->request.scriptPath);
    return run->id;
  }
  m_running.append(run);
  launch(run);
  updateBusy();
  return run->id;
}

void ScriptRunnerService::cancel(qint64 runId)
{
  for (int i = 0; i < m_queue.size(); ++i)
  {
    Run *run = m_queue.at(i);
    if (run->id != runId)
      continue;
    m_queue.removeAt(i);
    const QString scriptPath = run->request.scriptPath;
    delete run;
    emit runFinished(runId, scriptPath, -1, false, true, false);
    updateBusy();
    return;
  }
  Run *run = findRun(runId);
  if (!run || run->finishing)
    return; // finishing：退出收尾（冲刷输出）中，取消无意义且会谎报状态
  run->cancelRequested = true;
  terminateGracefully(run);
}

bool ScriptRunnerService::isRunning(qint64 runId) const { return findRun(runId) != nullptr; }

bool ScriptRunnerService::isQueued(qint64 runId) const
{
  for (const Run *run : m_queue)
  {
    if (run->id == runId)
      return true;
  }
  return false;
}

int ScriptRunnerService::runningCount() const { return int(m_running.size()); }
int ScriptRunnerService::queuedCount() const { return int(m_queue.size()); }

qint64 ScriptRunnerService::processId(qint64 runId) const
{
  const Run *run = findRun(runId);
  return run ? run->pid : 0;
}

void ScriptRunnerService::setMaxConcurrent(int n)
{
  n = qBound(1, n, 4);
  if (n == m_maxConcurrent)
    return;
  m_maxConcurrent = n;
  pumpQueue();
  updateBusy();
}

ScriptRunnerService::Run *ScriptRunnerService::findRun(qint64 runId) const
{
  for (Run *run : m_running)
  {
    if (run->id == runId)
      return run;
  }
  return nullptr;
}

void ScriptRunnerService::updateBusy()
{
  const bool busy = !m_running.isEmpty() || !m_queue.isEmpty();
  if (busy == m_busy)
    return;
  m_busy = busy;
  emit busyChanged(busy);
}

void ScriptRunnerService::pumpQueue()
{
  while (m_running.size() < m_maxConcurrent && !m_queue.isEmpty())
  {
    Run *run = m_queue.takeFirst();
    m_running.append(run);
    launch(run);
  }
}

void ScriptRunnerService::launch(Run *run)
{
  auto *proc = new QProcess(this);
  run->proc = proc;
  proc->setProcessChannelMode(QProcess::SeparateChannels);
#ifdef Q_OS_UNIX
  // 独立进程组：取消/超时时整组回收脚本拉起的子进程，不留孤儿
  //（stratigraphicwebsession 先例）。
  proc->setChildProcessModifier([] { ::setsid(); });
#endif
  auto env = QProcessEnvironment::systemEnvironment();
  // 无缓冲保证流式回传的时效；不落 __pycache__ 污染脚本目录。
  env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
  env.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
  proc->setProcessEnvironment(env);
  proc->setWorkingDirectory(run->request.workingDir);

  connect(proc, &QProcess::started, this, [this, run] {
    if (!run->proc)
      return;
    run->pid = run->proc->processId();
    // started 送达前已被取消/超时：terminateGracefully 已在 pid==0 时跑过
    //（terminate 落空、killTimer 已武装），此刻 pid 就位——补整组 SIGTERM，
    // 不再等满优雅窗口。若 killTimer 已耗尽（spawn 慢于优雅窗口，SIGKILL 档
    // 在 pid==0 时落空、单发不再武装），递进链在此闭合：直接整组 SIGKILL，
    // 保证忽略 SIGTERM 的脚本也必然了结。
    if ((run->cancelRequested || run->timedOut) && run->pid > 0)
    {
      const bool graceElapsed = run->killTimer && !run->killTimer->isActive();
#ifdef Q_OS_UNIX
      ::kill(-run->pid, graceElapsed ? SIGKILL : SIGTERM);
      if (graceElapsed)
        run->proc->kill();
#else
      if (graceElapsed)
        run->proc->kill();
      else
        run->proc->terminate();
#endif
    }
  });
  connect(proc, &QProcess::readyReadStandardOutput, this,
          [this, run] { drainChannel(run, false, false); });
  connect(proc, &QProcess::readyReadStandardError, this,
          [this, run] { drainChannel(run, true, false); });
  connect(proc, &QProcess::errorOccurred, this, [this, run](QProcess::ProcessError err) {
    if (err == QProcess::FailedToStart)
      finishRun(run, -1, true);
  });
  connect(proc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
          [this, run](int exitCode, QProcess::ExitStatus status) {
            finishRun(run, exitCode, status != QProcess::NormalExit);
          });
  if (run->request.timeoutMs > 0)
  {
    run->timeoutTimer = new QTimer(proc);
    run->timeoutTimer->setSingleShot(true);
    connect(run->timeoutTimer, &QTimer::timeout, this, [this, run] {
      run->timedOut = true;
      terminateGracefully(run);
    });
    run->timeoutTimer->start(run->request.timeoutMs);
  }
  proc->start(run->request.interpreter,
              QStringList{run->request.scriptPath} + run->request.args);
  emit runStarted(run->id, run->request.scriptPath);
}

void ScriptRunnerService::drainChannel(Run *run, bool stderrChannel, bool flush)
{
  QProcess *proc = run->proc;
  if (!proc)
    return;
  QByteArray &buffer = stderrChannel ? run->errBuffer : run->outBuffer;
  buffer += stderrChannel ? proc->readAllStandardError() : proc->readAllStandardOutput();
  int nl = -1;
  while ((nl = buffer.indexOf('\n')) >= 0)
  {
    QByteArray line = buffer.left(nl);
    buffer.remove(0, nl + 1);
    if (line.endsWith('\r'))
      line.chop(1);
    emit runOutput(run->id, QString::fromLocal8Bit(line), stderrChannel);
  }
  if (flush && !buffer.isEmpty())
  {
    QByteArray line = buffer;
    buffer.clear();
    if (line.endsWith('\r'))
      line.chop(1);
    emit runOutput(run->id, QString::fromLocal8Bit(line), stderrChannel);
  }
}

void ScriptRunnerService::terminateGracefully(Run *run)
{
  if (!run->proc || run->killTimer)
    return;
#ifdef Q_OS_UNIX
  if (run->pid > 0)
    ::kill(-run->pid, SIGTERM); // 整组：脚本拉起的子进程一并收到
  else
    run->proc->terminate();
#else
  run->proc->terminate();
#endif
  run->killTimer = new QTimer(run->proc);
  run->killTimer->setSingleShot(true);
  connect(run->killTimer, &QTimer::timeout, this, [this, run] {
    QProcess *proc = run->proc;
    if (!proc)
      return;
#ifdef Q_OS_UNIX
    if (run->pid > 0)
      ::kill(-run->pid, SIGKILL);
#endif
    proc->kill();
  });
  run->killTimer->start(kKillGraceMs);
}

void ScriptRunnerService::finishRun(Run *run, int exitCode, bool crashed)
{
  QProcess *proc = run->proc;
  if (!proc)
    return; // 已了结（防御：finished 与 FailedToStart 不会双发）
  run->finishing = true;
  // 冲刷残余输出：残缺行作为最后一行发出，保持整行纪律。
  drainChannel(run, false, true);
  drainChannel(run, true, true);
  run->proc = nullptr;
  if (run->timeoutTimer)
  {
    run->timeoutTimer->stop();
    delete run->timeoutTimer;
    run->timeoutTimer = nullptr;
  }
  if (run->killTimer)
  {
    run->killTimer->stop();
    delete run->killTimer;
    run->killTimer = nullptr;
  }
  proc->disconnect(this);
  const qint64 id = run->id;
  const QString scriptPath = run->request.scriptPath;
  const bool cancelled = run->cancelRequested && !run->timedOut;
  const bool timedOut = run->timedOut;
  m_running.removeAll(run);
  proc->deleteLater();
  // timers 已停且删、proc 已 disconnect——没有任何挂起路径会再触碰
  // run（全部同线程直连），直接释放。
  delete run;
  emit runFinished(id, scriptPath, exitCode, crashed, cancelled, timedOut);
  pumpQueue();
  updateBusy();
}
