// 层：数据
#include "pythonsandbox_linux_image.h"
#include "services/pythonsandbox.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QtEndian>
#include <cmath>
#include <cerrno>
#include <fcntl.h>
#include <linux/memfd.h>
#include <linux/openat2.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace paleo::python {
namespace {
constexpr qint64 MaxBytes=512LL*1024*1024;
constexpr int MaxFiles=4000;
struct ScopedFd {
  int value=-1;
  explicit ScopedFd(int fd=-1):value(fd) {}
  ~ScopedFd(){if(value>=0)::close(value);}
};
bool digest(const QString &s) {
  if(s.size()!=64)return false;
  for(auto c:s)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
  return true;
}
bool relative(const QString &path) {
  if(path.isEmpty()||path.size()>512||path.startsWith('/')||path.contains('\\')||path.contains(':')||path.contains(QChar(0)))return false;
  for(const auto &part:path.split('/'))if(part.isEmpty()||part=="."||part=="..")return false;
  return true;
}
int beneath(int directory,const QString &path) {
  const auto name=QFile::encodeName(path);
  struct open_how how{};how.flags=O_RDONLY|O_CLOEXEC|O_NOFOLLOW;how.resolve=RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS;
  return int(::syscall(SYS_openat2,directory,name.constData(),&how,sizeof(how)));
}
bool writeAll(int fd,const QByteArray &bytes) {
  qsizetype offset=0;
  while(offset<bytes.size()) {
    const auto n=::write(fd,bytes.constData()+offset,size_t(bytes.size()-offset));
    if(n<0&&errno==EINTR)continue;
    if(n<=0)return false;
    offset+=n;
  }
  return true;
}
QString capture(int directory,const QJsonObject &row,SealedRuntimeImage &image,const std::function<bool()> &cancelled) {
  const auto name=row.value("path").toString(),sha=row.value("sha256").toString();
  const auto number=row.value("size").toDouble(-1);
  if(row.keys()!=QStringList{"executable","path","sha256","size"}||!relative(name)||!digest(sha)||
      !row.value("executable").isBool()||!std::isfinite(number)||number<0||number!=std::floor(number)||
      number>MaxBytes||image.bytes>MaxBytes-qint64(number))return "python_manifest_invalid";
  ScopedFd source(beneath(directory,name));if(source.value<0)return "python_runtime_changed";
  struct stat st{};
  if(::fstat(source.value,&st)||!S_ISREG(st.st_mode)||st.st_size!=qint64(number))return "python_runtime_changed";
  unsigned flags=MFD_CLOEXEC|MFD_ALLOW_SEALING;
#ifdef MFD_NOEXEC_SEAL
  flags|=MFD_NOEXEC_SEAL;
#endif
  ScopedFd created(int(::syscall(SYS_memfd_create,"paleo-python-image",flags)));
  if(created.value<0)return "python_snapshot_memfd_unavailable";
  ScopedFd target(::fcntl(created.value,F_DUPFD_CLOEXEC,16));if(target.value<0)return "python_snapshot_fd_budget";
  QCryptographicHash hash(QCryptographicHash::Sha256);qint64 count=0;
  QByteArray bytes(64*1024,Qt::Uninitialized);
  for(;;) {
    if(cancelled&&cancelled())return "cancelled";
    const auto n=::read(source.value,bytes.data(),size_t(bytes.size()));
    if(n<0&&errno==EINTR)continue;
    if(n<0)return "python_runtime_changed";
    if(n==0)break;
    if(count>qint64(number)-n)return "python_runtime_changed";
    const QByteArrayView block(bytes.constData(),n);hash.addData(block);
    if(!writeAll(target.value,QByteArray::fromRawData(block.data(),block.size())))return "python_snapshot_write_failed";
    count+=n;
  }
  if(count!=qint64(number)||QString::fromLatin1(hash.result().toHex())!=sha)return "python_runtime_changed";
  const int seals=F_SEAL_SEAL|F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_WRITE;
  if(::fchmod(target.value,0444)||::lseek(target.value,0,SEEK_SET)<0||::fcntl(target.value,F_ADD_SEALS,seals)||
      (::fcntl(target.value,F_GET_SEALS)&seals)!=seals)return "python_snapshot_sealing_failed";
  image.bytes+=count;image.files.append({name,target.value,count,row.value("executable").toBool()});
  target.value=-1;return {};
}
}
SealedRuntimeImage::~SealedRuntimeImage(){for(const auto &file:files)if(file.fd>=0)::close(file.fd);}
std::shared_ptr<SealedRuntimeImage> snapshotLinuxRuntime(std::shared_ptr<const VerifiedRuntime> runtime,qint64 capacity,
    const std::function<bool()> &cancelled,QString &code) {
  const auto fail=[&](const QString &reason){code=reason;return std::shared_ptr<SealedRuntimeImage>();};
  if(!runtime||!digest(runtime->manifestSha256)||runtime->root.contains(QChar(0)))return fail("python_manifest_anchor_required");
  if(cancelled&&cancelled())return fail("cancelled");
  const auto root=QFile::encodeName(runtime->root);ScopedFd directory(::open(root.constData(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
  if(directory.value<0)return fail("python_environment_missing");
  ScopedFd manifest(beneath(directory.value,"runtime-manifest.json"));if(manifest.value<0)return fail("python_manifest_invalid");
  struct stat st{};if(::fstat(manifest.value,&st)||!S_ISREG(st.st_mode)||st.st_size<1||st.st_size>4*1024*1024)return fail("python_manifest_invalid");
  QByteArray raw;QByteArray block(64*1024,Qt::Uninitialized);
  for(;;) {
    if(cancelled&&cancelled())return fail("cancelled");
    const auto n=::read(manifest.value,block.data(),size_t(block.size()));
    if(n<0&&errno==EINTR)continue;
    if(n<0||raw.size()>4*1024*1024-n)return fail("python_manifest_invalid");
    if(!n)break;
    raw.append(block.constData(),n);
  }
  if(QString::fromLatin1(QCryptographicHash::hash(raw,QCryptographicHash::Sha256).toHex())!=runtime->manifestSha256)
    return fail("python_manifest_changed");
  QJsonParseError parse;const auto document=QJsonDocument::fromJson(raw,&parse);
  if(parse.error!=QJsonParseError::NoError||!document.isObject()||!document.object().value("files").isArray())return fail("python_manifest_invalid");
  const auto rows=document.object().value("files").toArray();if(rows.isEmpty()||rows.size()>MaxFiles)return fail("python_snapshot_fd_budget");
  struct rlimit descriptors{};if(::getrlimit(RLIMIT_NOFILE,&descriptors))return fail("python_snapshot_fd_budget");
  if(descriptors.rlim_cur<4096) {
    if(descriptors.rlim_max<4096)return fail("python_snapshot_fd_budget");
    descriptors.rlim_cur=4096;if(::setrlimit(RLIMIT_NOFILE,&descriptors))return fail("python_snapshot_fd_budget");
  }
  auto image=std::make_shared<SealedRuntimeImage>();image->runtime=std::move(runtime);image->manifestBytes=std::move(raw);
  QSet<QString> names;
  for(const auto &entry:rows) {
    if(!entry.isObject())return fail("python_manifest_invalid");
    const auto row=entry.toObject();const auto name=row.value("path").toString();
    if(names.contains(name))return fail("python_manifest_invalid");names.insert(name);
    const auto size=row.value("size").toDouble(-1);
    if(!std::isfinite(size)||size<0||size>MaxBytes||size!=std::floor(size)||image->bytes>capacity-qint64(size))
      return fail("python_snapshot_memory_budget");
    if(const auto invalid=capture(directory.value,row,*image,cancelled);!invalid.isEmpty())return fail(invalid);
  }
  if(image->bytes!=image->runtime->treeSize)return fail("python_runtime_changed");
  code="ok";return image;
}
std::shared_ptr<SealedRuntimeImage> snapshotLinuxInputs(const InputBundle &input,const ExecutionPolicy &policy,qint64 capacity,qint64 bufferedCapacity,
    const std::function<bool()> &cancelled,QString &code) {
  const auto fail=[&](const QString &reason){code=reason;return std::shared_ptr<SealedRuntimeImage>();};
  const auto anchor=input.reference.value("manifestHash").toString();
  if(!input.ok()||!digest(anchor)||input.root.contains(QChar(0)))return fail("python_input_reference_invalid");
  const auto path=QFile::encodeName(input.root);ScopedFd directory(::open(path.constData(),O_DIRECTORY|O_RDONLY|O_CLOEXEC|O_NOFOLLOW));
  if(directory.value<0)return fail("python_input_changed");
  ScopedFd manifest(beneath(directory.value,"manifest.json"));struct stat st{};
  if(manifest.value<0||::fstat(manifest.value,&st)||!S_ISREG(st.st_mode)||st.st_size<1||st.st_size>64*1024)return fail("python_input_manifest_invalid");
  QByteArray raw;raw.resize(st.st_size);qint64 offset=0;
  while(offset<raw.size()) {
    if(cancelled&&cancelled())return fail("cancelled");
    const auto n=::read(manifest.value,raw.data()+offset,size_t(raw.size()-offset));
    if(n<0&&errno==EINTR)continue;
    if(n<=0)return fail("python_input_changed");offset+=n;
  }
  char extra;if(::read(manifest.value,&extra,1)!=0)return fail("python_input_changed");
  if(QString::fromLatin1(QCryptographicHash::hash(raw,QCryptographicHash::Sha256).toHex())!=anchor)return fail("python_input_changed");
  QJsonParseError parse;const auto document=QJsonDocument::fromJson(raw,&parse);const auto object=document.object();
  const auto rows=object.value("files").toArray();
  if(parse.error!=QJsonParseError::NoError||!document.isObject()||object.value("schema")!="paleo.python-inputs.v1"||
      !object.value("metadata").isObject()||rows.size()<2||rows.size()>16)return fail("python_input_manifest_invalid");
  const auto protocol=object.value("metadata").toObject().value("inputProtocol").toString();
  const bool frame=protocol==QLatin1String(SandboxAnalysisInputProtocol);
  if(!frame&&protocol!=QLatin1String(SandboxJsonInputProtocol))return fail("python_input_protocol_invalid");
  if(capacity<raw.size()||capacity>MaxBytes)return fail("python_snapshot_memory_budget");
  auto image=std::make_shared<SealedRuntimeImage>();image->manifestBytes=raw;QSet<QString> names;
  qint64 declared=raw.size(),stdinSize=frame?4:0,jsonSize=0,payloadBytes=0;
  static const QRegularExpression allowed("^[a-z][a-z0-9_.-]{0,63}$");
  for(const auto &entry:rows) {
    auto row=entry.toObject();const auto name=row.value("path").toString();const auto number=row.value("size").toDouble(-1);
    if(row.keys()!=QStringList{"path","sha256","size"}||names.contains(name)||!allowed.match(name).hasMatch()||
        name=="manifest.json"||!std::isfinite(number)||number<0||number!=std::floor(number)||
        number>(name=="code.py"?policy.codeBytes:policy.inputBytes)||
        declared-raw.size()>policy.inputBytes+policy.codeBytes-qint64(number))return fail("python_input_manifest_invalid");
    declared+=qint64(number);
    if(name!="code.py") {
      if(payloadBytes>policy.inputBytes-qint64(number))return fail("python_input_budget_exceeded");
      payloadBytes+=qint64(number);
    }
    if(name=="request.json") {stdinSize+=qint64(number);jsonSize=qint64(number);}
    if(frame&&name=="data.bin")stdinSize+=qint64(number);
    names.insert(name);
  }
  const QSet<QString> expected=frame?QSet<QString>{"code.py","request.json","data.bin"}:QSet<QString>{"code.py","request.json"};
  if(names!=expected)return fail("python_input_protocol_invalid");
  if(jsonSize<1||jsonSize>64*1024)return fail("python_input_budget_exceeded");
  if(declared>policy.diskBytes-policy.outputBytes-policy.logBytes)return fail("python_disk_budget_exceeded");
  if(declared>capacity||declared>bufferedCapacity-stdinSize)return fail("python_snapshot_memory_budget");
  for(const auto &entry:rows) {
    auto row=entry.toObject();row.insert("executable",false);
    if(const auto invalid=capture(directory.value,row,*image,cancelled);!invalid.isEmpty())
      return fail(invalid=="python_runtime_changed"?QStringLiteral("python_input_changed"):invalid);
  }
  if(frame) {
    QByteArray header(4,Qt::Uninitialized);qToLittleEndian(quint32(jsonSize),header.data());
    image->stdinChunks.append(header);
  }
  const QStringList payloads=frame?QStringList{"request.json","data.bin"}:QStringList{"request.json"};
  for(const auto &name:payloads)for(const auto &file:image->files)if(file.path==name) {
    QByteArray bytes(file.size,Qt::Uninitialized);qint64 offset=0;
    while(offset<file.size) {
      if(cancelled&&cancelled())return fail("cancelled");
      const auto n=::read(file.fd,bytes.data()+offset,size_t(qMin<qint64>(64*1024,file.size-offset)));
      if(n<0&&errno==EINTR)continue;
      if(n<=0)return fail("python_snapshot_read_failed");offset+=n;
    }
    if(::lseek(file.fd,0,SEEK_SET)<0)return fail("python_snapshot_read_failed");
    image->stdinChunks.append(bytes);
  }
  image->bufferedBytes=stdinSize;
  const auto &json=image->stdinChunks[frame?1:0];QStringDecoder utf8(QStringDecoder::Utf8);
  const QString decoded=utf8(json);Q_UNUSED(decoded);
  QJsonParseError jsonError;const auto request=QJsonDocument::fromJson(json,&jsonError);
  if(utf8.hasError()||jsonError.error!=QJsonParseError::NoError||!request.isObject())return fail("python_input_protocol_invalid");
  // Manifest bytes were already authenticated. Never reopen them after checking SHA.
  ScopedFd source(int(::syscall(SYS_memfd_create,"paleo-input-manifest",MFD_CLOEXEC|MFD_ALLOW_SEALING)));
  if(source.value<0||!writeAll(source.value,raw)||::fchmod(source.value,0444)||::lseek(source.value,0,SEEK_SET)<0||
      ::fcntl(source.value,F_ADD_SEALS,F_SEAL_SEAL|F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_WRITE))return fail("python_snapshot_sealing_failed");
  const int retained=::fcntl(source.value,F_DUPFD_CLOEXEC,16);if(retained<0)return fail("python_snapshot_fd_budget");
  image->files.append({"manifest.json",retained,raw.size(),false});image->bytes+=raw.size();
  code="ok";return image;
}
}
