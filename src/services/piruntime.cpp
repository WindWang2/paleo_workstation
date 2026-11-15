// 层：数据
#include "piruntime.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QMutex>
#include <QScopeGuard>
#include <QSet>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>
#include <cmath>

namespace paleo::pi {
namespace {
constexpr qint64 MaxTreeBytes=256LL*1024*1024;
bool digest(const QString &value) {
  if(value.size()!=64)return false;
  for(auto c:value)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
  return value!=QString(64,'0');
}
bool linked(const QFileInfo &info) {
#ifdef Q_OS_WIN
  return info.isSymLink()||info.isJunction();
#else
  return info.isSymLink();
#endif
}
QString platform() {
  if(QSysInfo::currentCpuArchitecture()!=QLatin1String("x86_64"))return {};
#if defined(Q_OS_WIN)
  return "windows-x86_64";
#elif defined(Q_OS_LINUX)
  return "linux-x86_64";
#else
  return {};
#endif
}
bool relative(const QString &value) {
  if(value.isEmpty()||value.size()>512||value.contains('\\')||value.contains(':')||value.contains(QChar(0))||
      QDir::isAbsolutePath(value)||QDir::cleanPath(value)!=value)return false;
  for(const auto &part:value.split('/')) {
    if(part.isEmpty()||part=="."||part==".."||part.endsWith('.')||part.endsWith(' '))return false;
    for(auto c:part)if(c.unicode()<32||QStringLiteral("<>\"|?*").contains(c))return false;
    const auto base=part.section('.',0,0).toUpper();
    if(QSet<QString>{"CON","PRN","AUX","NUL"}.contains(base)||
        (base.size()==4&&(base.startsWith("COM")||base.startsWith("LPT"))&&base[3]>='1'&&base[3]<='9'))return false;
  }
  return true;
}
bool keys(const QJsonObject &value,QStringList expected) {
  auto actual=value.keys();actual.sort();expected.sort();return actual==expected;
}
QByteArray hashFile(QFile &file,qint64 expected,const std::function<bool()> &stopped) {
  QCryptographicHash hash(QCryptographicHash::Sha256);
  while(expected>0) {
    if(stopped())return {};
    const auto bytes=file.read(qMin<qint64>(64*1024,expected));if(bytes.isEmpty())return {};
    hash.addData(bytes);expected-=bytes.size();
  }
  if(!file.read(1).isEmpty()||file.error()!=QFileDevice::NoError)return {};
  return hash.result().toHex();
}
std::shared_ptr<void> lease(const QString &root,const QString &anchor) {
  struct Pool {QMutex mutex;QHash<QString,std::weak_ptr<void>> entries;};static Pool pool;
#ifdef Q_OS_WIN
  const auto key=root.toCaseFolded()+QChar(0)+anchor;
#else
  const auto key=root+QChar(0)+anchor;
#endif
  QMutexLocker locker(&pool.mutex);
  for(auto it=pool.entries.begin();it!=pool.entries.end();)if(it.value().expired())it=pool.entries.erase(it);else ++it;
  if(auto current=pool.entries.value(key).lock())return current;
  const auto directory=QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)+"/paleo/pi-runtime-leases";
  if(!QDir().mkpath(directory))return {};
  const auto token=QCryptographicHash::hash(key.toUtf8(),QCryptographicHash::Sha256).toHex();
  auto current=std::make_shared<QLockFile>(directory+'/'+QString::fromLatin1(token)+".lock");current->setStaleLockTime(0);
  if(!current->tryLock(0))return {};pool.entries.insert(key,current);return current;
}
}
RuntimeInspection inspectRuntime(const RuntimeConfig &config,const std::function<bool()> &cancelled) {
  RuntimeInspection out;out.status={{"enabled",config.enabled},{"integrityVerified",false},{"executionReady",false},
    {"isSandbox",false},{"osMemoryLimitEnforced",false},{"osCpuLimitEnforced",false},{"modelTransportReady",false}};
  bool interrupted=false;const auto stopped=[&]{if(cancelled&&cancelled())interrupted=true;return interrupted;};
  const auto fail=[&](const char *code){out.code=stopped()?QStringLiteral("cancelled"):QLatin1String(code);out.status.insert("code",out.code);return out;};
  if(!config.enabled)return fail("pi_disabled");
  if(QCoreApplication::instance()&&QThread::currentThread()==QCoreApplication::instance()->thread())return fail("pi_worker_required");
  if(!digest(config.manifestSha256))return fail("pi_manifest_anchor_required");
  if(platform().isEmpty())return fail("pi_platform_unsupported");
  const QFileInfo rootInfo(config.root);const auto root=rootInfo.canonicalFilePath();
  if(config.root.isEmpty()||root.isEmpty()||!rootInfo.isDir()||linked(rootInfo))return fail("pi_runtime_missing");
  auto retained=lease(root,config.manifestSha256);if(!retained)return fail("pi_runtime_busy");
  QFile file(root+"/pi-manifest.json");const QFileInfo manifestInfo(file);
  if(!manifestInfo.isFile()||linked(manifestInfo)||manifestInfo.size()<1||manifestInfo.size()>1024*1024||
      !file.open(QIODevice::ReadOnly))return fail("pi_manifest_invalid");
  const auto raw=file.read(1024*1024+1);file.close();
  if(raw.size()>1024*1024||QCryptographicHash::hash(raw,QCryptographicHash::Sha256).toHex()!=config.manifestSha256.toLatin1())return fail("pi_manifest_hash_mismatch");
  QJsonParseError parse;const auto document=QJsonDocument::fromJson(raw,&parse);
  if(parse.error!=QJsonParseError::NoError||!document.isObject())return fail("pi_manifest_invalid");
  const auto manifest=document.object();
  if(!keys(manifest,{"schema","sourceLockSha256","runtimeVersion","upstreamCommit","coreVersion","nodeVersion","platform","node","entry","treeSize","files"})||
      manifest.value("schema")!="paleo.pi-runtime.v1"||manifest.value("sourceLockSha256")!=QLatin1String(PALEO_AGENT_PI_SOURCE_LOCK_SHA)||
      manifest.value("upstreamCommit")!=QLatin1String(PALEO_AGENT_PI_UPSTREAM)||manifest.value("coreVersion")!=QLatin1String(PALEO_AGENT_PI_CORE_VERSION)||
      manifest.value("nodeVersion")!=QLatin1String(PALEO_AGENT_PI_NODE_VERSION)||manifest.value("platform")!=platform()||
      manifest.value("runtimeVersion")!=QLatin1String(PALEO_AGENT_PI_RUNTIME_VERSION)||
      manifest.value("entry")!="agent/sidecar.mjs"||!manifest.value("files").isArray())return fail("pi_manifest_incompatible");
  const QString node=platform()=="windows-x86_64"?"bin/node.exe":"bin/node";
  if(manifest.value("node")!=node||!manifest.value("runtimeVersion").isString()||manifest.value("runtimeVersion").toString().size()>128)return fail("pi_manifest_invalid");
  const auto rows=manifest.value("files").toArray();if(rows.isEmpty()||rows.size()>512)return fail("pi_manifest_budget");
  QSet<QString> expected;bool hasNode=false,hasEntry=false;qint64 tree=0;
  for(const auto &item:rows) {
    if(stopped())return fail("cancelled");
    if(!item.isObject())return fail("pi_manifest_invalid");const auto row=item.toObject();
    const auto path=row.value("path").toString(),sha=row.value("sha256").toString();const auto size=row.value("size").toDouble(-1);
    const auto folded=path.toCaseFolded();
    if(!keys(row,{"path","size","sha256","executable"})||!relative(path)||path=="pi-manifest.json"||expected.contains(folded)||
        !digest(sha)||!std::isfinite(size)||size<1||size>MaxTreeBytes||std::floor(size)!=size||!row.value("executable").isBool())return fail("pi_manifest_invalid");
    tree+=qint64(size);if(tree>MaxTreeBytes)return fail("pi_manifest_budget");expected.insert(folded);
    auto parent=QFileInfo(root+'/'+path).absolutePath();while(parent!=root) {
      const QFileInfo info(parent);if(!info.isDir()||linked(info))return fail("pi_runtime_link");
      const auto next=info.absolutePath();if(next==parent)return fail("pi_runtime_link");parent=next;
    }
    const QFileInfo info(root+'/'+path);if(!info.isFile()||linked(info)||info.size()!=qint64(size))return fail("pi_runtime_file_mismatch");
    QFile payload(info.absoluteFilePath());if(!payload.open(QIODevice::ReadOnly)||hashFile(payload,qint64(size),stopped)!=sha.toLatin1())return fail("pi_runtime_file_mismatch");
    if(path==node){hasNode=true;if(!row.value("executable").toBool()||!info.isExecutable()||sha!=QLatin1String(PALEO_AGENT_PI_NODE_SHA))return fail("pi_manifest_incompatible");}
    if(path=="agent/sidecar.mjs"){hasEntry=true;if(sha!=QLatin1String(PALEO_AGENT_PI_BUNDLE_SHA))return fail("pi_manifest_incompatible");}
  }
  if(!hasNode||!hasEntry||!manifest.value("treeSize").isDouble()||manifest.value("treeSize").toDouble()!=double(tree))return fail("pi_manifest_invalid");
  QDirIterator actual(root,QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden|QDir::System,QDirIterator::Subdirectories);
  int count=0;
  while(actual.hasNext()) {
    if(stopped())return fail("cancelled");actual.next();if(++count>2048)return fail("pi_manifest_budget");const auto info=actual.fileInfo();
    if(linked(info))return fail("pi_runtime_link");if(info.isDir())continue;
    const auto name=QDir(root).relativeFilePath(info.absoluteFilePath());
    if(name!="pi-manifest.json"&&(!info.isFile()||!expected.remove(name.toCaseFolded())))return fail("pi_runtime_extra_file");
  }
  if(!expected.isEmpty()||stopped())return fail("pi_runtime_file_mismatch");
  auto verified=std::shared_ptr<VerifiedRuntime>(new VerifiedRuntime);verified->m_root=root;verified->m_manifestSha256=config.manifestSha256;
  verified->m_node=root+'/'+node;verified->m_entry=root+"/agent/sidecar.mjs";verified->m_nodeVersion=manifest.value("nodeVersion").toString();
  verified->m_platform=platform();verified->m_upstream=manifest.value("upstreamCommit").toString();verified->m_coreVersion=manifest.value("coreVersion").toString();
  verified->m_retention=std::move(retained);out.runtime=std::move(verified);out.code="ok";out.status.insert("code",out.code);
  out.status.insert("integrityVerified",true);out.status.insert("nodeVersion",out.runtime->nodeVersion());return out;
}
}
