// 层：数据
#include "pythonsandbox_windows_internal.h"
#include "services/pythoninputbundle.h"
#include <QElapsedTimer>
#include <QFileInfo>
#ifdef Q_OS_WIN
#include <QDir>

namespace paleo::python {
namespace {
using namespace windows_detail;
bool requestValid(const SandboxRequest &r) {
  return r.runtime&&r.runtime->retention&&r.input&&r.input->ok();
}
QStringList environment(const SandboxRequest &request){
  wchar_t root[32768]{},system[32768]{};const auto rn=GetWindowsDirectoryW(root,32768),sn=GetSystemDirectoryW(system,32768);
  if(!rn||rn>=32768||!sn||sn>=32768)return {};
  const auto threads=QString::number(request.policy.threads);
  return {"SystemRoot="+QString::fromWCharArray(root),"WINDIR="+QString::fromWCharArray(root),"PATH="+QString::fromWCharArray(system),
      "TEMP="+QDir::toNativeSeparators(request.input->root),"TMP="+QDir::toNativeSeparators(request.input->root),
      "LOCALAPPDATA="+QDir::toNativeSeparators(request.input->root),"OPENBLAS_NUM_THREADS="+threads,"OMP_NUM_THREADS="+threads,"MKL_NUM_THREADS="+threads};
}
SandboxResult unavailable(const QString &detail) {
  SandboxResult result;result.code="python_sandbox_unavailable";result.error=detail.toUtf8();return result;
}
} // namespace
QJsonObject windowsSandboxReadiness() {
  const auto &available=windows_detail::api();
  return {{"code",available.ready()?"ok":"python_sandbox_unavailable"},{"backend","windows-lpac-broker.v1"},
    {"osBuild",int(available.build)},{"checksAtStart",true},{"processDiskWrites",false},{"registryWrites",false},{"network",false},
    {"maxProcesses",1},{"memoryAccounting","job_committed_bytes"},{"cpuAccounting","user_plus_kernel"},
    {"cpuPollingMilliseconds",10},{"cpuParallelismLimit",1},{"threadCountEnforced",false},{"requiresRuntimeProbe",true}};
}
SandboxResult runWindowsSandbox(const SandboxRequest &request) {
  using namespace windows_detail;
  if(!api().ready())return unavailable("required_windows_api_or_build");
  if(!request.policy.validate().isEmpty()){SandboxResult result;result.code="python_policy_invalid";return result;}
  if(!request.policy.allowGenerated){SandboxResult result;result.code="python_execution_denied";return result;}
  if(!requestValid(request)){SandboxResult result;result.code="python_sandbox_request_invalid";return result;}
  if(hostBaseReservation(request.policy)>request.policy.memoryBytes){SandboxResult result;result.code="python_snapshot_memory_budget";
    result.hostSnapshotReservedBytes=hostBaseReservation(request.policy);return result;}
  if(cancelled(request)){SandboxResult result;result.code="cancelled";return result;}
  const auto permit=request.permit?request.permit:claimExecutionPermit([&]{return cancelled(request);});
  if(!permit){SandboxResult result;result.code="cancelled";return result;}
  QElapsedTimer clock;clock.start();const auto wall=qint64(request.policy.wallSeconds)*1000;
  SandboxRequest effective=request;effective.cancelled=[&]{return cancelled(request)||clock.elapsed()>=wall;};
  Profile profile;QString detail;if(!cleanDeadProfiles(&detail)||!profile.prepare(&detail))return unavailable(detail);
  LockedTree runtime,input;
  if(!lockTree(request.runtime->root,1024LL*1024*1024,runtime,effective,&detail)||
      !lockTree(request.input->root,request.policy.inputBytes+request.policy.codeBytes+64*1024,input,effective,&detail)){
    if(cancelled(effective)){SandboxResult result;result.code=cancelled(request)?"cancelled":"python_process_timeout";return result;}
    return unavailable(detail);}
  if(runtime.root.compare(input.root,Qt::CaseInsensitive)==0||input.root.startsWith(runtime.root+'/',Qt::CaseInsensitive)||
      runtime.root.startsWith(input.root+'/',Qt::CaseInsensitive)||
      QFileInfo(request.runtime->interpreter).canonicalFilePath().compare(QDir(runtime.root).filePath("python.exe"),Qt::CaseInsensitive)!=0||
      QDir::cleanPath(QDir::fromNativeSeparators(request.runtime->interpreter)).compare(QDir(runtime.root).filePath("python.exe"),Qt::CaseInsensitive)!=0)
    return unavailable("root_or_interpreter_identity");
  if(input.bytes>request.policy.diskBytes-request.policy.outputBytes-request.policy.logBytes)
    return unavailable("broker_disk_reservation");
  if(!restrictProfile(profile,&detail)||!restrictTree(runtime,profile.runtimeCapability,&detail)||!restrictTree(input,profile.sid,&detail))return unavailable(detail);
  AuthenticatedSnapshot snapshot;
  if(!authenticateSnapshots(effective,runtime,input,snapshot,&detail)){SandboxResult result;result.code=detail;result.hostSnapshotReservedBytes=snapshot.reservedBytes;
    if(result.code=="cancelled"&&!cancelled(request)&&clock.elapsed()>=wall)result.code="python_process_timeout";return result;}
  if(cancelled(effective)){SandboxResult result;result.code=cancelled(request)?"cancelled":"python_process_timeout";return result;}
  process_detail::Launch launch;launch.program=snapshot.program;
  launch.arguments={"-I","-u","-B","-X","utf8",snapshot.codePath};launch.workingDirectory=input.root;
  launch.permit=permit;
  launch.environment=environment(request);launch.inputChunks=snapshot.inputChunks;launch.policy=request.policy;launch.cancelled=effective.cancelled;
  if(launch.environment.isEmpty())return unavailable("windows_environment");
  windows_process::Security security;security.packageSid=profile.sid;
  security.capabilities={{profile.registryCapability,SE_GROUP_ENABLED},{profile.runtimeCapability,SE_GROUP_ENABLED}};
  const auto work=windows_process::runChild(launch,&security);
  SandboxResult result;result.code=work.code;result.output=work.output;result.error=work.error;result.cpuMicros=work.cpuMicros;result.peakMemoryBytes=work.peakMemoryBytes;
  result.memoryPeakComplete=work.memoryPeakComplete;
  result.hostSnapshotReservedBytes=snapshot.reservedBytes;result.childMemoryLimitBytes=request.policy.memoryBytes;result.exitCode=work.exitCode;
  result.outputLog=work.outputLog;
  if(result.code=="cancelled"&&!cancelled(request)&&clock.elapsed()>=wall)result.code="python_process_timeout";
  if(!profile.cleanup(&detail)){result.code="python_sandbox_cleanup_failed";result.error=detail.toUtf8();
    if(result.outputLog.isEmpty())result.outputLog=result.output.first(qMin<qint64>(result.output.size(),request.policy.logBytes));result.output.clear();}
  return result;
}
} // namespace paleo::python
#else
namespace paleo::python {
QJsonObject windowsSandboxReadiness(){return {{"code","python_sandbox_unavailable"},{"backend","windows-lpac-broker.v1"}};}
SandboxResult runWindowsSandbox(const SandboxRequest &){SandboxResult result;result.code="python_sandbox_unavailable";return result;}
}
#endif
