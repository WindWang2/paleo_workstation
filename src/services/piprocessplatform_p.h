// 层：数据
#pragma once
#include <QProcess>
#include <QProcessEnvironment>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(Q_OS_LINUX)
#include <signal.h>
#include <sys/prctl.h>
#include <unistd.h>
#endif

namespace paleo::pi::detail {
inline QProcessEnvironment isolatedProcessEnvironment(const QString &directory) {
  QProcessEnvironment environment;
  environment.insert("LANG", "C.UTF-8");
  for (const auto &key : QStringList{"HOME", "USERPROFILE", "TEMP", "TMP", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME"})
    environment.insert(key, directory);
#ifdef Q_OS_WIN
  environment.insert("SystemRoot", qEnvironmentVariable("SystemRoot"));
#endif
  return environment;
}

// Shared lifetime controls for the fixed trusted Node entry, not an OS sandbox.
// Callers retain their own verification, launch, deadlines, cancellation and reap.
class ProcessTree final {
public:
  ProcessTree() {
#ifdef Q_OS_WIN
    m_job = CreateJobObjectW(nullptr, nullptr);
    if (!m_job) return;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    m_available = SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != 0;
#endif
  }
  ~ProcessTree() {
#ifdef Q_OS_WIN
    if (m_job) CloseHandle(m_job);
#endif
  }
  ProcessTree(const ProcessTree &) = delete;
  ProcessTree &operator=(const ProcessTree &) = delete;
  bool available() const {
#ifdef Q_OS_WIN
    return m_available;
#else
    return true;
#endif
  }
#if defined(Q_OS_LINUX) && QT_VERSION < QT_VERSION_CHECK(6, 6, 0)
  // Both consumers reject these Qt versions before configuring a child.
  // A future caller cannot silently skip the required Linux controls.
  void configure(QProcess &) const = delete;
#else
  void configure(QProcess &process) const {
#ifdef Q_OS_LINUX
    const auto parent = ::getpid();
    process.setChildProcessModifier([parent] {
      if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || ::prctl(PR_SET_PDEATHSIG, SIGKILL) || ::getppid() != parent) ::_exit(125);
    });
    QProcess::UnixProcessParameters parameters;
    parameters.flags = QProcess::UnixProcessFlag::CloseFileDescriptors;
    process.setUnixProcessParameters(parameters);
#else
    Q_UNUSED(process);
#endif
  }
#endif
  bool attach(QProcess &process) const {
#ifdef Q_OS_WIN
    const auto child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(process.processId()));
    const bool assigned = child && AssignProcessToJobObject(m_job, child);
    if (child) CloseHandle(child);
    return assigned;
#else
    Q_UNUSED(process);
    return true;
#endif
  }
private:
#ifdef Q_OS_WIN
  HANDLE m_job = nullptr;
  bool m_available = false;
#endif
};
} // namespace paleo::pi::detail
