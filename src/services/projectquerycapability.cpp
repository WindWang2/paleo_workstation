// 层：数据
#include "projectquery_internal.h"
#include "domain/wellref.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QThread>
#include <QUuid>
#include <cmath>

namespace paleo::query {
namespace {
int integer(const QJsonObject &args, const char *key, int fallback, int maximum) {
  if (!args.contains(key)) return fallback;
  const auto v = args.value(key);
  if (!v.isDouble() || !std::isfinite(v.toDouble()) || std::floor(v.toDouble()) != v.toDouble() ||
      v.toDouble() < 1 || v.toDouble() > maximum) return -1;
  return v.toInt();
}
qint64 stateBytes(const QueryState &state) {
  qint64 bytes = 1024;
  for (const auto &text : QStringList{state.kind,state.topic,state.queryDigest,state.queryId,state.modelToken,state.after,
       state.target,state.assetId,state.wellAfter,state.modelEntityAfter,state.scope.documentId,state.scope.chatSessionId})
    bytes += 128 + text.size() * 2;
  for (auto it = state.visited.cbegin(); it != state.visited.cend(); ++it) bytes += 128 + it.key().second.size() * 2;
  for (const auto &id : state.emitted) bytes += 96 + id.size() * 2;
  for (const auto &id : state.candidates) bytes += 96 + id.size() * 2;
  for (const auto &id : state.discovered) bytes += 96 + id.size() * 2;
  for (const auto &item : state.frontier) bytes += 96 + (item.id.size() + item.after.size()) * 2;
  for (const auto &edge : state.emittedEdges) bytes += 128 + (edge.first.size() + edge.second.size()) * 2;
  return bytes;
}
QueryState replaySnapshot(const QueryState &state) {
  QueryState saved; saved.scope = state.scope; saved.queryDigest = state.queryDigest; saved.queryId = state.queryId;
  saved.revision = state.revision; saved.mutation = state.mutation; saved.modelToken = state.modelToken;
  return saved;
}
}
QJsonObject queryFailure(const QString &code) {
  return {{"ok",false},{"code",code},{"error",code},{"contract","paleo.query.error.v1"}};
}
QString queryDigest(QJsonObject args, bool graph) {
  args.remove("cursor");
  args.insert("page_size", args.value("page_size").toInt(50));
  args.insert("max_bytes", args.value("max_bytes").toInt(8192));
  args.insert("query_kind", graph ? "lineage" : "project");
  return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(args).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}
QString querySafePath(const DataCatalog *catalog, const QString &path) {
  QString portable = path; portable.replace(QLatin1Char('\\'), QLatin1Char('/'));
  const bool windowsAbsolute = portable.startsWith(QLatin1String("//")) ||
    (portable.size() >= 3 && portable.at(0).isLetter() && portable.at(1) == QLatin1Char(':') && portable.at(2) == QLatin1Char('/'));
  if (path.isEmpty() || (!windowsAbsolute && QDir::isRelativePath(portable))) return path;
  if (windowsAbsolute && QDir::isRelativePath(portable)) return QStringLiteral("<external>/") + QFileInfo(portable).fileName();
  const QString relative = QDir(catalog->projectDir()).relativeFilePath(portable);
  if (relative != QLatin1String("..") && !relative.startsWith(QLatin1String("../")) && QDir::isRelativePath(relative)) return relative;
  return QStringLiteral("<external>/") + QFileInfo(portable).fileName();
}
QString queryDisplayName(const CatalogAsset &asset) { return asset.displayName.isEmpty() ? asset.id : asset.displayName; }
void queryVersionCitation(QJsonObject &entry, const DataCatalog *catalog, const QString &assetId) {
  const auto version = catalog->currentVersion(assetId);
  entry.insert("version_id", version.id); entry.insert("stage", version.stage);
  entry.insert("version_number", version.versionNumber); entry.insert("path", querySafePath(catalog, version.path));
}
QJsonObject queryWellReference(const CatalogEntity &entity, const QueryState &state) {
  const bool available = entity.extra.value(QStringLiteral("queryModelAvailable")).toBool();
  const auto head = QJsonDocument::fromJson(state.modelToken.toUtf8()).object();
  document::WellRef ref;
  ref.documentId = state.scope.documentId; ref.catalogEntityId = entity.id;
  ref.modelStoreId = QStringLiteral("area-model"); ref.modelAvailable = available;
  if (available) {
    ref.modelStoreIdentity = head.value("store").toString(); ref.modelGeneration = head.value("generation").toString();
    ref.modelWellId = entity.extra.value(QStringLiteral("areaModelWellId")).toLongLong();
    ref.queryHead = head.value("sequence").toString().toLongLong();
  }
  return ref.valid() ? document::wellRefJson(ref) : QJsonObject{};
}
bool appendWithinBudget(QJsonArray &array, const QJsonObject &entry, int budget, int overhead) {
  array.append(entry);
  if (QJsonDocument(array).toJson(QJsonDocument::Compact).size() + overhead <= budget) return true;
  array.removeLast(); return false;
}

ProjectQueryCapability::ProjectQueryCapability(QueryPolicy p) : m(std::make_unique<Impl>(p)) {
  m->cursorSecret = QUuid::createUuid().toRfc4122() + QUuid::createUuid().toRfc4122();
}
ProjectQueryCapability::~ProjectQueryCapability() = default;
void ProjectQueryCapability::clear() { m->cursors.clear(); m->cachedBytes = 0; }
void ProjectQueryCapability::bind(DataCatalog *catalog, Scope scope) {
  clear(); m->catalog = catalog; m->sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
  if (scope.documentId.isEmpty()) scope.documentId = QStringLiteral("session:") + m->sessionId;
  m->scope = std::move(scope);
}
QJsonObject ProjectQueryCapability::query(const QJsonObject &args) { return m->run(args, false); }
QJsonObject ProjectQueryCapability::lineage(const QJsonObject &args) { return m->run(args, true); }
void ProjectQueryCapability::Impl::prune() {
  const auto now = QDateTime::currentMSecsSinceEpoch();
  for (auto it = cursors.begin(); it != cursors.end(); ) {
    if (it->expiresAt < now) { cachedBytes -= it->bytes; it = cursors.erase(it); } else ++it;
  }
}
QString ProjectQueryCapability::Impl::cursorToken(qint64 expiresAt) const {
  const QByteArray body = QByteArray("qpage1.") + sessionId.toLatin1() + '.' +
      QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1() + '.' + QByteArray::number(expiresAt);
  const auto mac = QMessageAuthenticationCode::hash(body, cursorSecret, QCryptographicHash::Sha256).toHex();
  return QString::fromLatin1(body + '.' + mac);
}
QString ProjectQueryCapability::Impl::validateCursor(const QString &cursor) const {
  if (cursor.size() > 256) return QStringLiteral("cursor_invalid");
  const auto parts = cursor.toLatin1().split('.');
  if (parts.size() != 5 || parts[0] != "qpage1" || parts[4].size() != 64) return QStringLiteral("cursor_invalid");
  const auto body = parts[0] + '.' + parts[1] + '.' + parts[2] + '.' + parts[3];
  const auto mac = QMessageAuthenticationCode::hash(body, cursorSecret, QCryptographicHash::Sha256).toHex();
  unsigned difference = 0;
  for (int i = 0; i < mac.size(); ++i) difference |= unsigned(uchar(mac.at(i)) ^ uchar(parts[4].at(i)));
  if (difference) return QStringLiteral("cursor_invalid");
  if (parts[1] != sessionId.toLatin1()) return QStringLiteral("cursor_scope_mismatch");
  bool valid = false; const auto expires = parts[3].toLongLong(&valid);
  if (!valid) return QStringLiteral("cursor_invalid");
  return expires < QDateTime::currentMSecsSinceEpoch() ? QStringLiteral("cursor_expired") : QString();
}
QString ProjectQueryCapability::Impl::checkSnapshot(const QueryState &state, const QJsonObject &args, bool graph) const {
  if (state.scope != scope) return QStringLiteral("cursor_scope_mismatch");
  if (state.queryDigest != queryDigest(args, graph)) return QStringLiteral("cursor_query_mismatch");
  if (state.revision != quint64(catalog->catalogRevision()) || state.mutation != catalog->mutationSeq())
    return QStringLiteral("stale_cursor");
  if (!state.modelToken.isEmpty()) {
    WellProjection model(catalog->projectDir()); const auto error = model.open();
    if (!error.isEmpty()) return error;
    if (model.token() != state.modelToken) return QStringLiteral("model_revision_changed");
  }
  return {};
}
QJsonObject ProjectQueryCapability::Impl::run(const QJsonObject &args, bool graph) {
  if (!catalog || QThread::currentThread() != catalog->thread() || !catalog->isOpen()) return queryFailure("document_unavailable");
  if (scope.documentId.isEmpty() || scope.documentId.toUtf8().size() > 256 || scope.chatSessionId.toUtf8().size() > 256)
    return queryFailure("query_identity_invalid");
  if (QJsonDocument(args).toJson(QJsonDocument::Compact).size() > 16384) return queryFailure("query_request_budget_exceeded");
  if (args.contains("cursor") && !args.value("cursor").isString()) return queryFailure("cursor_invalid");
  const int rows = integer(args, "page_size", 50, 256);
  const int bytes = integer(args, "max_bytes", 8192, 65536);
  if (rows < 0 || bytes < 2048) return queryFailure("query_budget_invalid");
  QueryState state; const QString cursor = args.value("cursor").toString();
  if (!cursor.isEmpty()) {
    const auto invalidCursor = validateCursor(cursor); if (!invalidCursor.isEmpty()) return queryFailure(invalidCursor);
    const auto it = cursors.constFind(cursor);
    if (it == cursors.cend()) return queryFailure("cursor_replay_unavailable");
    if (it->expiresAt < QDateTime::currentMSecsSinceEpoch()) return queryFailure("cursor_expired");
    const QString invalid = checkSnapshot(it->state, args, graph);
    if (!invalid.isEmpty()) return queryFailure(invalid);
    if (!it->cached.isEmpty()) return it->cached;
    state = it->state;
  } else {
    state.kind = graph ? "lineage" : "project"; state.topic = args.value("topic").toString();
    state.queryId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    state.scope = scope; state.revision = catalog->catalogRevision(); state.mutation = catalog->mutationSeq();
    state.queryDigest = queryDigest(args, graph);
    if (graph) { const auto error = resolveSeed(state, args); if (!error.isEmpty()) return queryFailure(error); }
  }
  bool more = false; auto result = page(state, args, rows, bytes, more);
  if (!result.value("ok").toBool()) return QJsonDocument(result).toJson(QJsonDocument::Compact).size() <= bytes
    ? result : queryFailure("query_response_budget_exceeded");
  const QString stale = checkSnapshot(state, args, graph); if (!stale.isEmpty()) return queryFailure(stale);
  result.insert("hasMore", more); result.insert("nextCursor", QJsonValue::Null);
  result.insert("coverage", QJsonObject{{"documentId",scope.documentId},{"epoch",QString::number(scope.epoch)},
    {"catalogRevision",QString::number(state.revision)},{"mutationSequence",QString::number(state.mutation)},
    {"modelSnapshot",state.modelToken},{"sortVersion",graph?"directional-bfs@1":state.topic==QLatin1String("version_parents")?
       "parent-ordinal@1":state.topic==QLatin1String("well_details")?"link-row@1":"id@1"},{"complete",!more}});
  prune(); QString next;
  if (!cursor.isEmpty() && !cursors.contains(cursor)) return queryFailure("cursor_expired");
  // Consuming the next continuation proves receipt of the previous page. Keep
  // one retry page, rather than allocating a handle for every page forever.
  for (auto it = cursors.begin(); it != cursors.end(); ) {
    if (it.key() != cursor && it->state.queryId == state.queryId && !it->cached.isEmpty()) {
      cachedBytes -= it->bytes; it = cursors.erase(it);
    } else ++it;
  }
  const auto replay = cursor.isEmpty() ? QueryState{} : replaySnapshot(cursors.value(cursor).state);
  const qint64 reclaimed = cursor.isEmpty() ? 0 : cursors.value(cursor).bytes - stateBytes(replay);
  if (more) {
    const qint64 size = stateBytes(state);
    if (cursors.size() >= policy.maxCursors || cachedBytes - reclaimed + size + bytes > policy.maxCacheBytes)
      return queryFailure("cursor_capacity_exceeded");
    const auto expiry = QDateTime::currentMSecsSinceEpoch() + policy.ttlMs;
    next = cursorToken(expiry);
    result.insert("nextCursor", next);
    cursors.insert(next, QuerySlot{state,expiry,{},size}); cachedBytes += size;
    result.insert("truncated", QStringLiteral("page_available"));
  }
  const auto encoded = QJsonDocument(result).toJson(QJsonDocument::Compact);
  if (encoded.size() > bytes) {
    if (!next.isEmpty()) {cachedBytes -= cursors.value(next).bytes; cursors.remove(next);}
    return queryFailure("query_response_budget_exceeded");
  }
  if (!cursor.isEmpty()) {
    auto &slot = cursors[cursor];
    if (cachedBytes - reclaimed + encoded.size() > policy.maxCacheBytes) return queryFailure("cursor_capacity_exceeded");
    cachedBytes += encoded.size() - reclaimed;
    slot.cached = result; slot.state = replay; slot.bytes = stateBytes(replay) + encoded.size();
  }
  return result;
}
QJsonObject ProjectQueryCapability::Impl::page(QueryState &state, const QJsonObject &args, int rows, int bytes, bool &more) {
  if (state.kind == QLatin1String("lineage")) return graphPage(state, rows, bytes, more);
  if (state.topic == QLatin1String("well_details")) return wellDetails(state, args, rows, bytes, more);
  return listPage(state, args, rows, bytes, more);
}
}
