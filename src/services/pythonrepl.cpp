// 层：数据
#include "pythonrepl.h"

#include "pythonenv.h"
#include "scriptprocessscope.h"

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
  // stop() 递进窗口：exit() → 1s → terminate(SIGTERM 整组) → 1s → kill。
  constexpr int kStopGraceMs = 1000;
  // 解释器退出后给 Job 内残留后代的有界收敛窗口（#960）。
  constexpr int kStopReapMs = 2000;
} // namespace

PythonReplSession::PythonReplSession(QObject *parent) : QObject(parent) {}

PythonReplSession::~PythonReplSession()
{
  if (m_stopTimer)
  {
    m_stopTimer->stop();
    delete m_stopTimer;
    m_stopTimer = nullptr;
  }
  if (!m_proc)
    return;
  QProcess *proc = m_proc;
  m_proc = nullptr;
  proc->disconnect(this);
  clearProcessLaunchModifier(proc);
#ifdef Q_OS_UNIX
  if (m_pid > 0)
    ::kill(-m_pid, SIGKILL);
#endif
  // Windows：整树终止 + 有界等待——REPL 里 os.system/subprocess 拉起的
  // 后代连坐回收；不依赖事件循环继续派发。
  if (m_scope)
  {
    m_scope->terminateTree();
    m_scope->waitForTreeIdle(kStopReapMs);
  }
  proc->kill();
  proc->waitForFinished(kStopGraceMs);
  delete proc;
  m_pid = 0;
}

void PythonReplSession::start(const QString &interpreter)
{
  if (m_proc)
    return;
  // 新会话：代次 +1，旧会话的迟到 timer/进程信号按代次丢弃。
  ++m_generation;
  m_stopPhase = StopPhase::None;
  m_interpreter = interpreter.isEmpty() ? PythonEnvService::findBasePython() : interpreter;
  if (m_interpreter.isEmpty() || !QFileInfo(m_interpreter).isExecutable())
  {
    emit startFailed(tr("未找到 Python 解释器——请安装 Python 3，或设置 "
                        "PALEO_PYTHON 环境变量"));
    return;
  }
  m_pid = 0;
  auto *proc = new QProcess(this);
  m_proc = proc;
  m_scope = std::make_unique<ScriptProcessScope>();
  proc->setProcessChannelMode(QProcess::SeparateChannels);
#ifdef Q_OS_UNIX
  // 独立进程组：停止/析构时整组回收（REPL 里 os.system 拉起的子进程连坐）。
  proc->setChildProcessModifier([] { ::setsid(); });
#endif
  // Windows：创建时 Job 归属——归属/属性失败由修饰器以 FailedToStart 拒绝
  //（落到下方 startFailed 合同），不回退无归属进程（#960）。
  m_scope->configure(*proc);
  auto env = QProcessEnvironment::systemEnvironment();
  env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
  env.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
  // Python 3 子进程 IO 统一 UTF-8（#479）：管道编码钉死，宿主侧 toUtf8/
  // fromUtf8——Windows 非 UTF-8 locale 下中文输入输出不再乱码。
  env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
  // 3.13+ 新 REPL（pyrepl）的着色/多行特性不利管道回显，强制基础 REPL；
  // 旧版本不认识该变量，静默忽略。
  env.insert(QStringLiteral("PYTHON_BASIC_REPL"), QStringLiteral("1"));
  proc->setProcessEnvironment(env);
  const quint64 generation = m_generation;
  connect(proc, &QProcess::started, this, [this, generation] {
    if (generation != m_generation || !m_proc)
      return;
    m_pid = m_proc->processId();
    emit started(m_interpreter);
  });
  connect(proc, &QProcess::readyReadStandardOutput, this, [this, generation] {
    if (generation != m_generation || !m_proc)
      return;
    emit output(QString::fromUtf8(m_proc->readAllStandardOutput()), false);
  });
  connect(proc, &QProcess::readyReadStandardError, this, [this, generation] {
    if (generation != m_generation || !m_proc)
      return;
    emit output(QString::fromUtf8(m_proc->readAllStandardError()), true);
  });
  connect(proc, &QProcess::errorOccurred, this,
          [this, generation](QProcess::ProcessError err) {
            if (err != QProcess::FailedToStart || generation != m_generation || !m_proc)
              return;
            QProcess *p = m_proc;
            m_proc = nullptr;
            m_pid = 0;
            m_stopPhase = StopPhase::None;
            p->disconnect(this);
            clearProcessLaunchModifier(p);
            p->deleteLater();
            m_scope.reset();
            emit startFailed(tr("无法启动 Python：%1").arg(m_interpreter));
          });
  connect(proc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
          [this, generation](int code, QProcess::ExitStatus status) {
            if (generation != m_generation)
              return; // 旧会话的 finished 不触碰新会话
            teardown(code, status != QProcess::NormalExit);
          });
  proc->start(m_interpreter,
              {QStringLiteral("-i"), QStringLiteral("-u"), QStringLiteral("-q")});
}

void PythonReplSession::stop()
{
  if (!m_proc || m_stopPhase != StopPhase::None)
    return; // 连续 stop 幂等；停止递进中不重置档位
  // 先礼后兵：exit() 让解释器走正常退出路径。
  m_proc->write("exit()\n");
  m_stopPhase = StopPhase::Polite;
  armStopTimer(kStopGraceMs);
}

void PythonReplSession::armStopTimer(int timeoutMs)
{
  if (m_stopTimer)
  {
    m_stopTimer->stop();
    delete m_stopTimer; // 立即销毁：不留可再触发的挂起 timer
  }
  m_stopTimer = new QTimer(this);
  m_stopTimer->setSingleShot(true);
  const quint64 generation = m_generation;
  connect(m_stopTimer, &QTimer::timeout, this, [this, generation] {
    if (generation != m_generation || !m_stopTimer)
      return; // 旧会话的 timer 不命中新会话
    m_stopTimer->stop();
    delete m_stopTimer;
    m_stopTimer = nullptr;
    advanceStop();
  });
  m_stopTimer->start(timeoutMs);
}

void PythonReplSession::advanceStop()
{
  if (!m_proc)
  {
    m_stopPhase = StopPhase::None;
    return;
  }
  if (m_stopPhase == StopPhase::Polite)
  {
    m_stopPhase = StopPhase::Terminating;
#ifdef Q_OS_UNIX
    if (m_pid > 0)
      ::kill(-m_pid, SIGTERM);
    else
      m_proc->terminate();
#else
    m_proc->terminate(); // Windows 温和尝试；控制台常忽略——下一档整树强停
#endif
    armStopTimer(kStopGraceMs);
    return;
  }
  if (m_stopPhase == StopPhase::Terminating)
    forceStop();
}

void PythonReplSession::forceStop()
{
  if (!m_proc)
  {
    m_stopPhase = StopPhase::None;
    return;
  }
  m_stopPhase = StopPhase::Killing;
#ifdef Q_OS_UNIX
  if (m_pid > 0)
    ::kill(-m_pid, SIGKILL);
#endif
  // Windows：整树强停（含 REPL 拉起的后代）；proc->kill() 保留 Qt 侧
  // CrashExit 语义。
  if (m_scope)
    m_scope->terminateTree();
  m_proc->kill();
}

void PythonReplSession::sendLine(const QString &line)
{
  if (!m_proc)
    return;
  m_proc->write(line.toUtf8() + '\n');
}

void PythonReplSession::teardown(int exitCode, bool crashed)
{
  QProcess *proc = m_proc;
  if (!proc)
    return;
  const qint64 pid = m_pid;
  m_proc = nullptr;
  m_pid = 0;
  m_stopPhase = StopPhase::None;
  if (m_stopTimer)
  {
    m_stopTimer->stop();
    delete m_stopTimer;
    m_stopTimer = nullptr;
  }
  // 有界接收残余输出：解释器退出前已写管道但未触发 readyRead 的字节不丢。
  const QByteArray out = proc->readAllStandardOutput();
  const QByteArray err = proc->readAllStandardError();
  proc->disconnect(this);
  clearProcessLaunchModifier(proc);
  // 整树收敛（#960）：解释器先退、后代仍存活/持管道时在此强停 + 有界等待
  // ——「解释器已退」不掩盖仍存活的后代；收敛后才发 finished。
#ifdef Q_OS_UNIX
  if (pid > 0)
    ::kill(-pid, SIGKILL); // 组 id 在成员存活期间不可复用（同 scriptrunner）
#endif
  if (m_scope)
    m_scope->reclaim(kStopReapMs);
  proc->deleteLater();
  if (!out.isEmpty())
    emit output(QString::fromUtf8(out), false);
  if (!err.isEmpty())
    emit output(QString::fromUtf8(err), true);
  m_scope.reset(); // Job 句柄关闭（KILL_ON_JOB_CLOSE 内核兜底）
  emit finished(exitCode, crashed);
}
