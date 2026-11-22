// 层：功能
// workflow/operationdiscovery.cpp — 只读发现：快照、游标、分页与关系投影。
// 快照语义（W4）：一次受权读取（OperationLog::snapshotScope，单锁单次加载）
// 形成不可变摘要快照，后续页一律出自该快照；journal 其后变化不使旧页失效，
// 也不混入新行——需要当前状态时重新发现或调 operation_get。游标经 HMAC
// 绑定快照、位置与 TTL；换文档/作用域/查询条件分别给独立错误，不静默回首页。
#include "operationdiscovery.h"
#include "metadata/operationnamespace.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace paleo::capability {
namespace {

using operation_namespace::isCanonicalOperationId;

// 与既有 events()/operation_read 同族的页预算口径：行区占 max_bytes-512，
// 余量留给 envelope 固定字段；单行超预算明确报 minimumBytes，不截断 ID。
constexpr int kEnvelopeReserve = 512;
constexpr int kCursorMaxChars = 256;
constexpr int kAnchorMaxBytes = 256;
constexpr int kCapabilityMaxBytes = 512;
// 投影循环的协作取消粒度：Qt JSON 解析不可中途抢占（受 128 MiB 输入上限
// 约束的取消延迟），投影阶段逐块自查。
constexpr int kCancelStride = 64;

QJsonObject discoveryFailure(const QString &code) {
  return {{QStringLiteral("contract"), QLatin1String(OperationDiscoveryContract)},
          {QStringLiteral("ok"), false}, {QStringLiteral("code"), code}};
}

bool integerArgument(const QJsonObject &args, const char *key, int fallback, int minimum, int maximum, int &out) {
  out = fallback;
  if (!args.contains(QLatin1String(key)))
    return true;
  const auto value = args.value(QLatin1String(key));
  const double number = value.toDouble();
  if (!value.isDouble() || !std::isfinite(number) || number != std::floor(number) ||
      number < minimum || number > maximum)
    return false;
  out = int(number);
  return true;
}

bool stringArgument(const QJsonObject &args, const char *key, QString &out) {
  const auto value = args.value(QLatin1String(key));
  if (!value.isUndefined() && !value.isString())
    return false;
  out = value.toString();
  return true;
}

// 摘要行：仅取记录里已经存在的安全字段。UUID 不编码时间，事件环首项也
// 不是创建事件——这里不捏造 createdAt/updatedAt；排序键在 coverage 里声明。
QJsonObject summaryRow(const OperationLog::Record &record, int consumers) {
  const auto ports = record.artifactManifest.value(QStringLiteral("ports")).toArray();
  return {{QStringLiteral("operationId"), record.operationId},
          {QStringLiteral("capabilityId"), record.capabilityId},
          {QStringLiteral("contractVersion"), record.contractVersion},
          {QStringLiteral("execution"), record.execution},
          {QStringLiteral("commitState"), record.commitState},
          {QStringLiteral("presentation"), record.presentation},
          {QStringLiteral("revision"), QString::number(record.stateRevision)},
          {QStringLiteral("computeCount"), record.computeCount},
          {QStringLiteral("cancelRequested"), record.cancelRequested},
          {QStringLiteral("requestHash"), record.requestHash},
          {QStringLiteral("resultsAvailable"), !ports.isEmpty()},
          {QStringLiteral("resultPorts"), int(ports.size())},
          {QStringLiteral("sourceOperation"), !record.frozenRequest.value(QStringLiteral("sourceOperationId")).toString().isEmpty()},
          {QStringLiteral("consumers"), consumers},
          {QStringLiteral("planSteps"), int(record.artifactManifest.value(QStringLiteral("childOperations")).toArray().size())},
          {QStringLiteral("parentVersions"), int(record.stagePlan.value(QStringLiteral("parents")).toArray().size())},
          {QStringLiteral("external"), !record.externalRequest.isEmpty()}};
}

struct ChildStep {
  QString nodeId, operationId;
};

struct RelationFacts {
  QString sourceOperationId;
  QVector<ChildStep> childSteps;
  QStringList parentVersions;
};

RelationFacts relationFacts(const OperationLog::Record &record) {
  RelationFacts facts;
  facts.sourceOperationId = record.frozenRequest.value(QStringLiteral("sourceOperationId")).toString();
  for (const auto &entry : record.artifactManifest.value(QStringLiteral("childOperations")).toArray()) {
    const auto object = entry.toObject();
    facts.childSteps.append({object.value(QStringLiteral("nodeId")).toString(),
                             object.value(QStringLiteral("operationId")).toString()});
  }
  for (const auto &value : record.stagePlan.value(QStringLiteral("parents")).toArray())
    facts.parentVersions.append(value.toString());
  return facts;
}

qint64 rowBytes(const QJsonValue &row) {
  return QJsonDocument(row.toObject()).toJson(QJsonDocument::Compact).size() + 64;
}

} // namespace

struct OperationDiscovery::Impl {
  explicit Impl(DiscoveryPolicy value) : policy(value) {
    cursorSecret = QUuid::createUuid().toRfc4122() + QUuid::createUuid().toRfc4122();
  }
  DiscoveryPolicy policy;
  QByteArray cursorSecret;
  std::function<qint64()> clock = [] { return QDateTime::currentMSecsSinceEpoch(); };
  mutable QMutex mutex;

  struct Snapshot {
    QString key, queryDigest;
    QString project, documentId, callerScope, journalDigest;
    bool journalIncomplete = false;
    QJsonArray rows;  // operations=摘要行；relations=预建关系行
    int matched = 0;
    QString sortKey;
    qint64 expiresAt = 0;
    qint64 bytes = 0;
  };
  struct CursorSlot {
    QString snapshotKey;
    qint64 expiresAt = 0;
  };
  QHash<QString, Snapshot> snapshots;
  QHash<QString, CursorSlot> cursors;
  qint64 cachedBytes = 0;
  int scans = 0;

  qint64 now() const { return clock ? clock() : QDateTime::currentMSecsSinceEpoch(); }

  void pruneExpired(const qint64 nowMs) {
    for (auto it = cursors.begin(); it != cursors.end();) {
      if (it->expiresAt < nowMs)
        it = cursors.erase(it);
      else
        ++it;
    }
    for (auto it = snapshots.begin(); it != snapshots.end();) {
      if (it->expiresAt < nowMs) {
        cachedBytes -= it->bytes;
        it = snapshots.erase(it);
      } else
        ++it;
    }
  }

  QString cursorToken(const QString &snapshotKey, int position, qint64 expiresAt) const {
    const QByteArray body = QByteArrayLiteral("opdisc1.") + snapshotKey.toLatin1() + '.' +
                            QByteArray::number(position) + '.' + QByteArray::number(expiresAt);
    const auto mac = QMessageAuthenticationCode::hash(body, cursorSecret, QCryptographicHash::Sha256).toHex();
    return QString::fromLatin1(body + '.' + mac);
  }

  // MAC 常时比较 + 到期区分：坏 MAC 是 cursor_invalid，过期是 cursor_expired
  //（不静默回第一页）。位置/到期解析失败归 invalid。
  QString validateCursor(const QString &cursor, const qint64 nowMs, QString *snapshotKey, int *position) const {
    if (cursor.size() > kCursorMaxChars)
      return QStringLiteral("cursor_invalid");
    const auto parts = cursor.toLatin1().split('.');
    if (parts.size() != 5 || parts[0] != "opdisc1" || parts[4].size() != 64)
      return QStringLiteral("cursor_invalid");
    const auto body = parts[0] + '.' + parts[1] + '.' + parts[2] + '.' + parts[3];
    const auto mac = QMessageAuthenticationCode::hash(body, cursorSecret, QCryptographicHash::Sha256).toHex();
    unsigned difference = 0;
    for (int i = 0; i < mac.size(); ++i)
      difference |= uint(uchar(mac.at(i))) ^ uint(uchar(parts[4].at(i)));
    if (difference)
      return QStringLiteral("cursor_invalid");
    bool positionOk = false, expiryOk = false;
    const int parsedPosition = parts[2].toInt(&positionOk);
    const qint64 expiry = parts[3].toLongLong(&expiryOk);
    if (!expiryOk || expiry < nowMs)
      return QStringLiteral("cursor_expired");
    if (!positionOk || parsedPosition < 0)
      return QStringLiteral("cursor_invalid");
    if (!cursors.contains(cursor))
      return QStringLiteral("cursor_expired");
    *snapshotKey = QString::fromLatin1(parts[1]);
    *position = parsedPosition;
    return {};
  }

  // 查询身份：绑定视图/筛选/锚点/页预算（缺省归一），与 ProjectQuery 的
  // queryDigest 同口径——续页换条件即 cursor_query_mismatch，不混页。
  // 页缺省与工具声明（operationdiscoverytools.cpp）及上方校验同值。
  QString queryDigest(const QJsonObject &args) const {
    QJsonObject canonical = args;
    canonical.remove(QStringLiteral("cursor"));
    if (!canonical.contains(QStringLiteral("view")))
      canonical.insert(QStringLiteral("view"), QStringLiteral("operations"));
    if (!canonical.contains(QStringLiteral("page_size")))
      canonical.insert(QStringLiteral("page_size"), 16.0);
    if (!canonical.contains(QStringLiteral("max_bytes")))
      canonical.insert(QStringLiteral("max_bytes"), 8192.0);
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(canonical).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
  }

  // 关系行（W5）：直接持久目标必须是快照内同作用域 v2 记录；其余目标只给
  // 不泄漏身份的未解析事实。计划内步骤保留 nodeId/记录的 operationId 与
  // 来源类别，但明确标注不可据此独立 get/resume；父版本是 catalog 版本 ID，
  // 不是 operationId。次序固定：source_operation → consumer（按 operationId）
  // → plan_step（记录序）→ parent_version（记录序）。
  QJsonArray relationRows(const QVector<OperationLog::Record> &records, const QHash<QString, int> &rowIndex,
                          const QString &anchorId) {
    const auto anchor = std::find_if(records.cbegin(), records.cend(),
                                     [&](const OperationLog::Record &r) { return r.operationId == anchorId; });
    if (anchor == records.cend() || !rowIndex.contains(anchorId))
      return {};
    QJsonArray rows;
    const auto facts = relationFacts(*anchor);
    const auto appendTarget = [&rows](const QString &relation, const OperationLog::Record *target) {
      QJsonObject row;
      row.insert(QStringLiteral("relation"), relation);
      // 解析不到（他域/隔离/v1/不存在）时只给状态，不回显目标 ID。
      row.insert(QStringLiteral("target"), target ? summaryRow(*target, 0)
          : QJsonObject{{QStringLiteral("status"), QStringLiteral("unresolved_in_snapshot")}});
      rows.append(row);
    };
    if (!facts.sourceOperationId.isEmpty()) {
      const auto found = rowIndex.constFind(facts.sourceOperationId);
      appendTarget(QStringLiteral("source_operation"),
                   found != rowIndex.cend() ? &records.at(found.value()) : nullptr);
    }
    for (const auto &record : records) {
      if (record.frozenRequest.value(QStringLiteral("sourceOperationId")).toString() == anchorId)
        appendTarget(QStringLiteral("consumer"), &record);
    }
    for (const auto &step : facts.childSteps)
      rows.append(QJsonObject{{QStringLiteral("relation"), QStringLiteral("plan_step")},
          {QStringLiteral("nodeId"), step.nodeId},
          {QStringLiteral("operationId"), isCanonicalOperationId(step.operationId) ? QJsonValue(step.operationId) : QJsonValue()},
          {QStringLiteral("source"), QStringLiteral("artifact-manifest@1")},
          {QStringLiteral("access"), QStringLiteral("plan_step_not_independently_accessible")},
          {QStringLiteral("kind"), QStringLiteral("plan_step")}});
    for (const auto &version : facts.parentVersions)
      rows.append(QJsonObject{{QStringLiteral("relation"), QStringLiteral("parent_version")},
          {QStringLiteral("versionId"), version},
          {QStringLiteral("kind"), QStringLiteral("catalog_version")},
          {QStringLiteral("note"), QStringLiteral("catalog version id; not an operationId")}});
    return rows;
  }
};

OperationDiscovery::OperationDiscovery(DiscoveryPolicy policy) : m(std::make_unique<Impl>(std::move(policy))) {}
OperationDiscovery::~OperationDiscovery() = default;

void OperationDiscovery::invalidate() {
  QMutexLocker lock(&m->mutex);
  m->snapshots.clear();
  m->cursors.clear();
  m->cachedBytes = 0;
}

void OperationDiscovery::setClock(std::function<qint64()> clock) {
  QMutexLocker lock(&m->mutex);
  m->clock = std::move(clock);
}

int OperationDiscovery::scanCount() const {
  QMutexLocker lock(&m->mutex);
  return m->scans;
}
qint64 OperationDiscovery::retainedBytes() const {
  QMutexLocker lock(&m->mutex);
  return m->cachedBytes;
}
int OperationDiscovery::liveSnapshots() const {
  QMutexLocker lock(&m->mutex);
  return int(m->snapshots.size());
}
int OperationDiscovery::liveCursors() const {
  QMutexLocker lock(&m->mutex);
  return int(m->cursors.size());
}

QJsonObject OperationDiscovery::query(const DiscoveryScan &scan, const QJsonObject &arguments,
                                      const std::function<bool()> &cancelled) {
  // 1) 实参先于磁盘严格校验（W6）：只接受已声明筛选；未声明键不参与任何
  //    读取决策（其存在改变 queryDigest，续页自然拒绝）。
  QString view, anchor, stateFilter, capabilityFilter, cursor;
  if (!stringArgument(arguments, "view", view) || !stringArgument(arguments, "anchor_operation_id", anchor) ||
      !stringArgument(arguments, "execution_state", stateFilter) ||
      !stringArgument(arguments, "capability_id", capabilityFilter) || !stringArgument(arguments, "cursor", cursor))
    return discoveryFailure(QStringLiteral("invalid_argument_type"));
  if (view.isEmpty())
    view = QStringLiteral("operations");
  if (scan.documentId.isEmpty() || scan.callerScope.isEmpty() || scan.documentId.toUtf8().size() > 256 ||
      scan.callerScope.toUtf8().size() > 512 || scan.projectDir.isEmpty())
    return discoveryFailure(QStringLiteral("discovery_scope_invalid"));
  const bool relations = view == QStringLiteral("relations");
  if (!relations && view != QStringLiteral("operations"))
    return discoveryFailure(QStringLiteral("invalid_discovery_view"));
  if (!stateFilter.isEmpty() &&
      !QStringList{QStringLiteral("queued"),   QStringLiteral("running"),    QStringLiteral("prepared"),
                   QStringLiteral("succeeded"), QStringLiteral("failed"),    QStringLiteral("cancelled"),
                   QStringLiteral("interrupted"), QStringLiteral("external_unknown")}.contains(stateFilter))
    return discoveryFailure(QStringLiteral("invalid_execution_state_filter"));
  if (capabilityFilter.toUtf8().size() > kCapabilityMaxBytes)
    return discoveryFailure(QStringLiteral("invalid_capability_filter"));
  if (relations && (anchor.toUtf8().size() > kAnchorMaxBytes || !isCanonicalOperationId(anchor)))
    return discoveryFailure(QStringLiteral("invalid_anchor_operation"));
  int pageSize = 0, maxBytes = 0;
  if (!integerArgument(arguments, "page_size", 16, 1, 32, pageSize) ||
      !integerArgument(arguments, "max_bytes", 8192, 2048, 24576, maxBytes))
    return discoveryFailure(QStringLiteral("invalid_page_budget"));

  QMutexLocker lock(&m->mutex);
  const auto nowMs = m->now();
  m->pruneExpired(nowMs);
  const QString digest = m->queryDigest(arguments);

  // 2) 定位快照：cursor 续页（不可变快照内取页）或新扫描。
  QString snapshotKey;
  int position = 0;
  if (!cursor.isEmpty()) {
    const auto invalid = m->validateCursor(cursor, nowMs, &snapshotKey, &position);
    if (!invalid.isEmpty())
      return discoveryFailure(invalid);
    const auto found = m->snapshots.constFind(snapshotKey);
    if (found == m->snapshots.cend() || found->expiresAt < nowMs)
      return discoveryFailure(QStringLiteral("cursor_expired"));
    if (found->documentId != scan.documentId || found->callerScope != scan.callerScope ||
        found->project != scan.projectDir)
      return discoveryFailure(QStringLiteral("cursor_scope_mismatch"));
    if (found->queryDigest != digest)
      return discoveryFailure(QStringLiteral("cursor_query_mismatch"));
    if (position > found->rows.size())
      return discoveryFailure(QStringLiteral("cursor_invalid"));
    snapshotKey = found.key(); // 快照已定位且本调用内不再变更 snapshots。
  } else {
    // 新查询：一次受权读取（W2/W3）。扫描在 worker 线程；OperationLog 的
    // 目录互斥与既有 get/events 读路径同口径——每快照一次，不逐页重扫。
    if (cancelled && cancelled())
      return discoveryFailure(QStringLiteral("cancelled"));
    OperationLog log(scan.operationDir, scan.projectDir, scan.writeClaim);
    const auto scoped = log.snapshotScope(scan.documentId, scan.callerScope);
    ++m->scans;
    if (scoped.code != QLatin1String("ok"))
      return discoveryFailure(QStringLiteral("operation_log_unavailable"));

    QHash<QString, int> consumers; // operationId -> 同 scope 直接消费者数
    QHash<QString, int> rowIndex;
    for (int i = 0; i < scoped.records.size(); ++i) {
      const auto &record = scoped.records.at(i);
      rowIndex.insert(record.operationId, i);
      const auto source = record.frozenRequest.value(QStringLiteral("sourceOperationId")).toString();
      if (!source.isEmpty())
        consumers[source] = consumers.value(source, 0) + 1;
    }

    Impl::Snapshot built;
    built.key = QUuid::createUuid().toString(QUuid::WithoutBraces);
    built.queryDigest = digest;
    built.project = scan.projectDir;
    built.documentId = scan.documentId;
    built.callerScope = scan.callerScope;
    built.journalDigest = QString::fromLatin1(scoped.journalDigest.toHex());
    built.journalIncomplete = scoped.quarantinedRows > 0;
    built.expiresAt = nowMs + m->policy.ttlMs;

    if (relations) {
      if (!rowIndex.contains(anchor)) {
        // 与 lookupRecordById 同口径区分隔离与不存在（G4 语义）；两者都不
        // 启动关系分页，也不泄漏他人条目。
        return discoveryFailure(scoped.quarantinedIds.contains(anchor)
                                    ? QStringLiteral("operation_record_quarantined")
                                    : QStringLiteral("operation_not_found"));
      }
      built.rows = m->relationRows(scoped.records, rowIndex, anchor);
      built.sortKey = QStringLiteral("relation-kind@1");
    } else {
      for (int i = 0; i < scoped.records.size(); ++i) {
        if (cancelled && (i % kCancelStride) == 0 && cancelled())
          return discoveryFailure(QStringLiteral("cancelled"));
        const auto &record = scoped.records.at(i);
        if (!stateFilter.isEmpty() && record.execution != stateFilter)
          continue;
        if (!capabilityFilter.isEmpty() && record.capabilityId != capabilityFilter)
          continue;
        built.rows.append(summaryRow(record, consumers.value(record.operationId, 0)));
      }
      built.sortKey = QStringLiteral("operationId-lex@1");
    }
    built.matched = int(built.rows.size());
    qint64 bytes = 512;
    for (const auto &row : built.rows)
      bytes += rowBytes(row);
    built.bytes = bytes;
    // 缓存满给结构化失败（W3）：只回收过期项，不挤掉仍有效的分页承诺。
    if (m->snapshots.size() >= m->policy.maxSnapshots || m->cachedBytes + bytes > m->policy.maxCacheBytes)
      return discoveryFailure(QStringLiteral("discovery_cache_full"));
    snapshotKey = built.key;
    m->cachedBytes += bytes;
    m->snapshots.insert(snapshotKey, std::move(built));
    position = 0;
  }
  const auto &snapshot = m->snapshots.value(snapshotKey);

  // 3) 从不可变快照取页：单行超预算报 minimumBytes，不截断 ID、不静默丢条。
  QJsonArray page;
  int index = position;
  for (; index < snapshot.rows.size(); ++index) {
    auto candidate = page;
    candidate.append(snapshot.rows.at(index));
    const auto bytes = QJsonDocument(candidate).toJson(QJsonDocument::Compact).size();
    if (page.isEmpty() && bytes > maxBytes - kEnvelopeReserve) {
      QJsonObject failure = discoveryFailure(QStringLiteral("page_budget_too_small"));
      failure.insert(QStringLiteral("minimumBytes"), bytes + kEnvelopeReserve);
      return failure;
    }
    if (int(candidate.size()) > pageSize || bytes > maxBytes - kEnvelopeReserve)
      break;
    page = candidate;
  }
  const bool hasMore = index < snapshot.rows.size();
  QJsonObject result;
  result.insert(QStringLiteral("contract"), QLatin1String(OperationDiscoveryContract));
  result.insert(QStringLiteral("ok"), true);
  result.insert(QStringLiteral("code"), QStringLiteral("ok"));
  result.insert(QStringLiteral("view"), relations ? QStringLiteral("relations") : QStringLiteral("operations"));
  result.insert(relations ? QStringLiteral("relations") : QStringLiteral("operations"), page);
  if (relations)
    result.insert(QStringLiteral("anchorOperationId"), anchor);
  result.insert(QStringLiteral("returnedCount"), int(page.size()));
  result.insert(QStringLiteral("hasMore"), hasMore);
  result.insert(QStringLiteral("nextCursor"), QJsonValue::Null);
  if (hasMore) {
    if (int(m->cursors.size()) >= m->policy.maxCursors)
      return discoveryFailure(QStringLiteral("cursor_capacity_exceeded"));
    const auto expiry = m->now() + m->policy.ttlMs;
    const QString next = m->cursorToken(snapshot.key, index, expiry);
    result.insert(QStringLiteral("nextCursor"), next);
    m->cursors.insert(next, Impl::CursorSlot{snapshot.key, expiry});
  }
  result.insert(QStringLiteral("coverage"), QJsonObject{
      {QStringLiteral("documentId"), scan.documentId},
      {QStringLiteral("journalDigest"), snapshot.journalDigest},
      {QStringLiteral("journalHealth"), snapshot.journalIncomplete ? QStringLiteral("incomplete") : QStringLiteral("ok")},
      {QStringLiteral("snapshotPolicy"), QStringLiteral("immutable-snapshot@1")},
      {QStringLiteral("sort"), snapshot.sortKey},
      {QStringLiteral("matched"), snapshot.matched},
      {QStringLiteral("pageComplete"), !hasMore}});
  result.insert(QStringLiteral("note"), QStringLiteral(
      "Recorded journal facts from one immutable read; query paleo.operation_get for reconciled current state."));
  if (QJsonDocument(result).toJson(QJsonDocument::Compact).size() > maxBytes) {
    if (hasMore)
      m->cursors.remove(result.value(QStringLiteral("nextCursor")).toString());
    return discoveryFailure(QStringLiteral("discovery_response_budget_exceeded"));
  }
  return result;
}

} // namespace paleo::capability
