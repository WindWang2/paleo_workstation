// 层：数据
#include "derivationgraph.h"
#include "../catalog/datacatalog.h"

#include <QCoreApplication>
#include <QHash>
#include <QQueue>
#include <algorithm>

namespace paleo::derivation
{
namespace
{
QString text(const char *source) { return QCoreApplication::translate("DerivationGraph", source); }
Kind kindOf(const CatalogVersion &v)
{
  return !v.managed ? Kind::External : v.stage == QLatin1String("RAW") ? Kind::Raw : Kind::Derived;
}
// 迭代、含种子、访问集去重；环不递归。也用于 stale 的祖先上下文。
QSet<QString> closure(const QHash<QString, QStringList> &adj, const QStringList &seeds)
{
  QSet<QString> seen;
  QQueue<QString> queue;
  for (const QString &id : seeds)
    if (!seen.contains(id)) { seen.insert(id); queue.enqueue(id); }
  while (!queue.isEmpty())
    for (const QString &id : adj.value(queue.dequeue()))
      if (!seen.contains(id)) { seen.insert(id); queue.enqueue(id); }
  return seen;
}
QHash<QString, int> depths(const QHash<QString, QStringList> &adj, const QStringList &seeds)
{
  QHash<QString, int> distance;
  QQueue<QString> queue;
  for (const QString &id : seeds)
    if (!distance.contains(id)) { distance.insert(id, 0); queue.enqueue(id); }
  while (!queue.isEmpty())
  {
    const QString parent = queue.dequeue();
    for (const QString &id : adj.value(parent))
      if (!distance.contains(id)) { distance.insert(id, distance.value(parent) + 1); queue.enqueue(id); }
  }
  return distance;
}
}

Graph Service::build(const DataCatalog *cat, const Query &q)
{
  Graph out;
  if (!cat || !cat->isOpen())
  {
    out.message = text(QT_TRANSLATE_NOOP("DerivationGraph", "工程未打开，衍生血缘不可用"));
    return out;
  }
  out.available = true;
  QHash<QString, CatalogVersion> versions;
  for (const CatalogVersion &v : cat->versions()) versions.insert(v.id, v);
  QStringList seeds;
  if (!q.versionId.isEmpty())
  {
    if (versions.contains(q.versionId)) seeds << q.versionId;
  }
  else if (!q.assetId.isEmpty())
  {
    const CatalogVersion v = cat->currentVersion(q.assetId);
    if (!v.id.isEmpty()) seeds << v.id;
  }
  else if (!q.entityId.isEmpty())
  {
    for (const EntityAssetLink &link : cat->linksForEntity(q.entityId))
      if (!link.unresolved)
      {
        const CatalogVersion v = cat->currentVersion(link.assetId);
        if (!v.id.isEmpty() && !seeds.contains(v.id)) seeds << v.id;
      }
  }
  if (seeds.isEmpty())
  {
    out.message = text(QT_TRANSLATE_NOOP("DerivationGraph", "请选择有版本记录的实体或资产"));
    return out;
  }
  seeds.sort();
  QHash<QString, QStringList> parents, children;
  for (const CatalogVersion &v : versions)
    for (const QString &parent : v.parentVersionIds)
      if (versions.contains(parent) && !parents[v.id].contains(parent))
      {
        parents[v.id] << parent;
        children[parent] << v.id;
      }
  const auto up = depths(parents, seeds);
  // 反查闭包复用既有 API；这里只算层数，绝不新增另一份 catalog 反查接口。
  QSet<QString> candidates;
  for (auto it = up.cbegin(); it != up.cend(); ++it) candidates.insert(it.key());
  for (const QString &seed : seeds)
    for (const CatalogVersion &v : cat->downstreamClosure(seed)) candidates.insert(v.id);
  const auto down = depths(children, seeds);
  QStringList staleSeeds;
  QSet<QString> missing;
  QHash<QString, QStringList> entityIds;
  QHash<QString, CatalogAsset> assets;
  QHash<QString, QString> entityNames;
  for (const QString &id : candidates)
  {
    const CatalogVersion &v = versions[id];
    if (v.extra.value(QStringLiteral("stale")).toBool()) staleSeeds << id;
    for (const QString &parent : v.parentVersionIds)
      if (!versions.contains(parent)) missing.insert(parent);
    if (!assets.contains(v.assetId))
    {
      assets.insert(v.assetId, cat->assetById(v.assetId));
      for (const EntityAssetLink &link : cat->linksForAsset(v.assetId))
        if (!link.unresolved && !link.entityId.isEmpty())
        {
          entityIds[v.assetId] << link.entityId;
          const CatalogEntity e = cat->entityById(link.entityId);
          if (!e.id.isEmpty()) entityNames.insert(e.id, e.name.isEmpty() ? e.id : e.name);
        }
    }
  }
  out.missingParents = missing.size();
  for (auto it = entityNames.cbegin(); it != entityNames.cend(); ++it) out.entities << Choice{it.key(), it.value()};
  std::sort(out.entities.begin(), out.entities.end(), [](const Choice &a, const Choice &b) {
    return a.name == b.name ? a.id < b.id : a.name < b.name;
  });
  const QSet<QString> staleContext = q.staleOnly ? closure(parents, staleSeeds) : QSet<QString>();
  QStringList ordered = candidates.values();
  // 种子优先，再按与种子的最近距离；顺序不依赖 QHash 迭代（截图/折叠可复现）。
  const auto rank = [&up, &down](const QString &id) {
    return qMin(up.value(id, 1000000), down.value(id, 1000000));
  };
  std::sort(ordered.begin(), ordered.end(), [&rank](const QString &a, const QString &b) {
    return rank(a) == rank(b) ? a < b : rank(a) < rank(b);
  });
  const int cap = qBound(1, q.maxNodes, 160);
  const int upLimit = qBound(0, q.upstreamDepth, 12), downLimit = qBound(0, q.downstreamDepth, 12);
  QSet<QString> visible;
  for (const QString &id : ordered)
  {
    const CatalogVersion &v = versions[id];
    const CatalogAsset &asset = assets[v.assetId];
    if ((!q.entityFilter.isEmpty() && !entityIds.value(v.assetId).contains(q.entityFilter)) ||
        (!q.assetFilter.isEmpty() && !asset.displayName.contains(q.assetFilter, Qt::CaseInsensitive) &&
         !v.assetId.contains(q.assetFilter, Qt::CaseInsensitive)) ||
        (q.kindFilter >= 0 && int(kindOf(v)) != q.kindFilter) ||
        (q.staleOnly && !staleContext.contains(id)))
    { ++out.filtered; continue; }
    const bool seed = seeds.contains(id);
    const bool inDepth = up.value(id, 1000000) <= upLimit || down.value(id, 1000000) <= downLimit;
    if (!inDepth || out.nodes.size() >= cap)
    {
      // 三组互斥计数：环成员同时属于两向时按较短距离归类，上游同距优先。
      if (seed) ++out.collapsedSeeds;
      else if (up.value(id, 1000000) <= down.value(id, 1000000)) ++out.collapsedUpstream;
      else ++out.collapsedDownstream;
      continue;
    }
    Node n;
    n.versionId = id; n.assetId = v.assetId;
    n.versionName = text(QT_TRANSLATE_NOOP("DerivationGraph", "v%1 · %2")).arg(v.versionNumber)
        .arg(v.fileName.isEmpty() ? v.id : v.fileName);
    n.assetName = asset.displayName.isEmpty() ? v.assetId : asset.displayName;
    n.stage = v.stage; n.kind = kindOf(v);
    n.stale = v.extra.value(QStringLiteral("stale")).toBool();
    n.staleReason = v.extra.value(QStringLiteral("staleReason")).toString();
    n.column = seed ? 0 : up.contains(id) && up.value(id) <= down.value(id, 1000000)
                              ? -up.value(id) : down.value(id);
    out.nodes << n;
    visible.insert(id);
  }
  QSet<QPair<QString, QString>> drawn;
  for (const Node &child : out.nodes)
    for (const QString &parent : parents.value(child.versionId))
    {
      const auto edge = qMakePair(parent, child.versionId);
      if (visible.contains(parent) && !drawn.contains(edge))
      { drawn.insert(edge); out.edges << Edge{parent, child.versionId}; }
    }
  if (out.nodes.isEmpty()) out.message = text(QT_TRANSLATE_NOOP("DerivationGraph", "没有符合过滤条件的版本"));
  else if (out.edges.isEmpty())
  {
    bool hasRelations = false;
    for (const QString &id : candidates)
      if (!parents.value(id).isEmpty() || !children.value(id).isEmpty()) { hasRelations = true; break; }
    out.message = out.missingParents ? text(QT_TRANSLATE_NOOP("DerivationGraph", "源版本缺失，无法显示完整衍生关系"))
                : hasRelations ? text(QT_TRANSLATE_NOOP("DerivationGraph", "当前深度或过滤下没有连线"))
                               : text(QT_TRANSLATE_NOOP("DerivationGraph", "该版本无衍生记录"));
  }
  return out;
}

QSet<QString> Service::selectionClosure(const DataCatalog *cat, const Graph &graph, const QString &id)
{
  QSet<QString> related{id};
  if (cat && cat->isOpen())
  {
    for (const CatalogVersion &v : cat->downstreamClosure(id)) related.insert(v.id);
    QStringList queue{id};
    QSet<QString> seen{id};
    for (qsizetype head = 0; head < queue.size(); ++head)
      for (const QString &parent : cat->versionById(queue.at(head)).parentVersionIds)
        if (!seen.contains(parent)) { seen.insert(parent); queue << parent; }
    related |= seen;
  }
  QSet<QString> visible;
  for (const Node &n : graph.nodes) if (related.contains(n.versionId)) visible.insert(n.versionId);
  return visible;
}
} // namespace paleo::derivation
