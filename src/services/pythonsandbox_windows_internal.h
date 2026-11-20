// 层：数据
#pragma once
#include "pythonsandbox_windows.h"
#ifdef Q_OS_WIN
#include "services/pythonprocess_windows_internal.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <userenv.h>
#include <QVector>
#include <functional>
#include <memory>
#include <vector>

namespace paleo::python::windows_detail {
using Handle = windows_process::Handle;
struct Api {
  using CreateProfile = HRESULT(WINAPI*)(PCWSTR,PCWSTR,PCWSTR,PSID_AND_ATTRIBUTES,DWORD,PSID*);
  using DeleteProfile = HRESULT(WINAPI*)(PCWSTR);
  using FolderPath = HRESULT(WINAPI*)(PCWSTR,PWSTR*);
  using Capability = BOOL(WINAPI*)(PCWSTR,PSID**,DWORD*,PSID**,DWORD*);
  HMODULE userenv = nullptr;
  CreateProfile create = nullptr;
  DeleteProfile remove = nullptr;
  FolderPath folder = nullptr;
  Capability derive = nullptr;
  DWORD build = 0;
  bool ready() const { return build>=14393 && create && remove && folder && derive; }
  Api();
  ~Api();
};
const Api &api();
using windows_process::native;
bool cancelled(const SandboxRequest &request);
QString canonicalRoot(const QString &path);
// journal.cpp 提供的共享判活/根解析原语（profile 与私有视图记录共用口径）。
QString journalRoot(QString *error);
quint64 processCreation(HANDLE process);
bool ownerStopped(DWORD id, quint64 started, bool &known);
bool validMoniker(const QString &moniker);

struct Profile {
  QString moniker, base, journal;
  PSID sid = nullptr;
  PSID registryCapability = nullptr, runtimeCapability = nullptr;
  PSID *registryGroups = nullptr, *runtimeGroups = nullptr;
  DWORD registryGroupCount = 0, runtimeGroupCount = 0;
  bool created = false;
  ~Profile();
  bool prepare(QString *error);
  bool cleanup(QString *error=nullptr);
};
bool cleanDeadProfiles(QString *error);
bool registerProfile(Profile &profile,QString *error);
struct LockedTree {
  QString root;
  std::vector<Handle> handles;
  QVector<QString> paths;
  qint64 bytes = 0;
};
struct AuthenticatedSnapshot {
  QString program,codePath;
  QList<QByteArray> inputChunks;
  qint64 reservedBytes=0;
};
bool authenticateSnapshots(const SandboxRequest &request,const LockedTree &runtime,const LockedTree &input,
                           AuthenticatedSnapshot &snapshot,QString *error);
qint64 hostBaseReservation(const ExecutionPolicy &policy);
bool lockTree(const QString &path,qint64 maximum,LockedTree &tree,const SandboxRequest &request,QString *error);
bool restrictTree(const LockedTree &tree,PSID readSid,QString *error);
bool restrictProfile(const Profile &profile,QString *error);
bool protectedFileAcl(const QString &path,PSID readSid,QString *error);

// pythonsandbox_windows_view.cpp —— 认证的私有执行视图（#959）：
// 原 runtime/工程输入树不再逐节点改写 DACL；由已锁定并认证的来源构造
// 服务自有副本（runtime 按 generation 内容寻址共享 + 租约，输入按 profile
// 隔离），仅对自有副本设置沙箱 ACL。
struct PrivateView {
  QString runtimeRoot,inputRoot; // 私有执行路径（区别于来源身份）
  qint64 viewBytes=0;            // 视图实际字节（=manifest treeSize）
  int viewFiles=0;
  bool reusedView=false;         // true=按已验证内容身份复用既有视图
};
bool acquirePrivateView(const SandboxRequest &request,const LockedTree &runtime,const LockedTree &input,
                        const AuthenticatedSnapshot &snapshot,const Profile &profile,PrivateView &view,QString *error);
// 归还视图租约并清理本次私有输入；失败保留租约记录（下次恢复重试），
// 返回 false 时 *error 带具体不完整状态。残留资源恢复（死宿主租约/
// staging/输入 + 超额无租约视图回收）随 acquire 的首段临界区执行。
bool releasePrivateView(const PrivateView &view,QString *error);

namespace view_test {
// 仅测试二进制置位的失败注入（复制/ACL/租约/清理各一档）；生产代码零引用，
// 只制造失败，不提供任何「关闭隔离」的运行时开关。
using FailureHook=std::function<bool()>;
void setFailuresForTest(FailureHook copy,FailureHook acl,FailureHook lease,FailureHook release);
}

} // namespace paleo::python::windows_detail
#endif
