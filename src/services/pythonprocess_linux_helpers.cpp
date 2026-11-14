// 层：数据
#include "pythonprocess_linux_internal.h"
#include "pythonprocess_linux_lock.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
#include <cerrno>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace paleo::python::process_detail::linux_native {
Fd::~Fd() { if(value>=0)::close(value); }
namespace {
bool trustedParents(const QString &path) {
  auto directory=QFileInfo(path).absolutePath();
  for(;;) {
    const auto raw=QFile::encodeName(directory);struct stat st{};
    if(::stat(raw.constData(),&st)||!S_ISDIR(st.st_mode)||st.st_uid!=0||(st.st_mode&(S_IWGRP|S_IWOTH)))return false;
    if(directory=="/")return true;
    directory=QFileInfo(directory).absolutePath();
  }
}
bool trustedFile(const QJsonObject &row) {
  const auto path=row.value("path").toString();const auto raw=QFile::encodeName(path);
  struct stat st{};
  if(::stat(raw.constData(),&st)||st.st_uid!=0||!S_ISREG(st.st_mode)||
      (st.st_mode&(S_IWGRP|S_IWOTH|S_ISUID|S_ISGID))||st.st_size<1||st.st_size>64*1024*1024||
      QFileInfo(path).canonicalFilePath()!=path||!trustedParents(path))return false;
  const auto capabilities=::getxattr(raw.constData(),"security.capability",nullptr,0);
  if(capabilities>=0||(errno!=ENODATA&&errno!=ENOTSUP))return false;
  QFile file(path);QCryptographicHash hash(QCryptographicHash::Sha256);
  return file.open(QIODevice::ReadOnly)&&hash.addData(&file)&&
    QString::fromLatin1(hash.result().toHex())==row.value("sha256").toString();
}
}
QJsonObject verifiedHelpers() {
  for(const auto &item:QJsonDocument::fromJson(LinuxHelperLock).array()) {
    const auto profile=item.toObject();bool valid=true;
    for(const auto &entry:profile.value("files").toArray())if(!trustedFile(entry.toObject())) {valid=false;break;}
    if(valid)return profile;
  }
  return {};
}
QProcessEnvironment helperEnvironment() {
  const auto uid=::getuid();const auto path=QStringLiteral("/run/user/%1").arg(uid);
  const auto raw=QFile::encodeName(path);struct stat st{};
  if(uid==0||::stat(raw.constData(),&st)||!S_ISDIR(st.st_mode)||st.st_uid!=uid||
      (st.st_mode&077)!=0||!QFileInfo::exists(path+"/bus"))return {};
  QProcessEnvironment env;env.insert("LANG","C.UTF-8");env.insert("PATH","/usr/bin");
  env.insert("XDG_RUNTIME_DIR",path);env.insert("DBUS_SESSION_BUS_ADDRESS","unix:path="+path+"/bus");return env;
}
QStringList scopeArguments(const QString &unit,const ExecutionPolicy &p) {
  return {"--user","--scope","--quiet","--collect","--expand-environment=no","--unit="+unit,
    "-p","MemoryMax="+QString::number(p.memoryBytes),"-p","MemorySwapMax=0","-p","TasksMax=8",
    "-p","CPUQuota=100%","-p","RuntimeMaxSec="+QString::number(p.wallSeconds)+"s",
    "-p","TimeoutStopSec=1s","-p","KillMode=control-group","-p","OOMPolicy=kill"};
}
QString prepareEnvelope(const Launch &request,Prepared &prepared) {
  if(const auto code=request.policy.validate();!code.isEmpty())return code;
  if(verifiedHelpers().isEmpty())return "python_linux_helpers_untrusted";
  prepared.environment=helperEnvironment();if(prepared.environment.isEmpty())return "python_linux_user_manager_missing";
  int pipe[2];if(::pipe2(pipe,O_CLOEXEC))return "python_linux_barrier_failed";
  prepared.blockRead=std::make_shared<Fd>(pipe[0]);prepared.blockWrite=std::make_shared<Fd>(pipe[1]);
  prepared.unit="paleo-python-"+QUuid::createUuid().toString(QUuid::Id128);
  prepared.arguments=scopeArguments(prepared.unit,request.policy);return {};
}
void configureProcess(QProcess &process,const Prepared &prepared,const ExecutionPolicy &policy) {
  const int filterFd=prepared.filter?prepared.filter->value:-1,blockFd=prepared.blockRead->value;
  const qint64 fileBytes=prepared.fileBytes;
  process.setProcessEnvironment(prepared.environment);process.setProcessChannelMode(QProcess::SeparateChannels);
  process.setChildProcessModifier([filterFd,blockFd,fileBytes,policy,&process] {
    // No allocation or Qt logging after fork. Copies prevent descriptor collisions.
    const int filterCopy=filterFd>=0?::fcntl(filterFd,F_DUPFD_CLOEXEC,5):-1;
    const int blockCopy=::fcntl(blockFd,F_DUPFD_CLOEXEC,5);
    if(blockCopy<0||(filterFd>=0&&(filterCopy<0||::dup2(filterCopy,3)<0))||::dup2(blockCopy,4)<0)
      process.failChildProcessModifier("python process descriptors",errno);
    if(filterFd<0)::close(3);
    const struct rlimit cpu{rlim_t(policy.cpuSeconds),rlim_t(policy.cpuSeconds)},file{rlim_t(fileBytes),rlim_t(fileBytes)},core{0,0};
    if(::setrlimit(RLIMIT_CPU,&cpu)||::setrlimit(RLIMIT_FSIZE,&file)||::setrlimit(RLIMIT_CORE,&core))
      process.failChildProcessModifier("python process limits",errno);
  });
  QProcess::UnixProcessParameters params;
  params.flags=QProcess::UnixProcessFlag::CloseFileDescriptors;params.lowestFileDescriptorToClose=5;
  process.setUnixProcessParameters(params);
}
}
