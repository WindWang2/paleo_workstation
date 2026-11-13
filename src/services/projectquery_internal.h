// 层：数据
#pragma once
#include "projectquerycapability.h"
#include "querywellprojection.h"
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QPointer>
#include <QQueue>
#include <QSet>

namespace paleo::query {
struct TraversalEntry {
  QString id, after;
  int direction = 0, depth = 0;
};
struct QueryState {
  QString kind, topic, queryDigest, queryId, modelToken, after, target, assetId;
  quint64 revision = 0, mutation = 0;
  Scope scope;
  QSet<QString> candidates;
  QString wellAfter, modelEntityAfter;
  qlonglong modelAfter = 0, pendingModel = 0;
  bool storedSearchDone = false, modelSearchDone = false;
  bool pendingModelIsLast = false;
  int upstream = 3, downstream = 1;
  QQueue<TraversalEntry> frontier;
  QHash<QPair<int,QString>, int> visited;
  QSet<QString> emitted;
  QSet<QString> discovered;
  QSet<QPair<QString,QString>> emittedEdges;
};
struct QuerySlot {
  QueryState state;
  qint64 expiresAt = 0;
  QJsonObject cached;
  qint64 bytes = 0;
};
struct ProjectQueryCapability::Impl {
  explicit Impl(QueryPolicy p) : policy(p) {}
  QueryPolicy policy;
  QPointer<DataCatalog> catalog;
  Scope scope;
  QHash<QString, QuerySlot> cursors;
  qint64 cachedBytes = 0;
  QString sessionId;
  QByteArray cursorSecret;
  QJsonObject run(const QJsonObject &args, bool graph);
  QString checkSnapshot(const QueryState &state, const QJsonObject &args, bool graph) const;
  QJsonObject page(QueryState &state, const QJsonObject &args, int rows, int bytes, bool &more);
  QJsonObject listPage(QueryState &state, const QJsonObject &args, int rows, int bytes, bool &more);
  QJsonObject wellDetails(QueryState &state, const QJsonObject &args, int rows, int bytes, bool &more);
  QJsonObject graphPage(QueryState &state, int rows, int bytes, bool &more);
  QString resolveSeed(QueryState &state, const QJsonObject &args);
  QString resolveWell(QueryState &state, const QJsonObject &args, WellProjection &model, bool &more);
  void prune();
  QString cursorToken(qint64 expiresAt) const;
  QString validateCursor(const QString &cursor) const;
};
QJsonObject queryFailure(const QString &code);
QString queryDigest(QJsonObject args, bool graph);
QString querySafePath(const DataCatalog *catalog, const QString &path);
QString queryDisplayName(const CatalogAsset &asset);
void queryVersionCitation(QJsonObject &entry, const DataCatalog *catalog, const QString &assetId);
QJsonObject queryWellReference(const CatalogEntity &entity, const QueryState &state);
bool appendWithinBudget(QJsonArray &array, const QJsonObject &entry, int budget, int overhead = 1024);
}
