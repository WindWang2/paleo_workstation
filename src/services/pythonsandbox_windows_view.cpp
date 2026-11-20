// 层：数据
#include "pythonsandbox_windows_internal.h"
#ifdef Q_OS_WIN
#include "services/pythoninputbundle.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>

#include <algorithm>

namespace paleo::python::windows_detail {
namespace {
constexpr auto ViewSchema = "paleo.python-sandbox-view.v1";
constexpr auto LeaseSchema = "paleo.python-sandbox-view-lease.v1";
// 视图缓存上限：同 generation 复用，最多保留 2 代无租约视图（按 marker
// 时间戳逐出最旧）。单视图字节受 runtime manifest treeSize（≤1 GiB）约束。
constexpr int MaxCachedViews = 2;
// 复制/写入前的最小余量：视图 treeSize + 私有输入之外还要留的空闲盘。
constexpr qint64 MinFreeDiskBytes = 64LL * 1024 * 1024;

view_test::FailureHook g_failCopy, g_failAcl, g_failLease, g_failRelease;

QString hexDigestOrEmpty(const QString &value) {
  if(value.size()!=64)return {};
  for(const auto c:value)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return {};
  return value;
}
bool writeRecord(const QString &path,const QJsonObject &record) {
  QSaveFile file(path);
  const auto bytes=QJsonDocument(record).toJson(QJsonDocument::Compact);
  return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size()&&file.commit();
}
// 严格解析：固定键集 + schema；记录损坏一律返回 false（调用方 fail-closed，
// 不盲删也不采纳记录内路径）。
bool parseRecord(const QString &path,const QStringList &keys,const QString &schema,QJsonObject &record) {
  const QFileInfo info(path);
  if(info.isSymLink()||info.isJunction()||!info.isFile()||info.size()>4096)return false;
  QFile file(path);if(!file.open(QIODevice::ReadOnly))return false;
  QJsonParseError parse;const auto doc=QJsonDocument::fromJson(file.readAll(),&parse);file.close();
  if(parse.error!=QJsonParseError::NoError||!doc.isObject())return false;
  record=doc.object();
  return record.keys()==keys&&record.value("schema").toString()==schema;
}
qint64 freeBytes(const QString &root) {
  ULARGE_INTEGER available{};
  return GetDiskFreeSpaceExW((PCWSTR)native(root).c_str(),&available,nullptr,nullptr)?qint64(available.QuadPart):-1;
}
QString relativeUnder(const QString &root,const QString &path) {
  if(!path.startsWith(root+'/',Qt::CaseInsensitive))return {};
  const auto value=path.mid(root.size()+1);
  return value.isEmpty()||value.contains("..")||value.contains('\\')||value.contains(QChar(0))?QString():value;
}
struct Layout {
  QString control,views,leases,inputs;
  bool create(QString *error) {
    control=journalRoot(error);
    if(control.isEmpty())return false;
    views=QDir(control).filePath("runtime-views");
    leases=QDir(control).filePath("view-leases");
    inputs=QDir(control).filePath("inputs");
    for(const auto &dir:{views,leases,inputs})
      if(!QDir().mkpath(dir)){*error="python_view_layout_create";return false;}
    return true;
  }
};
QString hostIdentity() {return QString::number(GetCurrentProcessId())+'-'+QString::number(processCreation(GetCurrentProcess()));}
QString leaseFileName(const QString &manifestSha) {return manifestSha+'.'+hostIdentity()+".json";}
// owner 判活与 profile journal 同口径（PID + 进程创建时间），杜绝 PID 复用
// 误判。known=false 表示无法判定，调用方保留并诊断。
bool ownerRecordAlive(const QJsonObject &record,bool &known) {
  bool numeric=false;const auto started=record.value("hostCreated100ns").toString().toULongLong(&numeric);
  const auto pid=record.value("hostPid").toInteger(-1);
  if(!numeric||!started||pid<1||pid>0xffffffffLL){known=false;return false;}
  return !ownerStopped(DWORD(pid),started,known);
}
bool removeOwnLeases(const Layout &layout,QString *error) {
  const auto suffix='.'+hostIdentity()+".json";
  for(const auto &name:QDir(layout.leases).entryList({"*.json"},QDir::Files|QDir::Hidden|QDir::System)) {
    if(!name.endsWith(suffix))continue;
    if(!QFile::remove(QDir(layout.leases).filePath(name))){
      if(error)*error="python_view_lease_remove";
      return false;
    }
  }
  return true;
}
// 经锁定句柄读 manifest（树已独占打开，普通 QFile 无法再开）。
bool manifestFromLockedTree(const LockedTree &runtime,QJsonObject &manifest,QString *error) {
  const QString wanted=QDir(runtime.root).filePath("runtime-manifest.json");
  for(int i=0;i<runtime.paths.size();++i) {
    if(runtime.paths.at(i).compare(wanted,Qt::CaseInsensitive)!=0)continue;
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(runtime.handles.at(size_t(i)).get(),&size)||size.QuadPart<=0||size.QuadPart>4*1024*1024){
      *error="python_view_manifest_size";return false;}
    QByteArray bytes(int(size.QuadPart),Qt::Uninitialized);DWORD read=0;
    if(!ReadFile(runtime.handles.at(size_t(i)).get(),bytes.data(),DWORD(bytes.size()),&read,nullptr)||
        qint64(read)!=size.QuadPart){*error="python_view_manifest_read";return false;}
    QJsonParseError parse;const auto doc=QJsonDocument::fromJson(bytes,&parse);
    if(parse.error!=QJsonParseError::NoError||!doc.isObject()){*error="python_view_manifest_invalid";return false;}
    manifest=doc.object();return true;
  }
  *error="python_view_manifest_missing";return false;
}
// 流式复制 + 摘要合并：一次读同时产出副本与校验（不二次整读来源）。
bool copyFileAuthenticated(HANDLE source,const QString &destination,qint64 expectedSize,const QString &expectedSha,
                           const SandboxRequest &request,qint64 *hashedBytes,QString *error) {
  if(expectedSize<0||expectedSha!=hexDigestOrEmpty(expectedSha)){*error="python_view_manifest_invalid";return false;}
  QFile target(destination);
  if(!target.open(QIODevice::WriteOnly|QIODevice::Truncate)){*error="python_view_copy_open";return false;}
  QCryptographicHash hash(QCryptographicHash::Sha256);
  char buffer[64*1024];qint64 remaining=expectedSize;
  while(remaining>0) {
    if(cancelled(request)){*error="cancelled";return false;}
    DWORD got=0;const DWORD want=DWORD(qMin<qint64>(qint64(sizeof(buffer)),remaining));
    if(!ReadFile(source,buffer,want,&got,nullptr)||!got){*error="python_view_read";return false;}
    hash.addData(buffer,got);
    if(target.write(buffer,got)!=qint64(got)){*error="python_view_write";return false;}
    remaining-=got;*hashedBytes+=got;
  }
  target.close();
  if(target.error()!=QFileDevice::NoError||target.size()!=expectedSize){*error="python_view_copy_size";return false;}
  if(QString::fromLatin1(hash.result().toHex())!=expectedSha){*error="python_runtime_changed";return false;}
  return true;
}
// 复用视图的完整再验证：逐文件按 manifest 重新哈希（同尺寸篡改可检出），
// 重解析点/硬链接/多余/缺失文件一律拒绝。
bool verifyViewTree(const QString &viewRoot,const QJsonObject &manifest,const QString &anchorSha,
                    const SandboxRequest &request,int *fileCount,qint64 *hashedBytes,QString *error) {
  QHash<QString,QJsonObject> rows;
  for(const auto &value:manifest.value("files").toArray())rows.insert(value.toObject().value("path").toString(),value.toObject());
  if(rows.isEmpty()){*error="python_view_manifest_invalid";return false;}
  QSet<QString> expected(rows.keyBegin(),rows.keyEnd());
  QDirIterator it(viewRoot,QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden|QDir::System,QDirIterator::Subdirectories);
  while(it.hasNext()) {
    const auto path=it.next();
    if(cancelled(request)){*error="cancelled";return false;}
    const QFileInfo info(path);
    if(info.isSymLink()||info.isJunction()){*error="python_view_reparse_or_device";return false;}
    if(info.isDir())continue;
    const auto name=relativeUnder(viewRoot,path);
    if(name.isEmpty()){*error="python_view_escape";return false;}
    if(name.compare("view.json",Qt::CaseInsensitive)==0)continue; // 主机侧 ready marker
    if(name.compare("runtime-manifest.json",Qt::CaseInsensitive)==0) {
      QFile anchor(path);
      if(!anchor.open(QIODevice::ReadOnly)){*error="python_view_read";return false;}
      QCryptographicHash hash(QCryptographicHash::Sha256);
      while(!anchor.atEnd()) {
        if(cancelled(request)){*error="cancelled";return false;}
        const auto bytes=anchor.read(64*1024);
        if(bytes.isEmpty()){*error="python_view_read";return false;}
        hash.addData(bytes);*hashedBytes+=bytes.size();
      }
      anchor.close();
      if(QString::fromLatin1(hash.result().toHex())!=anchorSha){*error="python_runtime_changed";return false;}
      continue;
    }
    if(!expected.remove(name)){*error="python_view_extra_file";return false;}
    const auto row=rows.value(name);const auto size=row.value("size").toInteger(-1);
    if(size<0){*error="python_view_manifest_invalid";return false;}
    const auto wide=native(path);
    Handle handle(CreateFileW(wide.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    BY_HANDLE_FILE_INFORMATION info2{};
    if(!handle.valid()||!GetFileInformationByHandle(handle.get(),&info2)||
        info2.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DEVICE)){*error="python_view_reparse_or_device";return false;}
    const qint64 actual=(quint64(info2.nFileSizeHigh)<<32)|info2.nFileSizeLow;
    if(actual!=size||info2.nNumberOfLinks!=1){*error="python_runtime_changed";return false;}
    QCryptographicHash hash(QCryptographicHash::Sha256);
    char buffer[64*1024];qint64 remaining=size;
    while(remaining>0) {
      if(cancelled(request)){*error="cancelled";return false;}
      DWORD got=0;const DWORD want=DWORD(qMin<qint64>(qint64(sizeof(buffer)),remaining));
      if(!ReadFile(handle.get(),buffer,want,&got,nullptr)||!got){*error="python_view_read";return false;}
      hash.addData(buffer,got);*hashedBytes+=got;remaining-=got;
    }
    if(QString::fromLatin1(hash.result().toHex())!=row.value("sha256").toString()){*error="python_runtime_changed";return false;}
    ++*fileCount;
  }
  if(!expected.isEmpty()){*error="python_view_missing_file";return false;}
  return true;
}
} // namespace

void view_test::setFailuresForTest(FailureHook copy,FailureHook acl,FailureHook lease,FailureHook release) {
  g_failCopy=std::move(copy);g_failAcl=std::move(acl);g_failLease=std::move(lease);g_failRelease=std::move(release);
}

// 视图构建：staging 复制（根先设 capability 只读可继承 ACL，子项随建随承，
// 不再逐节点改写安全描述符）→ ready marker → 原子改名。最终目录名出现即
// 代表完整且逐文件摘要已验证。并发同名视图已就位时改走复用。
bool buildView(const SandboxRequest &request,const LockedTree &runtime,const Profile &profile,
               const QJsonObject &manifest,const QString &anchorSha,const Layout &layout,
               const QString &viewRoot,PrivateView &view,QString *error) {
  const QString staging=viewRoot+'.'+profile.moniker+".staging";
  if(QFileInfo::exists(staging)&&!QDir(staging).removeRecursively()){*error="python_view_staging_busy";return false;}
  if(!QDir().mkpath(staging)){*error="python_view_staging_create";return false;}
  const auto cleanup=qScopeGuard([&]{QDir(staging).removeRecursively();});
  if(g_failAcl&&g_failAcl()){*error="python_view_acl_forced";return false;}
  // 仅对自有副本设 ACL（#959）：根节点 PROTECTED + capability 只读可继承。
  if(!protectedFileAcl(staging,profile.runtimeCapability,error))return false;
  const auto started=processCreation(GetCurrentProcess());
  if(!started||!writeRecord(QDir(staging).filePath("staging.json"),
      QJsonObject{{"schema",ViewSchema},{"state","staging"},{"manifestSha256",anchorSha},
                  {"hostPid",qint64(GetCurrentProcessId())},{"hostCreated100ns",QString::number(started)}})) {
    *error="python_view_staging_record";return false;}
  // 来源→副本索引：LockedTree 的 paths/handles 按序对位。
  QHash<QString,HANDLE> sources;
  for(int i=0;i<runtime.paths.size();++i)
    sources.insert(relativeUnder(runtime.root,runtime.paths.at(i)),runtime.handles.at(size_t(i)).get());
  qint64 hashed=0;int files=0;
  for(const auto &value:manifest.value("files").toArray()) {
    if(cancelled(request)){*error="cancelled";return false;}
    if(g_failCopy&&g_failCopy()){*error="python_view_copy_forced";return false;}
    const auto row=value.toObject();const auto name=row.value("path").toString();
    const auto size=row.value("size").toInteger(-1);
    HANDLE source=sources.value(name);
    if(name.isEmpty()||!source){*error="python_view_manifest_invalid";return false;}
    const QString destination=QDir(staging).filePath(name);
    if(!QDir(QFileInfo(destination).absolutePath()).mkpath(".")){*error="python_view_dir_create";return false;}
    if(!copyFileAuthenticated(source,destination,size,row.value("sha256").toString(),request,&hashed,error))return false;
    ++files;
  }
  const qint64 treeSize=manifest.value("treeSize").toInteger(-1);
  if(treeSize<0||treeSize!=hashed){*error="python_view_manifest_invalid";return false;}
  // runtime-manifest.json 本体也入副本：其哈希即锚 SHA，复制时一并核验。
  if(!sources.contains("runtime-manifest.json")){*error="python_view_manifest_missing";return false;}
  {
    QFile destination(QDir(staging).filePath("runtime-manifest.json"));
    HANDLE source=sources.value("runtime-manifest.json");
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(source,&size)||size.QuadPart<=0||size.QuadPart>4*1024*1024){*error="python_view_manifest_size";return false;}
    if(!destination.open(QIODevice::WriteOnly|QIODevice::Truncate)){*error="python_view_copy_open";return false;}
    QCryptographicHash hash(QCryptographicHash::Sha256);
    char buffer[64*1024];qint64 remaining=size.QuadPart;
    while(remaining>0) {
      if(cancelled(request)){*error="cancelled";return false;}
      DWORD got=0;const DWORD want=DWORD(qMin<qint64>(qint64(sizeof(buffer)),remaining));
      if(!ReadFile(source,buffer,want,&got,nullptr)||!got){*error="python_view_read";return false;}
      hash.addData(buffer,got);
      if(destination.write(buffer,got)!=qint64(got)){*error="python_view_write";return false;}
      remaining-=got;hashed+=got;
    }
    destination.close();
    if(destination.error()!=QFileDevice::NoError||QString::fromLatin1(hash.result().toHex())!=anchorSha){
      *error="python_runtime_changed";return false;}
  }
  if(!writeRecord(QDir(staging).filePath("view.json"),
      QJsonObject{{"schema",ViewSchema},{"state","ready"},{"manifestSha256",anchorSha},
                  {"files",files},{"treeSize",treeSize},
                  {"hostPid",qint64(GetCurrentProcessId())},{"hostCreated100ns",QString::number(started)}})) {
    *error="python_view_record";return false;}
  if(!QDir(staging).rename(staging,viewRoot)) {
    // 并发同名视图已就位（另一宿主先完成）：删除自有 staging，改走复用。
    if(!QFileInfo::exists(viewRoot)){*error="python_view_rename";return false;}
  } else {
    QFile::remove(QDir(viewRoot).filePath("staging.json")); // 复用验证只认 view.json
  }
  view.viewBytes=treeSize;view.viewFiles=files;
  return true;
}

bool takeViewLease(const Layout &layout,const QString &manifestSha,QString *error) {
  if(g_failLease&&g_failLease()){*error="python_view_lease_forced";return false;}
  const QString started=QString::number(processCreation(GetCurrentProcess()));
  if(started=="0"){*error="python_view_owner_identity";return false;}
  // 记录相对视图路径（诊断用）；清理一律按 manifestSha 在根内重建路径，
  // 绝不采纳记录文件里的任意绝对路径。
  const QJsonObject record{{"schema",LeaseSchema},{"manifestSha256",manifestSha},
      {"viewPath","runtime-views/"+manifestSha},{"hostPid",qint64(GetCurrentProcessId())},{"hostCreated100ns",started}};
  if(!writeRecord(QDir(layout.leases).filePath(leaseFileName(manifestSha)),record)){*error="python_view_lease_write";return false;}
  return true;
}

bool releasePrivateView(const PrivateView &view,QString *error) {
  if(g_failRelease&&g_failRelease()){*error="python_view_release_forced";return false;}
  Layout layout;
  if(!layout.create(error))return false;
  QLockFile lock(QDir(layout.control).filePath(".lock"));lock.setStaleLockTime(30000);
  if(!lock.tryLock(5000)){*error="python_view_journal_busy";return false;}
  // 私有输入按 run 清理；失败保留目录与租约记录，由恢复路径按 owner 判活
  // 重试——不删记录掩盖残留（runWindowsSandbox 映射为清理待恢复状态）。
  if(!view.inputRoot.isEmpty()&&QFileInfo::exists(view.inputRoot)&&!QDir(view.inputRoot).removeRecursively()) {
    *error="python_view_input_remove";return false;}
  return removeOwnLeases(layout,error);
}

// 恢复逻辑主体：调用方必须已持有 control .lock（acquire 路径）。
bool recoverViewResourcesLocked(const Layout &layout,QString *error) {
  // 1) 租约记录：owner 判活与 profile journal 同口径。解析失败/归属未知 →
  // 保留并按 fail-closed 报告（不盲删、不信任记录内路径）。
  const auto leases=QDir(layout.leases).entryList({"*.json"},QDir::Files|QDir::Hidden|QDir::System,QDir::Name);
  if(leases.size()>512){*error="python_view_lease_budget";return false;}
  QSet<QString> blocked; // 活租约或归属未知的 generation：逐出阶段视为占用
  for(const auto &name:leases) {
    const auto path=QDir(layout.leases).filePath(name);
    QJsonObject record;
    if(!parseRecord(path,QStringList{"hostCreated100ns","hostPid","manifestSha256","schema","viewPath"},LeaseSchema,record)) {
      const auto sha=hexDigestOrEmpty(QFileInfo(name).completeBaseName().section('.',0,0));
      if(sha.isEmpty()){*error="python_view_lease_invalid";return false;}
      blocked.insert(sha); // 文件名身份合法但内容损坏：保守视为占用
      continue;
    }
    const auto sha=hexDigestOrEmpty(record.value("manifestSha256").toString());
    if(sha.isEmpty()||record.value("viewPath").toString()!="runtime-views/"+sha){*error="python_view_lease_invalid";return false;}
    bool known=false;const bool alive=ownerRecordAlive(record,known);
    if(!known){blocked.insert(sha);*error="python_view_lease_owner_unknown";return false;}
    if(alive){blocked.insert(sha);continue;}
    if(!QFile::remove(path)){*error="python_view_lease_remove";return false;}
  }
  // 2) 残留 staging：marker 记 owner；死宿主 staging 整树回收，无 marker
  // 或归属不明的保留并诊断（不凭目录名臆断可删）。
  const auto entries=QDir(layout.views).entryInfoList(QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden|QDir::System,QDir::Name);
  for(const auto &entry:entries) {
    if(!entry.fileName().endsWith(".staging"))continue;
    const auto staging=entry.absoluteFilePath();
    QJsonObject record;
    if(!parseRecord(QDir(staging).filePath("staging.json"),QStringList{"hostCreated100ns","hostPid","manifestSha256","schema","state"},ViewSchema,record)) {
      qWarning("pythonsandbox view: unattributed staging left in place: %s",qPrintable(staging));
      continue;
    }
    bool known=false;const bool alive=ownerRecordAlive(record,known);
    if(!known){*error="python_view_staging_owner_unknown";return false;}
    if(!alive&&!QDir(staging).removeRecursively()){*error="python_view_staging_remove";return false;}
  }
  // 3) 残留私有输入（宿主死于复制/授权/运行期）：input marker 判 owner。
  const auto inputs=QDir(layout.inputs).entryInfoList(QDir::Dirs|QDir::NoDotAndDotDot,QDir::Name);
  for(const auto &entry:inputs) {
    const auto root=entry.absoluteFilePath();
    QJsonObject record;
    if(!validMoniker(entry.fileName())){qWarning("pythonsandbox view: unattributed input left in place: %s",qPrintable(root));continue;}
    if(!parseRecord(QDir(root).filePath("input.json"),QStringList{"hostCreated100ns","hostPid","schema","state"},ViewSchema,record))continue;
    bool known=false;const bool alive=ownerRecordAlive(record,known);
    if(!known){*error="python_view_input_owner_unknown";return false;}
    if(!alive&&!QDir(root).removeRecursively()){*error="python_view_input_remove";return false;}
  }
  // 4) 视图缓存逐出：超出上限的无租约视图按 marker 修改时间回收；占用中
  // 的（活租约/归属未知）不动——一任务清理不碰另一运行中的资源。
  struct Candidate {QString path;QDateTime modified;};
  QList<Candidate> idle;
  for(const auto &entry:QDir(layout.views).entryInfoList(QDir::Dirs|QDir::NoDotAndDotDot|QDir::Hidden,QDir::Name)) {
    if(entry.fileName().endsWith(".staging"))continue;
    const auto sha=hexDigestOrEmpty(entry.fileName());
    QJsonObject record;
    if(sha.isEmpty()||blocked.contains(sha)||!parseRecord(QDir(entry.absoluteFilePath()).filePath("view.json"),
        QStringList{"files","hostCreated100ns","hostPid","manifestSha256","schema","state","treeSize"},ViewSchema,record))continue;
    idle.append({entry.absoluteFilePath(),QFileInfo(QDir(entry.absoluteFilePath()).filePath("view.json")).lastModified()});
  }
  if(idle.size()>MaxCachedViews) {
    std::sort(idle.begin(),idle.end(),[](const Candidate &a,const Candidate &b){return a.modified<b.modified;});
    for(int i=0;i<idle.size()-MaxCachedViews;++i)
      if(!QDir(idle.at(i).path).removeRecursively()){*error="python_view_evict";return false;}
  }
  return true;
}

bool recoverViewResources(QString *error) {
  Layout layout;
  if(!layout.create(error))return false;
  QLockFile lock(QDir(layout.control).filePath(".lock"));lock.setStaleLockTime(30000);
  if(!lock.tryLock(5000)){*error="python_view_journal_busy";return false;}
  return recoverViewResourcesLocked(layout,error);
}

bool acquirePrivateView(const SandboxRequest &request,const LockedTree &runtime,const LockedTree &input,
                        const AuthenticatedSnapshot &snapshot,const Profile &profile,PrivateView &view,QString *error) {
  Layout layout;
  if(!layout.create(error))return false;
  QLockFile lock(QDir(layout.control).filePath(".lock"));lock.setStaleLockTime(30000);
  if(!lock.tryLock(5000)){*error="python_view_journal_busy";return false;}
  if(!recoverViewResourcesLocked(layout,error))return false;
  QJsonObject manifest;
  if(!manifestFromLockedTree(runtime,manifest,error))return false;
  const QString anchorSha=hexDigestOrEmpty(request.runtime->manifestSha256);
  const qint64 treeSize=manifest.value("treeSize").toInteger(-1);
  if(anchorSha.isEmpty()||treeSize<0||treeSize>1024LL*1024*1024){*error="python_view_manifest_invalid";return false;}
  const qint64 available=freeBytes(layout.control);
  if(available>=0&&available<treeSize+request.policy.inputBytes+MinFreeDiskBytes){*error="python_view_disk_space";return false;}
  const QString viewRoot=QDir(layout.views).filePath(anchorSha);
  if(QFileInfo::exists(viewRoot)) {
    // 复用既有视图：按已验证内容身份索引——完整重哈希防同尺寸篡改；
    // 校验失败拒绝执行（不静默重建掩盖差异）。
    qint64 hashed=0;int files=0;
    if(!verifyViewTree(viewRoot,manifest,anchorSha,request,&files,&hashed,error))return false;
    view.reusedView=true;view.viewBytes=treeSize;view.viewFiles=files;
  } else {
    if(!buildView(request,runtime,profile,manifest,anchorSha,layout,viewRoot,view,error))return false;
    view.reusedView=false;
  }
  if(!takeViewLease(layout,anchorSha,error))return false;
  // 私有输入：profile 隔离目录 + 包 SID 只读（继承），TEMP/TMP/LOCALAPPDATA
  // 都指向这里；写盘按既有 LPAC 合同仍被拒绝。code.py 从锁定句柄流式复制
  // 并按 manifest 摘要校验；request.json/data.bin 复用已认证内存字节。
  const QString inputRoot=QDir(layout.inputs).filePath(profile.moniker);
  if(QFileInfo::exists(inputRoot)&&!QDir(inputRoot).removeRecursively()){*error="python_view_input_busy";return false;}
  bool inputReady=false;
  // 输入构造失败：清本次输入并归还租约（removeOwnLeases 免锁——本函数
  // 已持有 control .lock，releasePrivateView 会再锁造成自锁）。
  const auto fail=qScopeGuard([&]{
    if(inputReady)return;
    QDir(inputRoot).removeRecursively();
    view.inputRoot.clear();
    removeOwnLeases(layout,nullptr);
  });
  if(!QDir().mkpath(inputRoot)){*error="python_view_input_create";return false;}
  if(g_failAcl&&g_failAcl()){*error="python_view_acl_forced";return false;}
  if(!protectedFileAcl(inputRoot,profile.sid,error))return false;
  const auto started=QString::number(processCreation(GetCurrentProcess()));
  if(started=="0"||!writeRecord(QDir(inputRoot).filePath("input.json"),
      QJsonObject{{"schema",ViewSchema},{"state","input"},{"hostPid",qint64(GetCurrentProcessId())},
                  {"hostCreated100ns",started}})){*error="python_view_input_record";return false;}
  const QString codeName="code.py";
  HANDLE codeSource=nullptr;
  for(int i=0;i<input.paths.size();++i)
    if(relativeUnder(input.root,input.paths.at(i))==codeName){codeSource=input.handles.at(size_t(i)).get();break;}
  if(!codeSource){*error="python_view_manifest_missing";return false;}
  QJsonObject codeRow;
  for(const auto &value:input.manifest.value("files").toArray())
    if(value.toObject().value("path").toString()==codeName){codeRow=value.toObject();break;}
  qint64 hashed=0;
  if(!copyFileAuthenticated(codeSource,QDir(inputRoot).filePath(codeName),codeRow.value("size").toInteger(-1),
                            codeRow.value("sha256").toString(),request,&hashed,error))
    return false;
  if(snapshot.inputChunks.size()==3) {
    QFile data(QDir(inputRoot).filePath("data.bin"));
    if(!data.open(QIODevice::WriteOnly|QIODevice::Truncate)||data.write(snapshot.inputChunks.at(2))!=snapshot.inputChunks.at(2).size()||
        data.error()!=QFileDevice::NoError){*error="python_view_input_write";return false;}
    data.close();
  }
  if(!snapshot.inputChunks.isEmpty()) {
    QFile jsonFile(QDir(inputRoot).filePath("request.json"));
    const QByteArray &payload=snapshot.inputChunks.size()==3?snapshot.inputChunks.at(1):snapshot.inputChunks.at(0);
    if(!jsonFile.open(QIODevice::WriteOnly|QIODevice::Truncate)||jsonFile.write(payload)!=payload.size()||
        jsonFile.error()!=QFileDevice::NoError){*error="python_view_input_write";return false;}
    jsonFile.close();
  }
  inputReady=true;
  view.runtimeRoot=viewRoot;view.inputRoot=inputRoot;
  return true;
}
} // namespace paleo::python::windows_detail
#else
// 非 Windows：内部声明仅在 Q_OS_WIN 下存在，无需桩（Linux 强隔离走
// pythonsandbox_linux.*）。空翻译单元保留挂点，产物不含符号。
#endif
