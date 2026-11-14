// 层：数据
#include "pythonprocess_windows_internal.h"
#ifdef Q_OS_WIN
#include <QDir>
#include <QUuid>
#include <sddl.h>

namespace paleo::python::windows_process {
namespace {
QString winError(const char *what,DWORD error){return QLatin1String(what)+QLatin1Char(':')+QString::number(error);}
bool pipe(Handle &reader,Handle &writer,QString *error){
  SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};HANDLE r=nullptr,w=nullptr;
  if(!CreatePipe(&r,&w,&sa,4096)){*error=winError("pipe",GetLastError());return false;}reader.reset(r);writer.reset(w);return true;
}
bool inputPipe(Handle &reader,Handle &writer,QString *error){
  HANDLE raw=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw)){*error="pipe_token";return false;}Handle token(raw);
  DWORD size=0;GetTokenInformation(token.get(),TokenUser,nullptr,0,&size);std::vector<BYTE> info(size);
  if(!size||!GetTokenInformation(token.get(),TokenUser,info.data(),size,&size)){*error="pipe_user";return false;}
  LPWSTR user=nullptr;if(!ConvertSidToStringSidW(((TOKEN_USER*)info.data())->User.Sid,&user)){*error="pipe_sid";return false;}
  const auto sddl=QStringLiteral("D:P(A;;GA;;;SY)(A;;GA;;;")+QString::fromWCharArray(user)+')';LocalFree(user);
  PSECURITY_DESCRIPTOR sd=nullptr;if(!ConvertStringSecurityDescriptorToSecurityDescriptorW((PCWSTR)sddl.utf16(),SDDL_REVISION_1,&sd,nullptr)){*error="pipe_acl";return false;}
  SECURITY_ATTRIBUTES sa{sizeof(sa),sd,FALSE};const auto name=QStringLiteral("\\\\.\\pipe\\PaleoPythonStdin-")+QUuid::createUuid().toString(QUuid::Id128);
  writer.reset(CreateNamedPipeW((PCWSTR)name.utf16(),PIPE_ACCESS_OUTBOUND|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,0,&sa));LocalFree(sd);
  if(!writer.valid()){*error=winError("input_pipe",GetLastError());return false;}
  SECURITY_ATTRIBUTES inherit{sizeof(inherit),nullptr,TRUE};reader.reset(CreateFileW((PCWSTR)name.utf16(),GENERIC_READ,0,&inherit,OPEN_EXISTING,0,nullptr));
  if(!reader.valid()){*error=winError("input_pipe_client",GetLastError());return false;}
  if(!ConnectNamedPipe(writer.get(),nullptr)&&GetLastError()!=ERROR_PIPE_CONNECTED){*error=winError("input_pipe_connect",GetLastError());return false;}
  return true;
}
std::wstring quote(const QString &argument){
  std::wstring out=L"\"";size_t slashes=0;
  for(const auto c:argument.toStdWString()){
    if(c==L'\\'){++slashes;continue;}if(c==L'\"'){out.append(slashes*2+1,L'\\');out+=c;}else{out.append(slashes,L'\\');out+=c;}slashes=0;}
  out.append(slashes*2,L'\\');out+=L'\"';return out;
}
struct Attributes {
  std::vector<BYTE> memory;LPPROC_THREAD_ATTRIBUTE_LIST value=nullptr;
  ~Attributes(){if(value)DeleteProcThreadAttributeList(value);}
  bool init(DWORD count){SIZE_T size=0;InitializeProcThreadAttributeList(nullptr,count,0,&size);if(!size)return false;memory.resize(size);
    auto pointer=(LPPROC_THREAD_ATTRIBUTE_LIST)memory.data();if(!InitializeProcThreadAttributeList(pointer,count,0,&size))return false;value=pointer;return true;}
  bool add(DWORD_PTR key,void *data,SIZE_T size){return UpdateProcThreadAttribute(value,0,key,data,size,nullptr,nullptr);}
};
std::wstring environment(QStringList values){
  if(values.size()>256)return {};values.sort(Qt::CaseInsensitive);std::wstring result;QString last;
  for(const auto &item:values){const auto split=item.indexOf('=');if(split<1||item.contains(QChar(0)))return {};
    const auto key=item.left(split);if(key.compare(last,Qt::CaseInsensitive)==0)return {};last=key;
    result+=item.toStdWString();result.push_back(0);if(result.size()>32760)return {};}
  if(result.empty())result.push_back(0);result.push_back(0);return result;
}
} // namespace
bool Pipes::create(QString *error){
  if(!inputPipe(inputRead,inputWrite,error)||!pipe(outputRead,outputWrite,error)||!pipe(errorRead,errorWrite,error))return false;
  for(const auto h:{inputWrite.get(),outputRead.get(),errorRead.get()})
    if(!SetHandleInformation(h,HANDLE_FLAG_INHERIT,0)){*error=winError("pipe_inheritance",GetLastError());return false;}
  return true;
}
bool Child::start(const process_detail::Launch &request,const Security *security,const Job &job,Pipes &pipes,QString *error){
  Attributes attrs;if(!attrs.init(security?4:2)){*error=winError("attributes_init",GetLastError());return false;}
  SECURITY_CAPABILITIES caps{};DWORD optout=PROCESS_CREATION_ALL_APPLICATION_PACKAGES_OPT_OUT;
  if(security){
    if(!security->packageSid||!IsValidSid(security->packageSid)||!security->lessPrivileged||security->capabilities.isEmpty()||security->capabilities.size()>16){*error="security_identity";return false;}
    for(const auto &item:security->capabilities)if(!item.Sid||!IsValidSid(item.Sid)||item.Attributes!=SE_GROUP_ENABLED){*error="security_capability";return false;}
    caps.AppContainerSid=security->packageSid;caps.Capabilities=const_cast<SID_AND_ATTRIBUTES*>(security->capabilities.constData());caps.CapabilityCount=DWORD(security->capabilities.size());
    if(!attrs.add(PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,&caps,sizeof(caps))||
        !attrs.add(PROC_THREAD_ATTRIBUTE_ALL_APPLICATION_PACKAGES_POLICY,&optout,sizeof(optout))){*error=winError("attributes_security",GetLastError());return false;}}
  HANDLE jobs[]={job.handle.get()};HANDLE inherited[]={pipes.inputRead.get(),pipes.outputWrite.get(),pipes.errorWrite.get()};
  if(!attrs.add(PROC_THREAD_ATTRIBUTE_JOB_LIST,jobs,sizeof(jobs))||!attrs.add(PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited))){*error=winError("attributes_job",GetLastError());return false;}
  std::wstring command=quote(QDir::toNativeSeparators(request.program));
  for(const auto &arg:request.arguments){command+=L' ';command+=quote(arg);if(command.size()>32760){*error="argument_budget";return false;}}
  auto env=environment(request.environment);if(env.empty()){*error="windows_environment";return false;}
  STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES|STARTF_FORCEOFFFEEDBACK;
  startup.StartupInfo.hStdInput=pipes.inputRead.get();startup.StartupInfo.hStdOutput=pipes.outputWrite.get();startup.StartupInfo.hStdError=pipes.errorWrite.get();startup.lpAttributeList=attrs.value;
  PROCESS_INFORMATION info{};const auto program=native(request.program),cwd=native(request.workingDirectory);
  const bool ok=CreateProcessW(program.c_str(),command.data(),nullptr,nullptr,TRUE,
      EXTENDED_STARTUPINFO_PRESENT|CREATE_UNICODE_ENVIRONMENT|CREATE_NO_WINDOW,env.data(),cwd.c_str(),&startup.StartupInfo,&info);
  if(!ok){*error=winError("process_start",GetLastError());return false;}
  process.reset(info.hProcess);id=info.dwProcessId;CloseHandle(info.hThread);return true;
}
bool Writer::start(Handle input,QList<QByteArray> data,QString *detail){
  pipe=std::move(input);chunks=std::move(data);event.reset(CreateEventW(nullptr,TRUE,FALSE,nullptr));
  if(!event.valid()){*detail=winError("input_event",GetLastError());return false;}return true;
}
void Writer::pump(){
  if(error||!pipe.valid())return;DWORD written=0;
  if(active){if(!GetOverlappedResult(pipe.get(),&pending,&written,FALSE)){
      const auto code=GetLastError();if(code==ERROR_IO_INCOMPLETE)return;error=code;active=false;pipe.reset();return;}
    offset+=written;active=false;ResetEvent(event.get());}
  while(index<chunks.size()&&offset>=chunks[index].size()){++index;offset=0;}
  if(index>=chunks.size()){pipe.reset();return;}
  pending={};pending.hEvent=event.get();const auto &data=chunks[index];const auto count=DWORD(qMin<qsizetype>(64*1024,data.size()-offset));
  if(WriteFile(pipe.get(),data.constData()+offset,count,&written,&pending)){offset+=written;return;}
  const auto code=GetLastError();if(code==ERROR_IO_PENDING){active=true;return;}error=code;pipe.reset();
}
void Writer::close(){
  if(active&&pipe.valid()){CancelIoEx(pipe.get(),&pending);DWORD written=0;GetOverlappedResult(pipe.get(),&pending,&written,TRUE);active=false;}
  pipe.reset();chunks.clear();
}
} // namespace paleo::python::windows_process
#endif
