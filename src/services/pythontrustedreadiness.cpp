// 层：数据
#include "pythontrustedreadiness.h"
#include "pythonprocess.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>

namespace paleo::python {
namespace {
constexpr char ProbeSource[]="import sys,numpy as np\n"
  "assert float(np.arange(5,dtype=np.float64)@np.arange(5,dtype=np.float64))==30.0\n"
  "sys.stdout.buffer.write(b'paleo.trusted-readiness.v1\\n')\n";
constexpr char ProbeSha[]="82e94359c1eccff051c032379685bc5d91958db7a15d88c23cbed20212ec5db5";
constexpr char ProbeOutput[]="paleo.trusted-readiness.v1\n";
constexpr qint64 QueueMilliseconds=15000;
QJsonObject state(const QString &code,const RuntimeResult &runtime={},bool busy=false) {
  QJsonObject out{{"contract","paleo.python-trusted-readiness.v1"},{"profile",QLatin1String(ProfileId)},
    {"runtimeReady",runtime.ready()},{"executionReady",code=="ok"},{"ready",code=="ok"},{"busy",busy},{"code",code}};
  if(runtime.ready())out.insert("manifestSha256",runtime.runtime->manifestSha256);
  return out;
}
bool stopped(const std::function<bool()> &cancelled) {
  try{return cancelled && cancelled();}catch(...){return true;}
}
}
TrustedReadinessProvider::TrustedReadinessProvider(std::shared_ptr<RuntimeProvider> runtime)
  :m_runtime(std::move(runtime)),m_last(state("python_trusted_execution_unprobed")) {}
QJsonObject TrustedReadinessProvider::snapshot() const {
  if(!m_runtime)return state("python_environment_disabled");
  const auto policy=m_runtime->policy();
  if(const auto code=policy.validate();!code.isEmpty())return state(code);
  if(!policy.allowTrusted)return state("python_execution_denied",m_runtime->snapshot());
  if(!m_mutex.tryLock())return state("python_trusted_readiness_probing",m_runtime->snapshot(),true);
  const auto unlock=qScopeGuard([this]{m_mutex.unlock();});
  const auto runtime=m_runtime->snapshot();
  if(!runtime.ready())return state(runtime.code,runtime);
  if(m_last.value("manifestSha256").toString()!=runtime.runtime->manifestSha256)
    return state("python_trusted_execution_unprobed",runtime);
  return m_last;
}
QJsonObject TrustedReadinessProvider::inspect(const std::function<bool()> &cancelled) {
  const auto *app=QCoreApplication::instance();
  if(!app || QThread::currentThread()==app->thread())return state("python_readiness_worker_required");
  QElapsedTimer queue;queue.start();
  const auto userStopped=[&]{return stopped(cancelled);};
  const auto queueStopped=[&]{return userStopped() || queue.elapsed()>=QueueMilliseconds;};
  while(!m_mutex.tryLock(25)) {
    if(queueStopped())return state(userStopped()?"cancelled":"python_readiness_queue_timeout");
  }
  const auto unlock=qScopeGuard([this]{m_mutex.unlock();});
  RuntimeResult runtime;
  const auto finish=[&](const QString &code){m_last=state(code,runtime);return m_last;};
  m_last=state("python_trusted_readiness_probing",{},true);
  if(userStopped())return finish("cancelled");
  if(!m_runtime)return finish("python_environment_disabled");
  auto policy=m_runtime->policy();
  if(const auto code=policy.validate();!code.isEmpty())return finish(code);
  if(!policy.allowTrusted)return finish("python_execution_denied");
  const auto permit=claimExecutionPermit(queueStopped);
  if(!permit)return finish(userStopped()?"cancelled":"python_readiness_queue_timeout");
  runtime=m_runtime->inspect(userStopped);
  if(!runtime.ready())return finish(runtime.code);
  if(userStopped())return finish("cancelled");
  const QByteArray source(ProbeSource);
  if(source.size()>policy.codeBytes)return finish("python_script_invalid");
  if(source.size()>policy.diskBytes)return finish("python_input_disk_budget_exceeded");
  QTemporaryDir directory;if(!directory.isValid())return finish("python_probe_workspace_unavailable");
  const auto path=directory.filePath("code.py");QFile file(path);
  if(!file.open(QIODevice::WriteOnly)||file.write(source)!=source.size())return finish("python_probe_workspace_unavailable");
  file.close();
  policy.cpuSeconds=qMin(policy.cpuSeconds,5);policy.wallSeconds=qMin(policy.wallSeconds,10);
  policy.outputBytes=qMin<qint64>(policy.outputBytes,1024);policy.logBytes=qMin<qint64>(policy.logBytes,1024);
  TrustedProcessRequest request;request.runtime=runtime.runtime;request.scriptPath=path;
  request.scriptSha256=QLatin1String(ProbeSha);request.policy=policy;request.cancelled=userStopped;request.permit=permit;
  const auto process=runTrustedProcess(request);
  if(userStopped())return finish("cancelled");
  if(!process.ok())return finish(process.code);
  if(process.output!=QByteArray(ProbeOutput)||!process.error.isEmpty())return finish("python_trusted_probe_mismatch");
  return finish("ok");
}
} // namespace paleo::python
