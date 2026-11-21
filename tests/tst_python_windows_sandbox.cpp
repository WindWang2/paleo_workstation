// 层：测试壳
#include <QtGlobal>
#ifdef Q_OS_WIN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <tlhelp32.h>
#include <winioctl.h>
#include <aclapi.h>
#include <sddl.h>
#include "services/pythonsandbox_windows_internal.h"
#endif
#include "services/pythonsandbox_windows.h"
#include "services/pythoninputbundle.h"
#include "services/pythonprocess.h"
#include <QtTest>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDeadlineTimer>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QThread>
#include <QSaveFile>
#include <QSemaphore>
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
// #959 oracle：逐节点安全描述符的语义规范化快照——owner/group SID、
// DACL 控制标志（含 PROTECTED/继承态）与 ACE 集合（类型|标志|掩码|受托人
// SID，排序后拼接）。不依赖 SDDL 字符串的偶然排序；同时并入字节摘要，
// 同一趟走查覆盖「字节 + 安全描述符 + 继承状态」三重不变量。
QString nodeSecurityText(const QString &path){
  const auto wide=QDir::toNativeSeparators(path);PSECURITY_DESCRIPTOR sd=nullptr;PACL dacl=nullptr;
  const auto fields=DACL_SECURITY_INFORMATION|OWNER_SECURITY_INFORMATION|GROUP_SECURITY_INFORMATION;
  if(GetNamedSecurityInfoW((LPWSTR)wide.utf16(),SE_FILE_OBJECT,fields,nullptr,nullptr,&dacl,nullptr,&sd)!=ERROR_SUCCESS)return {};
  const auto cleanup=qScopeGuard([&]{if(sd)LocalFree(sd);});
  PSID user=nullptr,group=nullptr;BOOL present=false,defaulted=false;SECURITY_DESCRIPTOR_CONTROL control=0;DWORD revision=0;
  if(!GetSecurityDescriptorOwner(sd,&user,&defaulted)||!GetSecurityDescriptorGroup(sd,&group,&defaulted)||
     !GetSecurityDescriptorControl(sd,&control,&revision))return {};
  auto sidText=[](PSID sid){LPWSTR text=nullptr;if(!sid||!ConvertSidToStringSidW(sid,&text))return QStringLiteral("?");const auto value=QString::fromWCharArray(text);LocalFree(text);return value;};
  QStringList aces;
  if(!GetSecurityDescriptorDacl(sd,&present,&dacl,&defaulted))return {};
  if(present&&dacl) {
    for(WORD i=0;i<dacl->AceCount;++i) {
      void *ace=nullptr;if(!GetAce(dacl,i,&ace))return {};
      const auto *header=(ACE_HEADER*)ace;DWORD mask=0;PSID trustee=nullptr;QString kind;
      if(header->AceType==ACCESS_ALLOWED_ACE_TYPE){const auto *allowed=(ACCESS_ALLOWED_ACE*)ace;mask=allowed->Mask;trustee=(PSID)&allowed->SidStart;kind="A";}
      else if(header->AceType==ACCESS_DENIED_ACE_TYPE){const auto *denied=(ACCESS_DENIED_ACE*)ace;mask=denied->Mask;trustee=(PSID)&denied->SidStart;kind="D";}
      else kind=QStringLiteral("T%1").arg(int(header->AceType));
      aces.append(QStringLiteral("%1:%2:%3:%4").arg(kind).arg(int(header->AceFlags),4,16,QChar('0'))
        .arg(mask,8,16,QChar('0')).arg(trustee?sidText(trustee):QStringLiteral("?")));
    }
  }
  aces.sort();
  return QStringLiteral("O=%1|G=%2|C=%3|ACL=%4").arg(sidText(user)).arg(sidText(group))
    .arg(control,4,16,QChar('0')).arg(aces.join(QLatin1Char(';')));
}
QHash<QString,QString> treeSnapshot(const QString &root){
  QHash<QString,QString> snapshot;
  QDirIterator it(root,QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden|QDir::System,QDirIterator::Subdirectories);
  while(it.hasNext()){
    const auto path=it.next();QCryptographicHash hash(QCryptographicHash::Sha256);
    if(QFileInfo(path).isFile()){QFile file(path);if(!file.open(QIODevice::ReadOnly))continue;
      while(!file.atEnd())hash.addData(file.read(64*1024));file.close();}
    snapshot.insert(QDir(root).relativeFilePath(path),
                    nodeSecurityText(path)+QLatin1Char('|')+QString::fromLatin1(hash.result().toHex()));
  }
  return snapshot;
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
// 任务 B（同 generation 两宿主重叠）：独立 helper 进程 = 独立 profile/输入/
// 执行许可；python 睡 30s，父进程等两个「已入运行」marker 后强杀 A，证明
// 重叠与 KILL_ON_JOB_CLOSE 兜底。结果 JSON 由本 helper 落盘。
int overlapHost(const QStringList &args){
  const auto inspected=verifyRuntime({args[2],anchor(),true},{},std::make_shared<int>(0));
  if(!inspected.ready())return 3;
  QSaveFile launched(args[4]);
  if(!launched.open(QIODevice::WriteOnly)||launched.write("1")!=1||!launched.commit())return 4;
  auto bundle=input(args[3],"import time\ntime.sleep(30)\n");
  SandboxRequest request;request.runtime=inspected.runtime;request.input=bundle;request.policy.allowGenerated=true;
  const auto result=runWindowsSandbox(request);
  QSaveFile file(args[5]);
  const auto bytes=QJsonDocument(QJsonObject{{"code",result.code},{"viewReused",result.viewReused}}).toJson();
  if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit())return 5;
  return result.ok()?0:6;
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
    // 任务 B：死宿主的视图租约也须随维护运行清空（journal 扩展恢复面）。
    const auto leases=QDir(QDir(controlRoot()).filePath("view-leases")).entryList({"*.json"},QDir::Files);
    QVERIFY2(leases.isEmpty(),qPrintable(leases.join(QLatin1Char(','))));
  }
  void sourceTreesKeepTheirSecurityDescriptors(){
    // #959 oracle：原 runtime/工程输入树逐节点「owner/group + DACL 控制标志
    //（含 PROTECTED/继承）+ ACE 集合 + 字节摘要」在成功与取消两种运行前后
    // 保持不变。运行期间源树被独占句柄锁死（既改不了字节也改不了 SD）。
    QTemporaryDir project(QDir(projects).filePath("acl-XXXXXX"));QVERIFY(project.isValid());
    auto bundle=input(project.path(),"import time\ntime.sleep(30)\n");QVERIFY(bundle->ok());
    QElapsedTimer clock;clock.start();
    const auto runtimeBefore=treeSnapshot(runtime),inputBefore=treeSnapshot(bundle->root);
    QVERIFY(!runtimeBefore.isEmpty());QVERIFY(!inputBefore.isEmpty());
    {SandboxRequest ok;ok.runtime=verified;ok.input=input(project.path(),"print('ok')");ok.policy.allowGenerated=true;
     const auto result=runWindowsSandbox(ok);QVERIFY2(result.ok(),qPrintable(result.code+':'+QString::fromUtf8(result.error)));}
    {SandboxRequest cancel;cancel.runtime=verified;cancel.input=bundle;cancel.policy.allowGenerated=true;
     QElapsedTimer stop;stop.start();cancel.cancelled=[&]{return stop.elapsed()>2000;};
     const auto result=runWindowsSandbox(cancel);QCOMPARE(result.code,QString("cancelled"));}
    const auto runtimeAfter=treeSnapshot(runtime),inputAfter=treeSnapshot(bundle->root);
    QCOMPARE(runtimeAfter.size(),runtimeBefore.size());QCOMPARE(inputAfter.size(),inputBefore.size());
    for(auto it=runtimeBefore.begin();it!=runtimeBefore.end();++it)QVERIFY2(runtimeAfter.value(it.key())==it.value(),qPrintable(it.key()));
    for(auto it=inputBefore.begin();it!=inputBefore.end();++it)QVERIFY2(inputAfter.value(it.key())==it.value(),qPrintable(it.key()));
    qInfo()<<"ACL/byte snapshot rows"<<runtimeBefore.size()+inputBefore.size()<<"in"<<clock.elapsed()<<"ms";
  }
  void privateViewIsUsedAndReusedAcrossRuns(){
    // 前序用例可能已构建视图（内容寻址跨用例留存）：先清缓存视图，钉住
    // 「首建 → 复用」两段，不依赖用例次序。
    const auto runtimeViewDir=QDir(controlRoot()).filePath("runtime-views");
    for(const auto &entry:QDir(runtimeViewDir).entryList(QDir::Dirs|QDir::NoDotAndDotDot))
      QVERIFY(QDir(QDir(runtimeViewDir).filePath(entry)).removeRecursively());
    const auto code="import json,sys,os\nprint(json.dumps({'exe':sys.executable,'cwd':os.getcwd()}))";
    const auto first=run(code);QVERIFY2(first.ok(),qPrintable(first.code+':'+QString::fromUtf8(first.error)));
    QVERIFY(!first.viewReused);QVERIFY(first.viewBytes>0);QVERIFY(first.viewFiles>0);
    const auto second=run(code);QVERIFY2(second.ok(),qPrintable(second.code+':'+QString::fromUtf8(second.error)));
    QVERIFY(second.viewReused);QCOMPARE(second.viewBytes,first.viewBytes);
    for(const SandboxResult *result:{&first,&second}) {
      const auto object=QJsonDocument::fromJson(result->output).object();
      const auto exe=QDir::fromNativeSeparators(object.value("exe").toString()),cwd=QDir::fromNativeSeparators(object.value("cwd").toString());
      // 执行视图在服务控制根内；来源 runtime/工程输入不再是被执行路径。
      QVERIFY2(exe.startsWith(controlRoot()+'/'),qPrintable(exe));
      QVERIFY2(!exe.startsWith(runtime+'/',Qt::CaseInsensitive),qPrintable(exe));
      QVERIFY2(cwd.startsWith(controlRoot()+'/'),qPrintable(cwd));
      QVERIFY2(QFileInfo(exe).fileName().compare("python.exe",Qt::CaseInsensitive)==0,qPrintable(exe));
    }
    QVERIFY(QDir(controlRoot()).exists("runtime-views"));
  }
  void viewTamperingIsRefusedAndSourceStaysIntact(){
    const auto first=run("print('build')");QVERIFY2(first.ok(),qPrintable(first.code+':'+QString::fromUtf8(first.error)));
    // 同尺寸篡改私有视图：复用验证须在执行前拒绝（不静默重建掩盖差异）。
    const auto viewRoot=QDir(QDir(controlRoot()).filePath("runtime-views")).entryList(QDir::Dirs|QDir::NoDotAndDotDot).constFirst();
    const auto target=QDir(QDir(QDir(controlRoot()).filePath("runtime-views")).filePath(viewRoot)).filePath("LICENSE.txt");
    QVERIFY(QFile::exists(target));
    const auto wide=QDir::toNativeSeparators(target);const auto attributes=GetFileAttributesW((PCWSTR)wide.utf16());
    QVERIFY(SetFileAttributesW((PCWSTR)wide.utf16(),attributes&~FILE_ATTRIBUTE_READONLY));
    const auto restore=qScopeGuard([&]{SetFileAttributesW((PCWSTR)wide.utf16(),attributes);});
    {QFile file(target);QVERIFY(file.open(QIODevice::ReadWrite));QVERIFY(file.size()>0);file.seek(0);char byte=0;QVERIFY(file.getChar(&byte));
     file.seek(0);QVERIFY(file.putChar(char(byte^1)));file.close();}
    const auto second=run("print('forbidden')");QCOMPARE(second.code,QString("python_runtime_changed"));QVERIFY(second.output.isEmpty());
  }
  void failedVerificationReleasesOnlyItsOwnLease(){
    using windows_detail::view_test::setVerifyHookForTest;
    for(const bool tamper:{false,true}) {
      const auto warm=run("print('warm')");
      QVERIFY2(warm.ok(),qPrintable(warm.code+':'+QString::fromUtf8(warm.error)));
      QTemporaryDir firstProject(QDir(projects).filePath("lease-first-XXXXXX"));
      QTemporaryDir secondProject(QDir(projects).filePath("lease-second-XXXXXX"));
      QVERIFY(firstProject.isValid());QVERIFY(secondProject.isValid());
      const auto permit=claimExecutionPermit();
      QVERIFY(permit);
      SandboxRequest first;first.runtime=verified;first.input=input(firstProject.path(),"print('first')");
      first.policy.allowGenerated=true;first.permit=permit;
      SandboxRequest second;second.runtime=verified;second.input=input(secondProject.path(),"print('second')");
      second.policy.allowGenerated=true;second.permit=permit;
      std::atomic<bool> cancelSecond{false};
      second.cancelled=[&]{return cancelSecond.load();};
      QSemaphore entered,proceed;
      std::atomic<int> calls{0};
      setVerifyHookForTest([&]{
        if(calls.fetch_add(1)==0){entered.release();proceed.acquire();}
        else if(!tamper)cancelSecond.store(true);
      });
      SandboxResult firstResult;
      std::thread worker([&]{firstResult=runWindowsSandbox(first);});
      const auto cleanup=qScopeGuard([&]{
        proceed.release();
        if(worker.joinable())worker.join();
        setVerifyHookForTest({});
      });
      QVERIFY2(entered.tryAcquire(1,30000),"first request did not reach view verification");
      const auto views=QDir(QDir(controlRoot()).filePath("runtime-views"));
      const auto viewNames=views.entryList(QDir::Dirs|QDir::NoDotAndDotDot);
      QCOMPARE(viewNames.size(),1);
      const auto target=QDir(views.filePath(viewNames.first())).filePath("LICENSE.txt");
      QByteArray original;
      DWORD attributes=INVALID_FILE_ATTRIBUTES;
      if(tamper) {
        QFile file(target);QVERIFY(file.open(QIODevice::ReadOnly));original=file.readAll();file.close();
        QVERIFY(!original.isEmpty());
        const auto wide=QDir::toNativeSeparators(target);
        attributes=GetFileAttributesW((PCWSTR)wide.utf16());
        QVERIFY(attributes!=INVALID_FILE_ATTRIBUTES);
        QVERIFY(SetFileAttributesW((PCWSTR)wide.utf16(),attributes&~FILE_ATTRIBUTE_READONLY));
        QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));
        auto changed=original;changed[0]=changed[0]=='X'?'Y':'X';
        QCOMPARE(file.write(changed),qint64(changed.size()));file.close();
      }
      const auto restore=qScopeGuard([&]{
        if(!tamper||attributes==INVALID_FILE_ATTRIBUTES)return;
        QFile file(target);
        if(file.open(QIODevice::WriteOnly|QIODevice::Truncate)){file.write(original);file.close();}
        const auto wide=QDir::toNativeSeparators(target);
        SetFileAttributesW((PCWSTR)wide.utf16(),attributes);
      });
      const auto secondResult=runWindowsSandbox(second);
      QCOMPARE(secondResult.code,tamper?QString("python_runtime_changed"):QString("cancelled"));
      const auto leases=QDir(QDir(controlRoot()).filePath("view-leases")).entryList({"*.json"},QDir::Files);
      QCOMPARE(leases.size(),1);
      if(tamper) {
        QFile file(target);QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));
        QCOMPARE(file.write(original),qint64(original.size()));file.close();
      }
      proceed.release();worker.join();
      QVERIFY2(firstResult.ok(),qPrintable(firstResult.code+':'+QString::fromUtf8(firstResult.error)));
      QVERIFY(QDir(QDir(controlRoot()).filePath("view-leases")).entryList({"*.json"},QDir::Files).isEmpty());
    }
  }
  void cancellationDoesNotHideLeaseCleanupFailure(){
    using windows_detail::view_test::setFailuresForTest;
    using windows_detail::view_test::setVerifyHookForTest;
    const auto warm=run("print('warm')");
    QVERIFY2(warm.ok(),qPrintable(warm.code+':'+QString::fromUtf8(warm.error)));
    QTemporaryDir project(QDir(projects).filePath("lease-cleanup-XXXXXX"));QVERIFY(project.isValid());
    std::atomic<bool> cancel{false};
    setVerifyHookForTest([&]{cancel.store(true);});
    setFailuresForTest({},{},{},[]{return true;});
    const auto reset=qScopeGuard([]{setVerifyHookForTest({});setFailuresForTest({},{},{},{});});
    SandboxRequest request;request.runtime=verified;request.input=input(project.path(),"print('never')");
    request.policy.allowGenerated=true;request.cancelled=[&]{return cancel.load();};
    const auto result=runWindowsSandbox(request);
    QCOMPARE(result.code,QString("python_sandbox_cleanup_failed"));
    const auto leases=QDir(QDir(controlRoot()).filePath("view-leases"));
    const auto names=leases.entryList({"*.json"},QDir::Files);QCOMPARE(names.size(),1);
    QVERIFY(QFile::remove(leases.filePath(names.first())));
  }
  void legacyDeadHostLeaseIsRecovered(){
    const auto warm=run("print('warm')");
    QVERIFY2(warm.ok(),qPrintable(warm.code+':'+QString::fromUtf8(warm.error)));
    const auto leases=QDir(QDir(controlRoot()).filePath("view-leases"));
    const auto legacy=leases.filePath(anchor()+".4294967295-1.json");
    QSaveFile file(legacy);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QJsonObject record{{"schema","paleo.python-sandbox-view-lease.v1"},
      {"manifestSha256",anchor()},{"viewPath","runtime-views/"+anchor()},
      {"hostPid",qint64(4294967295LL)},{"hostCreated100ns","1"}};
    const auto bytes=QJsonDocument(record).toJson();
    QCOMPARE(file.write(bytes),qint64(bytes.size()));QVERIFY(file.commit());
    const auto next=run("print('recovered')");
    QVERIFY2(next.ok(),qPrintable(next.code+':'+QString::fromUtf8(next.error)));
    QVERIFY(!QFileInfo::exists(legacy));
  }
  void viewFailureInjectionsRefuseExecution(){
    using windows_detail::view_test::setFailuresForTest;
    const auto clear=qScopeGuard([]{setFailuresForTest({},{},{},{});});
    const auto runtimeViewDir=QDir(controlRoot()).filePath("runtime-views");
    // 复制失败：删掉既有视图强制走构建路径——拒绝执行且无 ready 视图残留。
    for(const auto &entry:QDir(runtimeViewDir).entryList(QDir::Dirs|QDir::NoDotAndDotDot))
      QVERIFY(QDir(QDir(runtimeViewDir).filePath(entry)).removeRecursively());
    setFailuresForTest([]{return true;},{},{},{});
    QCOMPARE(run("print('forbidden')").code,QString("python_view_copy_forced"));
    // 复制失败连 staging 一并回收：不残留任何（部分复制的）视图目录。
    QVERIFY(QDir(runtimeViewDir).entryList(QDir::Dirs|QDir::NoDotAndDotDot).isEmpty());
    setFailuresForTest({},[]{return true;},{},{});
    QCOMPARE(run("print('forbidden')").code,QString("python_view_acl_forced"));
    setFailuresForTest({},{},[]{return true;},{});
    QCOMPARE(run("print('forbidden')").code,QString("python_view_lease_forced"));
    setFailuresForTest({},{},{},[]{return true;});
    const auto leaked=run("print('cleanup-pending')");QCOMPARE(leaked.code,QString("python_sandbox_cleanup_failed"));QVERIFY(leaked.output.isEmpty());
    QVERIFY(!leaked.outputLog.isEmpty()); // 私有输出有界保留、不进成功链
    // 租约记录保留（可重试凭据），下次正常运行后由恢复路径回收。
    setFailuresForTest({},{},{},{});
    const auto healed=run("print('maintenance')");QVERIFY2(healed.ok(),qPrintable(healed.code+':'+QString::fromUtf8(healed.error)));
  }
  void twoHostsOverlapAndCrashIsReconciled(){
    QTemporaryDir projectA(QDir(projects).filePath("overlap-a-XXXXXX")),projectB(QDir(projects).filePath("overlap-b-XXXXXX"));
    QVERIFY(projectA.isValid()&&projectB.isValid());
    const auto markerA=projectA.filePath("launched"),resultA=projectA.filePath("result.json");
    const auto markerB=projectB.filePath("launched"),resultB=projectB.filePath("result.json");
    QProcess hostA,hostB;
    hostA.start(QCoreApplication::applicationFilePath(),{"--view-overlap",runtime,projectA.path(),markerA,resultA});
    hostB.start(QCoreApplication::applicationFilePath(),{"--view-overlap",runtime,projectB.path(),markerB,resultB});
    QVERIFY(hostA.waitForStarted(5000));QVERIFY(hostB.waitForStarted(5000));
    // 屏障：两个 helper 都已进入运行（各自 python 睡 30s）→ 真实重叠。
    const QDeadlineTimer barrier(60000);
    while((!QFile::exists(markerA)||!QFile::exists(markerB))&&!barrier.hasExpired())
      QThread::msleep(100);
    QVERIFY2(QFile::exists(markerA)&&QFile::exists(markerB),"both hosts must enter their runs");
    QThread::msleep(2000); // 重叠窗口：A/B 的 python 同时在跑
    HANDLE victim=OpenProcess(PROCESS_TERMINATE,FALSE,DWORD(hostA.processId()));
    QVERIFY(victim);
    TerminateProcess(victim,88); // A 宿主硬终止（崩溃面）
    CloseHandle(victim);
    QVERIFY(hostA.waitForFinished(10000));
    QVERIFY2(hostB.waitForFinished(60000),"surviving host must finish its own run");
    QFile file(resultB);QVERIFY(file.open(QIODevice::ReadOnly));
    const auto record=QJsonDocument::fromJson(file.readAll()).object();file.close();
    QVERIFY2(record.value("code").toString()=="ok",qPrintable(record.value("code").toString()));
    // A 崩溃不删 B 的执行文件：视图仍在且下一次维护运行后租约清空。
    const auto next=run("print('maintenance')");QVERIFY2(next.ok(),qPrintable(next.code+':'+QString::fromUtf8(next.error)));
    const auto leases=QDir(QDir(controlRoot()).filePath("view-leases")).entryList({"*.json"},QDir::Files);
    QVERIFY2(leases.isEmpty(),qPrintable(leases.join(QLatin1Char(','))));
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
  if(app.arguments().size()==6&&app.arguments()[1]=="--view-overlap")return overlapHost(app.arguments());
#endif
  if(qEnvironmentVariable("PALEO_REQUIRE_AGENT_PYTHON_SANDBOX")!="1"&&runtimeRoot().isEmpty()){
    TestPythonWindowsSandboxMissing test;return QTest::qExec(&test,argc,argv);}
  TestPythonWindowsSandbox test;return QTest::qExec(&test,argc,argv);
}
#include "tst_python_windows_sandbox.moc"
