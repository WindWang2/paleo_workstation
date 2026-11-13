// 层：功能
// workflow/capabilityoperationcontrol.cpp — v2 控制日志的身份、CAS 和有界事件。
#include "capabilitygateway.h"
#include "capabilityops_internal.h"
#include "metadata/projectlock.h"
#include "catalog/purgelease.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUuid>
#include <limits>

namespace paleo::capability {
namespace {
bool bounded(const QJsonObject &value, int bytes) {
  return QJsonDocument(value).toJson(QJsonDocument::Compact).size() <= bytes;
}
bool validSeed(const OperationLog::Record &record) {
  static const QRegularExpression sha(QStringLiteral("^[0-9a-f]{64}$"));
  for (const auto &id : {record.documentId, record.callerScope, record.clientRequestId, record.capabilityId})
    if (id.isEmpty() || id.toUtf8().size() > 512) return false;
  return record.contractVersion > 0 && sha.match(record.requestHash).hasMatch() &&
      sha.match(record.planHash).hasMatch() && bounded(record.frozenRequest, 256 * 1024) &&
      bounded(record.stagePlan, 64 * 1024) && bounded(record.presentationIntent, 16 * 1024) &&
      bounded(record.externalRequest, 16 * 1024);
}
QJsonObject event(const OperationLog::Record &record) {
  return {{"revision", QString::number(record.stateRevision)}, {"execution", record.execution},
      {"commitState", record.commitState}, {"presentation", record.presentation}, {"code", record.code},
      {"attempt", record.ownerAttempt}, {"computeCount", record.computeCount},
      {"cancelRequested", record.cancelRequested}, {"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
}
bool immutable(const OperationLog::Record &before, const OperationLog::Record &after) {
  return before.schemaVersion == 2 && after.schemaVersion == 2 && before.operationId == after.operationId &&
      before.key == after.key && before.documentId == after.documentId && before.callerScope == after.callerScope &&
      before.clientRequestId == after.clientRequestId && before.requestHash == after.requestHash &&
      before.capabilityId == after.capabilityId && before.contractVersion == after.contractVersion &&
      before.implementationVersion == after.implementationVersion && before.planHash == after.planHash &&
      before.storeGeneration == after.storeGeneration && before.frozenRequest == after.frozenRequest &&
      before.stagePlan == after.stagePlan && before.presentationIntent == after.presentationIntent &&
      before.externalRequest == after.externalRequest &&
      (before.remoteCompletionProof.isEmpty() || before.remoteCompletionProof == after.remoteCompletionProof) &&
      (before.remoteJobId.isEmpty() || before.remoteJobId == after.remoteJobId) &&
      (!before.cancelRequested || after.cancelRequested) &&
      (before.preparedManifest.isEmpty() || before.preparedManifest == after.preparedManifest) &&
      (before.artifactManifest.isEmpty() || before.artifactManifest == after.artifactManifest);
}
bool transitionAllowed(const OperationLog::Record &before, const OperationLog::Record &after,
                       OperationLog::Transition reason) {
  using Transition = OperationLog::Transition;
  if (before.commitState == QLatin1String("committed"))
    return after.commitState == before.commitState && after.execution == QLatin1String("succeeded") &&
           after.computeCount == before.computeCount;
  if (before.commitState == QLatin1String("publishing")) {
    if (after.commitState == QLatin1String("committed"))
      return after.execution == QLatin1String("succeeded") && after.computeCount == before.computeCount;
    if (reason == Transition::Reconcile && after.commitState == QLatin1String("not_requested"))
      return after.execution == QLatin1String("prepared") && after.computeCount == before.computeCount;
    return after.commitState == before.commitState && after.execution == before.execution &&
           after.computeCount == before.computeCount;
  }
  if (after.commitState != QLatin1String("not_requested") && after.commitState != QLatin1String("publishing"))
    return false;
  if (after.commitState == QLatin1String("publishing"))
    return before.execution == QLatin1String("prepared") && after.execution == QLatin1String("prepared") &&
           !after.preparedManifest.isEmpty() && after.computeCount == before.computeCount;
  const bool newAttempt = after.ownerAttempt != before.ownerAttempt;
  if(before.execution==QLatin1String("queued") && after.execution==before.execution && !newAttempt && after.computeCount==before.computeCount)return true;
  if (newAttempt && reason != Transition::Resume && before.execution != QLatin1String("queued")) return false;
  if (after.execution == QLatin1String("running")) {
    if (before.execution == QLatin1String("running"))
      return !newAttempt && after.computeCount == before.computeCount;
    const bool copySource=before.frozenRequest.value("phase").toString()==QLatin1String("publish") &&
        !before.frozenRequest.value("sourceOperationId").toString().isEmpty();
    return (before.execution == QLatin1String("queued") ||
            (before.execution == QLatin1String("interrupted") && reason == Transition::Resume)) &&
           newAttempt && qint64(after.computeCount) == qint64(before.computeCount) + (copySource?0:1);
  }
  if (before.execution == QLatin1String("succeeded") || before.execution == QLatin1String("failed") ||
      before.execution == QLatin1String("cancelled"))
    return after.execution == before.execution && after.computeCount == before.computeCount;
  if (after.computeCount != before.computeCount) return false;
  if (before.execution == QLatin1String("external_unknown"))
    return after.execution == before.execution ||
        (reason == Transition::Reconcile && !before.remoteJobId.isEmpty() &&
         (after.execution == QLatin1String("succeeded") || after.execution == QLatin1String("failed")));
  if(before.execution==QLatin1String("running") && !before.externalRequest.isEmpty() &&
      after.execution==QLatin1String("succeeded") && after.externalOutcome==QLatin1String("succeeded") &&
      (!after.remoteJobId.isEmpty() || !after.remoteCompletionProof.isEmpty()))
    return reason==Transition::Reconcile;
  if (after.execution == QLatin1String("external_unknown"))
    return before.execution == QLatin1String("running") && !after.externalRequest.isEmpty();
  if (after.execution == QLatin1String("succeeded"))
    return before.execution == QLatin1String("running") && !after.artifactManifest.isEmpty();
  if (after.execution == QLatin1String("prepared"))
    return before.execution == QLatin1String("running") || before.execution == QLatin1String("prepared") ||
        (before.execution == QLatin1String("queued") && before.frozenRequest.value("phase").toString() == QLatin1String("prepare"));
  if (after.execution == QLatin1String("interrupted"))
    return before.execution == QLatin1String("running") && reason == Transition::Reconcile;
  return after.execution == QLatin1String("cancelled") || after.execution == QLatin1String("failed");
}
} // namespace

bool OperationLog::ownsWrites() const {
  if (!m_claim || !m_claim->isHeldFor(m_projectDir)) return false;
  const auto root = QFileInfo(m_projectDir).canonicalFilePath();
  const auto expected = QDir(root).filePath(QStringLiteral(".paleo/capability-ops"));
  const auto actual = QFileInfo(m_path).absolutePath();
  if (QDir::cleanPath(actual) != QDir::cleanPath(expected) || QFileInfo(expected).isSymLink() ||
      QFileInfo(m_path).isSymLink() || QFileInfo(m_cachePath).isSymLink() ||
      QFileInfo(QDir(root).filePath(QStringLiteral(".paleo"))).isSymLink()) return false;
  return true;
}

OperationLog::Accept OperationLog::lookup(const QString &documentId, const QString &callerScope,
                                         const QString &clientRequestId) {
  QMutexLocker lock(&operation_detail::mutexFor(m_path));
  if (!loadOperations()) return {QStringLiteral("operation_log_unavailable"), {}};
  const auto found = m_byKey.constFind(operation_detail::key(documentId, callerScope, clientRequestId));
  return found == m_byKey.cend() ? Accept{QStringLiteral("not_found"), {}}
                                : Accept{QStringLiteral("found"), *found};
}
OperationLog::Record OperationLog::artifactOwner(const QString &documentId,const QString &scope,const QString &relativePath) {
  QMutexLocker lock(&operation_detail::mutexFor(m_path));if(!loadOperations())return {};
  for(const auto &r:m_byKey) {
    if(r.schemaVersion!=2 || r.documentId!=documentId || r.callerScope!=scope)continue;
    for(const auto &entry:r.artifactManifest.value("ports").toArray())
      if(entry.toObject().value("path").toString()==relativePath)return r;
  }
  return {};
}

OperationLog::Accept OperationLog::acceptDurable(const Record &seed) {
  if (!ownsWrites()) return {QStringLiteral("project_write_ownership_required"), {}};
  const auto catalogPath=QDir(m_projectDir).absoluteFilePath("artifacts/metadata/catalog.json");
  if(CatalogPurgeLease::isHeld(catalogPath))return {QStringLiteral("catalog_purge_in_progress"),{}};
  if (!validSeed(seed)) return {QStringLiteral("invalid_operation_intent"), {}};
  QMutexLocker lock(&operation_detail::mutexFor(m_path));
  if(!ownsWrites() || CatalogPurgeLease::isHeld(catalogPath))return {QStringLiteral("catalog_purge_in_progress"),{}};
  if (!loadOperations()) return {QStringLiteral("operation_log_unavailable"), {}};
  const auto identity = operation_detail::key(seed.documentId, seed.callerScope, seed.clientRequestId);
  if (m_byKey.contains(identity)) {
    const auto record = m_byKey.value(identity);
    return {record.requestHash == seed.requestHash ? QStringLiteral("replay") : QStringLiteral("conflict"), record};
  }
  if (m_byKey.size() >= 4096) return {QStringLiteral("operation_capacity_exceeded"), {}};
  auto record = seed;
  record.schemaVersion = 2; record.key = identity;
  record.operationId = QStringLiteral("op-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
  record.execution = QStringLiteral("queued"); record.commitState = QStringLiteral("not_requested");
  record.presentation = record.presentationIntent.isEmpty()?QStringLiteral("not_requested"):QStringLiteral("pending"); record.code = QStringLiteral("accepted");
  record.computeCount = 0; record.stateRevision = 1; record.ownerAttempt.clear(); record.ownerProcess.clear();
  record.cancelRequested = false; record.preparedManifest = {}; record.artifactManifest = {};
  record.remoteJobId.clear(); record.externalOutcome.clear();
  record.events = QJsonArray{event(record)};
  m_byKey.insert(identity, record); m_idToKey.insert(record.operationId, identity);
  if (!QDir().mkpath(QFileInfo(m_path).absolutePath()) || !persistOperations()) {
    m_byKey.remove(identity); m_idToKey.remove(record.operationId);
    return {QStringLiteral("persist_failed"), {}};
  }
  return {QStringLiteral("created"), record};
}

QString OperationLog::saveDurable(const Record &input, quint64 expectedRevision, const QString &expectedAttempt,
                                  Record *current, Transition transition) {
  const Record proposal = input; // current is allowed to alias the caller's proposal.
  if (!ownsWrites()) return QStringLiteral("project_write_ownership_required");
  QMutexLocker lock(&operation_detail::mutexFor(m_path));
  if (!loadOperations()) return QStringLiteral("operation_log_unavailable");
  const auto found = m_byKey.constFind(proposal.key);
  if (found == m_byKey.cend()) return QStringLiteral("unknown_operation");
  const Record before = *found;
  if (current) *current = before;
  if (before.schemaVersion != 2) return QStringLiteral("legacy_unverifiable");
  if (before.stateRevision != expectedRevision || before.ownerAttempt != expectedAttempt)
    return QStringLiteral("operation_state_conflict");
  if (!immutable(before, proposal) || !transitionAllowed(before, proposal, transition) ||
      !bounded(proposal.preparedManifest, 64 * 1024) || !bounded(proposal.artifactManifest, 64 * 1024) || proposal.summaryJson.size()>96*1024 ||
      before.stateRevision == std::numeric_limits<quint64>::max()) return QStringLiteral("invalid_operation_transition");
  if (proposal.ownerAttempt.size() > 256 || proposal.ownerProcess.size() > 256 || proposal.remoteJobId.size() > 1024 ||
      !bounded(proposal.externalRequest, 16 * 1024) || !bounded(proposal.remoteCompletionProof, 4096))
    return QStringLiteral("invalid_operation_owner");
  auto record = proposal;
  record.stateRevision = before.stateRevision + 1;
  record.events = before.events; record.events.append(event(record));
  while (record.events.size() > 128) record.events.removeFirst();
  m_byKey.insert(record.key, record);
  if (!persistOperations()) { m_byKey.insert(before.key, before); return QStringLiteral("persist_failed"); }
  if (current) *current = record;
  return QStringLiteral("ok");
}

} // namespace paleo::capability
