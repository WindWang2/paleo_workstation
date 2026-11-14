// 层：数据
#include "pythonsandbox_windows_internal.h"
#ifdef Q_OS_WIN
#include <aclapi.h>
#include <sddl.h>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QUuid>

namespace paleo::python::windows_detail {
namespace {
constexpr int MaxEntries = 4096;
struct Local { void *value=nullptr;~Local(){if(value)LocalFree(value);} };
QString winError(const char *what,DWORD error) { return QLatin1String(what)+QLatin1Char(':')+QString::number(error); }
bool descriptor(PSID readSid,DWORD full,DWORD read,PACL *out,QString *error) {
  Handle token;HANDLE raw=nullptr;
  if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw)){*error=winError("token",GetLastError());return false;}token.reset(raw);
  DWORD size=0;GetTokenInformation(token.get(),TokenUser,nullptr,0,&size);std::vector<BYTE> info(size);
  if(!size||!GetTokenInformation(token.get(),TokenUser,info.data(),size,&size)){*error=winError("token_user",GetLastError());return false;}
  Local system,owner;
  if(!ConvertStringSidToSidW(L"S-1-5-18",(PSID*)&system.value)||!ConvertStringSidToSidW(L"S-1-3-4",(PSID*)&owner.value))return false;
  PSID ids[]={((TOKEN_USER*)info.data())->User.Sid,system.value,readSid,owner.value};DWORD rights[]={full,full,read,READ_CONTROL};
  EXPLICIT_ACCESSW access[4]{};int count=0;
  for(int i=0;i<4;++i){if(!ids[i])continue;auto &entry=access[count++];entry.grfAccessPermissions=rights[i];entry.grfAccessMode=SET_ACCESS;
    entry.grfInheritance=SUB_CONTAINERS_AND_OBJECTS_INHERIT;entry.Trustee.TrusteeForm=TRUSTEE_IS_SID;entry.Trustee.ptstrName=(LPWSTR)ids[i];}
  const auto code=SetEntriesInAclW(count,access,nullptr,out);if(code)*error=winError("acl_build",code);return code==ERROR_SUCCESS;
}
bool capability(const wchar_t *name,PSID &sid,PSID *&groups,DWORD &count,QString *error) {
  PSID *sids=nullptr;DWORD sidCount=0;
  if(!api().derive(name,&groups,&count,&sids,&sidCount)){*error=winError("capability",GetLastError());return false;}
  if(sidCount!=1){for(DWORD i=0;i<sidCount;++i)LocalFree(sids[i]);LocalFree(sids);*error="capability_count";return false;}
  sid=sids[0];LocalFree(sids);return true;
}
bool secureRegistry(HKEY root,PSID sid,int &count,QString *error) {
  if(++count>MaxEntries){*error="registry_entry_budget";return false;}Local acl;
  if(!descriptor(sid,KEY_ALL_ACCESS,KEY_READ,(PACL*)&acl.value,error))return false;
  SECURITY_DESCRIPTOR sd{};InitializeSecurityDescriptor(&sd,SECURITY_DESCRIPTOR_REVISION);SetSecurityDescriptorDacl(&sd,TRUE,(PACL)acl.value,FALSE);
  const auto code=RegSetKeySecurity(root,DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,&sd);
  if(code){*error=winError("registry_acl",code);return false;}
  for(DWORD index=0;;++index){wchar_t name[256];DWORD length=256;
    const auto item=RegEnumKeyExW(root,index,name,&length,nullptr,nullptr,nullptr,nullptr);
    if(item==ERROR_NO_MORE_ITEMS)break;if(item){*error=winError("registry_enum",item);return false;}
    HKEY child=nullptr;const auto opened=RegOpenKeyExW(root,name,0,READ_CONTROL|WRITE_DAC|KEY_ENUMERATE_SUB_KEYS,&child);
    if(opened){*error=winError("registry_open",opened);return false;}
    const bool ok=secureRegistry(child,sid,count,error);RegCloseKey(child);if(!ok)return false;}
  return true;
}
bool appendPath(const QString &path,qint64 maximum,LockedTree &tree,QString *error) {
  if(tree.paths.size()>=MaxEntries||path.size()>1024){*error="tree_entry_budget";return false;}
  const auto wide=native(path);Handle h(CreateFileW(wide.c_str(),GENERIC_READ|READ_CONTROL,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
  BY_HANDLE_FILE_INFORMATION info{};
  if(!h.valid()||!GetFileInformationByHandle(h.get(),&info)){*error=winError("tree_open",GetLastError());return false;}
  if(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DEVICE)){*error="tree_reparse_or_device";return false;}
  if(!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)){
    const auto bytes=(quint64(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
    if(info.nNumberOfLinks!=1||bytes>quint64(maximum)||tree.bytes>maximum-qint64(bytes)){*error="tree_file_budget_or_alias";return false;}
    tree.bytes+=qint64(bytes);}
  tree.paths.append(path);tree.handles.emplace_back(std::move(h));return true;
}
} // namespace
Api::Api() {
  using Version=LONG(WINAPI*)(OSVERSIONINFOW*);auto version=(Version)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlGetVersion");
  OSVERSIONINFOW info{};info.dwOSVersionInfoSize=sizeof(info);if(version&&version(&info)>=0&&info.dwMajorVersion>=10)build=info.dwBuildNumber;
  userenv=LoadLibraryExW(L"userenv.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!userenv)return;
  create=(CreateProfile)GetProcAddress(userenv,"CreateAppContainerProfile");remove=(DeleteProfile)GetProcAddress(userenv,"DeleteAppContainerProfile");
  folder=(FolderPath)GetProcAddress(userenv,"GetAppContainerFolderPath");
  derive=(Capability)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"),"DeriveCapabilitySidsFromName");
}
Api::~Api() { if(userenv)FreeLibrary(userenv); }
const Api &api() { static const Api value;return value; }
bool cancelled(const SandboxRequest &request) { try{return request.cancelled&&request.cancelled();}catch(...){return true;} }
QString canonicalRoot(const QString &path) {
  if(path.isEmpty()||path.size()>8192||path.contains(QChar(0))||!QDir::isAbsolutePath(path))return {};
  const auto clean=QDir::cleanPath(QDir::fromNativeSeparators(path));const QFileInfo info(clean);const auto canonical=info.canonicalFilePath();
  if(!info.isDir()||info.isSymLink()||info.isJunction()||canonical.startsWith("//")||clean.compare(canonical,Qt::CaseInsensitive)!=0)return {};
  auto current=canonical;while(current.size()>3){const auto attributes=GetFileAttributesW(native(current).c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_REPARSE_POINT))return {};current=QFileInfo(current).absolutePath();}
  return canonical;
}
Profile::~Profile() {
  cleanup();
  if(sid)FreeSid(sid);if(registryCapability)LocalFree(registryCapability);if(runtimeCapability)LocalFree(runtimeCapability);
  for(DWORD i=0;i<registryGroupCount;++i)LocalFree(registryGroups[i]);if(registryGroups)LocalFree(registryGroups);
  for(DWORD i=0;i<runtimeGroupCount;++i)LocalFree(runtimeGroups[i]);if(runtimeGroups)LocalFree(runtimeGroups);
}
bool Profile::prepare(QString *error) {
  moniker="PaleoPython."+QUuid::createUuid().toString(QUuid::Id128);
  if(!registerProfile(*this,error))return false;
  const auto code=api().create((PCWSTR)moniker.utf16(),L"Paleo isolated Python task",L"Temporary read-only broker task",nullptr,0,&sid);
  if(FAILED(code)){*error=winError("profile_create",DWORD(code));return false;}created=true;
  Local sidText;if(!ConvertSidToStringSidW(sid,(LPWSTR*)&sidText.value)){*error="profile_sid";return false;}
  PWSTR folder=nullptr;const auto got=api().folder((PCWSTR)sidText.value,&folder);const auto path=QString::fromWCharArray(folder?folder:L"");if(folder)CoTaskMemFree(folder);
  if(FAILED(got)||QFileInfo(path).fileName().compare("AC",Qt::CaseInsensitive)!=0){*error="profile_folder";return false;}
  base=canonicalRoot(QFileInfo(path).absolutePath());
  if(base.isEmpty()||QFileInfo(base).fileName().compare(moniker,Qt::CaseInsensitive)!=0){*error="profile_identity";return false;}
  return capability(L"registryRead",registryCapability,registryGroups,registryGroupCount,error)&&
      capability(L"paleoPythonRuntimeRead",runtimeCapability,runtimeGroups,runtimeGroupCount,error);
}
bool lockTree(const QString &path,qint64 maximum,LockedTree &tree,const SandboxRequest &request,QString *error) {
  tree.root=canonicalRoot(path);if(tree.root.isEmpty()){*error="tree_root";return false;}
  if(!appendPath(tree.root,maximum,tree,error))return false;
  QDirIterator it(tree.root,QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden|QDir::System,QDirIterator::Subdirectories);
  while(it.hasNext()){if(cancelled(request)){*error="cancelled";return false;}const auto item=it.next();if(!appendPath(item,maximum,tree,error))return false;}
  return true;
}
bool protectedFileAcl(const QString &path,PSID sid,QString *error) {
  Local acl;if(!descriptor(sid,FILE_ALL_ACCESS,FILE_GENERIC_READ|FILE_GENERIC_EXECUTE,(PACL*)&acl.value,error))return false;
  const auto wide=native(path);const auto code=SetNamedSecurityInfoW((LPWSTR)wide.c_str(),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,
      nullptr,nullptr,(PACL)acl.value,nullptr);
  if(code)*error=winError("file_acl",code);return code==ERROR_SUCCESS;
}
bool restrictTree(const LockedTree &tree,PSID sid,QString *error) {
  for(const auto &path:tree.paths)if(!protectedFileAcl(path,sid,error))return false;return true;
}
bool restrictProfile(const Profile &profile,QString *error) {
  LockedTree tree;SandboxRequest request;if(!lockTree(profile.base,16*1024*1024,tree,request,error)||!restrictTree(tree,profile.sid,error))return false;
  const auto key=QStringLiteral("Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppContainer\\Storage\\")+profile.moniker;
  HKEY registry=nullptr;const auto opened=RegOpenKeyExW(HKEY_CURRENT_USER,(PCWSTR)key.utf16(),0,READ_CONTROL|WRITE_DAC|KEY_ENUMERATE_SUB_KEYS,&registry);
  if(opened){*error=winError("profile_registry",opened);return false;}int count=0;const bool ok=secureRegistry(registry,profile.sid,count,error);RegCloseKey(registry);return ok;
}
} // namespace paleo::python::windows_detail
#endif
