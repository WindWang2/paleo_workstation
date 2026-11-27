// 层：数据
#include "pythonsandbox_windows_view_logic.h"

#include <QRegularExpression>

#include <algorithm>

namespace paleo::python::windows_view_logic
{

QString hexDigest64OrEmpty(const QString &value)
{
  if (value.size() != 64)
    return {};
  for (const QChar c : value)
  {
    if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
          (c >= QLatin1Char('a') && c <= QLatin1Char('f'))))
      return {};
  }
  return value;
}

bool isValidSandboxMoniker(const QString &moniker)
{
  static const QRegularExpression pattern(QStringLiteral("^PaleoPython\\.[0-9a-f]{32}$"));
  return pattern.match(moniker).hasMatch();
}

QString relativePathUnderRoot(const QString &root, const QString &path)
{
  if (root.isEmpty() || !path.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive))
    return {};
  const QString value = path.mid(root.size() + 1);
  if (value.isEmpty() || value.contains(QStringLiteral("..")) ||
      value.contains(QLatin1Char('\\')) || value.contains(QChar(0)))
    return {};
  return value;
}

bool isReservedViewEntryName(const QString &path)
{
  return path.compare(QStringLiteral("view.json"), Qt::CaseInsensitive) == 0 ||
         path.compare(QStringLiteral("staging.json"), Qt::CaseInsensitive) == 0;
}

QString buildLeaseFileName(const QString &manifestSha, quint64 hostPid,
                           quint64 hostCreated100ns, const QString &moniker)
{
  return manifestSha + QLatin1Char('.') + QString::number(hostPid) + QLatin1Char('-') +
         QString::number(hostCreated100ns) + QLatin1Char('.') + moniker +
         QStringLiteral(".json");
}

ParsedStagingName parseStagingDirName(const QString &dirName)
{
  static const QRegularExpression stagingName(QStringLiteral(
      "^([0-9a-f]{64})\\.([0-9]+)-([0-9]+)\\.(PaleoPython\\.[0-9a-f]{32})\\.staging$"));
  ParsedStagingName out;
  const QRegularExpressionMatch match = stagingName.match(dirName);
  if (!match.hasMatch())
    return out;
  bool pidOk = false;
  bool createdOk = false;
  const quint64 pid = match.captured(2).toULongLong(&pidOk);
  const quint64 created = match.captured(3).toULongLong(&createdOk);
  if (!pidOk || !createdOk || pid < 1 || pid > 0xffffffffULL || created == 0)
    return out;
  out.manifestSha = match.captured(1);
  out.hostPid = pid;
  out.hostCreated100ns = created;
  out.moniker = match.captured(4);
  out.valid = true;
  return out;
}

OwnerIdentity parseOwnerIdentity(const QJsonObject &record)
{
  OwnerIdentity out;
  bool numeric = false;
  const quint64 started =
      record.value(QStringLiteral("hostCreated100ns")).toString().toULongLong(&numeric);
  const qint64 pid = record.value(QStringLiteral("hostPid")).toInteger(-1);
  if (!numeric || started == 0 || pid < 1 || pid > 0xffffffffLL)
    return out;
  out.hostPid = static_cast<quint64>(pid);
  out.hostCreated100ns = started;
  out.valid = true;
  return out;
}

bool validateProfileJournalRecord(const QJsonObject &record, const QString &expectedFileName,
                                  QString *monikerOut, OwnerIdentity *ownerOut)
{
  static const QStringList kKeys{QStringLiteral("hostCreated100ns"), QStringLiteral("hostPid"),
                                 QStringLiteral("moniker"), QStringLiteral("schema")};
  if (record.keys() != kKeys)
    return false;
  if (record.value(QStringLiteral("schema")).toString() != QLatin1String(kProfileJournalSchema))
    return false;
  const QString moniker = record.value(QStringLiteral("moniker")).toString();
  if (!isValidSandboxMoniker(moniker) || expectedFileName != moniker + QStringLiteral(".json"))
    return false;
  const OwnerIdentity owner = parseOwnerIdentity(record);
  if (!owner.valid)
    return false;
  if (monikerOut)
    *monikerOut = moniker;
  if (ownerOut)
    *ownerOut = owner;
  return true;
}

bool validateViewLeaseRecord(const QJsonObject &record, QString *manifestShaOut,
                             OwnerIdentity *ownerOut)
{
  static const QStringList kKeys{QStringLiteral("hostCreated100ns"), QStringLiteral("hostPid"),
                                 QStringLiteral("manifestSha256"), QStringLiteral("schema"),
                                 QStringLiteral("viewPath")};
  if (record.keys() != kKeys)
    return false;
  if (record.value(QStringLiteral("schema")).toString() != QLatin1String(kViewLeaseSchema))
    return false;
  const QString sha = hexDigest64OrEmpty(record.value(QStringLiteral("manifestSha256")).toString());
  if (sha.isEmpty() ||
      record.value(QStringLiteral("viewPath")).toString() != QStringLiteral("runtime-views/") + sha)
    return false;
  const OwnerIdentity owner = parseOwnerIdentity(record);
  if (!owner.valid)
    return false;
  if (manifestShaOut)
    *manifestShaOut = sha;
  if (ownerOut)
    *ownerOut = owner;
  return true;
}

bool validateStagingMarkerRecord(const QJsonObject &record, OwnerIdentity *ownerOut)
{
  static const QStringList kKeys{QStringLiteral("hostCreated100ns"), QStringLiteral("hostPid"),
                                 QStringLiteral("manifestSha256"), QStringLiteral("schema"),
                                 QStringLiteral("state")};
  if (record.keys() != kKeys)
    return false;
  if (record.value(QStringLiteral("schema")).toString() != QLatin1String(kViewMarkerSchema) ||
      record.value(QStringLiteral("state")).toString() != QStringLiteral("staging"))
    return false;
  if (hexDigest64OrEmpty(record.value(QStringLiteral("manifestSha256")).toString()).isEmpty())
    return false;
  const OwnerIdentity owner = parseOwnerIdentity(record);
  if (!owner.valid)
    return false;
  if (ownerOut)
    *ownerOut = owner;
  return true;
}

bool validateInputMarkerRecord(const QJsonObject &record, OwnerIdentity *ownerOut)
{
  static const QStringList kKeys{QStringLiteral("hostCreated100ns"), QStringLiteral("hostPid"),
                                 QStringLiteral("schema"), QStringLiteral("state")};
  if (record.keys() != kKeys)
    return false;
  if (record.value(QStringLiteral("schema")).toString() != QLatin1String(kViewMarkerSchema) ||
      record.value(QStringLiteral("state")).toString() != QStringLiteral("input"))
    return false;
  const OwnerIdentity owner = parseOwnerIdentity(record);
  if (!owner.valid)
    return false;
  if (ownerOut)
    *ownerOut = owner;
  return true;
}

bool validateReadyViewRecord(const QJsonObject &record, const QString &expectedSha)
{
  static const QStringList kKeys{QStringLiteral("files"), QStringLiteral("hostCreated100ns"),
                                 QStringLiteral("hostPid"), QStringLiteral("manifestSha256"),
                                 QStringLiteral("schema"), QStringLiteral("state"),
                                 QStringLiteral("treeSize")};
  if (record.keys() != kKeys)
    return false;
  if (record.value(QStringLiteral("schema")).toString() != QLatin1String(kViewMarkerSchema) ||
      record.value(QStringLiteral("state")).toString() != QStringLiteral("ready"))
    return false;
  const QString sha = hexDigest64OrEmpty(record.value(QStringLiteral("manifestSha256")).toString());
  if (sha.isEmpty() || sha != expectedSha)
    return false;
  if (record.value(QStringLiteral("files")).toInteger(-1) <= 0 ||
      record.value(QStringLiteral("treeSize")).toInteger(-1) < 0)
    return false;
  return parseOwnerIdentity(record).valid;
}

RecoveryAction decideResourceRecovery(OwnerState ownerState, bool recordValid)
{
  if (!recordValid || ownerState == OwnerState::Unknown)
    return RecoveryAction::FailClosed;
  if (ownerState == OwnerState::Dead || ownerState == OwnerState::AliveSelfIdle)
    return RecoveryAction::Remove;
  return RecoveryAction::Keep;
}

QStringList selectViewsToEvict(const QList<CachedViewCandidate> &candidates,
                               const QSet<QString> &blockedShas, int maxCached)
{
  QList<CachedViewCandidate> idle;
  for (const CachedViewCandidate &c : candidates)
  {
    if (hexDigest64OrEmpty(c.sha).isEmpty() || blockedShas.contains(c.sha))
      continue;
    idle.append(c);
  }
  if (idle.size() <= std::max(0, maxCached))
    return {};
  std::sort(idle.begin(), idle.end(), [](const CachedViewCandidate &a, const CachedViewCandidate &b) {
    if (a.modifiedMs != b.modifiedMs)
      return a.modifiedMs < b.modifiedMs;
    return a.sha < b.sha;
  });
  QStringList evict;
  const int dropCount = int(idle.size()) - std::max(0, maxCached);
  for (int i = 0; i < dropCount; ++i)
    evict.append(idle.at(i).path);
  return evict;
}

} // namespace paleo::python::windows_view_logic
