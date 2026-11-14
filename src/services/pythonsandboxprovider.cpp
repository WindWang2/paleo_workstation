// 层：数据
#include "pythonsandboxprovider.h"
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QThread>
#include <QScopeGuard>
#if defined(Q_OS_WIN)
#include "pythonsandbox_windows.h"
#elif defined(Q_OS_LINUX) && QT_VERSION >= QT_VERSION_CHECK(6,7,0)
#include "pythonsandbox_linux.h"
#endif

namespace paleo::python {
namespace {
QJsonObject unavailable(const QString &code) {return {{"code",code},{"ready",false},{"checksAtStart",true}};}
}
QJsonObject SandboxProvider::snapshot() const {
  if(!m_mutex.tryLock())return unavailable("python_sandbox_probe_pending");
  const auto result=m_state.isEmpty()?unavailable("python_sandbox_not_probed"):m_state;m_mutex.unlock();return result;
}
QJsonObject SandboxProvider::inspect(const std::function<bool()> &cancelled) {
  const auto *app=QCoreApplication::instance();
  if(!app || QThread::currentThread()==app->thread())return unavailable("python_readiness_worker_required");
  QElapsedTimer deadline;deadline.start();
  const auto stopped=[&]{return (cancelled && cancelled()) || deadline.elapsed()>=15000;};
  while(!m_mutex.tryLock(25))if(stopped())return unavailable(cancelled && cancelled()?"cancelled":"python_sandbox_probe_busy");
  const auto unlock=qScopeGuard([this]{m_mutex.unlock();});
  QJsonObject state;
  if(stopped())return unavailable(cancelled && cancelled()?"cancelled":"python_sandbox_probe_busy");
#if defined(Q_OS_WIN)
  state=windowsSandboxReadiness();
#elif defined(Q_OS_LINUX) && QT_VERSION >= QT_VERSION_CHECK(6,7,0)
  const auto code=linuxSandboxReadiness(stopped);
  state={{"code",code},{"backend","linux-sealed-bwrap-cgroup.v1"},{"checksAtStart",true},
    {"network",false},{"processDiskWrites",false},{"cpuParallelismLimit",1},{"threadCountEnforced",true}};
#else
  state=unavailable("python_sandbox_unavailable");
#endif
  if(stopped())return unavailable(cancelled && cancelled()?"cancelled":"python_sandbox_probe_busy");
  state["ready"]=state.value("code")==QLatin1String("ok");m_state=state;return state;
}
SandboxResult SandboxProvider::run(const SandboxRequest &request) const {
  if(!request.policy.allowGenerated){SandboxResult result;result.code="python_execution_denied";return result;}
#if defined(Q_OS_WIN)
  return runWindowsSandbox(request);
#elif defined(Q_OS_LINUX) && QT_VERSION >= QT_VERSION_CHECK(6,7,0)
  return runLinuxSandbox(request);
#else
  Q_UNUSED(request);return {};
#endif
}
}
