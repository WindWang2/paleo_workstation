// 层：测试壳
#include <QtGlobal>
#ifdef Q_OS_WIN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <tlhelp32.h>
#include <winioctl.h>
#include <aclapi.h>
#include <sddl.h>
#endif
#include "services/pythonsandbox_windows.h"
#include "services/pythoninputbundle.h"
#include <QtTest>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <atomic>
#include <chrono>
#include <thread>

using namespace paleo::python;
namespace {
std::shared_ptr<const InputBundle> input(const QString &project,const QString &code,const QByteArray &json="{}",
                                       const QByteArray &binary={},bool frame=false){
  QMap<QString,QByteArray> files{{"code.py",code.toUtf8()},{"request.json",json}};
  if(frame)files.insert("data.bin",binary);
  const QJsonObject metadata{{"inputProtocol",frame?SandboxAnalysisInputProtocol:SandboxJsonInputProtocol}};
  return std::make_shared<InputBundle>(freezeInputBundle(project,files,metadata,ExecutionPolicy{}));
}
QString controlRoot(){return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("python-sandbox-control");}
QString runtimeRoot(){auto root=qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_ROOT");return QFileInfo(root).canonicalFilePath();}
QString anchor(){return qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_MANIFEST_SHA256");}
#ifdef Q_OS_WIN
bool grantLegacyPackageRead(const QString &path){
  const auto wide=QDir::toNativeSeparators(path);PSECURITY_DESCRIPTOR descriptor=nullptr;PACL original=nullptr,updated=nullptr;PSID sid=nullptr;
  if(GetNamedSecurityInfoW((LPWSTR)wide.utf16(),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&original,nullptr,&descriptor)!=ERROR_SUCCESS)return false;
  const auto cleanup=qScopeGuard([&]{if(descriptor)LocalFree(descriptor);if(updated)LocalFree(updated);if(sid)LocalFree(sid);});
  if(!ConvertStringSidToSidW(L"S-1-15-2-1",&sid))return false;EXPLICIT_ACCESSW access{};access.grfAccessPermissions=FILE_GENERIC_READ;
  access.grfAccessMode=GRANT_ACCESS;access.Trustee.TrusteeForm=TRUSTEE_IS_SID;access.Trustee.ptstrName=(LPWSTR)sid;
  return SetEntriesInAclW(1,&access,original,&updated)==ERROR_SUCCESS&&SetNamedSecurityInfoW((LPWSTR)wide.utf16(),SE_FILE_OBJECT,
    DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,updated,nullptr)==ERROR_SUCCESS;
}
bool hasPythonChild(){
  HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);bool found=false;
  if(snapshot!=INVALID_HANDLE_VALUE&&Process32FirstW(snapshot,&entry)){do{
    if(entry.th32ParentProcessID==GetCurrentProcessId()&&QString::fromWCharArray(entry.szExeFile).compare("python.exe",Qt::CaseInsensitive)==0){found=true;break;}
  }while(Process32NextW(snapshot,&entry));}if(snapshot!=INVALID_HANDLE_VALUE)CloseHandle(snapshot);return found;
}
bool createJunction(const QString &path,const QString &target){
  if(!QDir().mkpath(path))return false;const auto name=QDir::toNativeSeparators(target),substitute=QStringLiteral("\\??\\")+name;
  const auto subBytes=int(substitute.size()*2),nameBytes=int(name.size()*2);QByteArray buffer;QDataStream out(&buffer,QIODevice::WriteOnly);out.setByteOrder(QDataStream::LittleEndian);
  out<<quint32(IO_REPARSE_TAG_MOUNT_POINT)<<quint16(8+subBytes+2+nameBytes+2)<<quint16(0)<<quint16(0)<<quint16(subBytes)<<quint16(subBytes+2)<<quint16(nameBytes);
  out.writeRawData((const char*)substitute.utf16(),subBytes);out<<quint16(0);out.writeRawData((const char*)name.utf16(),nameBytes);out<<quint16(0);
  HANDLE handle=CreateFileW((PCWSTR)QDir::toNativeSeparators(path).utf16(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
  if(handle==INVALID_HANDLE_VALUE)return false;DWORD count=0;const bool ok=DeviceIoControl(handle,FSCTL_SET_REPARSE_POINT,buffer.data(),DWORD(buffer.size()),nullptr,0,&count,nullptr);CloseHandle(handle);return ok;
}
int crashHost(const QStringList &args){
  const auto inspected=verifyRuntime({args[2],anchor(),true},{},std::make_shared<int>(0));
  auto bundle=input(args[3],"import time\ntime.sleep(30)\n");
  std::atomic<bool> stop=false;std::thread monitor([&]{
    for(int turn=0;turn<1000&&!stop.load();++turn){HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);PROCESSENTRY32W item{};item.dwSize=sizeof(item);
      if(snapshot!=INVALID_HANDLE_VALUE&&Process32FirstW(snapshot,&item)){do{
        if(item.th32ParentProcessID==GetCurrentProcessId()&&QString::fromWCharArray(item.szExeFile).compare("python.exe",Qt::CaseInsensitive)==0){
          std::this_thread::sleep_for(std::chrono::milliseconds(100));QSaveFile file(args[4]);
          const auto bytes=QJsonDocument(QJsonObject{{"host",qint64(GetCurrentProcessId())},{"child",qint64(item.th32ProcessID)}}).toJson();
          if(file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size()&&file.commit())TerminateProcess(GetCurrentProcess(),77);
        }
      }while(Process32NextW(snapshot,&item));}if(snapshot!=INVALID_HANDLE_VALUE)CloseHandle(snapshot);std::this_thread::sleep_for(std::chrono::milliseconds(20));}
  });
  SandboxRequest request;request.runtime=inspected.runtime;request.input=bundle;request.policy.allowGenerated=true;
  const auto result=runWindowsSandbox(request);stop=true;monitor.join();fprintf(stderr,"crash fixture failed: %s %s\n",qPrintable(result.code),result.error.constData());return 2;
}
#endif
}
class TestPythonWindowsSandbox : public QObject {
  Q_OBJECT
  QString runtime,projects;
  std::shared_ptr<const VerifiedRuntime> verified;
  SandboxResult run(const QString &code,const QByteArray &json="{}",const QByteArray &binary={},bool frame=false,ExecutionPolicy policy={}){
    QTemporaryDir project(QDir(projects).filePath("project-XXXXXX"));if(!project.isValid())return {};
    SandboxRequest request;request.runtime=verified;request.input=input(project.path(),code,json,binary,frame);
    request.policy=policy;request.policy.allowGenerated=true;return runWindowsSandbox(request);
  }
private slots:
  void initTestCase(){
    runtime=runtimeRoot();QVERIFY2(!runtime.isEmpty()&&!anchor().isEmpty(),"Configure the pinned runtime and host manifest anchor for the required native isolation suite");
    const auto result=verifyRuntime({runtime,anchor(),true});QVERIFY2(result.ready(),qPrintable(result.code));verified=result.runtime;
    projects=QDir(QCoreApplication::applicationDirPath()).filePath("sandbox-projects");QVERIFY(QDir().mkpath(projects));projects=QFileInfo(projects).canonicalFilePath();
    QCOMPARE(windowsSandboxReadiness().value("code").toString(),QString("ok"));
    qInfo()<<"Required Windows isolation suite with pinned runtime"<<verified->manifestSha256;
  }
  void authenticatedNumpyAndJson(){
    const auto result=run("import sys,json,numpy as n\nr=json.load(sys.stdin)\nprint(json.dumps({'dot':float(n.arange(5)@n.arange(5)),'value':r['value']}))",R"({"value":17})");
    QVERIFY2(result.ok(),qPrintable(result.code+':'+QString::fromUtf8(result.error)));const auto object=QJsonDocument::fromJson(result.output).object();
    QCOMPARE(object.value("dot").toDouble(),30.0);QCOMPARE(object.value("value").toInt(),17);
    QVERIFY(result.memoryPeakComplete);QVERIFY(result.peakMemoryBytes>0);QVERIFY(result.hostSnapshotReservedBytes>0);
    QVERIFY(result.hostSnapshotReservedBytes<=ExecutionPolicy{}.memoryBytes);QCOMPARE(result.childMemoryLimitBytes,ExecutionPolicy{}.memoryBytes);
    qInfo()<<"NumPy resource counters: cpu_us"<<result.cpuMicros<<"child_commit_peak"<<result.peakMemoryBytes<<"host_reservation"<<result.hostSnapshotReservedBytes;
  }
  void authenticatedAnalysisTransport(){
    QByteArray binary;QDataStream stream(&binary,QIODevice::WriteOnly);stream.setByteOrder(QDataStream::LittleEndian);stream<<double(1)<<double(2)<<double(3);
    const auto code="import sys,struct,json,numpy as n\ns=sys.stdin.buffer\nk=struct.unpack('<I',s.read(4))[0]\nr=json.loads(s.read(k))\nb=s.read()\nprint(json.dumps({'bytes':len(b),'sum':float(n.frombuffer(b,dtype='<f8').sum()),'unit':r['unit']}))";
    const auto result=run(code,R"({"unit":"m"})",binary,true);QVERIFY2(result.ok(),qPrintable(result.code+':'+QString::fromUtf8(result.error)));
    const auto object=QJsonDocument::fromJson(result.output).object();QCOMPARE(object.value("bytes").toInt(),24);QCOMPARE(object.value("sum").toDouble(),6.0);QCOMPARE(object.value("unit").toString(),QString("m"));
  }
  void framedInputIncludesItsLengthPrefixInBudget(){
    ExecutionPolicy policy;policy.inputBytes=4096;
    const auto code="import sys\nsys.stdout.buffer.write(str(len(sys.stdin.buffer.read())).encode('ascii'))";
    const auto oversized=run(code,"{}",QByteArray(4094,'x'),true,policy);
    QCOMPARE(oversized.code,QString("python_input_budget_exceeded"));QVERIFY(oversized.output.isEmpty());QCOMPARE(oversized.cpuMicros,qint64(0));
    const auto exact=run(code,"{}",QByteArray(4090,'x'),true,policy);
    QVERIFY2(exact.ok(),qPrintable(exact.code+':'+QString::fromUtf8(exact.error)));QCOMPARE(exact.output,QByteArray("4096"));
  }
  void processProfileAndHostWritesAreDenied(){
    QTemporaryDir project(QDir(projects).filePath("host-XXXXXX"));QVERIFY(project.isValid());const auto sentinel=project.filePath("private.txt");
    QFile file(sentinel);QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write("private"),qint64(7));file.close();
    const auto code="import os,sys,pathlib,json\nr=json.load(sys.stdin);d={}\n"
      "for k,p in [('input',pathlib.Path.cwd()/'bad.tmp'),('runtime',pathlib.Path(sys.executable).parent/'bad.tmp'),('profile',pathlib.Path(os.environ['LOCALAPPDATA'])/'bad.tmp'),('profileBase',pathlib.Path(os.environ['LOCALAPPDATA']).parent/'bad.tmp')]:\n"
      " try:\n  p.write_bytes(b'bad');d[k]=False\n except OSError:\n  d[k]=True\n"
      "try:\n pathlib.Path(r['sentinel']).read_bytes();d['hostRead']=False\nexcept OSError:\n d['hostRead']=True\nprint(json.dumps(d))";
    SandboxRequest request;request.runtime=verified;request.input=input(project.path(),code,QJsonDocument(QJsonObject{{"sentinel",sentinel}}).toJson());request.policy.allowGenerated=true;
    const auto result=runWindowsSandbox(request);QVERIFY2(result.ok(),qPrintable(result.code+':'+QString::fromUtf8(result.error)));
    const auto values=QJsonDocument::fromJson(result.output).object();QCOMPARE(values.size(),5);for(const auto &key:values.keys())QVERIFY(values.value(key).toBool());
    QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),QByteArray("private"));
  }
  void hostNamedPipeIsDenied(){
    const auto name=QStringLiteral("\\\\.\\pipe\\PaleoHostSentinel-")+QUuid::createUuid().toString(QUuid::Id128);
    HANDLE pipe=CreateNamedPipeW((PCWSTR)name.utf16(),PIPE_ACCESS_DUPLEX,PIPE_TYPE_BYTE|PIPE_NOWAIT|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,0,nullptr);QVERIFY(pipe!=INVALID_HANDLE_VALUE);
    const auto cleanup=qScopeGuard([&]{CloseHandle(pipe);});
    const auto code="import ctypes,json,sys\np=json.load(sys.stdin)['pipe'];k=ctypes.WinDLL('kernel32',use_last_error=True)\nk.CreateFileW.argtypes=[ctypes.c_wchar_p,ctypes.c_uint,ctypes.c_uint,ctypes.c_void_p,ctypes.c_uint,ctypes.c_uint,ctypes.c_void_p]\nk.CreateFileW.restype=ctypes.c_void_p\nh=k.CreateFileW(p,0xC0000000,0,None,3,0,None)\nprint(json.dumps({'blocked':h in (None,ctypes.c_void_p(-1).value)}))";
    const auto result=run(code,QJsonDocument(QJsonObject{{"pipe",name}}).toJson());QVERIFY2(result.ok(),qPrintable(result.code+':'+QString::fromUtf8(result.error)));
    QVERIFY(QJsonDocument::fromJson(result.output).object().value("blocked").toBool());
  }
  void realProfileRegistryEnvironmentAndHostProcessAreDenied(){
    QTemporaryDir project(QDir(projects).filePath("legacy-package-XXXXXX"));QVERIFY(project.isValid());const auto legacy=project.filePath("legacy.txt");
    QFile legacyFile(legacy);QVERIFY(legacyFile.open(QIODevice::WriteOnly));QCOMPARE(legacyFile.write("legacy-only"),qint64(11));legacyFile.close();QVERIFY(grantLegacyPackageRead(legacy));
    const auto key=QStringLiteral("Software\\PaleoTests\\SandboxSentinel-")+QUuid::createUuid().toString(QUuid::Id128);
    HKEY registry=nullptr;QCOMPARE(RegCreateKeyExW(HKEY_CURRENT_USER,(PCWSTR)key.utf16(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&registry,nullptr),LONG(ERROR_SUCCESS));
    const auto cleanup=qScopeGuard([&]{RegCloseKey(registry);RegDeleteTreeW(HKEY_CURRENT_USER,(PCWSTR)key.utf16());qunsetenv("PALEO_SANDBOX_SECRET");});
    const QByteArray value("untouched");QCOMPARE(RegSetValueExW(registry,L"sentinel",0,REG_BINARY,(const BYTE*)value.constData(),DWORD(value.size())),LONG(ERROR_SUCCESS));
    qputenv("PALEO_SANDBOX_SECRET","must-stay-in-host");
    const auto code=QString::fromLatin1(R"PY(import ctypes as c,ctypes.wintypes as w,json,os,pathlib,sys,winreg
r=json.load(sys.stdin);d={};k=c.WinDLL('kernel32',use_last_error=True);a=c.WinDLL('advapi32',use_last_error=True)
k.GetCurrentProcess.restype=w.HANDLE;k.CloseHandle.argtypes=[w.HANDLE]
a.OpenProcessToken.argtypes=[w.HANDLE,w.DWORD,c.POINTER(w.HANDLE)]
a.GetTokenInformation.argtypes=[w.HANDLE,c.c_int,c.c_void_p,w.DWORD,c.POINTER(w.DWORD)]
a.ConvertSidToStringSidW.argtypes=[c.c_void_p,c.POINTER(w.LPWSTR)]
t=w.HANDLE();assert a.OpenProcessToken(k.GetCurrentProcess(),8,c.byref(t))
n=w.DWORD();a.GetTokenInformation(t,31,None,0,c.byref(n));b=c.create_string_buffer(n.value)
assert a.GetTokenInformation(t,31,b,n,c.byref(n));sid=c.cast(b,c.POINTER(c.c_void_p))[0];s=w.LPWSTR()
assert a.ConvertSidToStringSidW(sid,c.byref(s));folder=w.LPWSTR();u=c.WinDLL('userenv');u.GetAppContainerFolderPath.argtypes=[w.LPCWSTR,c.POINTER(w.LPWSTR)]
assert u.GetAppContainerFolderPath(s,c.byref(folder))==0
base=pathlib.Path(folder.value).parent
for name,path in [('realProfile',pathlib.Path(folder.value)/'denied.tmp'),('realProfileBase',base/'denied.tmp')]:
 try:
  path.write_bytes(b'forbidden');d[name]=False
 except OSError:
  d[name]=True
for name,path in [('privateRegistry','Software\\PaleoSandboxDenied'),('hostRegistry',r['key'])]:
 try:
  with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER,path,0,winreg.KEY_SET_VALUE|winreg.KEY_CREATE_SUB_KEY) as h:
   winreg.SetValueEx(h,'sentinel',0,winreg.REG_BINARY,b'changed')
  d[name]=False
 except OSError:
  d[name]=True
k.OpenProcess.argtypes=[w.DWORD,w.BOOL,w.DWORD];k.OpenProcess.restype=w.HANDLE
h=k.OpenProcess(0x001f0fff,False,r['host']);d['hostProcess']=not bool(h)
if h:k.CloseHandle(h)
d['environment']='PALEO_SANDBOX_SECRET' not in os.environ
try:
 pathlib.Path(r['legacy']).read_bytes();d['lessPrivileged']=False
except OSError:
 d['lessPrivileged']=True
print(json.dumps(d))
)PY");
    const auto result=run(code,QJsonDocument(QJsonObject{{"key",key},{"host",qint64(GetCurrentProcessId())},{"legacy",legacy}}).toJson());
    QVERIFY2(result.ok(),qPrintable(result.code+':'+QString::fromUtf8(result.error)));const auto values=QJsonDocument::fromJson(result.output).object();
    QCOMPARE(values.size(),7);for(const auto &name:values.keys())QVERIFY2(values.value(name).toBool(),qPrintable(name));
    char actual[64]{};DWORD size=sizeof(actual),type=0;QCOMPARE(RegQueryValueExW(registry,L"sentinel",nullptr,&type,(BYTE*)actual,&size),LONG(ERROR_SUCCESS));
    QCOMPARE(QByteArray(actual,size),value);
  }
  void otherWorkspaceAndDiskWritesAreDenied(){
    QTemporaryDir project(QDir(projects).filePath("other-XXXXXX"));QVERIFY(project.isValid());auto other=input(project.path(),"print('private')");QVERIFY(other->ok());
    const auto path=QDir(other->root).filePath("code.py");QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));const auto before=file.readAll();file.close();
    const auto code="import sys,json,pathlib\nr=json.load(sys.stdin);d={}\ntry:\n pathlib.Path(r['other']).read_bytes();d['otherRead']=False\nexcept OSError:\n d['otherRead']=True\ncount=0\nfor i in range(1024):\n try:\n  (pathlib.Path.cwd()/('flood-'+str(i))).write_bytes(b'x'*8192);count+=1\n except OSError:\n  pass\nd['created']=count\nprint(json.dumps(d))";
    const auto result=run(code,QJsonDocument(QJsonObject{{"other",path}}).toJson());QVERIFY2(result.ok(),qPrintable(result.code+':'+QString::fromUtf8(result.error)));
    const auto values=QJsonDocument::fromJson(result.output).object();QVERIFY(values.value("otherRead").toBool());QCOMPARE(values.value("created").toInt(),0);
    QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),before);
  }
  void loopbackCannotReachHost(){
    WSADATA data{};QCOMPARE(WSAStartup(MAKEWORD(2,2),&data),0);SOCKET server=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);QVERIFY(server!=INVALID_SOCKET);
    const auto cleanup=qScopeGuard([&]{closesocket(server);WSACleanup();});sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    QCOMPARE(bind(server,(sockaddr*)&address,sizeof(address)),0);QCOMPARE(listen(server,1),0);int size=sizeof(address);QCOMPARE(getsockname(server,(sockaddr*)&address,&size),0);
    const auto code="import socket,json,sys\np=json.load(sys.stdin)['port']\ntry:\n s=socket.create_connection(('127.0.0.1',p),timeout=.2);s.close();blocked=False\nexcept OSError:\n blocked=True\ntry:\n s=socket.socket();s.settimeout(.2);outbound=s.connect_ex(('198.51.100.1',9))==10013;s.close()\nexcept OSError as e:\n outbound=e.winerror==10013\nprint(json.dumps({'blocked':blocked,'outbound':outbound}))";
    const auto result=run(code,QJsonDocument(QJsonObject{{"port",ntohs(address.sin_port)}}).toJson());QVERIFY2(result.ok(),qPrintable(result.code+':'+QString::fromUtf8(result.error)));
    QVERIFY(QJsonDocument::fromJson(result.output).object().value("blocked").toBool());QVERIFY(QJsonDocument::fromJson(result.output).object().value("outbound").toBool());u_long nonblocking=1;QCOMPARE(ioctlsocket(server,FIONBIO,&nonblocking),0);
    SOCKET accepted=accept(server,nullptr,nullptr);if(accepted!=INVALID_SOCKET)closesocket(accepted);QVERIFY(accepted==INVALID_SOCKET);
  }
  void generatedAuthorityIsRequired(){
    QTemporaryDir project(QDir(projects).filePath("denied-XXXXXX"));QVERIFY(project.isValid());SandboxRequest request;request.runtime=verified;request.input=input(project.path(),"print('forbidden')");request.policy.allowTrusted=true;
    const auto result=runWindowsSandbox(request);QCOMPARE(result.code,QString("python_execution_denied"));QVERIFY(result.output.isEmpty());QCOMPARE(result.exitCode,-1);
  }
  void protocolAndSnapshotBudgetAreChecked(){
    ExecutionPolicy policy;policy.memoryBytes=64*1024*1024;auto result=run("print('forbidden')","{}",{},false,policy);QCOMPARE(result.code,QString("python_snapshot_memory_budget"));QVERIFY(result.output.isEmpty());
    QTemporaryDir project(QDir(projects).filePath("protocol-XXXXXX"));QVERIFY(project.isValid());const QMap<QString,QByteArray> files{{"code.py","print('forbidden')"},{"request.json","{}"}};
    SandboxRequest request;request.runtime=verified;request.input=std::make_shared<InputBundle>(freezeInputBundle(project.path(),files,{{"inputProtocol","arbitrary"}},ExecutionPolicy{}));request.policy.allowGenerated=true;
    result=runWindowsSandbox(request);QCOMPARE(result.code,QString("python_input_protocol_invalid"));QVERIFY(result.output.isEmpty());
  }
  void outputMemoryAndProcessLimits(){
    ExecutionPolicy policy;policy.outputBytes=32768;auto result=run("print('x'*65536)","{}",{},false,policy);QCOMPARE(result.code,QString("python_process_output_limit"));QVERIFY(result.output.isEmpty());
    result=run("x=bytearray(1024*1024*1024)");QCOMPARE(result.code,QString("python_process_memory_limit"));QVERIFY(result.output.isEmpty());
    result=run("import subprocess,sys\nsubprocess.run([sys.executable,'-c','pass'],check=True)");QCOMPARE(result.code,QString("python_process_process_limit"));QVERIFY(result.output.isEmpty());
  }
  void wallCpuLogAndCancellationLimits(){
    ExecutionPolicy policy;policy.wallSeconds=3;auto result=run("import time\ntime.sleep(30)","{}",{},false,policy);
    QCOMPARE(result.code,QString("python_process_timeout"));QVERIFY(result.output.isEmpty());
    policy={};policy.cpuSeconds=1;policy.wallSeconds=10;result=run("while True:pass","{}",{},false,policy);
    QCOMPARE(result.code,QString("python_process_cpu_limit"));QVERIFY(result.cpuMicros>=900000&&result.cpuMicros<1500000);QVERIFY(result.output.isEmpty());
    qInfo()<<"One-second CPU limit observed_us"<<result.cpuMicros<<"commit_peak"<<result.peakMemoryBytes;
    policy={};policy.logBytes=32768;result=run("import sys\nsys.stderr.write('x'*65536)","{}",{},false,policy);
    QCOMPARE(result.code,QString("python_process_log_limit"));QVERIFY(result.error.size()<=32768);QVERIFY(result.output.isEmpty());
    QTemporaryDir project(QDir(projects).filePath("cancel-XXXXXX"));QVERIFY(project.isValid());SandboxRequest request;
    request.runtime=verified;request.input=input(project.path(),"import time\ntime.sleep(30)");request.policy.allowGenerated=true;
    QElapsedTimer clock;clock.start();request.cancelled=[&]{return clock.elapsed()>3000;};result=runWindowsSandbox(request);
    QCOMPARE(result.code,QString("cancelled"));QVERIFY(result.output.isEmpty());QVERIFY(clock.elapsed()<8000);
  }
  void authenticatedFilesStayLockedUntilChildIsReaped(){
    QTemporaryDir project(QDir(projects).filePath("locked-XXXXXX"));QVERIFY(project.isValid());auto bundle=input(project.path(),"import time\ntime.sleep(.5)\nprint('locked')");QVERIFY(bundle->ok());
    const QStringList paths{QDir(runtime).filePath("LICENSE.txt"),QDir(bundle->root).filePath("code.py")};QList<DWORD> attributes;
    for(const auto &path:paths)attributes.append(GetFileAttributesW((PCWSTR)QDir::toNativeSeparators(path).utf16()));
    const auto restore=qScopeGuard([&]{for(int i=0;i<paths.size();++i)SetFileAttributesW((PCWSTR)QDir::toNativeSeparators(paths[i]).utf16(),attributes[i]);});
    std::atomic<bool> done=false;bool attempted=false,blocked=true;SandboxRequest request;request.runtime=verified;request.input=bundle;request.policy.allowGenerated=true;
    std::thread monitor([&]{while(!done.load()){if(hasPythonChild()){attempted=true;
      for(int i=0;i<paths.size();++i){const auto path=QDir::toNativeSeparators(paths[i]);blocked&=SetFileAttributesW((PCWSTR)path.utf16(),attributes[i]&~FILE_ATTRIBUTE_READONLY)!=0;
        HANDLE handle=CreateFileW((PCWSTR)path.utf16(),GENERIC_WRITE|DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
        blocked&=handle==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_SHARING_VIOLATION;if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
      const auto root=QDir::toNativeSeparators(runtime);HANDLE directory=CreateFileW((PCWSTR)root.utf16(),DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
      blocked&=directory==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_SHARING_VIOLATION;if(directory!=INVALID_HANDLE_VALUE)CloseHandle(directory);
      const auto moved=project.path()+"-moved";const bool renamed=MoveFileExW((PCWSTR)QDir::toNativeSeparators(project.path()).utf16(),(PCWSTR)QDir::toNativeSeparators(moved).utf16(),0);
      if(renamed){blocked=false;MoveFileExW((PCWSTR)QDir::toNativeSeparators(moved).utf16(),(PCWSTR)QDir::toNativeSeparators(project.path()).utf16(),0);}break;
    }std::this_thread::sleep_for(std::chrono::milliseconds(100));}});const auto result=runWindowsSandbox(request);done=true;monitor.join();
    QVERIFY2(result.ok(),qPrintable(result.code+':'+QString::fromUtf8(result.error)));QVERIFY(attempted);QVERIFY(blocked);
  }
  void generatedThreadsRemainWithinTheJobCpuBudget(){
    ExecutionPolicy policy;policy.cpuSeconds=1;policy.wallSeconds=15;
    const auto result=run("import threading,numpy as n\na=n.ones(200000)\ndef work():\n while True:n.dot(a,a)\nt=[threading.Thread(target=work) for _ in range(4)]\nfor x in t:x.start()\nfor x in t:x.join()","{}",{},false,policy);
    QCOMPARE(result.code,QString("python_process_cpu_limit"));QVERIFY(result.cpuMicros>=900000&&result.cpuMicros<1500000);
    QVERIFY(result.peakMemoryBytes<=policy.memoryBytes);QVERIFY(result.output.isEmpty());
    QCOMPARE(windowsSandboxReadiness().value("threadCountEnforced").toBool(true),false);QCOMPARE(windowsSandboxReadiness().value("cpuParallelismLimit").toInt(),1);
    qInfo()<<"Four native NumPy threads aggregate_cpu_us"<<result.cpuMicros<<"commit_peak"<<result.peakMemoryBytes;
  }
  void failureStdoutIsPrivateAndBounded(){
    auto result=run("import sys\nprint('partial',end='',flush=True)\nsys.exit(7)");QCOMPARE(result.code,QString("python_process_failed"));QCOMPARE(result.exitCode,7);
    QVERIFY(result.output.isEmpty());QCOMPARE(result.outputLog,QByteArray("partial"));
    ExecutionPolicy policy;policy.logBytes=1024;result=run("import sys\nprint('x'*8192,flush=True)\nsys.exit(7)","{}",{},false,policy);
    QCOMPARE(result.code,QString("python_process_failed"));QVERIFY(result.output.isEmpty());QCOMPARE(result.outputLog.size(),1024);
  }
  void changedInputCannotExecute(){
    QTemporaryDir project(QDir(projects).filePath("changed-XXXXXX"));QVERIFY(project.isValid());auto bundle=input(project.path(),"print('forbidden')",R"({"value":17})");QVERIFY(bundle->ok());
    const auto path=QDir(bundle->root).filePath("request.json"),wide=QDir::toNativeSeparators(path);const auto attributes=GetFileAttributesW((PCWSTR)wide.utf16());QVERIFY(SetFileAttributesW((PCWSTR)wide.utf16(),attributes&~FILE_ATTRIBUTE_READONLY));
    const auto before=QFileInfo(path).lastModified();QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));QCOMPARE(file.write(R"({"value":97})"),qint64(12));QVERIFY(file.setFileTime(before,QFileDevice::FileModificationTime));file.close();
    SandboxRequest request;request.runtime=verified;request.input=bundle;request.policy.allowGenerated=true;const auto result=runWindowsSandbox(request);QCOMPARE(result.code,QString("python_input_changed"));QVERIFY(result.output.isEmpty());
  }
  void hardlinkCannotGrantHostAccess(){
    QTemporaryDir project(QDir(projects).filePath("alias-XXXXXX"));QVERIFY(project.isValid());auto bundle=input(project.path(),"print('forbidden')");QVERIFY(bundle->ok());
    const auto sentinel=project.filePath("private.txt");QFile file(sentinel);QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write("private"),qint64(7));file.close();
    const auto alias=QDir(bundle->root).filePath("code.py"),wide=QDir::toNativeSeparators(alias);const auto attributes=GetFileAttributesW((PCWSTR)wide.utf16());QVERIFY(SetFileAttributesW((PCWSTR)wide.utf16(),attributes&~FILE_ATTRIBUTE_READONLY));QVERIFY(QFile::remove(alias));
    QVERIFY(CreateHardLinkW((PCWSTR)wide.utf16(),(PCWSTR)QDir::toNativeSeparators(sentinel).utf16(),nullptr));const auto cleanup=qScopeGuard([&]{QFile::remove(alias);});
    SandboxRequest request;request.runtime=verified;request.input=bundle;request.policy.allowGenerated=true;const auto result=runWindowsSandbox(request);QCOMPARE(result.code,QString("python_sandbox_unavailable"));QVERIFY(result.output.isEmpty());
    QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),QByteArray("private"));
  }
  void reparseCannotGrantHostAccess(){
    QTemporaryDir project(QDir(projects).filePath("junction-XXXXXX"));QVERIFY(project.isValid());auto bundle=input(project.path(),"print('forbidden')");QVERIFY(bundle->ok());
    const auto target=project.filePath("private");QVERIFY(QDir().mkpath(target));QFile file(QDir(target).filePath("sentinel.txt"));QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write("untouched"),qint64(9));file.close();
    const auto alias=QDir(bundle->root).filePath("alias");QVERIFY(createJunction(alias,target));const auto cleanup=qScopeGuard([&]{RemoveDirectoryW((PCWSTR)QDir::toNativeSeparators(alias).utf16());});
    SandboxRequest request;request.runtime=verified;request.input=bundle;request.policy.allowGenerated=true;const auto result=runWindowsSandbox(request);
    QCOMPARE(result.code,QString("python_sandbox_unavailable"));QVERIFY(result.output.isEmpty());QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),QByteArray("untouched"));
  }
  void frozenRuntimeTamperingIsRefused(){
    const auto path=QDir(runtime).filePath("LICENSE.txt"),wide=QDir::toNativeSeparators(path);QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));const auto original=file.readAll();file.close();QVERIFY(!original.isEmpty());
    const auto before=QFileInfo(path).lastModified();const auto attributes=GetFileAttributesW((PCWSTR)wide.utf16());QVERIFY(SetFileAttributesW((PCWSTR)wide.utf16(),attributes&~FILE_ATTRIBUTE_READONLY));
    const auto restore=qScopeGuard([&]{QFile output(path);if(output.open(QIODevice::WriteOnly|QIODevice::Truncate)){output.write(original);output.close();}SetFileAttributesW((PCWSTR)wide.utf16(),attributes);});
    auto changed=original;changed[0]=char(changed[0]^1);QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));QCOMPARE(file.write(changed),qint64(changed.size()));QVERIFY(file.setFileTime(before,QFileDevice::FileModificationTime));file.close();
    const auto result=run("print('forbidden')");QCOMPARE(result.code,QString("python_runtime_changed"));QVERIFY(result.output.isEmpty());
  }
  void hostCrashIsReconciledWithoutLateChild(){
    QTemporaryDir project(QDir(projects).filePath("crash-XXXXXX"));QVERIFY(project.isValid());const auto artifact=project.filePath("child.json");
    QProcess helper;helper.start(QCoreApplication::applicationFilePath(),{"--profile-crash",runtime,project.path(),artifact});QVERIFY(helper.waitForStarted(5000));QVERIFY(helper.waitForFinished(30000));QCOMPARE(helper.exitCode(),77);
    QFile file(artifact);QVERIFY(file.open(QIODevice::ReadOnly));const auto record=QJsonDocument::fromJson(file.readAll()).object();file.close();const auto child=DWORD(record.value("child").toInteger());QVERIFY(child>0);
    HANDLE process=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,child);if(process){QCOMPARE(WaitForSingleObject(process,1000),DWORD(WAIT_OBJECT_0));CloseHandle(process);}
    QVERIFY(!QDir(controlRoot()).entryList({"*.json"},QDir::Files).isEmpty());const auto next=run("print('maintenance')");QVERIFY2(next.ok(),qPrintable(next.code+':'+QString::fromUtf8(next.error)));
    QVERIFY(QDir(controlRoot()).entryList({"*.json"},QDir::Files).isEmpty());
  }
};
class TestPythonWindowsSandboxMissing:public QObject {
  Q_OBJECT
private slots:
  void optionalEnvironmentNeverGrantsExecution(){
    SandboxRequest request;request.policy.allowTrusted=true;const auto result=runWindowsSandbox(request);QVERIFY(!result.ok());QVERIFY(result.output.isEmpty());QCOMPARE(result.exitCode,-1);QVERIFY(!result.memoryPeakComplete);
    qInfo()<<"Pinned isolation suite not configured; this test validates unavailable behavior only";
  }
};
int main(int argc,char **argv){
  QCoreApplication app(argc,argv);QCoreApplication::setOrganizationName("PaleoTests");QCoreApplication::setApplicationName("tst_python_windows_sandbox");
#ifdef Q_OS_WIN
  if(app.arguments().size()==5&&app.arguments()[1]=="--profile-crash")return crashHost(app.arguments());
#endif
  if(qEnvironmentVariable("PALEO_REQUIRE_AGENT_PYTHON_SANDBOX")!="1"&&runtimeRoot().isEmpty()){
    TestPythonWindowsSandboxMissing test;return QTest::qExec(&test,argc,argv);}
  TestPythonWindowsSandbox test;return QTest::qExec(&test,argc,argv);
}
#include "tst_python_windows_sandbox.moc"
