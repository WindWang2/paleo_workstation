// 层：数据
#include "pythonsandboxprovider.h"
#include "pythoninputbundle.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#if defined(Q_OS_WIN)
#include "pythonsandbox_windows.h"
#elif defined(Q_OS_LINUX)
#include "pythonsandbox_linux.h"
#endif

namespace paleo::python {
namespace {
constexpr auto ProbeContract="paleo.python-sandbox-readiness.v1";
QJsonObject unavailable(const QString &code) {return {{"code",code},{"ready",false},{"checksAtStart",true}};}
bool ownerThread() {const auto *app=QCoreApplication::instance();return !app || QThread::currentThread()==app->thread();}
QByteArray probeSource() {
  return R"PY(import json, sys
import numpy as np
request = json.load(sys.stdin)
if request != {'probe': 'paleo.python-sandbox-readiness.v1'}:
    raise ValueError('unexpected readiness input')
a = np.array([[1.0, 2.0], [3.0, 4.0]], dtype=np.float64)
b = np.array([[5.0, 6.0], [7.0, 8.0]], dtype=np.float64)
value = {'contract': 'paleo.python-sandbox-readiness.v1',
         'numpy': np.__version__, 'python': '.'.join(map(str, sys.version_info[:3])),
         'product': np.matmul(a, b).tolist()}
sys.stdout.buffer.write((json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False) + '\n').encode('utf-8'))
)PY";
}
QString probeHash() {return QString::fromLatin1(QCryptographicHash::hash(probeSource(),QCryptographicHash::Sha256).toHex());}
QByteArray expectedOutput() {
  return QByteArray("{\"contract\":\"paleo.python-sandbox-readiness.v1\",\"numpy\":\"")+NumpyVersion+
    "\",\"product\":[[19.0,22.0],[43.0,50.0]],\"python\":\""+PythonVersion+"\"}\n";
}
QJsonObject platformState() {
#ifdef Q_OS_WIN
  auto state=windowsSandboxReadiness();state["ready"]=false;return state;
#elif defined(Q_OS_LINUX)
  return {{"code",QT_VERSION>=QT_VERSION_CHECK(6,7,0)?"ok":"python_sandbox_qt_unsupported"},
    {"backend","linux-sealed-bwrap-cgroup.v1"},{"checksAtStart",true},{"network",false},{"processDiskWrites",false},
    {"cpuParallelismLimit",1},{"threadCountEnforced",true},{"ready",false}};
#else
  return unavailable("python_sandbox_unavailable");
#endif
}
SandboxResult dispatch(const SandboxRequest &request) {
#ifdef Q_OS_WIN
  return runWindowsSandbox(request);
#elif defined(Q_OS_LINUX)
  return runLinuxSandbox(request);
#else
  Q_UNUSED(request);return {};
#endif
}
void evidence(QJsonObject &state,const SandboxResult &result) {
  state["cpuMicros"]=QString::number(result.cpuMicros);state["observedPeakMemoryBytes"]=QString::number(result.peakMemoryBytes);
  state["memoryPeakComplete"]=result.memoryPeakComplete;state["hostSnapshotReservedBytes"]=QString::number(result.hostSnapshotReservedBytes);
  state["childMemoryLimitBytes"]=QString::number(result.childMemoryLimitBytes);
}
}
SandboxProvider::SandboxProvider(std::shared_ptr<RuntimeProvider> runtime):m_runtime(std::move(runtime)) {}
QJsonObject SandboxProvider::snapshot() const {
  if(!m_mutex.tryLock())return unavailable("python_sandbox_probe_pending");
  const auto state=m_state.isEmpty()?unavailable("python_sandbox_not_probed"):m_state;m_mutex.unlock();
  if(state.value("ready").toBool() && m_runtime) {
    const auto runtime=m_runtime->snapshot();if(!runtime.ready())return unavailable(runtime.code);
    if(state.value("runtimeManifestSha256")!=runtime.runtime->manifestSha256 || state.value("policy")!=QJsonValue(m_runtime->policy().json()))
      return unavailable("python_sandbox_changed");
  }
  return state;
}
QJsonObject SandboxProvider::inspect(const std::function<bool()> &cancelled) {
  if(ownerThread())return unavailable("python_readiness_worker_required");
  QElapsedTimer queue;queue.start();
  const auto queueStopped=[&]{return (cancelled && cancelled()) || queue.elapsed()>=15000;};
  while(!m_mutex.tryLock(25))if(queueStopped())return unavailable(cancelled && cancelled()?"cancelled":"python_sandbox_probe_busy");
  const auto unlock=qScopeGuard([this]{m_mutex.unlock();});auto state=platformState();
  state["requiresRuntimeProbe"]=true;state["runtimeProbeVerified"]=false;
  state["probeContract"]=QLatin1String(ProbeContract);state["probeCodeSha256"]=probeHash();
  const auto fail=[&](const QString &code){state["code"]=code;state["ready"]=false;m_state=state;return state;};
  if(queueStopped())return fail(cancelled && cancelled()?"cancelled":"python_sandbox_probe_busy");
  if(!m_runtime)return fail("python_environment_disabled");
  const auto policy=m_runtime->policy();if(!policy.validate().isEmpty())return fail("python_policy_invalid");
  if(!policy.allowGenerated)return fail("python_execution_denied");
  if(state.value("code")!="ok")return fail(state.value("code").toString("python_sandbox_unavailable"));
  const auto permit=claimExecutionPermit(queueStopped);
  if(!permit)return fail(cancelled && cancelled()?"cancelled":"python_sandbox_probe_busy");
  QElapsedTimer elapsed;elapsed.start();bool userStopped=false,timedOut=false;
  const auto stopped=[&]{
    if(cancelled && cancelled())userStopped=true;
    if(elapsed.elapsed()>=qint64(policy.wallSeconds)*1000)timedOut=true;
    return userStopped || timedOut;
  };
  const auto stoppedCode=[&]{return userStopped?QStringLiteral("cancelled"):QStringLiteral("python_process_timeout");};
  const auto runtime=m_runtime->inspectIntegrity(stopped);if(stopped())return fail(stoppedCode());
  if(!runtime.ready())return fail(runtime.code);
  state["runtimeManifestSha256"]=runtime.runtime->manifestSha256;state["policy"]=policy.json();
  QTemporaryDir project;if(!project.isValid())return fail("python_sandbox_probe_input_unavailable");
  const auto control=QJsonDocument(QJsonObject{{"probe",QLatin1String(ProbeContract)}}).toJson(QJsonDocument::Compact);
  const auto input=freezeInputBundle(project.path(),{{"code.py",probeSource()},{"request.json",control}},
    {{"inputProtocol",QLatin1String(SandboxJsonInputProtocol)},{"purpose","runtime-readiness"}},policy,stopped);
  if(stopped())return fail(stoppedCode());if(!input.ok())return fail(input.code);
  SandboxRequest request;request.runtime=runtime.runtime;request.input=std::make_shared<InputBundle>(input);
  request.policy=policy;request.permit=permit;request.cancelled=stopped;
  // The admission probe itself uses the real strong backend, before any cached
  // readiness exists. No trusted-process mode or widened policy is introduced.
  const auto result=dispatch(request);evidence(state,result);
  if(!result.error.isEmpty())state["diagnostic"]=QString::fromUtf8(result.error.first(qMin<qint64>(result.error.size(),qMin<qint64>(4096,policy.logBytes))));
  if(stopped())return fail(stoppedCode());if(!result.ok())return fail(result.code);
  if(result.output!=expectedOutput())return fail("python_sandbox_probe_output_invalid");
  state["code"]="ok";state["ready"]=true;state["requiresRuntimeProbe"]=false;state["runtimeProbeVerified"]=true;
  state["probeElapsedMilliseconds"]=QString::number(elapsed.elapsed());m_state=state;return state;
}
SandboxResult SandboxProvider::run(const SandboxRequest &request) const {
  const auto fail=[](const QString &code){SandboxResult result;result.code=code;return result;};
  if(ownerThread())return fail("python_execution_worker_required");
  if(!m_runtime)return fail("python_environment_disabled");
  const auto policy=m_runtime->policy();if(!policy.allowGenerated || !request.policy.allowGenerated)return fail("python_execution_denied");
  if(!policy.validate().isEmpty() || request.policy.json()!=policy.json())return fail("python_policy_changed");
  const auto runtime=m_runtime->snapshot();if(!runtime.ready())return fail(runtime.code);
  if(!request.runtime || request.runtime->manifestSha256!=runtime.runtime->manifestSha256 ||
      request.runtime->root!=runtime.runtime->root || request.runtime->interpreter!=runtime.runtime->interpreter)return fail("python_runtime_changed");
  const auto state=snapshot();if(!state.value("ready").toBool())return fail(state.value("code").toString("python_sandbox_unavailable"));
  if(state.value("runtimeManifestSha256")!=runtime.runtime->manifestSha256 || state.value("policy")!=QJsonValue(policy.json()) ||
      !state.value("runtimeProbeVerified").toBool() || state.value("probeCodeSha256")!=probeHash())return fail("python_sandbox_changed");
  return dispatch(request);
}
}
