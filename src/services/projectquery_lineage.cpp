// 层：数据
#include "projectquery_internal.h"
#include <QJsonDocument>
#include <cmath>

namespace paleo::query {
namespace {
int depth(const QJsonObject &args, const char *key, int fallback) {
  if (!args.contains(key)) return fallback;
  const auto value = args.value(key);
  if (!value.isDouble() || !std::isfinite(value.toDouble()) || std::floor(value.toDouble()) != value.toDouble() ||
      value.toDouble() < 0 || value.toDouble() > 12) return -1;
  return value.toInt();
}
QJsonObject node(const DataCatalog *catalog, const QString &id, int column) {
  const auto version = catalog->versionById(id); const auto asset = catalog->assetById(version.assetId);
  const QString kind = version.id.isEmpty() ? QStringLiteral("missing") : version.stage == QLatin1String("RAW") ?
    QStringLiteral("raw") : version.managed ? QStringLiteral("derived") : QStringLiteral("external");
  return {{"version_id",id},{"asset",queryDisplayName(asset)},{"asset_id",version.assetId},{"stage",version.stage},
    {"kind",kind},{"available",!version.id.isEmpty()},{"stale",version.extra.value("stale").toBool()},{"column",column},
    {"version_name",version.fileName.isEmpty()?id:version.fileName},{"path",querySafePath(catalog,version.path)}};
}
}
QString ProjectQueryCapability::Impl::resolveSeed(QueryState &state, const QJsonObject &args) {
  const QString key = args.value("asset").toString();
  if (key.isEmpty() || key.size() > 512) return QStringLiteral("invalid_asset_ref");
  const auto version = catalog->versionById(key);
  if (version.id == key) { state.target = key; state.assetId = version.assetId; }
  else {
    auto asset = catalog->assetById(key);
    if (asset.id.isEmpty()) {
      const auto matches = catalog->assetNamePage(key, {}, 2);
      if (matches.totalCount > 1) return QStringLiteral("asset_ambiguous");
      if (!matches.items.isEmpty()) asset = matches.items.first();
    }
    state.assetId = asset.id; state.target = catalog->currentVersion(asset.id).id;
  }
  if (state.target.isEmpty()) return QStringLiteral("version_not_found");
  if (state.target.size() > 256) return QStringLiteral("lineage_ref_budget_exceeded");
  state.discovered.insert(state.target);
  state.upstream = depth(args, "upstream_depth", 3); state.downstream = depth(args, "downstream_depth", 1);
  if (state.upstream < 0 || state.downstream < 0) return QStringLiteral("lineage_depth_invalid");
  for (int direction : {-1, 1}) {
    state.frontier.enqueue({state.target, {}, direction, 0});
    state.visited.insert(qMakePair(direction, state.target), 0);
  }
  return {};
}

QJsonObject ProjectQueryCapability::Impl::graphPage(QueryState &state, int rows, int bytes, bool &more) {
  QJsonArray nodes, edges, selection;
  int work = rows * 8 + 8;
  while (!state.frontier.isEmpty() && work-- > 0) {
    auto item = state.frontier.dequeue();
    if (!state.emitted.contains(item.id)) {
      if (nodes.size() >= rows || !appendWithinBudget(nodes, node(catalog, item.id, item.direction * item.depth), bytes,
            1792 + QJsonDocument(edges).toJson(QJsonDocument::Compact).size())) {
        state.frontier.prepend(item);
        if (nodes.isEmpty() && edges.isEmpty()) return queryFailure("query_entry_too_large");
        break;
      }
      state.emitted.insert(item.id); selection.append(item.id);
    }
    const int limit = item.direction < 0 ? state.upstream : state.downstream;
    if (item.depth >= limit) continue;
    if (edges.size() >= rows * 4) {state.frontier.prepend(item); break;}
    const auto adjacent = item.direction < 0 ? catalog->versionParentsPage(item.id, item.after, 1) :
                                              catalog->versionChildrenPage(item.id, item.after, 1);
    if (adjacent.items.isEmpty()) continue;
    const auto nextId = adjacent.items.first();
    if (nextId.isEmpty() || nextId.size() > 256) return queryFailure("lineage_ref_budget_exceeded");
    const auto edge = item.direction < 0 ? qMakePair(nextId, item.id) : qMakePair(item.id, nextId);
    if (!state.emittedEdges.contains(edge)) {
      if (state.emittedEdges.size() >= policy.maxTraversalNodes * 4) return queryFailure("lineage_state_budget_exceeded");
      if (!appendWithinBudget(edges, QJsonObject{{"parent",edge.first},{"child",edge.second}}, bytes,
            1792 + QJsonDocument(nodes).toJson(QJsonDocument::Compact).size())) {
        state.frontier.prepend(item); break;
      }
      state.emittedEdges.insert(edge);
    }
    item.after = adjacent.itemKeys.first();
    if (adjacent.hasMore) state.frontier.prepend(item);
    const auto visit = qMakePair(item.direction, nextId);
    if (!state.visited.contains(visit)) {
      if (state.visited.size() >= policy.maxTraversalNodes * 2 ||
          (!state.discovered.contains(nextId) && state.discovered.size() >= policy.maxTraversalNodes))
        return queryFailure("lineage_state_budget_exceeded");
      state.discovered.insert(nextId);
      state.visited.insert(visit, item.depth + 1); state.frontier.enqueue({nextId, {}, item.direction, item.depth + 1});
    }
  }
  more = !state.frontier.isEmpty();
  return {{"ok",true},{"code","ok"},{"contract","paleo.lineage.page.v1"},{"tool","paleo.asset_lineage"},
    {"asset_id",state.assetId},{"version_id",state.target},{"available",true},{"nodes",nodes},{"edges",edges},
    {"selection",QJsonObject{{"count",selection.size()},{"version_ids",selection}}},{"returnedCount",nodes.size()},
    {"totalCount",more?QJsonValue(QJsonValue::Null):QJsonValue(state.emitted.size())},
    {"upstreamDepth",state.upstream},{"downstreamDepth",state.downstream},
    {"collapsed_upstream",QJsonValue::Null},{"collapsed_downstream",QJsonValue::Null},{"collapsed_seeds",0}};
}
}
