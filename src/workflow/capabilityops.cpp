// 层：功能
// workflow/capabilityops.cpp — 持久 operation 身份、数值缓存与进程内预算。
#include "capabilitygateway.h"
#include "capabilityops_internal.h"
#include "metadata/operationnamespace.h"

#include <algorithm>
#include <atomic>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QSharedPointer>
#include <QUuid>

namespace paleo::capability {
namespace {

using operation_namespace::isCanonicalAttemptId;
using operation_namespace::isCanonicalOperationId;

QJsonObject recordJson(const OperationLog::Record &record) {
  QJsonObject object;
  object.insert(QStringLiteral("operationId"), record.operationId);
  object.insert(QStringLiteral("key"), record.key);
  object.insert(QStringLiteral("requestHash"), record.requestHash);
  object.insert(QStringLiteral("execution"), record.execution);
  object.insert(QStringLiteral("commitState"), record.commitState);
  object.insert(QStringLiteral("presentation"), record.presentation);
  object.insert(QStringLiteral("code"), record.code);
  object.insert(QStringLiteral("computeCount"), record.computeCount);
  object.insert(QStringLiteral("summary"), QString::fromUtf8(record.summaryJson.toBase64()));
  if (record.schemaVersion == 2) {
    object.insert("schemaVersion", 2);
    object.insert("documentId", record.documentId); object.insert("callerScope", record.callerScope);
    object.insert("clientRequestId", record.clientRequestId); object.insert("capabilityId", record.capabilityId);
    object.insert("contractVersion", record.contractVersion); object.insert("implementationVersion", record.implementationVersion);
    object.insert("planHash", record.planHash); object.insert("storeGeneration", record.storeGeneration);
    object.insert("stateRevision", QString::number(record.stateRevision));
    object.insert("ownerAttempt", record.ownerAttempt); object.insert("ownerProcess", record.ownerProcess);
    object.insert("cancelRequested", record.cancelRequested); object.insert("frozenRequest", record.frozenRequest);
    object.insert("stagePlan", record.stagePlan); object.insert("preparedManifest", record.preparedManifest);
    object.insert("artifactManifest", record.artifactManifest); object.insert("presentationIntent", record.presentationIntent);
    object.insert("events", record.events);
    object.insert("externalRequest", record.externalRequest); object.insert("remoteJobId", record.remoteJobId);
    object.insert("externalOutcome", record.externalOutcome);
    object.insert("remoteCompletionProof", record.remoteCompletionProof);
  }
  return object;
}

// 严格解析：任何字段形状与本仓写入口（recordJson）不一致即整条记录拒绝
// （G4-W2：不得用 toObject()/toString() 的默认空值把坏形状伪装成合法空
// 记录）。拒绝的记录进入隔离区——邻居照常可读，journal 原文件永不改写。
struct ParsedRecord {
  OperationLog::Record record;
  QString error;
};
ParsedRecord recordFromJson(const QJsonValue &value) {
  ParsedRecord parsed;
  if (!value.isObject()) {
    parsed.error = QStringLiteral("record_not_object");
    return parsed;
  }
  const QJsonObject object = value.toObject();
  OperationLog::Record &record = parsed.record;
  const auto stringField = [&object](const char *name, QString &out) {
    const auto field = object.value(QLatin1String(name));
    if (!field.isString()) return false;
    out = field.toString();
    return true;
  };
  const auto objectField = [&object](const char *name, QJsonObject &out) {
    const auto field = object.value(QLatin1String(name));
    if (!field.isObject()) return false;
    out = field.toObject();
    return true;
  };
  const bool baseOk = stringField("operationId", record.operationId) &&
      stringField("key", record.key) && stringField("requestHash", record.requestHash) &&
      stringField("execution", record.execution) && stringField("commitState", record.commitState) &&
      stringField("presentation", record.presentation) && stringField("code", record.code);
  if (!baseOk) {
    parsed.error = QStringLiteral("record_field_type");
    return parsed;
  }
  const auto compute = object.value(QStringLiteral("computeCount"));
  const auto summary = object.value(QStringLiteral("summary"));
  if (!compute.isDouble() || !summary.isString()) {
    parsed.error = QStringLiteral("record_field_type");
    return parsed;
  }
  record.computeCount = compute.toInt();
  record.summaryJson = QByteArray::fromBase64(summary.toString().toLatin1());
  const auto schema = object.value(QStringLiteral("schemaVersion"));
  // 真正的 v1 旧记录不含 schemaVersion；出现即必须是非 1/2 的数字之外不受
  // 支持（未知 schema 只隔离该条，不猜测含义，也不清空整本日志）。
  if (schema.isDouble())
    record.schemaVersion = schema.toInt();
  else if (!schema.isUndefined())
  {
    parsed.error = QStringLiteral("record_field_type");
    return parsed;
  }
  if (record.schemaVersion == 2) {
    bool ok = stringField("documentId", record.documentId) && stringField("callerScope", record.callerScope) &&
        stringField("clientRequestId", record.clientRequestId) && stringField("capabilityId", record.capabilityId) &&
        stringField("implementationVersion", record.implementationVersion) &&
        stringField("planHash", record.planHash) && stringField("storeGeneration", record.storeGeneration) &&
        stringField("ownerAttempt", record.ownerAttempt) && stringField("ownerProcess", record.ownerProcess) &&
        stringField("remoteJobId", record.remoteJobId) && stringField("externalOutcome", record.externalOutcome);
    const auto contract = object.value(QStringLiteral("contractVersion"));
    const auto revision = object.value(QStringLiteral("stateRevision"));
    const auto cancelled = object.value(QStringLiteral("cancelRequested"));
    const auto events = object.value(QStringLiteral("events"));
    ok = ok && contract.isDouble() && revision.isString() && (cancelled.isBool() || cancelled.isUndefined()) &&
        events.isArray() && objectField("frozenRequest", record.frozenRequest) &&
        objectField("stagePlan", record.stagePlan) && objectField("preparedManifest", record.preparedManifest) &&
        objectField("artifactManifest", record.artifactManifest) &&
        objectField("presentationIntent", record.presentationIntent) &&
        objectField("externalRequest", record.externalRequest) &&
        objectField("remoteCompletionProof", record.remoteCompletionProof);
    record.contractVersion = contract.toInt();
    record.cancelRequested = cancelled.toBool();
    record.events = events.toArray();
    bool valid = false;
    record.stateRevision = revision.toString().toULongLong(&valid);
    if (!ok || !valid) {
      parsed.error = QStringLiteral("record_field_type");
      return parsed;
    }
  } else if (record.schemaVersion != 1) {
    parsed.error = QStringLiteral("record_unknown_schema");
    return parsed;
  }
  // 路径身份契约（G4-W1）：operationId 恒为 op-<uuid>（两个生成器一致，
  // restoreSeismicTraceResult 同口径）；attempt 为裸 UUID 或 attempt-<uuid>
  // （生产与已发布控制 fixture）。历史合法数据不受影响；篡改形态在此隔离。
  if (!isCanonicalOperationId(record.operationId)) {
    parsed.error = QStringLiteral("record_unsafe_operation_id");
    return parsed;
  }
  if (!record.ownerAttempt.isEmpty() && !isCanonicalAttemptId(record.ownerAttempt)) {
    parsed.error = QStringLiteral("record_unsafe_attempt_id");
    return parsed;
  }
  return parsed;
}

QMutex g_cancelMutex;
QSet<QString> g_cancelledIds;

QMutex &directoryMutex(const QString &path) {
  const QString canonical = QFileInfo(path).absoluteFilePath();
  static QMutex gate;
  static QHash<QString, QSharedPointer<QMutex>> mutexes;
  QMutexLocker lock(&gate);
  auto it = mutexes.find(canonical);
  if (it == mutexes.end())
    it = mutexes.insert(canonical, QSharedPointer<QMutex>::create());
  return *it.value();
}

bool writeJson(const QString &path, const QJsonDocument &document) {
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly))
    return false;
  const QByteArray bytes = document.toJson(QJsonDocument::Compact);
  if(bytes.size()>128LL*1024*1024)return false;
  if (file.write(bytes) != bytes.size())
    return false;
  return file.commit();
}

bool readJson(const QString &path, QJsonDocument &document) {
  QFile file(path);
  document = {};
  if (!file.exists())
    return true;
  if (QFileInfo(path).isSymLink() || file.size() > 128LL * 1024 * 1024)
    return false;
  if (!file.open(QIODevice::ReadOnly))
    return false;
  QJsonParseError error;
  document = QJsonDocument::fromJson(file.readAll(), &error);
  return file.error() == QFileDevice::NoError && error.error == QJsonParseError::NoError;
}

bool terminal(const OperationLog::Record &record) {
  return record.execution == QLatin1String("succeeded") || record.execution == QLatin1String("failed") ||
         record.execution == QLatin1String("cancelled") || record.execution == QLatin1String("prepared");
}

bool canAdvance(const OperationLog::Record &before, const OperationLog::Record &after) {
  if (before.execution == QLatin1String("queued")) {
    if (before.commitState != QLatin1String("not_requested") || after.commitState != QLatin1String("not_requested"))
      return false;
    if (after.execution == QLatin1String("running"))
      return qint64(after.computeCount) == qint64(before.computeCount) + 1;
    return terminal(after) && after.computeCount == before.computeCount;
  }
  if (before.execution != QLatin1String("running") || after.computeCount != before.computeCount)
    return false;
  if (before.commitState == QLatin1String("publishing"))
    return terminal(after) && (after.commitState == QLatin1String("committed") || after.commitState == QLatin1String("failed"));
  return before.commitState == QLatin1String("not_requested") &&
         ((after.execution == QLatin1String("running") && after.commitState == QLatin1String("publishing")) ||
          (terminal(after) && (after.commitState == QLatin1String("not_requested") || after.commitState == QLatin1String("failed"))));
}

// 负数表示关闭。0 表示下一次 operations.json 写入失败。
std::atomic<int> g_operationWriteSkips{-1};

bool failThisOperationWrite() {
  int skip = g_operationWriteSkips.load();
  while (skip >= 0)
    if (g_operationWriteSkips.compare_exchange_weak(skip, skip - 1))
      return skip == 0;
  return false;
}

} // namespace

QMutex &operation_detail::mutexFor(const QString &path) { return directoryMutex(path); }
QString operation_detail::key(const QString &documentId, const QString &callerScope, const QString &clientRequestId) {
  return QString::fromUtf8(QJsonDocument(QJsonArray{documentId, callerScope, clientRequestId}).toJson(QJsonDocument::Compact));
}

ProcessBudgetLedger &ProcessBudgetLedger::instance() {
  static ProcessBudgetLedger ledger;
  return ledger;
}

void ProcessBudgetLedger::setLimit(qint64 bytes) {
  QMutexLocker lock(&m_mutex);
  m_limit = bytes;
}

qint64 ProcessBudgetLedger::limit() const {
  QMutexLocker lock(&m_mutex);
  return m_limit;
}

qint64 ProcessBudgetLedger::reserved() const {
  QMutexLocker lock(&m_mutex);
  return m_reserved;
}

bool ProcessBudgetLedger::tryReserve(qint64 bytes) {
  QMutexLocker lock(&m_mutex);
  if (bytes < 0 || bytes > m_limit - m_reserved)
    return false;
  m_reserved += bytes;
  return true;
}

void ProcessBudgetLedger::release(qint64 bytes) {
  QMutexLocker lock(&m_mutex);
  m_reserved = std::max<qint64>(0, m_reserved - std::max<qint64>(0, bytes));
}

OperationLog::OperationLog(const QString &directory)
  : m_path(QDir(directory).absoluteFilePath(QStringLiteral("operations.json"))),
    m_cachePath(QDir(directory).absoluteFilePath(QStringLiteral("numeric-cache.json"))) {}

OperationLog::OperationLog(const QString &directory, const QString &projectDir,
                           std::shared_ptr<const ProjectWriteClaim> claim)
  : OperationLog(directory) {
  m_projectDir = projectDir; m_claim = std::move(claim);
  const auto relative = QDir(projectDir).relativeFilePath(QFileInfo(directory).absoluteFilePath());
  const auto root = QFileInfo(projectDir).canonicalFilePath();
  if (!root.isEmpty() && QDir::cleanPath(relative) == QLatin1String(".paleo/capability-ops")) {
    m_path = QDir(root).filePath(QStringLiteral(".paleo/capability-ops/operations.json"));
    m_cachePath = QDir(root).filePath(QStringLiteral(".paleo/capability-ops/numeric-cache.json"));
  }
}

bool OperationLog::loadOperations() {
  m_byKey.clear();
  m_idToKey.clear();
  m_quarantinedIds.clear();
  m_quarantinedKeys.clear();
  QJsonDocument document;
  if (!readJson(m_path, document) || (!document.isNull() && !document.isArray()))
    return false;
  const QJsonArray rows = document.array();
  // 第一遍：严格解析。单条坏记录只隔离自身（含身份不明/类型错误/未知
  // schema），不得连坐整本日志，也不得伪装成合法空记录。
  struct Entry { Record record; QString error; };
  QList<Entry> entries;
  QSet<QString> duplicateKeys, duplicateIds, seenKeys, seenIds;
  for (const QJsonValue &value : rows) {
    const auto parsed = recordFromJson(value);
    Entry entry{parsed.record, parsed.error};
    if (entry.error.isEmpty()) {
      const Record &record = entry.record;
      if (record.events.size() > 128 || record.key.isEmpty())
        entry.error = QStringLiteral("record_shape_invalid");
      else if (record.schemaVersion == 2 && (record.stateRevision == 0 || record.documentId.isEmpty() ||
          record.callerScope.isEmpty() || record.clientRequestId.isEmpty() ||
          record.key != operation_detail::key(record.documentId, record.callerScope, record.clientRequestId)))
        entry.error = QStringLiteral("record_identity_mismatch");
    }
    if (entry.error.isEmpty()) {
      if (seenKeys.contains(entry.record.key)) duplicateKeys.insert(entry.record.key);
      else seenKeys.insert(entry.record.key);
      if (seenIds.contains(entry.record.operationId)) duplicateIds.insert(entry.record.operationId);
      else seenIds.insert(entry.record.operationId);
    }
    entries.append(entry);
  }
  // 第二遍：重复 key/operationId 歧义不任取赢家——涉事各条全部隔离。
  for (int i = 0; i < entries.size(); ++i) {
    Entry &entry = entries[i];
    if (!entry.error.isEmpty())
      continue;
    if (duplicateKeys.contains(entry.record.key) || duplicateIds.contains(entry.record.operationId))
      entry.error = QStringLiteral("record_duplicate_identity");
  }
  for (const Entry &entry : entries) {
    if (!entry.error.isEmpty()) {
      // 仅登记可安全显示的形态事实；不回读、不改写原始行（原始
      // operations.json 保持字节不变，坏行只能由人工处置）。
      if (isCanonicalOperationId(entry.record.operationId))
        m_quarantinedIds.insert(entry.record.operationId);
      if (!entry.record.key.isEmpty())
        m_quarantinedKeys.insert(entry.record.key);
      continue;
    }
    m_byKey.insert(entry.record.key, entry.record);
    m_idToKey.insert(entry.record.operationId, entry.record.key);
  }
  return true;
}

void OperationLog::armOperationFileWriteFault(int succeedBeforeFail) {
  g_operationWriteSkips.store(succeedBeforeFail);
}

bool OperationLog::persistOperations() const {
  // 含隔离记录的 journal 一律拒绝覆写（G4-W2 无损纪律）：任何把内存里的
  // 好记录直接写回都等于删除坏行——原始文件只能由人工处置。这里的 const
  // 语义依赖"写路径都在持锁并刚完成 loadOperations 的上下文"这一既有
  // 调用约定；隔离状态是本次读取的结论。
  if (!m_quarantinedIds.isEmpty() || !m_quarantinedKeys.isEmpty())
    return false;
  QJsonArray rows;
  for (const Record &record : m_byKey)
    rows.append(recordJson(record));
  return !failThisOperationWrite() && writeJson(m_path, QJsonDocument(rows));
}

OperationLog::Accept OperationLog::accept(const QString &documentId, const QString &callerScope,
                                          const QString &clientRequestId, const QString &requestHash) {
  QMutexLocker lock(&directoryMutex(m_path));
  Accept result;
  if (!loadOperations()) {
    result.code = QStringLiteral("operation_log_unavailable");
    return result;
  }
  const QString key = QString::fromUtf8(QJsonDocument(QJsonArray{documentId, callerScope, clientRequestId}).toJson(QJsonDocument::Compact));
  if (m_byKey.contains(key)) {
    result.record = m_byKey.value(key);
    result.code = result.record.requestHash == requestHash ? QStringLiteral("replay") : QStringLiteral("conflict");
    return result;
  }
  if (!m_quarantinedIds.isEmpty() || !m_quarantinedKeys.isEmpty()) {
    // 同一业务不得在身份不明的邻居在场时借"not found"二次立项。
    result.code = QStringLiteral("operation_log_quarantined");
    return result;
  }
  Record record;
  record.operationId = QStringLiteral("op-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
  record.key = key;
  record.requestHash = requestHash;
  record.execution = QStringLiteral("queued");
  record.commitState = QStringLiteral("not_requested");
  record.presentation = QStringLiteral("not_requested");
  record.code = QStringLiteral("accepted");
  m_byKey.insert(key, record);
  m_idToKey.insert(record.operationId, key);
  if (!QDir().mkpath(QFileInfo(m_path).absolutePath()) || !persistOperations()) {
    result.code = QStringLiteral("persist_failed");
    return result;
  }
  result.code = QStringLiteral("created");
  result.record = record;
  return result;
}

OperationLog::Record OperationLog::recordById(const QString &operationId) {
  QMutexLocker lock(&directoryMutex(m_path));
  if (!loadOperations())
    return {};
  return m_byKey.value(m_idToKey.value(operationId));
}

OperationLog::RecordLookup OperationLog::lookupRecordById(const QString &operationId) {
  QMutexLocker lock(&directoryMutex(m_path));
  RecordLookup result;
  if (!loadOperations()) {
    result.code = QStringLiteral("operation_log_unavailable");
    return result;
  }
  if (m_idToKey.contains(operationId)) {
    result.code = QStringLiteral("found");
    result.record = m_byKey.value(m_idToKey.value(operationId));
    return result;
  }
  // 区分"目标记录被隔离"与"确实不存在"：前者绝不能触发二次立项或被当作
  // not found 重新接受同一业务请求。
  result.code = m_quarantinedIds.contains(operationId) ? QStringLiteral("operation_record_quarantined")
                                                       : QStringLiteral("operation_not_found");
  return result;
}

OperationLog::JournalHealth OperationLog::journalHealth() {
  QMutexLocker lock(&directoryMutex(m_path));
  JournalHealth health;
  if (!loadOperations()) {
    health.code = QStringLiteral("operation_log_unavailable");
    return health;
  }
  health.quarantined = m_quarantinedIds.size() + m_quarantinedKeys.size();
  health.code = health.quarantined ? QStringLiteral("operation_log_quarantined") : QStringLiteral("ok");
  return health;
}

bool OperationLog::save(const Record &record, Record *current) {
  QMutexLocker lock(&directoryMutex(m_path));
  const auto report = [current](const Record &persisted, bool saved) {
    if (current)
      *current = persisted;
    return saved;
  };
  if (!loadOperations())
    return report({}, false);
  const auto found = m_byKey.constFind(record.key);
  if (found == m_byKey.cend() || found->operationId != record.operationId || found->requestHash != record.requestHash)
    return report({}, false);
  const Record existing = *found;
  if (existing.schemaVersion == 2) return report(existing, false); // v2 requires revision/attempt CAS.
  if (existing == record)
    return report(existing, terminal(existing));
  if (!canAdvance(existing, record))
    return report(existing, false);
  m_byKey.insert(record.key, record);
  if (!persistOperations())
    return report(existing, false);
  return report(record, true);
}

QString OperationLog::cancel(const QString &operationId) {
  QMutexLocker lock(&directoryMutex(m_path));
  if (!loadOperations())
    return QStringLiteral("operation_log_unavailable");
  const QString key = m_idToKey.value(operationId);
  if (!m_byKey.contains(key))
    return m_quarantinedIds.contains(operationId) ? QStringLiteral("operation_record_quarantined")
                                                  : QStringLiteral("unknown_operation");
  if (!m_quarantinedIds.isEmpty() || !m_quarantinedKeys.isEmpty())
    return QStringLiteral("operation_log_quarantined");
  Record record = m_byKey.value(key);
  if (record.commitState == QLatin1String("committed"))
    return QStringLiteral("cancel_after_commit");
  if (record.schemaVersion == 2) return QStringLiteral("durable_coordinator_required");
  if (record.commitState == QLatin1String("publishing"))
    return QStringLiteral("commit_uncertain");
  if (record.execution == QLatin1String("cancelled"))
    return QStringLiteral("cancelled");
  if (record.execution != QLatin1String("queued") && record.execution != QLatin1String("running"))
    return QStringLiteral("already_terminal");
  record.execution = QStringLiteral("cancelled");
  record.code = QStringLiteral("cancelled");
  record.commitState = QStringLiteral("not_requested");
  m_byKey.insert(key, record);
  if (!persistOperations())
    return QStringLiteral("persist_failed");
  QMutexLocker cancelLock(&g_cancelMutex);
  g_cancelledIds.insert(operationId);
  return record.code;
}

QByteArray OperationLog::cacheGet(const QString &fingerprint) {
  QMutexLocker lock(&directoryMutex(m_path));
  QJsonDocument document;
  if (!readJson(m_cachePath, document))
    return {};
  return QByteArray::fromBase64(document.object().value(fingerprint).toString().toLatin1());
}

bool cancelRequested(const QString &operationId) {
  QMutexLocker lock(&g_cancelMutex);
  return g_cancelledIds.contains(operationId);
}

void noteIssuedResult(const QString &documentId, const QString &path) {
  QMutexLocker lock(&g_cancelMutex);
  g_cancelledIds.insert(QStringLiteral("result:") + documentId + QChar(0x1F) + path);
}

bool isIssuedResult(const QString &documentId, const QString &path) {
  QMutexLocker lock(&g_cancelMutex);
  return g_cancelledIds.contains(QStringLiteral("result:") + documentId + QChar(0x1F) + path);
}

void OperationLog::cachePut(const QString &fingerprint, const QByteArray &summaryJson) {
  QMutexLocker lock(&directoryMutex(m_path));
  QJsonDocument document;
  readJson(m_cachePath, document);
  QJsonObject cache = document.object();
  cache.insert(fingerprint, QString::fromLatin1(summaryJson.toBase64()));
  if (QDir().mkpath(QFileInfo(m_cachePath).absolutePath()))
    writeJson(m_cachePath, QJsonDocument(cache));
}

} // namespace paleo::capability
