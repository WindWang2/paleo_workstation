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
#include <limits>

namespace paleo::capability {
namespace {

using operation_namespace::isCanonicalOperationId;

// 与既有 events()/operation_read 同族的页预算口径：行区占 max_bytes 减去
// envelope 余量（coverage+note+documentId+snapshotDigest+游标最坏 ~0.9 KiB，
// 取 1 KiB）；单行超预算明确报 minimumBytes，不截断 ID。
constexpr int kEnvelopeReserve = 1024;
constexpr int kCursorMaxChars = 256;
constexpr int kAnchorMaxBytes = 256;
constexpr int kCapabilityMaxBytes = 512;
// 请求体上限（与 ProjectQuery 的 query_request_budget 同值；runner 另有单次
// 调用 16 KiB 实参预算）。
constexpr int kRequestMaxBytes = 16 * 1024;
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

// 已声明请求的规范形式。validateRequestValue 是纯函数：无共享状态、无磁盘、
// 无锁——宿主在创建任务之前调用它，无效条件不占任务槽；query() 入口再校验
// 一次（纵深防御，成本可忽略）。只接受已声明筛选；未声明键不参与任何读取
// 决策（其存在改变 queryDigest，续页自然拒绝）。游标在此只查形态，MAC/到期/
// 作用域绑定在服务内校验（该路径不读盘）。
struct DiscoveryRequest {
  QString view, anchor, stateFilter, capabilityFilter, cursor;
  bool relations = false;
  int pageSize = 0, maxBytes = 0;
};

QString validateRequestValue(const QJsonObject &arguments, DiscoveryRequest &out, QString *field) {
  const auto fail = [field](const QString &code, const char *name) {
    if (field)
      *field = QLatin1String(name);
    return code;
  };
  if (QJsonDocument(arguments).toJson(QJsonDocument::Compact).size() > kRequestMaxBytes)
    return fail(QStringLiteral("discovery_request_budget_exceeded"), "");
  if (!stringArgument(arguments, "view", out.view) ||
      !stringArgument(arguments, "anchor_operation_id", out.anchor) ||
      !stringArgument(arguments, "execution_state", out.stateFilter) ||
      !stringArgument(arguments, "capability_id", out.capabilityFilter) ||
      !stringArgument(arguments, "cursor", out.cursor))
    return fail(QStringLiteral("invalid_argument_type"), "");
  out.relations = out.view == QStringLiteral("relations");
  // view 键出现就必须取精确词表值（缺省=未提供键）；空串不给隐式缺省。
  const bool hasView = arguments.contains(QStringLiteral("view"));
  if ((hasView && out.view.isEmpty()) || (!out.relations && hasView && out.view != QStringLiteral("operations")))
    return fail(QStringLiteral("invalid_discovery_view"), "view");
  if (!out.stateFilter.isEmpty() &&
      !QStringList{QStringLiteral("queued"),   QStringLiteral("running"),    QStringLiteral("prepared"),
                   QStringLiteral("succeeded"), QStringLiteral("failed"),    QStringLiteral("cancelled"),
                   QStringLiteral("interrupted"), QStringLiteral("external_unknown")}.contains(out.stateFilter))
    return fail(QStringLiteral("invalid_execution_state_filter"), "execution_state");
  if (out.capabilityFilter.toUtf8().size() > kCapabilityMaxBytes)
    return fail(QStringLiteral("invalid_capability_filter"), "capability_id");
  // 筛选只属于 operations 视图：与 relations 组合显式拒绝，不静默忽略。
  if (out.relations && (!out.stateFilter.isEmpty() || !out.capabilityFilter.isEmpty()))
    return fail(QStringLiteral("invalid_filter_for_view"),
                out.stateFilter.isEmpty() ? "capability_id" : "execution_state");
  if (out.relations && (out.anchor.toUtf8().size() > kAnchorMaxBytes || !isCanonicalOperationId(out.anchor)))
    return fail(QStringLiteral("invalid_anchor_operation"), "anchor_operation_id");
  if (out.cursor.size() > kCursorMaxChars)
    return fail(QStringLiteral("cursor_invalid"), "cursor");
  if (!integerArgument(arguments, "page_size", 16, 1, 32, out.pageSize))
    return fail(QStringLiteral("invalid_page_budget"), "page_size");
  if (!integerArgument(arguments, "max_bytes", 8192, 2048, 24576, out.maxBytes))
    return fail(QStringLiteral("invalid_page_budget"), "max_bytes");
  return {};
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
    instanceTag = QUuid::createUuid().toRfc4122().left(4).toHex();
  }
  DiscoveryPolicy policy;
  QByteArray cursorSecret;
  // 公开随机标记（非密钥）：标明游标所属的服务实例。会话/宿主失效会重建服务
  // （新密钥），旧实例的游标据此判为"不再有效"（cursor_expired）而非"传错了"。
  QByteArray instanceTag;
  std::function<qint64()> clock = [] { return QDateTime::currentMSecsSinceEpoch(); };
  mutable QMutex mutex;

  struct Snapshot {
    QString key, queryDigest;
    QString project, documentId, callerScope, snapshotDigest;
    bool journalIncomplete = false;
    QJsonArray rows;  // operations=摘要行；relations=预建关系行
    int matched = 0;
    QString sortKey;
    qint64 expiresAt = 0;
    qint64 bytes = 0;
    quint64 useSeq = 0; // LRU：单调使用序号（墙钟同毫秒会打平，序号严格可判）
  };
  struct CursorSlot {
    QString snapshotKey;
    qint64 expiresAt = 0;
    bool consumed = false; // false = 已发给模型且尚未被消费的续页（分页承诺）
  };
  QHash<QString, Snapshot> snapshots;
  QHash<QString, CursorSlot> cursors;
  qint64 cachedBytes = 0;
  quint64 useCounter = 0; // 见 Snapshot::useSeq
  int scans = 0;
  quint64 generation = 0; // invalidate() 递增：在途扫描不得把旧结果登记回缓存

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
    const QByteArray body = QByteArrayLiteral("opdisc1.") + instanceTag + '.' + snapshotKey.toLatin1() + '.' +
                            QByteArray::number(position) + '.' + QByteArray::number(expiresAt);
    const auto mac = QMessageAuthenticationCode::hash(body, cursorSecret, QCryptographicHash::Sha256).toHex();
    return QString::fromLatin1(body + '.' + mac);
  }

  // 形态：opdisc1.<instanceTag>.<snapshotKey>.<position>.<expiry>.<HMAC>。
  // 实例标记不符 = 游标来自已失效/重建的服务实例 → cursor_expired（对模型而言
  // 是"不再有效、请重新发现"，不是"你传错了"）。MAC 常时比较：坏 MAC 是
  // cursor_invalid；过期/未登记是 cursor_expired；均不静默回第一页。
  QString validateCursor(const QString &cursor, const qint64 nowMs, QString *snapshotKey, int *position) const {
    if (cursor.size() > kCursorMaxChars)
      return QStringLiteral("cursor_invalid");
    const auto parts = cursor.toLatin1().split('.');
    if (parts.size() != 6 || parts[0] != "opdisc1" || parts[5].size() != 64)
      return QStringLiteral("cursor_invalid");
    if (parts[1] != instanceTag)
      return QStringLiteral("cursor_expired");
    const auto body = parts[0] + '.' + parts[1] + '.' + parts[2] + '.' + parts[3] + '.' + parts[4];
    const auto mac = QMessageAuthenticationCode::hash(body, cursorSecret, QCryptographicHash::Sha256).toHex();
    unsigned difference = 0;
    for (int i = 0; i < mac.size(); ++i)
      difference |= uint(uchar(mac.at(i))) ^ uint(uchar(parts[5].at(i)));
    if (difference)
      return QStringLiteral("cursor_invalid");
    bool positionOk = false, expiryOk = false;
    const int parsedPosition = parts[3].toInt(&positionOk);
    const qint64 expiry = parts[4].toLongLong(&expiryOk);
    if (!expiryOk || expiry < nowMs)
      return QStringLiteral("cursor_expired");
    if (!positionOk || parsedPosition < 0)
      return QStringLiteral("cursor_invalid");
    if (!cursors.contains(cursor))
      return QStringLiteral("cursor_expired");
    *snapshotKey = QString::fromLatin1(parts[2]);
    *position = parsedPosition;
    return {};
  }

  // 该快照是否仍有未消费的续页游标——即对模型的分页承诺。
  bool hasForwardCursor(const QString &snapshotKey) const {
    for (const auto &slot : cursors)
      if (slot.snapshotKey == snapshotKey && !slot.consumed)
        return true;
    return false;
  }

  void dropSnapshot(const QString &snapshotKey) {
    for (auto it = cursors.begin(); it != cursors.end();) {
      if (it->snapshotKey == snapshotKey)
        it = cursors.erase(it);
      else
        ++it;
    }
    const auto found = snapshots.constFind(snapshotKey);
    if (found != snapshots.cend()) {
      cachedBytes -= found->bytes;
      snapshots.erase(found);
    }
  }

  // 容量不足时腾位（调用方已持锁）。淘汰优先级：① 其他 callerScope 的快照
  // （会话切换后的遗留；作用域绑定本就禁止续用）；② 本作用域已无未消费续页
  // 的快照（已走完/被放弃/首页失败）；③ 本作用域最久未使用的在途分页。同档
  // 按 LRU。③ 是有意的：典型用法是"list 第一页挑到 ID 就去调 get、不再翻页"，
  // 若把在途分页当作不可淘汰的承诺，大作用域里第 5 次 list 就会失败。被淘汰
  // 的在途游标之后得到明确的 cursor_expired（不静默、不回首页、不混页）。
  // 只有新快照自身放不进字节上限（或份数上限为 0）才腾不出——此时不做无谓
  // 淘汰，直接给结构化失败并附 retryAfterMs（现存快照最早到期）。返回空表示
  // 已腾出。
  QString makeRoom(const QString &callerScope, qint64 bytes, qint64 nowMs, qint64 *retryAfterMs) {
    const auto over = [&] {
      return snapshots.size() >= policy.maxSnapshots || cachedBytes + bytes > policy.maxCacheBytes;
    };
    if (!over())
      return {};
    if (policy.maxSnapshots < 1 || bytes > policy.maxCacheBytes) {
      qint64 earliest = std::numeric_limits<qint64>::max();
      for (const auto &snapshot : snapshots)
        earliest = qMin(earliest, snapshot.expiresAt - nowMs);
      *retryAfterMs = earliest == std::numeric_limits<qint64>::max() ? 0 : qMax<qint64>(0, earliest);
      return QStringLiteral("discovery_cache_full");
    }
    for (int tier = 0; tier < 3 && over(); ++tier) {
      // 同档内逐个淘汰最久未使用者，不再超限即停；该档耗尽则进入下一档。
      while (over()) {
        QString candidate;
        quint64 oldest = std::numeric_limits<quint64>::max();
        for (auto it = snapshots.cbegin(); it != snapshots.cend(); ++it) {
          const bool otherScope = it->callerScope != callerScope;
          const bool skip = tier == 0    ? !otherScope
                            : tier == 1 ? (otherScope || hasForwardCursor(it.key()))
                                        : false;
          if (skip)
            continue;
          if (candidate.isEmpty() || it->useSeq < oldest) {
            oldest = it->useSeq;
            candidate = it.key();
          }
        }
        if (candidate.isEmpty())
          break;
        dropSnapshot(candidate);
      }
    }
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
                          const QHash<QString, int> &consumers, const QString &anchorId) {
    const auto anchor = std::find_if(records.cbegin(), records.cend(),
                                     [&](const OperationLog::Record &r) { return r.operationId == anchorId; });
    if (anchor == records.cend() || !rowIndex.contains(anchorId))
      return {};
    QJsonArray rows;
    const auto facts = relationFacts(*anchor);
    const auto appendTarget = [&rows, &consumers](const QString &relation, const OperationLog::Record *target) {
      QJsonObject row;
      row.insert(QStringLiteral("relation"), relation);
      // 解析不到（他域/隔离/v1/不存在）时只给状态，不回显目标 ID。
      row.insert(QStringLiteral("target"), target ? summaryRow(*target, consumers.value(target->operationId, 0))
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
  ++m->generation;
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

QString OperationDiscovery::validateRequest(const QJsonObject &arguments, QString *field) {
  DiscoveryRequest request;
  return validateRequestValue(arguments, request, field);
}

QJsonObject OperationDiscovery::query(const DiscoveryScan &scan, const QJsonObject &arguments,
                                      const std::function<bool()> &cancelled) {
  // 1) 实参先于磁盘严格校验（W6）：宿主已在建任务前调用 validateRequest 拒绝
  //    无效条件；此处为纵深防御，保证直调服务的调用方同样不越界。
  DiscoveryRequest request;
  if (const auto invalid = validateRequestValue(arguments, request, nullptr); !invalid.isEmpty())
    return discoveryFailure(invalid);
  const auto &anchor = request.anchor;
  const auto &stateFilter = request.stateFilter;
  const auto &capabilityFilter = request.capabilityFilter;
  const auto &cursor = request.cursor;
  const bool relations = request.relations;
  const int pageSize = request.pageSize, maxBytes = request.maxBytes;
  if (scan.documentId.isEmpty() || scan.callerScope.isEmpty() || scan.documentId.toUtf8().size() > 256 ||
      scan.callerScope.toUtf8().size() > 512 || scan.projectDir.isEmpty())
    return discoveryFailure(QStringLiteral("discovery_scope_invalid"));

  const QString digest = m->queryDigest(arguments); // 纯函数，无共享状态。

  // 2) cursor 续页：定位快照并整页在互斥内完成（纯内存，无磁盘）。
  if (!cursor.isEmpty()) {
    QMutexLocker lock(&m->mutex);
    return pageFromCursor(scan, digest, cursor, relations, anchor, pageSize, maxBytes);
  }

  // 3) 新查询：磁盘扫描在互斥外执行（W3：不让大 journal 读解析阻塞
  //    invalidate/续页/其他查询）；OperationLog 自身的目录互斥与既有
  //    get/events 读路径同口径——每快照一次，不逐页重扫。
  if (cancelled && cancelled())
    return discoveryFailure(QStringLiteral("cancelled"));
  quint64 generation = 0;
  {
    QMutexLocker lock(&m->mutex);
    generation = m->generation;
  }
  // OperationLog 与既有 get/results 一样拿宿主原值路径（词法形态一致，受管路径
  // 检查才成立）；canonical 只用于快照身份与归属比对。
  OperationLog log(scan.operationDir, scan.hostProjectDir, scan.writeClaim);
  const auto scoped = log.snapshotScope(scan.documentId, scan.callerScope);
  {
    QMutexLocker lock(&m->mutex);
    ++m->scans;
  }
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
  if (relations && !rowIndex.contains(anchor)) {
    // 与 lookupRecordById 同口径区分隔离与不存在（G4 语义）；两者都不
    // 启动关系分页，也不泄漏他人条目。
    return discoveryFailure(scoped.quarantinedIds.contains(anchor)
                                ? QStringLiteral("operation_record_quarantined")
                                : QStringLiteral("operation_not_found"));
  }

  Impl::Snapshot built;
  built.key = QUuid::createUuid().toString(QUuid::WithoutBraces);
  built.queryDigest = digest;
  built.project = scan.projectDir;
  built.documentId = scan.documentId;
  built.callerScope = scan.callerScope;
  built.snapshotDigest = QString::fromLatin1(scoped.scopedDigest.toHex());
  built.journalIncomplete = scoped.quarantinedRows > 0;
  if (relations) {
    built.rows = m->relationRows(scoped.records, rowIndex, consumers, anchor);
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

  // 首行即超页预算：登记之前就能判定——不占槽，也不为注定失败的页淘汰别人
  // 的在途分页（口径与 pageFromSnapshotLocked 的单行判定相同）。
  if (!built.rows.isEmpty()) {
    const auto firstRowBytes = QJsonDocument(QJsonArray{built.rows.first()}).toJson(QJsonDocument::Compact).size();
    if (firstRowBytes > maxBytes - kEnvelopeReserve) {
      QJsonObject failure = discoveryFailure(QStringLiteral("page_budget_too_small"));
      failure.insert(QStringLiteral("minimumBytes"), firstRowBytes + kEnvelopeReserve);
      return failure;
    }
  }

  QMutexLocker lock(&m->mutex);
  const auto nowMs = m->now();
  m->pruneExpired(nowMs);
  // 扫描期间发生 invalidate（会话/工程切换）：旧作用域的快照不得登记回
  // 新上下文的缓存——完成侧另有归属复核，这里挡住缓存复活。
  if (m->generation != generation)
    return discoveryFailure(QStringLiteral("stale_session"));
  built.expiresAt = nowMs + m->policy.ttlMs;
  built.useSeq = ++m->useCounter;
  // 容量（W3）：按优先级淘汰（他会话遗留 → 已走完 → 最久未用的在途分页），
  // 被淘汰的在途游标之后得明确的 cursor_expired；只有新快照自身放不下才结构化失败。
  qint64 retryAfterMs = 0;
  if (const auto full = m->makeRoom(scan.callerScope, bytes, nowMs, &retryAfterMs); !full.isEmpty()) {
    QJsonObject failure = discoveryFailure(full);
    failure.insert(QStringLiteral("retryAfterMs"), retryAfterMs);
    return failure;
  }
  const QString snapshotKey = built.key;
  m->cachedBytes += bytes;
  m->snapshots.insert(snapshotKey, std::move(built));
  const QJsonObject result = pageFromSnapshotLocked(snapshotKey, 0, relations, anchor, pageSize, maxBytes, QString());
  // 首页即失败（预算不足/响应超限）：没有任何承诺被发出，撤销登记不占槽。
  if (!result.value("ok").toBool())
    m->dropSnapshot(snapshotKey);
  return result;
}

// 调用方已持有 m->mutex。游标有效性是纯内存判定（MAC/到期/登记、快照归属、
// 查询摘要、位置上限）——续页取页与宿主建任务前的准入共用这一份逻辑。
QString OperationDiscovery::cursorErrorLocked(const DiscoveryScan &scan, const QString &digest,
                                              const QString &cursor, QString *snapshotKey, int *position) {
  const auto nowMs = m->now();
  m->pruneExpired(nowMs);
  const auto invalid = m->validateCursor(cursor, nowMs, snapshotKey, position);
  if (!invalid.isEmpty())
    return invalid;
  const auto found = m->snapshots.constFind(*snapshotKey);
  if (found == m->snapshots.cend() || found->expiresAt < nowMs)
    return QStringLiteral("cursor_expired");
  if (found->documentId != scan.documentId || found->callerScope != scan.callerScope ||
      found->project != scan.projectDir)
    return QStringLiteral("cursor_scope_mismatch");
  if (found->queryDigest != digest)
    return QStringLiteral("cursor_query_mismatch");
  if (*position > found->rows.size())
    return QStringLiteral("cursor_invalid");
  return {};
}

// 调用方已持有 m->mutex。
QJsonObject OperationDiscovery::pageFromCursor(const DiscoveryScan &scan, const QString &digest,
                                                const QString &cursor, bool relations, const QString &anchor,
                                                int pageSize, int maxBytes) {
  QString snapshotKey;
  int position = 0;
  if (const auto invalid = cursorErrorLocked(scan, digest, cursor, &snapshotKey, &position); !invalid.isEmpty())
    return discoveryFailure(invalid);
  return pageFromSnapshotLocked(snapshotKey, position, relations, anchor, pageSize, maxBytes, cursor);
}

QString OperationDiscovery::validateContinuation(const DiscoveryScan &scan, const QJsonObject &arguments) {
  DiscoveryRequest request;
  if (const auto invalid = validateRequestValue(arguments, request, nullptr); !invalid.isEmpty())
    return invalid;
  if (request.cursor.isEmpty())
    return {};
  if (scan.documentId.isEmpty() || scan.callerScope.isEmpty() || scan.documentId.toUtf8().size() > 256 ||
      scan.callerScope.toUtf8().size() > 512 || scan.projectDir.isEmpty())
    return QStringLiteral("discovery_scope_invalid");
  QMutexLocker lock(&m->mutex);
  QString snapshotKey;
  int position = 0;
  return cursorErrorLocked(scan, m->queryDigest(arguments), request.cursor, &snapshotKey, &position);
}

// 调用方已持有 m->mutex。
QJsonObject OperationDiscovery::pageFromSnapshotLocked(const QString &snapshotKey, int position, bool relations,
                                                        const QString &anchor, int pageSize, int maxBytes,
                                                        const QString &consumedCursor) {
  const auto nowMs = m->now();
  const auto found = m->snapshots.find(snapshotKey);
  if (found == m->snapshots.end())
    return discoveryFailure(QStringLiteral("cursor_expired"));
  found->useSeq = ++m->useCounter;
  const auto &snapshot = *found;
  // 刚被消费的游标保留作同页重试，但不再是"未消费的续页"。
  if (!consumedCursor.isEmpty()) {
    const auto slot = m->cursors.find(consumedCursor);
    if (slot != m->cursors.end())
      slot->consumed = true;
  }
  // 从不可变快照取页：单行超预算报 minimumBytes，不截断 ID、不静默丢条。
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
  QString next;
  if (hasMore) {
    if (int(m->cursors.size()) >= m->policy.maxCursors)
      return discoveryFailure(QStringLiteral("cursor_capacity_exceeded"));
    const auto expiry = nowMs + m->policy.ttlMs;
    next = m->cursorToken(snapshot.key, index, expiry);
    result.insert(QStringLiteral("nextCursor"), next);
    m->cursors.insert(next, Impl::CursorSlot{snapshot.key, expiry});
  }
  // 游标有界（W4）：消费下一续页即确认收到上一页——同一快照只保留
  // 「刚消费的可重试游标 +（若有）新发的续页游标」，更早的位置按已超越
  // 回收（重放旧位置得明确的 cursor_expired，不静默回第一页；末页同样
  // 只留可重试游标）；跨快照互不影响。
  for (auto it = m->cursors.begin(); it != m->cursors.end();) {
    if (it.key() != consumedCursor && it.key() != next && it->snapshotKey == snapshot.key)
      it = m->cursors.erase(it);
    else
      ++it;
  }
  result.insert(QStringLiteral("coverage"), QJsonObject{
      {QStringLiteral("documentId"), snapshot.documentId},
      {QStringLiteral("snapshotDigest"), snapshot.snapshotDigest},
      {QStringLiteral("journalHealth"), snapshot.journalIncomplete ? QStringLiteral("incomplete") : QStringLiteral("ok")},
      {QStringLiteral("snapshotPolicy"), QStringLiteral("immutable-snapshot@1")},
      {QStringLiteral("sort"), snapshot.sortKey},
      {QStringLiteral("matched"), snapshot.matched},
      // complete 与同族 paleo.query_project 的 coverage 同义：本页即末页。
      // 该键被工具结果信封原样提升（resultEnvelope），投影层据此判定覆盖已知。
      {QStringLiteral("complete"), !hasMore}});
  result.insert(QStringLiteral("note"), QStringLiteral(
      "Recorded journal facts from one immutable read; query paleo.operation_get for reconciled current state."));
  if (QJsonDocument(result).toJson(QJsonDocument::Compact).size() > maxBytes) {
    if (!next.isEmpty())
      m->cursors.remove(next);
    return discoveryFailure(QStringLiteral("discovery_response_budget_exceeded"));
  }
  return result;
}

} // namespace paleo::capability
