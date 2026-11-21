// 层：数据
#include "scriptprocessscope.h"

#include <QElapsedTimer>
#include <QProcess>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#else
#include <QtGlobal>
#endif

#ifdef Q_OS_WIN
namespace
{
// 树内进程的统一退出码（与受控链 TerminateJobObject 口径一致）。
constexpr DWORD kTreeExitCode = 90;
// Job 活跃进程计数的轮询步长：查询为权威口径，等待只是节流。
constexpr DWORD kIdlePollMs = 10;

std::atomic<bool> g_forceCreateFailure{false};

// 用户脚本的 Job 只求「树可回收」：仅 KILL_ON_JOB_CLOSE。不设
// ActiveProcess/内存/CPU 上限（显式用户脚本允许正常 subprocess），
// 不启用 breakaway（脱离 Job 的进程会逃出关闭句柄时的整树兜底）。
bool createOwnedJob(HANDLE *job)
{
  *job = CreateJobObjectW(nullptr, nullptr);
  if (!*job)
    return false;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(*job, JobObjectExtendedLimitInformation, &limits,
                               sizeof(limits)))
  {
    CloseHandle(*job);
    *job = nullptr;
    return false;
  }
  return true;
}
} // namespace

struct ScriptProcessScope::State
{
  HANDLE job = nullptr;
  void *attributeMemory = nullptr;
  LPPROC_THREAD_ATTRIBUTE_LIST attributes = nullptr;
  STARTUPINFOEXW startup{};
  HANDLE inherited[3]{};
  bool available = false; // Job + 属性表就绪
  bool prepared = false;  // 最近一次创建属性已备齐

  ~State()
  {
    if (attributes)
      DeleteProcThreadAttributeList(attributes);
    if (attributeMemory)
      HeapFree(GetProcessHeap(), 0, attributeMemory);
    if (job)
    {
      // 先显式终止再关句柄：销毁即收敛；KILL_ON_JOB_CLOSE 仍是内核兜底。
      TerminateJobObject(job, kTreeExitCode);
      CloseHandle(job);
    }
  }
};

void scriptprocessscope_test::forceCreateFailure() { g_forceCreateFailure.store(true); }
void scriptprocessscope_test::clearCreateFailure() { g_forceCreateFailure.store(false); }

void clearProcessLaunchModifier(QProcess *process)
{
  // 修饰器闭包捕获 Scope 的原生状态；进程转入待销毁状态前先释放闭包。
  process->setCreateProcessArgumentsModifier({});
}

ScriptProcessScope::ScriptProcessScope() : m_state(new State)
{
  if (g_forceCreateFailure.load())
    return;
  if (!createOwnedJob(&m_state->job))
    return;
  SIZE_T bytes = 0;
  InitializeProcThreadAttributeList(nullptr, 2, 0, &bytes);
  if (!bytes)
    return;
  m_state->attributeMemory = HeapAlloc(GetProcessHeap(), 0, bytes);
  if (!m_state->attributeMemory)
    return;
  auto attributes = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(m_state->attributeMemory);
  if (!InitializeProcThreadAttributeList(attributes, 2, 0, &bytes))
    return;
  m_state->attributes = attributes;
  m_state->available = true;
}

ScriptProcessScope::~ScriptProcessScope() { delete m_state; }
bool ScriptProcessScope::isValid() const { return m_state && m_state->available; }
bool ScriptProcessScope::launchPrepared() const { return m_state && m_state->prepared; }

void ScriptProcessScope::configure(QProcess &process)
{
  process.setCreateProcessArgumentsModifier(
      [state = m_state](QProcess::CreateProcessArguments *arguments) {
        state->prepared = false;
        arguments->inheritHandles = false;
        const STARTUPINFOW original =
            arguments->startupInfo ? *arguments->startupInfo : STARTUPINFOW{};
        // 毒化兜底：任一步失败则保持 cb=0 的扩展启动结构——CreateProcess
        // 必然失败（QProcess 报 FailedToStart），不会退回无 Job 进程。
        // 修饰器没有否决返回值，这是唯一的拒绝通道。
        state->startup = {};
        state->startup.StartupInfo.cb = 0;
        arguments->startupInfo = &state->startup.StartupInfo;
        arguments->flags |= EXTENDED_STARTUPINFO_PRESENT;
        // 管道模式必有 STARTF_USESTDHANDLES；其余通道形态直接拒绝。
        if (!state->available || !(original.dwFlags & STARTF_USESTDHANDLES))
          return;
        state->inherited[0] = original.hStdInput;
        state->inherited[1] = original.hStdOutput;
        state->inherited[2] = original.hStdError;
        for (const HANDLE handle : state->inherited)
        {
          DWORD flags = 0;
          if (!handle || handle == INVALID_HANDLE_VALUE ||
              GetFileType(handle) != FILE_TYPE_PIPE ||
              !GetHandleInformation(handle, &flags) || !(flags & HANDLE_FLAG_INHERIT))
            return;
        }
        if (state->inherited[0] == state->inherited[1] ||
            state->inherited[0] == state->inherited[2] ||
            state->inherited[1] == state->inherited[2])
          return;
        HANDLE jobs[] = {state->job};
        if (!UpdateProcThreadAttribute(state->attributes, 0,
                                       PROC_THREAD_ATTRIBUTE_JOB_LIST, jobs, sizeof(jobs),
                                       nullptr, nullptr))
          return;
        if (!UpdateProcThreadAttribute(state->attributes, 0,
                                       PROC_THREAD_ATTRIBUTE_HANDLE_LIST, state->inherited,
                                       sizeof(state->inherited), nullptr, nullptr))
          return;
        // Qt 传入的 STARTUPINFOW 较小；整体搬进自有扩展存储。属性表与句柄
        // 数组需存活到 start() 内 CreateProcess 返回——State 由 Run 持有。
        state->startup.StartupInfo = original;
        state->startup.StartupInfo.cb = sizeof(state->startup);
        state->startup.lpAttributeList = state->attributes;
        arguments->inheritHandles = true; // 管道需继承；HANDLE_LIST 限定实际继承面
        state->prepared = true;
      });
}

bool ScriptProcessScope::terminateTree()
{
  return m_state && m_state->job && TerminateJobObject(m_state->job, kTreeExitCode) != FALSE;
}

bool ScriptProcessScope::treeIdle() const
{
  if (!m_state || !m_state->job)
    return true;
  JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
  if (!QueryInformationJobObject(m_state->job, JobObjectBasicAccountingInformation, &info,
                                 sizeof(info), nullptr))
    return true; // Job 句柄已失效：无存活进程可言，不阻塞调用方
  return info.ActiveProcesses == 0;
}

bool ScriptProcessScope::waitForTreeIdle(int timeoutMs)
{
  QElapsedTimer clock;
  clock.start();
  while (!treeIdle())
  {
    if (clock.elapsed() >= timeoutMs)
      return false;
    Sleep(kIdlePollMs);
  }
  return true;
}

bool ScriptProcessScope::reclaim(int timeoutMs)
{
  if (!m_state || !m_state->job)
    return true;
  if (treeIdle())
    return true;
  if (!terminateTree())
    return false;
  return waitForTreeIdle(timeoutMs);
}
#else
// Unix：进程组归属与整组信号由调用方既有 setsid/kill(-pid) 语义承担。
struct ScriptProcessScope::State {};

void scriptprocessscope_test::forceCreateFailure() {}
void scriptprocessscope_test::clearCreateFailure() {}

void clearProcessLaunchModifier(QProcess *)
{
  // setCreateProcessArgumentsModifier 仅存在于 Windows Qt；Unix 无修饰器。
}

ScriptProcessScope::ScriptProcessScope() : m_state(nullptr) {}
ScriptProcessScope::~ScriptProcessScope() { delete m_state; }
bool ScriptProcessScope::isValid() const { return true; }
void ScriptProcessScope::configure(QProcess &) {}
bool ScriptProcessScope::launchPrepared() const { return true; }
bool ScriptProcessScope::terminateTree() { return false; }
bool ScriptProcessScope::treeIdle() const { return true; }
bool ScriptProcessScope::waitForTreeIdle(int) { return true; }
bool ScriptProcessScope::reclaim(int) { return true; }
#endif
