// 层：数据
#include "pythonrepl.h"

#include "pythonenv.h"

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
#ifdef Q_OS_UNIX
  if (m_pid > 0)
    ::kill(-m_pid, SIGKILL);
#endif
  proc->kill();
  proc->waitForFinished(kStopGraceMs);
  delete proc;
  m_pid = 0;
}

void PythonReplSession::start(const QString &interpreter)
{
  if (m_proc)
    return;
  m_interpreter = interpreter.isEmpty() ? PythonEnvService::findBasePython() : interpreter;
  if (m_interpreter.isEmpty() || !QFileInfo(m_interpreter).isExecutable())
  {
    emit startFailed(tr("未找到 Python 解释器——请安装 Python 3，或设置 "
                        "PALEO_PYTHON 环境变量指向解释器"));
    return;
  }
  m_pid = 0;
  auto *proc = new QProcess(this);
  m_proc = proc;
  proc->setProcessChannelMode(QProcess::SeparateChannels);
#ifdef Q_OS_UNIX
  // 独立进程组：停止/析构时整组回收（REPL 里 os.system 拉起的子进程连坐）。
  proc->setChildProcessModifier([] { ::setsid(); });
#endif
  auto env = QProcessEnvironment::systemEnvironment();
  env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
  env.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
  // 3.13+ 新 REPL（pyrepl）的着色/多行特性不利管道回显，强制基础 REPL；
  // 旧版本不认识该变量，静默忽略。
  env.insert(QStringLiteral("PYTHON_BASIC_REPL"), QStringLiteral("1"));
  proc->setProcessEnvironment(env);
  connect(proc, &QProcess::started, this, [this] {
    if (m_proc)
    {
      m_pid = m_proc->processId();
      emit started(m_interpreter);
    }
  });
  connect(proc, &QProcess::readyReadStandardOutput, this, [this] {
    if (m_proc)
      emit output(QString::fromLocal8Bit(m_proc->readAllStandardOutput()), false);
  });
  connect(proc, &QProcess::readyReadStandardError, this, [this] {
    if (m_proc)
      emit output(QString::fromLocal8Bit(m_proc->readAllStandardError()), true);
  });
  connect(proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
    if (err != QProcess::FailedToStart || !m_proc)
      return;
    QProcess *p = m_proc;
    m_proc = nullptr;
    m_pid = 0;
    p->disconnect(this);
    p->deleteLater();
    emit startFailed(tr("无法启动 Python：%1").arg(m_interpreter));
  });
  connect(proc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
          [this](int code, QProcess::ExitStatus status) {
            teardown(code, status != QProcess::NormalExit);
          });
  proc->start(m_interpreter,
              {QStringLiteral("-i"), QStringLiteral("-u"), QStringLiteral("-q")});
}

void PythonReplSession::stop()
{
  if (!m_proc || m_stopTimer)
    return;
  // 先礼后兵：exit() 让解释器走正常退出路径。
  m_proc->write("exit()\n");
  m_stopTimer = new QTimer(this);
  m_stopTimer->setSingleShot(true);
  connect(m_stopTimer, &QTimer::timeout, this, &PythonReplSession::escalateStop);
  m_stopTimer->start(kStopGraceMs);
}

void PythonReplSession::escalateStop()
{
  if (!m_proc)
    return;
  if (m_stopTimer)
  {
    m_stopTimer->deleteLater();
    m_stopTimer = nullptr;
  }
#ifdef Q_OS_UNIX
  if (m_pid > 0)
    ::kill(-m_pid, SIGTERM);
  else
    m_proc->terminate();
#else
  m_proc->terminate();
#endif
  m_stopTimer = new QTimer(this);
  m_stopTimer->setSingleShot(true);
  connect(m_stopTimer, &QTimer::timeout, this, [this] {
    if (!m_proc)
      return;
    m_stopTimer->deleteLater();
    m_stopTimer = nullptr;
#ifdef Q_OS_UNIX
    if (m_pid > 0)
      ::kill(-m_pid, SIGKILL);
#endif
    m_proc->kill();
  });
  m_stopTimer->start(kStopGraceMs);
}

void PythonReplSession::sendLine(const QString &line)
{
  if (!m_proc)
    return;
  m_proc->write(line.toLocal8Bit() + '\n');
}

void PythonReplSession::teardown(int exitCode, bool crashed)
{
  QProcess *proc = m_proc;
  if (!proc)
    return;
  m_proc = nullptr;
  m_pid = 0;
  if (m_stopTimer)
  {
    m_stopTimer->stop();
    delete m_stopTimer;
    m_stopTimer = nullptr;
  }
  proc->disconnect(this);
  proc->deleteLater();
  emit finished(exitCode, crashed);
}
