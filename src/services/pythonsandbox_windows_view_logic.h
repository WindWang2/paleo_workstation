// 层：数据
#pragma once

#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

// services/pythonsandbox_windows_view_logic.h — Windows 受控 Python 私有视图与
// journal 的纯跨平台校验/恢复决策逻辑（#959）。
//
// 无 Win32 API、无 QtWidgets：在 Linux 与 Windows 上均可编译并做单元测试，
// 生产侧由 pythonsandbox_windows_journal.cpp / pythonsandbox_windows_view.cpp
// 直接调用，确保跨平台测试覆盖的协议与恢复决策同源。
namespace paleo::python::windows_view_logic
{

inline constexpr auto kProfileJournalSchema = "paleo.python-sandbox-profile.v1";
inline constexpr auto kViewMarkerSchema = "paleo.python-sandbox-view.v1";
inline constexpr auto kViewLeaseSchema = "paleo.python-sandbox-view-lease.v1";
inline constexpr int kMaxCachedViews = 2;

struct OwnerIdentity
{
  quint64 hostPid = 0;
  quint64 hostCreated100ns = 0;
  bool valid = false;
};

struct ParsedStagingName
{
  QString manifestSha;
  quint64 hostPid = 0;
  quint64 hostCreated100ns = 0;
  QString moniker;
  bool valid = false;
};

struct CachedViewCandidate
{
  QString sha;
  QString path;
  qint64 modifiedMs = 0;
};

enum class OwnerState
{
  Dead,
  AliveOther,
  AliveSelfActive,
  AliveSelfIdle,
  Unknown
};

enum class RecoveryAction
{
  Keep,
  Remove,
  FailClosed
};

QString hexDigest64OrEmpty(const QString &value);
bool isValidSandboxMoniker(const QString &moniker);
QString relativePathUnderRoot(const QString &root, const QString &path);
bool isReservedViewEntryName(const QString &path);

QString buildLeaseFileName(const QString &manifestSha, quint64 hostPid,
                           quint64 hostCreated100ns, const QString &moniker);
ParsedStagingName parseStagingDirName(const QString &dirName);
OwnerIdentity parseOwnerIdentity(const QJsonObject &record);

bool validateProfileJournalRecord(const QJsonObject &record, const QString &expectedFileName,
                                  QString *monikerOut = nullptr, OwnerIdentity *ownerOut = nullptr);
bool validateViewLeaseRecord(const QJsonObject &record, QString *manifestShaOut = nullptr,
                             OwnerIdentity *ownerOut = nullptr);
bool validateStagingMarkerRecord(const QJsonObject &record, OwnerIdentity *ownerOut = nullptr);
bool validateInputMarkerRecord(const QJsonObject &record, OwnerIdentity *ownerOut = nullptr);
bool validateReadyViewRecord(const QJsonObject &record, const QString &expectedSha);

RecoveryAction decideResourceRecovery(OwnerState ownerState, bool recordValid);
QStringList selectViewsToEvict(const QList<CachedViewCandidate> &candidates,
                               const QSet<QString> &blockedShas,
                               int maxCached = kMaxCachedViews);

} // namespace paleo::python::windows_view_logic
