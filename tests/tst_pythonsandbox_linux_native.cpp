// 层：测试壳
#include "services/pythonsandbox_linux.h"
#include "services/pythoninputbundle.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <cstdio>
#include <cerrno>
#include <fcntl.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <unistd.h>
using namespace paleo::python;
namespace {
bool write(const QString &path,const QByteArray &bytes) {QFile f(path);return f.open(QIODevice::WriteOnly)&&f.write(bytes)==bytes.size();}
QString sha(const QByteArray &bytes) {return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
bool overwritePrivate(const QString &path,const QString &root,qint64 size) {
  if(!QFileInfo(path).canonicalFilePath().startsWith(root+'/'))return false;
  const auto raw=QFile::encodeName(path);const int fd=::open(raw.constData(),O_WRONLY|O_CLOEXEC|O_NOFOLLOW);
  if(fd<0)return false;const auto close=qScopeGuard([&]{::close(fd);});struct stat st{};
  if(::fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=::getuid()||st.st_nlink!=1||st.st_size!=size)return false;
  QByteArray block(64*1024,'X');qint64 offset=0;
  while(offset<size) {
    const auto count=::write(fd,block.constData(),size_t(qMin<qint64>(block.size(),size-offset)));
    if(count<0&&errno==EINTR)continue;
    if(count<=0)return false;offset+=count;
  }
  return true;
}
std::shared_ptr<const VerifiedRuntime> copyRuntime(const VerifiedRuntime &source,const QString &workspace) {
  const auto root=QFileInfo(workspace).canonicalFilePath();if(root.isEmpty())return {};
  const auto clone=root+"/runtime-copy";if(!QDir().mkdir(clone)||QFileInfo(clone).canonicalFilePath()!=clone)return {};
  QFile manifest(source.root+"/runtime-manifest.json");
  if(!manifest.open(QIODevice::ReadOnly)||manifest.size()>4*1024*1024)return {};
  const auto raw=manifest.read(4*1024*1024+1);if(sha(raw)!=source.manifestSha256)return {};
  const auto rows=QJsonDocument::fromJson(raw).object().value("files").toArray();qint64 total=0;
  if(rows.isEmpty()||rows.size()>4000||source.treeSize>512*1024*1024)return {};
  for(const auto &entry:rows) {
    const auto row=entry.toObject();const auto name=row.value("path").toString();
    const auto target=clone+'/'+name;const auto size=row.value("size").toInteger(-1);
    if(!QDir::isRelativePath(name)||name.split('/').contains("..")||name.contains(QChar(0))||size<0||size>source.treeSize-total)return {};
    if(!QDir().mkpath(QFileInfo(target).absolutePath()))return {};
    QFile from(source.root+'/'+name),to(target);if(!from.open(QIODevice::ReadOnly)||!to.open(QIODevice::WriteOnly|QIODevice::NewOnly))return {};
    qint64 remaining=size;
    while(remaining) {
      const auto block=from.read(qMin<qint64>(64*1024,remaining));
      if(block.isEmpty()||to.write(block)!=block.size())return {};
      remaining-=block.size();
    }
    if(!from.read(1).isEmpty())return {};to.close();total+=size;
    const auto mode=QFile::ReadOwner|QFile::WriteOwner|QFile::ReadGroup|QFile::ReadOther;
    if(!QFile::setPermissions(target,row.value("executable").toBool()?mode|QFile::ExeOwner|QFile::ExeGroup|QFile::ExeOther:mode))return {};
  }
  if(!write(clone+"/runtime-manifest.json",raw))return {};
  return verifyRuntime({clone,source.manifestSha256,true}).runtime;
}
std::shared_ptr<InputBundle> bundle(const QString &root,const QByteArray &code,const QByteArray &input,const QString &protocol,const QByteArray &data) {
  QDir().mkpath(root);QJsonArray files;
  QMap<QString,QByteArray> values{{"code.py",code},{"request.json",input}};
  if(protocol=="paleo.python-analysis-frame.v1")values.insert("data.bin",data);
  for(auto it=values.begin();it!=values.end();++it) {
    if(!write(root+"/"+it.key(),it.value()))return {};
    files.append(QJsonObject{{"path",it.key()},{"size",it.value().size()},{"sha256",sha(it.value())}});
  }
  const QJsonObject manifest{{"schema","paleo.python-inputs.v1"},{"metadata",QJsonObject{{"inputProtocol",protocol}}},{"files",files}};
  const auto raw=QJsonDocument(manifest).toJson(QJsonDocument::Compact);if(!write(root+"/manifest.json",raw))return {};
  auto out=std::make_shared<InputBundle>();out->root=root;out->code="ok";out->manifest=manifest;
  out->reference={{"manifestHash",sha(raw)}};return out;
}
}
int main(int argc,char **argv) {
  QCoreApplication app(argc,argv);if(app.arguments().size()<3)return 2;
  const auto checked=verifyRuntime({app.arguments()[1],app.arguments()[2],true});
  if(!checked.ready()) {fprintf(stderr,"fixture: %s\n",qPrintable(checked.code));return 3;}
  const auto runtime=checked.runtime;const auto select=app.arguments().value(3);
  const auto temporary=QFileInfo(QDir::tempPath()).canonicalFilePath();
  if(temporary==runtime->root||temporary.startsWith(runtime->root+'/'))return 4;
  QTemporaryDir inputs;if(!inputs.isValid())return 4;
  const auto cloned=copyRuntime(*runtime,inputs.path());if(!cloned)return 5;
  SandboxRequest request;request.runtime=runtime;request.policy.allowGenerated=true;
  qputenv("PALEO_SECRET","must-not-inherit");qputenv("LD_PRELOAD","/untrusted.so");qputenv("PATH","/untrusted");
  SandboxRequest denied;denied.policy.allowTrusted=true;const auto deniedCode=runLinuxSandbox(denied).code;
  QJsonArray rows;bool all=deniedCode=="python_generated_execution_denied";int index=0;QByteArray stdinBytes,data;
  QString protocol="paleo.python-json.v1";
  const auto run=[&](const char *name,const QByteArray &code,const QString &expected) {
    SandboxResult result;if(!select.isEmpty()&&select!=name)return result;
    request.input=bundle(inputs.path()+"/"+QString::number(index++),code,stdinBytes,protocol,data);if(!request.input)exit(9);
    QElapsedTimer elapsed;elapsed.start();result=runLinuxSandbox(request);
    const bool passed=result.code==expected&&(result.ok()||result.output.isEmpty());all=all&&passed;
    rows.append(QJsonObject{{"name",name},{"code",result.code},{"expected",expected},{"passed",passed},
      {"output",QString::fromUtf8(result.output).left(2048)},{"error",QString::fromUtf8(result.error).left(2048)},
      {"outputLog",QString::fromUtf8(result.outputLog).left(2048)},
      {"ms",double(elapsed.elapsed())},{"cpu",double(result.cpuMicros)},{"memory",double(result.peakMemoryBytes)},
      {"hostReserved",double(result.hostSnapshotReservedBytes)},{"childMemoryLimit",double(result.childMemoryLimitBytes)}});
    fprintf(stderr,"%s: %s (%lld ms)\n",name,qPrintable(result.code),elapsed.elapsed());return result;
  };
  stdinBytes="{\"values\":[1,2,3]}";
  run("numpy","import json,sys,numpy as np\na=json.load(sys.stdin)\nassert int(np.sum(a['values']))==6\nprint(np.__version__)\n","ok");
  protocol="paleo.python-analysis-frame.v1";stdinBytes="{\"count\":2}";data=QByteArray::fromHex("0000803f00000040");
  run("frame","import sys,json,numpy as np\nn=int.from_bytes(sys.stdin.buffer.read(4),'little')\np=json.loads(sys.stdin.buffer.read(n))\na=np.frombuffer(sys.stdin.buffer.read(),dtype='<f4')\nassert a.size==p['count'] and float(a.sum())==3\nprint('frame-ok')\n","ok");
  protocol="paleo.python-json.v1";data.clear();
  stdinBytes.clear();
  const int shm=::shmget(IPC_PRIVATE,4096,IPC_CREAT|0600);if(shm<0)return 7;
  void *ipc=::shmat(shm,nullptr,0);
  if(ipc==reinterpret_cast<void*>(-1)) {::shmctl(shm,IPC_RMID,nullptr);return 7;}
  if(::shmctl(shm,IPC_RMID,nullptr)) {::shmdt(ipc);return 7;}
  void *hostCheck=::shmat(shm,nullptr,0);
  if(hostCheck==reinterpret_cast<void*>(-1)) {::shmdt(ipc);return 7;}
  ::shmdt(hostCheck);
  const auto cleanup=qScopeGuard([&]{::shmdt(ipc);});
  QByteArray isolation=R"(import os,socket,ctypes,json
assert 'PALEO_SECRET' not in os.environ and 'LD_PRELOAD' not in os.environ
for p in ['/work/created','/tmp/created','/dev/shm/created','/input/code.py','/runtime/bin/python3.14']:
 try: open(p,'w'); raise AssertionError('write succeeded '+p)
 except OSError: pass
assert not os.path.exists('/home') and not os.path.exists('/run/user/1000/bus')
assert not os.path.exists('/proc/1/root/home')
for make in [lambda:socket.socket(),lambda:socket.socket(socket.AF_UNIX),os.fork,lambda:os.unshare(0x40000000)]:
 try: make(); raise AssertionError('resource escape')
 except (OSError,AttributeError):pass
libc=ctypes.CDLL(None,use_errno=True)
libc.shmat.restype=ctypes.c_void_p
assert libc.shmat(HOST_SHMID,None,0)==ctypes.c_void_p(-1).value
assert libc.syscall(101,0,1,0,0)==-1
assert libc.syscall(310,1,0,0,0,0,0)==-1
assert libc.syscall(425,1,0)==-1
import sys,fcntl,resource
assert resource.getrlimit(resource.RLIMIT_NOFILE)==(4096,4096)
fds={}
for fd in os.listdir('/proc/self/fd'):
 try:fds[fd]=os.readlink('/proc/self/fd/'+fd)
 except OSError:pass
print(json.dumps(fds),file=sys.stderr)
assert len(fds)<=4
for fd,target in fds.items():
 if int(fd)>2:
  assert target.startswith('/runtime/') and fcntl.fcntl(int(fd),fcntl.F_GETFL)&os.O_ACCMODE==os.O_RDONLY
assert open('/proc/self/status').read().split('NoNewPrivs:')[1].splitlines()[0].strip()=='1'
assert open('/proc/self/status').read().split('CapEff:')[1].splitlines()[0].strip()=='0000000000000000'
print('isolation-ok')
)";
  isolation.replace("HOST_SHMID",QByteArray::number(shm));run("isolation",isolation,"ok");
  protocol="paleo.python-analysis-frame.v1";stdinBytes="{}";data=QByteArray(4*1024*1024,'x');
  run("duplex","import os,sys\nos.write(1,b'A'*1048576)\nn=int.from_bytes(sys.stdin.buffer.read(4),'little')\nsys.stdin.buffer.read(n)\na=sys.stdin.buffer.read()\nassert len(a)==4194304\n","ok");
  stdinBytes.clear();data.clear();protocol="paleo.python-json.v1";
  request.policy.outputBytes=4096;
  run("stdout","import os\nwhile True: os.write(1,b'x'*4096)\n","python_process_output_limit");request.policy.outputBytes=64*1024*1024;
  request.policy.logBytes=4096;
  run("stderr","import os\nwhile True: os.write(2,b'x'*4096)\n","python_process_log_limit");request.policy.logBytes=1024*1024;
  request.policy.logBytes=64;
  const auto failed=run("failed-stdout","import os,sys\nos.write(1,b'A'*128)\nsys.exit(7)\n","python_process_failed");
  if(select.isEmpty()||select=="failed-stdout")all=all&&failed.output.isEmpty()&&failed.outputLog==QByteArray(64,'A');
  request.policy.logBytes=1024*1024;
  run("memory","a=bytearray(1024*1024*1024)\n","python_process_memory_limit");
  request.policy.cpuSeconds=1;request.policy.wallSeconds=30;
  run("cpu","while True: pass\n","python_process_cpu_limit");request.policy.cpuSeconds=60;
  request.policy.wallSeconds=1;
  run("wall","import time\ntime.sleep(30)\n","python_process_timeout");request.policy.wallSeconds=120;
  QElapsedTimer cancel;cancel.start();request.cancelled=[&]{return cancel.elapsed()>2000;};
  run("cancel","import time\ntime.sleep(30)\n","cancelled");request.cancelled={};
  request.runtime=cloned;
  const auto numpyFile=cloned->root+"/lib/python"+QString::fromLatin1(PythonVersion).section('.',0,1)+"/site-packages/numpy/__init__.py";
  const auto originalSize=QFileInfo(numpyFile).size();if(originalSize<1)return 8;bool changed=false;
  request.cancelled=[&]{
    QFile children(QStringLiteral("/proc/self/task/%1/children").arg(::getpid()));
    if(!changed&&children.open(QIODevice::ReadOnly)&&!children.readAll().trimmed().isEmpty()) {
      const auto codePath=request.input->root+"/code.py";
      changed=overwritePrivate(numpyFile,cloned->root,originalSize)&&overwritePrivate(codePath,inputs.path(),QFileInfo(codePath).size());
      if(!changed)return true;
    }
    return false;
  };
  run("toctou","import numpy\nassert numpy.__version__=='2.5.4'\nprint('sealed-code-and-runtime')\n","ok");
  request.cancelled={};if(select.isEmpty()||select=="toctou")all=all&&changed;
  if(select=="hostdeath")run("hostdeath","import time\ntime.sleep(30)\n","ok");
  puts(QJsonDocument(QJsonObject{{"all",all},{"deniedCode",deniedCode},{"toctouChanged",changed},{"rows",rows}}).toJson(QJsonDocument::Compact).constData());
  return all?0:3;
}
