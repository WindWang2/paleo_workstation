// 层：数据
#include "pythonprocess_windows_internal.h"
#ifdef Q_OS_WIN
#include <QDir>

namespace paleo::python::windows_process {
namespace {
QString winError(const char *what,DWORD error){return QLatin1String(what)+QLatin1Char(':')+QString::number(error);}
}
std::wstring native(const QString &path){auto value=QDir::toNativeSeparators(path).toStdWString();
  if(value.size()>240&&value.rfind(L"\\\\?\\",0)!=0)value=L"\\\\?\\"+value;return value;}
bool cancelled(const process_detail::Launch &request){try{return request.cancelled&&request.cancelled();}catch(...){return true;}}
bool Job::create(const ExecutionPolicy &policy,QString *error) {
  handle.reset(CreateJobObjectW(nullptr,nullptr));completion.reset(CreateIoCompletionPort(INVALID_HANDLE_VALUE,nullptr,0,1));
  if(!handle.valid()||!completion.valid()){*error=winError("job_create",GetLastError());return false;}
  JOBOBJECT_ASSOCIATE_COMPLETION_PORT association{};association.CompletionKey=(void*)1;association.CompletionPort=completion.get();
  if(!SetInformationJobObject(handle.get(),JobObjectAssociateCompletionPortInformation,&association,sizeof(association))){*error=winError("job_port",GetLastError());return false;}
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_ACTIVE_PROCESS|
      JOB_OBJECT_LIMIT_PROCESS_MEMORY|JOB_OBJECT_LIMIT_JOB_MEMORY|JOB_OBJECT_LIMIT_PROCESS_TIME|JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
  limits.BasicLimitInformation.ActiveProcessLimit=1;limits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart=LONGLONG(policy.cpuSeconds)*10000000;
  limits.ProcessMemoryLimit=SIZE_T(policy.memoryBytes);limits.JobMemoryLimit=SIZE_T(policy.memoryBytes);
  if(!SetInformationJobObject(handle.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits))){*error=winError("job_limits",GetLastError());return false;}
  const auto processors=GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);if(!processors){*error="processor_count";return false;}
  JOBOBJECT_CPU_RATE_CONTROL_INFORMATION rate{};rate.ControlFlags=JOB_OBJECT_CPU_RATE_CONTROL_ENABLE|JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
  rate.CpuRate=qBound<DWORD>(1,DWORD((10000ULL*quint64(policy.threads))/processors),10000);
  if(!SetInformationJobObject(handle.get(),JobObjectCpuRateControlInformation,&rate,sizeof(rate))){*error=winError("job_cpu_rate",GetLastError());return false;}
  JOBOBJECT_BASIC_UI_RESTRICTIONS ui{};ui.UIRestrictionsClass=JOB_OBJECT_UILIMIT_DESKTOP|JOB_OBJECT_UILIMIT_DISPLAYSETTINGS|
      JOB_OBJECT_UILIMIT_EXITWINDOWS|JOB_OBJECT_UILIMIT_GLOBALATOMS|JOB_OBJECT_UILIMIT_HANDLES|JOB_OBJECT_UILIMIT_READCLIPBOARD|
      JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS|JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
  if(!SetInformationJobObject(handle.get(),JobObjectBasicUIRestrictions,&ui,sizeof(ui))){*error=winError("job_ui",GetLastError());return false;}
  return true;
}
QString Job::notification(){
  for(int i=0;i<128;++i){DWORD message=0;ULONG_PTR key=0;LPOVERLAPPED process=nullptr;
    if(!GetQueuedCompletionStatus(completion.get(),&message,&key,&process,0))return {};
    if(key!=1)return "python_process_unavailable";
    if(message==JOB_OBJECT_MSG_PROCESS_MEMORY_LIMIT||message==JOB_OBJECT_MSG_JOB_MEMORY_LIMIT)return "python_process_memory_limit";
    if(message==JOB_OBJECT_MSG_ACTIVE_PROCESS_LIMIT)return "python_process_process_limit";
    if(message==JOB_OBJECT_MSG_END_OF_PROCESS_TIME||message==JOB_OBJECT_MSG_END_OF_JOB_TIME)return "python_process_cpu_limit";}
  return {};
}
void metrics(const Child &child,const Job &job,ProcessResult &result){
  FILETIME created{},ended{},kernel{},user{};if(GetProcessTimes(child.process.get(),&created,&ended,&kernel,&user)){
    ULARGE_INTEGER k{},u{};k.LowPart=kernel.dwLowDateTime;k.HighPart=kernel.dwHighDateTime;u.LowPart=user.dwLowDateTime;u.HighPart=user.dwHighDateTime;
    result.cpuMicros=qint64((k.QuadPart+u.QuadPart)/10);}
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};if(QueryInformationJobObject(job.handle.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits),nullptr)){
    result.peakMemoryBytes=qint64(limits.PeakJobMemoryUsed);
    if(WaitForSingleObject(child.process.get(),0)==WAIT_OBJECT_0)result.memoryPeakComplete=true;}
}
} // namespace paleo::python::windows_process
#endif
