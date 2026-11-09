// 层：功能
// workflow/capabilityops.cpp — 持久 operation 身份、数值缓存与进程内预算。
#include "capabilitygateway.h"

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
  return object;
}

OperationLog::Record recordFromJson(const QJsonObject &object) {
  OperationLog::Record record;
  record.operationId = object.value(QStringLiteral("operationId")).toString();
  record.key = object.value(QStringLiteral("key")).toString();
  record.requestHash = object.value(QStringLiteral("requestHash")).toString();
  record.execution = object.value(QStringLiteral("execution")).toString();
  record.commitState = object.value(QStringLiteral("commitState")).toString();
  record.presentation = object.value(QStringLiteral("presentation")).toString();
  record.code = object.value(QStringLiteral("code")).toString();
  record.computeCount = object.value(QStringLiteral("computeCount")).toInt();
  record.summaryJson = QByteArray::fromBase64(object.value(QStringLiteral("summary")).toString().toLatin1());
  return record;
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
  if (file.write(bytes) != bytes.size())
    return false;
  return file.commit();
}

bool readJson(const QString &path, QJsonDocument &document) {
  QFile file(path);
  document = {};
  if (!file.exists())
    return true;
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

bool OperationLog::loadOperations() {
  m_byKey.clear();
  m_idToKey.clear();
  QJsonDocument document;
  if (!readJson(m_path, document) || (!document.isNull() && !document.isArray()))
    return false;
  const QJsonArray rows = document.array();
  for (const QJsonValue &value : rows) {
    const Record record = recordFromJson(value.toObject());
    if (record.key.isEmpty() || record.operationId.isEmpty() || m_byKey.contains(record.key) || m_idToKey.contains(record.operationId))
      return false;
    m_byKey.insert(record.key, record);
    m_idToKey.insert(record.operationId, record.key);
  }
  return true;
}

void OperationLog::armOperationFileWriteFault(int succeedBeforeFail) {
  g_operationWriteSkips.store(succeedBeforeFail);
}

bool OperationLog::persistOperations() const {
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
    return QStringLiteral("unknown_operation");
  Record record = m_byKey.value(key);
  if (record.commitState == QLatin1String("committed"))
    return QStringLiteral("cancel_after_commit");
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
