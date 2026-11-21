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
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
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
// 与视图内部记号（marker）保留冲突的 manifest 条目名——存在即拒绝构建
//（fail-closed：不静默改写记号或漏验证该文件）。
const QStringList kReservedNames{"view.json","staging.json"};

view_test::FailureHook g_failCopy, g_failAcl, g_failLease, g_failRelease;
std::function<void()> g_verifyHook;
QMutex g_activeMutex;
QSet<QString> g_activeLeases;

void setLeaseActive(const QString &moniker,bool active) {
  QMutexLocker lock(&g_activeMutex);
  if(active)g_activeLeases.insert(moniker);else g_activeLeases.remove(moniker);
}
bool leaseIsActive(const QString &moniker) {
  QMutexLocker lock(&g_activeMutex);
  return g_activeLeases.contains(moniker);
}

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
// 句柄是共享读锁定的既有持有句柄；读取前先归零文件指针——同一句柄可能
// 已被读过（manifest 先读后复制），文件指针是句柄级状态。
bool rewind(HANDLE handle) {
  LARGE_INTEGER zero{};return SetFilePointerEx(handle,zero,nullptr,FILE_BEGIN)!=FALSE;
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
QString leaseFileName(const QString &manifestSha,const QString &moniker) {
  return manifestSha+'.'+hostIdentity()+'.'+moniker+".json";
}
// owner 判活与 profile journal 同口径（PID + 进程创建时间），杜绝 PID 复用
// 误判。known=false 表示无法判定，调用方保留并诊断。
bool ownerRecordAlive(const QJsonObject &record,bool &known) {
  bool numeric=false;const auto started=record.value("hostCreated100ns").toString().toULongLong(&numeric);
  const auto pid=record.value("hostPid").toInteger(-1);
  if(!numeric||!started||pid<1||pid>0xffffffffLL){known=false;return false;}
  return !ownerStopped(DWORD(pid),started,known);
}
// 每次运行的 profile 独占租约；调用方须持有 control 锁。
bool removeOwnLease(const Layout &layout,const QString &manifestSha,const QString &moniker,QString *error) {
  if(!validMoniker(moniker)){if(error)*error="python_view_lease_remove";return false;}
  const auto path=QDir(layout.leases).filePath(leaseFileName(manifestSha,moniker));
  if(!QFile::remove(path)&&QFileInfo::exists(path)) {
    if(error)*error="python_view_lease_remove";
    return false;
  }
  return true;
}
// 经锁定句柄读 manifest（树已共享读锁定；句柄独此一份，先归零指针）。
bool manifestFromLockedTree(const LockedTree &runtime,QJsonObject &manifest,QString *error) {
  const QString wanted=QDir(runtime.root).filePath("runtime-manifest.json");
  for(int i=0;i<runtime.paths.size();++i) {
    if(runtime.paths.at(i).compare(wanted,Qt::CaseInsensitive)!=0)continue;
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(runtime.handles.at(size_t(i)).get(),&size)||size.QuadPart<=0||size.QuadPart>4*1024*1024){
      *error="python_view_manifest_size";return false;}
    QByteArray bytes(int(size.QuadPart),Qt::Uninitialized);DWORD read=0;
    if(!rewind(runtime.handles.at(size_t(i)).get())||
        !ReadFile(runtime.handles.at(size_t(i)).get(),bytes.data(),DWORD(bytes.size()),&read,nullptr)||
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
  if(!rewind(source)){*error="python_view_read";return false;}
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
  if(g_verifyHook)g_verifyHook();
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
// 检疫验证失败的视图：仅当无其他租约时移除（他人运行中不动），下次运行
// 重建——损坏视图不再永久堵死该 generation。
bool quarantineView(const Layout &layout,const QString &manifestSha,const QString &moniker) {
  QLockFile lock(QDir(layout.control).filePath(".lock"));lock.setStaleLockTime(30000);
  if(!lock.tryLock(2000))return false;
  const auto ownLease=leaseFileName(manifestSha,moniker);
  const auto wanted=manifestSha+'.';
  for(const auto &name:QDir(layout.leases).entryList({"*.json"},QDir::Files|QDir::Hidden|QDir::System))
    if(name.startsWith(wanted)&&name!=ownLease)return false; // 其他运行持有：保留
  const auto viewRoot=QDir(layout.views).filePath(manifestSha);
  if(QFileInfo::exists(viewRoot)&&!QDir(viewRoot).removeRecursively())return false;
  return true;
}
} // namespace

void view_test::setFailuresForTest(FailureHook copy,FailureHook acl,FailureHook lease,FailureHook release) {
  g_failCopy=std::move(copy);g_failAcl=std::move(acl);g_failLease=std::move(lease);g_failRelease=std::move(release);
}
void view_test::setVerifyHookForTest(std::function<void()> hook) {g_verifyHook=std::move(hook);}

// 视图构建：staging 复制（根先设 capability 只读可继承 ACL，子项随建随承，
// 不再逐节点改写安全描述符）→ ready marker → 原子改名。最终目录名出现即
// 代表完整且逐文件摘要已验证。并发同名视图已就位时改走复用（renamed=false）。
bool buildView(const SandboxRequest &request,const LockedTree &runtime,const Profile &profile,
               const QJsonObject &manifest,const QString &anchorSha,const Layout &layout,
               const QString &viewRoot,PrivateView &view,bool &renamed,QString *error) {
  const QString staging=viewRoot+'.'+hostIdentity()+'.'+profile.moniker+".staging";
  if(QFileInfo::exists(staging)&&!QDir(staging).removeRecursively()){*error="python_view_staging_busy";return false;}
  if(!QDir().mkpath(staging)){*error="python_view_staging_create";return false;}
  const auto cleanup=qScopeGuard([&]{
    if(QFileInfo::exists(staging)&&
       ((g_failRelease&&g_failRelease())||!QDir(staging).removeRecursively()))
      *error="python_sandbox_cleanup_failed";
  });
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
    if(!rewind(source)||!destination.open(QIODevice::WriteOnly|QIODevice::Truncate)){*error="python_view_copy_open";return false;}
    QCryptographicHash hash(QCryptographicHash::Sha256);
    char buffer[64*1024];qint64 remaining=size.QuadPart;
    while(remaining>0) {
      if(cancelled(request)){*error="cancelled";return false;}
      DWORD got=0;const DWORD want=DWORD(qMin<qint64>(qint64(sizeof(buffer)),remaining));
      if(!ReadFile(source,buffer,want,&got,nullptr)||!got){*error="python_view_read";return false;}
      hash.addData(buffer,got);
      if(destination.write(buffer,got)!=qint64(got)){*error="python_view_write";return false;}
      remaining-=got;
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
  renamed=QDir(staging).rename(staging,viewRoot);
  if(renamed)
    QFile::remove(QDir(viewRoot).filePath("staging.json")); // 复用验证只认 view.json
  else if(!QFileInfo::exists(viewRoot)){*error="python_view_rename";return false;}
  // rename 失败但 viewRoot 已就位 = 并发宿主先完成：staging 由 cleanup 回收，
  // renamed=false 提示调用方改走复用验证。
  view.viewBytes=treeSize;view.viewFiles=files;
  return true;
}

bool takeViewLease(const Layout &layout,const QString &manifestSha,const QString &moniker,QString *error) {
  if(g_failLease&&g_failLease()){*error="python_view_lease_forced";return false;}
  if(!validMoniker(moniker)){*error="python_view_owner_identity";return false;}
  const QString started=QString::number(processCreation(GetCurrentProcess()));
  if(started=="0"){*error="python_view_owner_identity";return false;}
  // 记录相对视图路径（诊断用）；清理一律按 manifestSha 在根内重建路径，
  // 绝不采纳记录文件里的任意绝对路径。
  const QJsonObject record{{"schema",LeaseSchema},{"manifestSha256",manifestSha},
      {"viewPath","runtime-views/"+manifestSha},{"hostPid",qint64(GetCurrentProcessId())},
      {"hostCreated100ns",started}};
  if(!writeRecord(QDir(layout.leases).filePath(leaseFileName(manifestSha,moniker)),record)){*error="python_view_lease_write";return false;}
  return true;
}

bool releasePrivateView(const PrivateView &view,QString *error) {
  const auto deactivate=qScopeGuard([&]{setLeaseActive(view.leaseMoniker,false);});
  if(g_failRelease&&g_failRelease()){*error="python_view_release_forced";return false;}
  Layout layout;
  if(!layout.create(error))return false;
  const QString manifestSha=hexDigestOrEmpty(QFileInfo(view.runtimeRoot).fileName());
  QLockFile lock(QDir(layout.control).filePath(".lock"));lock.setStaleLockTime(30000);
  if(!lock.tryLock(5000)){*error="python_view_journal_busy";return false;}
  // 私有输入按 run 清理；失败保留目录与租约记录，由恢复路径按 owner 判活
  // 重试——不删记录掩盖残留（runWindowsSandbox 映射为清理待恢复状态）。
  if(!view.inputRoot.isEmpty()&&QFileInfo::exists(view.inputRoot)&&!QDir(view.inputRoot).removeRecursively()) {
    *error="python_view_input_remove";return false;}
  if(manifestSha.isEmpty()){*error="python_view_lease_remove";return false;}
  return removeOwnLease(layout,manifestSha,view.leaseMoniker,error);
}

// 恢复逻辑主体：调用方必须已持有 control .lock。
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
    if(alive) {
      const auto moniker=name.section('.',2,3);
      const bool own=record.value("hostPid").toInteger(-1)==qint64(GetCurrentProcessId())&&
        record.value("hostCreated100ns").toString()==QString::number(processCreation(GetCurrentProcess()))&&
        validMoniker(moniker)&&name==leaseFileName(sha,moniker);
      if(!own||leaseIsActive(moniker)){blocked.insert(sha);continue;}
      const auto inputRoot=QDir(layout.inputs).filePath(moniker);
      if(QFileInfo::exists(inputRoot)&&!QDir(inputRoot).removeRecursively()){*error="python_view_input_remove";return false;}
      if(!QFile::remove(path)){*error="python_view_lease_remove";return false;}
      continue;
    }
    if(!QFile::remove(path)){*error="python_view_lease_remove";return false;}
  }
  // 2) 残留 staging：marker 记 owner；死宿主 staging 整树回收，无 marker
  // 或归属不明的保留并诊断（不凭目录名臆断可删）。
  const auto entries=QDir(layout.views).entryInfoList(QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden|QDir::System,QDir::Name);
  for(const auto &entry:entries) {
    if(!entry.fileName().endsWith(".staging"))continue;
    if(entry.isSymLink()||entry.isJunction()){*error="python_view_staging_invalid";return false;}
    const auto staging=entry.absoluteFilePath();
    QJsonObject record;
    const bool hasMarker=parseRecord(QDir(staging).filePath("staging.json"),QStringList{"hostCreated100ns","hostPid","manifestSha256","schema","state"},ViewSchema,record);
    static const QRegularExpression stagingName(
      "^[0-9a-f]{64}\\.([0-9]+)-([0-9]+)\\.(PaleoPython\\.[0-9a-f]{32})\\.staging$");
    const auto match=stagingName.match(entry.fileName());
    if(!hasMarker) {
      if(!match.hasMatch()){
        qWarning("pythonsandbox view: unattributed staging left in place: %s",qPrintable(staging));
        continue;
      }
      record={{"hostPid",match.captured(1).toLongLong()},{"hostCreated100ns",match.captured(2)}};
    }
    bool known=false;const bool alive=ownerRecordAlive(record,known);
    if(!known){*error="python_view_staging_owner_unknown";return false;}
    const bool own=match.hasMatch()&&record.value("hostPid").toInteger(-1)==qint64(GetCurrentProcessId())&&
      record.value("hostCreated100ns").toString()==QString::number(processCreation(GetCurrentProcess()));
    if((!alive||(own&&!leaseIsActive(match.captured(3))))&&!QDir(staging).removeRecursively()){
      *error="python_view_staging_remove";return false;
    }
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

bool acquirePrivateView(const SandboxRequest &request,const LockedTree &runtime,const LockedTree &input,
                        const AuthenticatedSnapshot &snapshot,const Profile &profile,PrivateView &view,QString *error) {
  Layout layout;
  if(!layout.create(error))return false;
  const QString anchorSha=hexDigestOrEmpty(request.runtime->manifestSha256);
  if(anchorSha.isEmpty()){*error="python_view_manifest_invalid";return false;}
  if(!validMoniker(profile.moniker)){*error="python_view_owner_identity";return false;}
  setLeaseActive(profile.moniker,true);
  bool ownsLease=false,activeLease=true;
  const auto leaseCleanup=qScopeGuard([&]{
    if(ownsLease) {
      QLockFile lock(QDir(layout.control).filePath(".lock"));lock.setStaleLockTime(30000);
      if((g_failRelease&&g_failRelease())||!lock.tryLock(5000)||!removeOwnLease(layout,anchorSha,profile.moniker,nullptr))
        *error="python_sandbox_cleanup_failed";
    }
    if(activeLease)setLeaseActive(profile.moniker,false);
  });
  // 第一临界区（毫秒级）：恢复 + manifest/预算准入 + 「存在即取租约」。
  // 复制与完整重哈希都在锁外——GiB 级复制不得饿死其他宿主的 journal 操作。
  QJsonObject manifest;bool exists=false;
  {
    QLockFile lock(QDir(layout.control).filePath(".lock"));lock.setStaleLockTime(30000);
    if(!lock.tryLock(5000)){*error="python_view_journal_busy";return false;}
    if(!recoverViewResourcesLocked(layout,error))return false;
    if(!manifestFromLockedTree(runtime,manifest,error))return false;
    const qint64 treeSize=manifest.value("treeSize").toInteger(-1);
    if(treeSize<0||treeSize>1024LL*1024*1024){*error="python_view_manifest_invalid";return false;}
    for(const auto &value:manifest.value("files").toArray())
      if(kReservedNames.contains(value.toObject().value("path").toString(),Qt::CaseInsensitive)){
        *error="python_view_reserved_name";return false;}
    const qint64 available=freeBytes(layout.control);
    if(available>=0&&available<treeSize+request.policy.inputBytes+MinFreeDiskBytes){*error="python_view_disk_space";return false;}
    exists=QFileInfo::exists(QDir(layout.views).filePath(anchorSha));
    if(exists) {
      if(!takeViewLease(layout,anchorSha,profile.moniker,error))return false;
      ownsLease=true;
    }
  }
  const qint64 treeSize=manifest.value("treeSize").toInteger(-1);
  if(exists) {
    // 复用既有视图（租约在手，逐出不会碰）：完整重哈希防同尺寸篡改；失败
    // 即拒绝执行，且检疫移除（无他租约时）——不静默重建掩盖差异，也不让
    // 损坏视图永久堵死该 generation。
    qint64 hashed=0;int files=0;
    if(!verifyViewTree(QDir(layout.views).filePath(anchorSha),manifest,anchorSha,request,&files,&hashed,error)) {
      // 中途取消不是视图损坏：不检疫（避免把好视图删成 1 GiB 重复制）。
      if(*error!="cancelled") quarantineView(layout,anchorSha,profile.moniker);
      return false;}
    view.reusedView=true;view.viewBytes=treeSize;view.viewFiles=files;
  } else {
    bool renamed=false;
    if(!buildView(request,runtime,profile,manifest,anchorSha,layout,QDir(layout.views).filePath(anchorSha),view,renamed,error))
      return false;
    {
      QLockFile lock(QDir(layout.control).filePath(".lock"));lock.setStaleLockTime(30000);
      if(!lock.tryLock(5000)){*error="python_view_journal_busy";return false;}
      if(!QFileInfo::exists(QDir(layout.views).filePath(anchorSha))){*error="python_view_rename";return false;}
      if(!takeViewLease(layout,anchorSha,profile.moniker,error))return false;
      ownsLease=true;
    }
    if(!renamed) {
      // 输给并发宿主：视图非本宿主哈希——按复用口径完整验证后再用。
      qint64 hashed=0;int files=0;
      if(!verifyViewTree(QDir(layout.views).filePath(anchorSha),manifest,anchorSha,request,&files,&hashed,error)) {
        if(*error!="cancelled") quarantineView(layout,anchorSha,profile.moniker);
        return false;}
      view.viewFiles=files;
    }
    view.reusedView=!renamed;
  }
  // 私有输入（锁外）：profile 隔离目录 + 包 SID 只读（继承），TEMP/TMP/
  // LOCALAPPDATA 都指向这里；写盘按既有 LPAC 合同仍被拒绝。code.py 从锁定
  // 句柄流式复制并按 manifest 摘要校验；request.json/data.bin 复用已认证
  // 内存字节。
  const QString inputRoot=QDir(layout.inputs).filePath(profile.moniker);
  if(QFileInfo::exists(inputRoot)&&!QDir(inputRoot).removeRecursively()){*error="python_view_input_busy";return false;}
  bool inputReady=false;
  // 输入构造失败时清理私有输入；租约由外层保护器在控制锁下归还。
  const auto fail=qScopeGuard([&]{
    if(inputReady)return;
    if(QFileInfo::exists(inputRoot)&&!QDir(inputRoot).removeRecursively()) {
      *error="python_sandbox_cleanup_failed";
      ownsLease=false; // 留下租约和目录，由后续恢复处理。
      return;
    }
    view.inputRoot.clear();
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
  for(const auto &value:request.input->manifest.value("files").toArray())
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
  view.runtimeRoot=QDir(layout.views).filePath(anchorSha);view.inputRoot=inputRoot;
  view.leaseMoniker=profile.moniker;
  ownsLease=false; // 租约交给 releasePrivateView。
  activeLease=false;
  return true;
}
} // namespace paleo::python::windows_detail
#else
// 非 Windows：内部声明仅在 Q_OS_WIN 下存在，无需桩（Linux 强隔离走
// pythonsandbox_linux.*）。空翻译单元保留挂点，产物不含符号。
#endif
