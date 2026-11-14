// 层：数据
#include "pythonruntime.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSet>
#include <QSysInfo>
#include <cmath>

namespace paleo::python {
namespace {
constexpr qint64 MaxTreeBytes = 1024LL * 1024 * 1024;
constexpr int MaxFiles = 16384;
bool hexDigest(const QString &value) {
  if(value.size()!=64)return false;
  for(const auto c:value)if(!((c>='0' && c<='9') || (c>='a' && c<='f')))return false;
  return true;
}
bool safeRelative(const QString &path) {
  if(path.isEmpty() || path.size()>512 || path.contains('\\') || path.contains(':') || path.contains(QChar(0)) ||
      QDir::isAbsolutePath(path) || QDir::cleanPath(path)!=path)return false;
  for(const auto &part:path.split('/'))if(part.isEmpty() || part=="." || part==".." || part.endsWith('.') || part.endsWith(' '))return false;
  return true;
}
bool link(const QFileInfo &info) {
#ifdef Q_OS_WIN
  return info.isSymLink() || info.isJunction();
#else
  return info.isSymLink();
#endif
}
QString fileHash(QFile &file,qint64 expected,const std::function<bool()> &cancelled) {
  QCryptographicHash hash(QCryptographicHash::Sha256);
  qint64 remaining=expected;
  while(remaining>0) {
    if(cancelled && cancelled())return {};
    const auto bytes=file.read(qMin<qint64>(64*1024,remaining));
    if(bytes.isEmpty())return {};
    hash.addData(bytes);remaining-=bytes.size();
  }
  if(file.size()!=expected || file.error()!=QFileDevice::NoError)return {};
  return QString::fromLatin1(hash.result().toHex());
}
QString checkFiles(const QString &root,const QJsonArray &files,qint64 &total,const std::function<bool()> &cancelled) {
  if(files.isEmpty() || files.size()>MaxFiles)return "python_manifest_invalid";
  QSet<QString> expected,folded;total=0;
  for(const auto &value:files) {
    if(cancelled && cancelled())return "cancelled";
    if(!value.isObject())return "python_manifest_invalid";
    const auto row=value.toObject();const auto name=row.value("path").toString();
    const auto digest=row.value("sha256").toString();const auto size=row.value("size").toDouble(-1);
    const auto caseKey=name.toCaseFolded();
    if(row.keys()!=QStringList{"executable","path","sha256","size"} || !safeRelative(name) || name=="runtime-manifest.json" ||
        expected.contains(name) || folded.contains(caseKey) || !hexDigest(digest) || !row.value("executable").isBool() ||
        !std::isfinite(size) || size<0 || size>MaxTreeBytes || size!=std::floor(size) || total>MaxTreeBytes-qint64(size))
      return "python_manifest_invalid";
    expected.insert(name);folded.insert(caseKey);total+=qint64(size);
    const QFileInfo info(QDir(root).filePath(name));
    if(link(info) || !info.isFile() || info.canonicalFilePath()!=QDir(root).filePath(name) || info.size()!=qint64(size))
      return "python_runtime_changed";
    QFile file(info.absoluteFilePath());
    if(!file.open(QIODevice::ReadOnly) || fileHash(file,qint64(size),cancelled)!=digest)
      return cancelled && cancelled()?QStringLiteral("cancelled"):QStringLiteral("python_runtime_changed");
#ifndef Q_OS_WIN
    if(info.isExecutable()!=row.value("executable").toBool())return "python_runtime_permissions_changed";
#endif
  }
  QDirIterator it(root,QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden|QDir::System,QDirIterator::Subdirectories);
  int visited=0;
  while(it.hasNext()) {
    if(cancelled && cancelled())return "cancelled";
    const auto path=it.next();const auto info=it.fileInfo();
    if(++visited>MaxFiles*3 || link(info) || (!info.isDir() && !info.isFile()))return "python_runtime_changed";
    if(info.isFile()) {
      const auto relative=QDir(root).relativeFilePath(path);
      if(relative!="runtime-manifest.json" && !expected.remove(relative))return "python_runtime_changed";
    }
  }
  return expected.isEmpty()?QString():QStringLiteral("python_runtime_changed");
}
} // namespace
QString runtimePlatform() {
  if(QSysInfo::currentCpuArchitecture()!=QLatin1String("x86_64"))return {};
#if defined(Q_OS_WIN)
  return "windows-x86_64";
#elif defined(Q_OS_LINUX)
  return "linux-x86_64";
#else
  return {};
#endif
}
QString runtimeSourceLockSha256() { return QString::fromLatin1(PALEO_AGENT_PYTHON_SOURCE_LOCK_SHA); }

RuntimeResult verifyRuntime(const RuntimeConfig &config,const std::function<bool()> &cancelled,std::shared_ptr<void> retention) {
  RuntimeResult result;
  const auto fail=[&](const QString &code){result.code=code;return result;};
  if(!config.enabled)return fail("python_environment_disabled");
  if(config.root.isEmpty())return result;
  if(!hexDigest(config.manifestSha256))return fail("python_manifest_anchor_required");
  if(runtimePlatform().isEmpty())return fail("python_platform_unsupported");
  const QFileInfo rootInfo(config.root);const auto root=rootInfo.canonicalFilePath();
  if(root.isEmpty() || !rootInfo.isDir() || link(rootInfo))return fail("python_environment_missing");
  if(!retention) {
    const auto leasePath=QDir(root).absoluteFilePath("../.leases/"+config.manifestSha256+".lock");
    auto lease=std::make_shared<QLockFile>(leasePath);lease->setStaleLockTime(0);
    if(!QDir().mkpath(QFileInfo(leasePath).absolutePath()) || !lease->tryLock(0))return fail("python_runtime_busy");
    retention=std::move(lease);
  }
  QFile manifest(QDir(root).filePath("runtime-manifest.json"));
  if(link(QFileInfo(manifest)) || !manifest.open(QIODevice::ReadOnly) || manifest.size()>4*1024*1024)
    return fail("python_manifest_invalid");
  const auto bytes=manifest.read(4*1024*1024+1);
  if(bytes.size()>4*1024*1024 || manifest.size()!=bytes.size())return fail("python_manifest_invalid");
  if(QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())!=config.manifestSha256)
    return fail("python_manifest_changed");
  const auto parsed=QJsonDocument::fromJson(bytes);
  if(!parsed.isObject())return fail("python_manifest_invalid");
  const auto object=parsed.object();
  const QStringList keys{"files","interpreter","numpyVersion","platform","profile","pythonVersion","runtimeVersion","schema","sourceLockSha256","treeSize"};
  if(object.keys()!=keys || object.value("schema")!=QLatin1String(RuntimeSchema) || object.value("profile")!=QLatin1String(ProfileId) ||
      object.value("pythonVersion")!=QLatin1String(PythonVersion) || object.value("numpyVersion")!=QLatin1String(NumpyVersion) ||
      object.value("runtimeVersion")!="cpython-3.14.10-numpy-2.5.4-1" || object.value("platform")!=runtimePlatform() ||
      object.value("sourceLockSha256").toString()!=runtimeSourceLockSha256() || !object.value("files").isArray())
    return fail("python_manifest_invalid");
  const auto interpreter=object.value("interpreter").toString();
  if(!safeRelative(interpreter))return fail("python_manifest_invalid");
  qint64 total=0;const auto code=checkFiles(root,object.value("files").toArray(),total,cancelled);
  if(!code.isEmpty())return fail(code);
  if(object.value("treeSize").toDouble(-1)!=double(total))return fail("python_manifest_invalid");
  bool listed=false;
  for(const auto &value:object.value("files").toArray())
    if(value.toObject().value("path")==interpreter && value.toObject().value("executable").toBool())listed=true;
  const auto executable=QDir(root).filePath(interpreter);
  if(!listed || !QFileInfo(executable).isExecutable())return fail("python_interpreter_unavailable");
  auto runtime=std::make_shared<VerifiedRuntime>();runtime->root=root;runtime->interpreter=executable;
  runtime->manifestSha256=config.manifestSha256;runtime->runtimeVersion=object.value("runtimeVersion").toString();
  runtime->platform=runtimePlatform();runtime->treeSize=total;runtime->retention=std::move(retention);
  result.runtime=std::move(runtime);result.code="ok";return result;
}
} // namespace paleo::python
