// 层：数据
#include "pythonruntime.h"
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>

namespace paleo::python {
QJsonObject ExecutionPolicy::json() const {
  return {{"id",id},{"codeBytes",double(codeBytes)},{"inputBytes",double(inputBytes)},
    {"outputBytes",double(outputBytes)},{"logBytes",double(logBytes)},{"diskBytes",double(diskBytes)},
    {"memoryBytes",double(memoryBytes)},{"cpuSeconds",cpuSeconds},{"wallSeconds",wallSeconds},
    {"threads",threads},{"concurrent",concurrent},{"allowTrusted",allowTrusted},{"allowGenerated",allowGenerated}};
}
QString ExecutionPolicy::validate() const {
  const ExecutionPolicy ceiling;
  if(id!=ceiling.id || codeBytes<1 || codeBytes>ceiling.codeBytes || inputBytes<1 || inputBytes>ceiling.inputBytes ||
      outputBytes<1 || outputBytes>ceiling.outputBytes || logBytes<1 || logBytes>ceiling.logBytes ||
      diskBytes<1 || diskBytes>ceiling.diskBytes || memoryBytes<1 || memoryBytes>ceiling.memoryBytes ||
      cpuSeconds<1 || cpuSeconds>ceiling.cpuSeconds || wallSeconds<1 || wallSeconds>ceiling.wallSeconds ||
      threads!=1 || concurrent!=1)return "python_policy_invalid";
  return {};
}
QJsonObject RuntimeResult::json() const {
  QJsonObject out{{"contract","paleo.python-environment.v1"},{"ready",ready()},{"code",code},
    {"profile",QLatin1String(ProfileId)},{"generatedCodeAvailable",false}};
  if(runtime) {
    out.insert("runtimeVersion",runtime->runtimeVersion);out.insert("platform",runtime->platform);
    out.insert("manifestSha256",runtime->manifestSha256);
  }
  return out;
}
QString probeRuntime(const VerifiedRuntime &runtime,const std::function<bool()> &cancelled) {
  QProcess process;QProcessEnvironment env;
  env.insert("PATH",QFileInfo(runtime.interpreter).absolutePath());
#ifdef Q_OS_WIN
  const auto systemRoot=qEnvironmentVariable("SystemRoot");
  if(!systemRoot.isEmpty())env.insert("SystemRoot",systemRoot);
#endif
  env.insert("OPENBLAS_NUM_THREADS","1");env.insert("OMP_NUM_THREADS","1");env.insert("MKL_NUM_THREADS","1");
  process.setProcessEnvironment(env);process.setWorkingDirectory(runtime.root);
  const QString probe="import sys,json,numpy,os;print(json.dumps({'python':'.'.join(map(str,sys.version_info[:3])),'numpy':numpy.__version__,'isolated':sys.flags.isolated,'utf8':sys.flags.utf8_mode,'bytecode':sys.dont_write_bytecode,'path':os.environ.get('PATH','')}))";
  process.start(runtime.interpreter,{"-I","-u","-B","-X","utf8","-c",probe});
  QElapsedTimer elapsed;elapsed.start();QByteArray out,err;
  while(process.state()!=QProcess::NotRunning) {
    process.waitForFinished(25);out+=process.readAllStandardOutput();err+=process.readAllStandardError();
    const bool stop=cancelled && cancelled();
    if(stop || elapsed.elapsed()>10000 || out.size()+err.size()>16*1024) {
      process.kill();process.waitForFinished(2000);
      return stop?QStringLiteral("cancelled"):QStringLiteral("python_probe_budget_exceeded");
    }
  }
  out+=process.readAllStandardOutput();err+=process.readAllStandardError();
  if(process.exitStatus()!=QProcess::NormalExit || process.exitCode()!=0 || out.size()+err.size()>16*1024)
    return "python_dependency_unavailable";
  const auto result=QJsonDocument::fromJson(out).object();
  if(result.value("python")!=QLatin1String(PythonVersion) || result.value("numpy")!=QLatin1String(NumpyVersion) ||
      result.value("isolated").toInt()!=1 || result.value("utf8").toInt()!=1 || !result.value("bytecode").toBool() ||
      result.value("path").toString()!=QFileInfo(runtime.interpreter).absolutePath())
    return "python_runtime_probe_mismatch";
  return {};
}
RuntimeProvider::RuntimeProvider(RuntimeConfig config,ExecutionPolicy policy):m_config(std::move(config)),m_policy(std::move(policy)) {}
RuntimeResult RuntimeProvider::snapshot() const {QMutexLocker lock(&m_mutex);return m_last;}
RuntimeResult RuntimeProvider::inspect(const std::function<bool()> &cancelled) {
  QMutexLocker lock(&m_mutex);
  const auto policyError=m_policy.validate();
  if(!policyError.isEmpty()){m_last={policyError,{}};return m_last;}
  // One generation lease per provider. Revalidate bytes on every new inspection.
  m_last={};
  auto result=verifyRuntime(m_config,cancelled,m_retention.lock());
  if(result.ready())m_retention=result.runtime->retention;
  if(result.ready()) {
    const auto code=probeRuntime(*result.runtime,cancelled);
    if(!code.isEmpty())result={code,{}};
  }
  m_last=result;return result;
}
} // namespace paleo::python
