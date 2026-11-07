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

// 负数表示关闭。0 表示下一次 operations.json 写入失败。
std::atomic<int> g_operationWriteSkips{-1};

bool failThisOperationWrite() {
  const int skip = g_operationWriteSkips.load();
  if (skip < 0)
    return false;
  if (skip == 0) {
    g_operationWriteSkips.store(-1);
    return true;
  }
  g_operationWriteSkips.store(skip - 1);
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

OperationLog::OperationLog(const QString &directory) {
  QDir().mkpath(directory);
  m_path = directory + QStringLiteral("/operations.json");
  m_cachePath = directory + QStringLiteral("/numeric-cache.json");
  load();
}

void OperationLog::load() {
  m_byKey.clear();
  m_idToKey.clear();
  m_cache.clear();
  QFile file(m_path);
  if (file.open(QIODevice::ReadOnly)) {
    const QJsonArray rows = QJsonDocument::fromJson(file.readAll()).array();
    for (const QJsonValue &value : rows) {
      const Record record = recordFromJson(value.toObject());
      if (record.key.isEmpty() || record.operationId.isEmpty())
        continue;
      m_byKey.insert(record.key, record);
      m_idToKey.insert(record.operationId, record.key);
    }
  }
  QFile cache(m_cachePath);
  if (cache.open(QIODevice::ReadOnly)) {
    const QJsonObject object = QJsonDocument::fromJson(cache.readAll()).object();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
      m_cache.insert(it.key(), QByteArray::fromBase64(it.value().toString().toLatin1()));
  }
}

void OperationLog::armOperationFileWriteFault(int succeedBeforeFail) {
  g_operationWriteSkips.store(succeedBeforeFail);
}

bool OperationLog::persist() const {
  QJsonArray rows;
  for (const Record &record : m_byKey)
    rows.append(recordJson(record));
  QJsonObject cache;
  for (auto it = m_cache.constBegin(); it != m_cache.constEnd(); ++it)
    cache.insert(it.key(), QString::fromLatin1(it.value().toBase64()));
  // 回执以 operations.json 为准。这次写入失败时两个文件都保持原样。
  // 回执已经落盘后，缓存文件写失败不再把这次持久化判成失败。
  if (failThisOperationWrite() || !writeJson(m_path, QJsonDocument(rows)))
    return false;
  writeJson(m_cachePath, QJsonDocument(cache));
  return true;
}

OperationLog::Accept OperationLog::accept(const QString &documentId, const QString &callerScope,
                                          const QString &clientRequestId, const QString &requestHash) {
  QMutexLocker lock(&directoryMutex(m_path));
  load();
  Accept result;
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
  if (!persist()) {
    m_byKey.remove(key);
    m_idToKey.remove(record.operationId);
    result.code = QStringLiteral("persist_failed");
    return result;
  }
  result.code = QStringLiteral("created");
  result.record = record;
  return result;
}

OperationLog::Record OperationLog::recordById(const QString &operationId) {
  QMutexLocker lock(&directoryMutex(m_path));
  load();
  return m_byKey.value(m_idToKey.value(operationId));
}

bool OperationLog::save(const Record &record) {
  QMutexLocker lock(&directoryMutex(m_path));
  load();
  if (!m_byKey.contains(record.key))
    return false;
  Record stored = record;
  const Record existing = m_byKey.value(record.key);
  if (existing.execution == QLatin1String("cancelled") && record.commitState != QLatin1String("committed"))
    stored = existing;
  else if (existing.execution == QLatin1String("cancelled") && record.commitState == QLatin1String("committed")) {
    stored.execution = QStringLiteral("cancelled");
    stored.code = QStringLiteral("cancel_after_commit");
    stored.commitState = QStringLiteral("committed");
  }
  m_byKey.insert(stored.key, stored);
  m_idToKey.insert(stored.operationId, stored.key);
  return persist();
}

QString OperationLog::cancel(const QString &operationId) {
  QMutexLocker lock(&directoryMutex(m_path));
  load();
  const QString key = m_idToKey.value(operationId);
  if (!m_byKey.contains(key))
    return QStringLiteral("unknown_operation");
  Record record = m_byKey.value(key);
  if (record.commitState == QLatin1String("committed")) {
    record.code = QStringLiteral("cancel_after_commit");
    m_byKey.insert(key, record);
    persist();
    return record.code;
  }
  if (record.commitState == QLatin1String("publishing")) {
    record.code = QStringLiteral("commit_uncertain");
    m_byKey.insert(key, record);
    persist();
    return record.code;
  }
  {
    QMutexLocker cancelLock(&g_cancelMutex);
    g_cancelledIds.insert(operationId);
  }
  record.execution = QStringLiteral("cancelled");
  record.code = QStringLiteral("cancelled");
  record.commitState = QStringLiteral("not_requested");
  m_byKey.insert(key, record);
  persist();
  return record.code;
}

QByteArray OperationLog::cacheGet(const QString &fingerprint) {
  QMutexLocker lock(&directoryMutex(m_path));
  load();
  return m_cache.value(fingerprint);
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
  load();
  m_cache.insert(fingerprint, summaryJson);
  persist();
}

} // namespace paleo::capability
