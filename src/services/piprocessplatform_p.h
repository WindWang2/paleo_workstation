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
    if (!SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return;
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    if (!bytes) return;
    m_attributeMemory = HeapAlloc(GetProcessHeap(), 0, bytes);
    if (!m_attributeMemory) return;
    auto attributes = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(m_attributeMemory);
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) return;
    m_attributes = attributes; m_available = true;
#endif
  }
  ~ProcessTree() {
#ifdef Q_OS_WIN
    if (m_attributes) DeleteProcThreadAttributeList(m_attributes);
    if (m_attributeMemory) HeapFree(GetProcessHeap(), 0, m_attributeMemory);
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
  void configure(QProcess &process) {
#ifdef Q_OS_LINUX
    const auto parent = ::getpid();
    process.setChildProcessModifier([parent] {
      if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || ::prctl(PR_SET_PDEATHSIG, SIGKILL) || ::getppid() != parent) ::_exit(125);
    });
    QProcess::UnixProcessParameters parameters;
    parameters.flags = QProcess::UnixProcessFlag::CloseFileDescriptors;
    process.setUnixProcessParameters(parameters);
#elif defined(Q_OS_WIN)
    process.setCreateProcessArgumentsModifier([this](QProcess::CreateProcessArguments *arguments) {
      restrictHandles(arguments);
    });
#else
    Q_UNUSED(process);
#endif
  }
#endif
  bool attach(QProcess &process) const {
#ifdef Q_OS_WIN
    if (!m_startupReady) return false;
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
  void restrictHandles(QProcess::CreateProcessArguments *arguments) {
    m_startupReady = false;
    arguments->inheritHandles = false;
    const auto original = arguments->startupInfo ? *arguments->startupInfo : STARTUPINFOW{};
    // A Windows modifier has no veto return. Request a failed extended startup
    // until the entire whitelist succeeds; attach() independently refuses it.
    m_startup = {}; m_startup.StartupInfo.cb = 0; arguments->startupInfo = &m_startup.StartupInfo;
    arguments->flags |= EXTENDED_STARTUPINFO_PRESENT;
    if (!m_available || !(original.dwFlags & STARTF_USESTDHANDLES)) return;
    m_inherited[0] = original.hStdInput; m_inherited[1] = original.hStdOutput; m_inherited[2] = original.hStdError;
    for (const auto handle : m_inherited) {
      DWORD flags = 0;
      if (!handle || handle == INVALID_HANDLE_VALUE || GetFileType(handle) != FILE_TYPE_PIPE ||
          !GetHandleInformation(handle, &flags) || !(flags & HANDLE_FLAG_INHERIT)) return;
    }
    if (m_inherited[0] == m_inherited[1] || m_inherited[0] == m_inherited[2] || m_inherited[1] == m_inherited[2]) return;
    if (!UpdateProcThreadAttribute(m_attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  m_inherited, sizeof(m_inherited), nullptr, nullptr)) return;
    // Qt supplies a smaller STARTUPINFOW; copy it into our own extended storage.
    // The list and its handle-value array remain alive until QProcess is gone.
    m_startup.StartupInfo = original; m_startup.StartupInfo.cb = sizeof(m_startup);
    m_startup.lpAttributeList = m_attributes;
    arguments->inheritHandles = true; m_startupReady = true;
  }
  HANDLE m_job = nullptr;
  void *m_attributeMemory = nullptr;
  LPPROC_THREAD_ATTRIBUTE_LIST m_attributes = nullptr;
  STARTUPINFOEXW m_startup{};
  HANDLE m_inherited[3]{};
  bool m_available = false, m_startupReady = false;
#endif
};
} // namespace paleo::pi::detail
