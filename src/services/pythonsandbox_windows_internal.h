// 层：数据
#pragma once
#include "pythonsandbox_windows.h"
#ifdef Q_OS_WIN
#include "services/pythonprocess_windows_internal.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <userenv.h>
#include <QVector>
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

} // namespace paleo::python::windows_detail
#endif
