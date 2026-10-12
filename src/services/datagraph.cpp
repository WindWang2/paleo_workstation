// 层：数据
#include "datagraph.h"
#include "../catalog/catalogroles.h"
#include "../catalog/datacatalog.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QQueue>
#include <QSet>
#include <algorithm>

namespace paleo::datagraph {
namespace {
QString text(const char *s) {
  return QCoreApplication::translate("DataGraph", s);
}
bool stopped(const Cancel &cancel) { return cancel && cancel(); }
void report(const Progress &progress, double percent, const char *stage) {
  if (progress)
    progress(percent, text(stage));
}
void addEdge(Index &index, const Edge &edge) {
  const int row = index.edges.size();
  index.edges.append(edge);
  index.outgoing[edge.source].append(row);
  index.incoming[edge.target].append(row);
}
QString entityCategory(const QString &type) {
  if (type == QLatin1String("well") || type == QLatin1String("planned"))
    return QStringLiteral("well");
  if (type == QLatin1String("seismic_survey"))
    return QStringLiteral("seismic");
  if (type == QLatin1String("sequence_boundary"))
    return QStringLiteral("framework");
  return QStringLiteral("misc");
}
bool lineage(Relation r) {
  return r == Relation::Derived || r == Relation::Used ||
         r == Relation::Generated || r == Relation::EditBaseline ||
         r == Relation::Rollback;
}
void diagnoseCycles(Index &index, const Cancel &cancel) {
  // Iterative Kosaraju: only computational provenance; edit/rollback references
  // may legitimately cycle.
  QHash<QString, QStringList> forward, reverse;
  QSet<QString> selfLoops;
  for (const Edge &e : index.edges) {
    if (stopped(cancel))
      return;
    if (e.relation != Relation::Derived && e.relation != Relation::Used &&
        e.relation != Relation::Generated)
      continue;
    forward[e.source].append(e.target);
    reverse[e.target].append(e.source);
    if (e.source == e.target)
      selfLoops.insert(e.source);
  }
  QStringList roots = forward.keys();
  std::sort(roots.begin(), roots.end());
  QSet<QString> visited;
  QStringList order;
  for (const QString &root : roots) {
    if (visited.contains(root))
      continue;
    QVector<QPair<QString, bool>> stack{{root, false}};
    while (!stack.isEmpty()) {
      if (stopped(cancel))
        return;
      const auto entry = stack.takeLast();
      if (entry.second) {
        order.append(entry.first);
        continue;
      }
      if (visited.contains(entry.first))
        continue;
      visited.insert(entry.first);
      stack.append({entry.first, true});
      for (const QString &next : forward.value(entry.first))
        if (!visited.contains(next))
          stack.append({next, false});
    }
  }
  visited.clear();
  for (auto it = order.crbegin(); it != order.crend(); ++it) {
    if (visited.contains(*it))
      continue;
    QStringList component;
    QStringList stack{*it};
    visited.insert(*it);
    while (!stack.isEmpty()) {
      if (stopped(cancel))
        return;
      const QString id = stack.takeLast();
      component.append(id);
      for (const QString &next : reverse.value(id))
        if (!visited.contains(next)) {
          visited.insert(next);
          stack.append(next);
        }
    }
    if (component.size() > 1 || selfLoops.contains(component.front())) {
      ++index.cycles;
      for (const QString &id : component) {
        index.nodes[id].cycle = true;
        index.nodes[id].detail += text(" · 血缘环：需核查");
      }
    }
  }
}
bool matches(const Node &n, const Query &q) {
  return (q.category.isEmpty() || n.categories.contains(q.category)) &&
         (q.stage.isEmpty() || n.stage == q.stage ||
          (q.stage == QLatin1String("NONE") && n.stage.isEmpty())) &&
         (q.search.isEmpty() ||
          (n.title + QLatin1Char(' ') + n.detail + QLatin1Char(' ') + n.path)
              .contains(q.search, Qt::CaseInsensitive));
}
void layout(Graph &graph, const Cancel &cancel) {
  // 有界子图：稳定列布局，双向重心排序减少交叉；同名节点以稳定 ID 打破平局。
  QHash<int, QVector<int>> columns;
  for (int i = 0; i < graph.nodes.size(); ++i)
    columns[graph.nodes[i].column].append(i);
  QList<int> keys = columns.keys();
  std::sort(keys.begin(), keys.end());
  QHash<QString, int> rows;
  for (int c : keys) {
    auto &items = columns[c];
    std::sort(items.begin(), items.end(), [&](int a, int b) {
      return graph.nodes[a].title == graph.nodes[b].title
                 ? graph.nodes[a].id < graph.nodes[b].id
                 : graph.nodes[a].title < graph.nodes[b].title;
    });
    for (int i = 0; i < items.size(); ++i)
      rows[graph.nodes[items[i]].id] = i;
  }
  for (int pass = 0; pass < 4 && !stopped(cancel); ++pass) {
    QList<int> order = keys;
    if (pass % 2)
      std::reverse(order.begin(), order.end());
    for (int c : order) {
      auto &items = columns[c];
      QHash<QString, double> scores;
      for (int i : items) {
        const QString id = graph.nodes[i].id;
        double sum = 0;
        int count = 0;
        for (const Edge &e : graph.edges) {
          if (e.source == id && rows.contains(e.target)) {
            sum += rows[e.target];
            ++count;
          } else if (e.target == id && rows.contains(e.source)) {
            sum += rows[e.source];
            ++count;
          }
        }
        scores[id] = count ? sum / count : rows[id];
      }
      std::stable_sort(items.begin(), items.end(), [&](int a, int b) {
        return scores[graph.nodes[a].id] < scores[graph.nodes[b].id];
      });
      for (int i = 0; i < items.size(); ++i)
        rows[graph.nodes[items[i]].id] = i;
    }
  }
  for (int c : keys)
    for (int i : columns[c])
      graph.nodes[i].position =
          QPointF(c * 310.0, rows[graph.nodes[i].id] * 112.0);
}
} // namespace
QString entityKey(const QString &id) { return QStringLiteral("entity:") + id; }
QString assetKey(const QString &id) { return QStringLiteral("asset:") + id; }
QString versionKey(const QString &id) {
  return QStringLiteral("version:") + id;
}
QString categoryText(const QString &c) {
  if (c == QLatin1String("well"))
    return text("测井 / 井资料");
  if (c == QLatin1String("seismic"))
    return text("地震资料");
  if (c == QLatin1String("framework"))
    return text("地层框架");
  if (c == QLatin1String("product"))
    return text("综合资料 / 图件");
  return text("辅助资料");
}
QString stageText(const QString &s) {
  if (s == QLatin1String("RAW"))
    return text("原始资料");
  if (s == QLatin1String("INTERMEDIATE"))
    return text("中间资料");
  if (s == QLatin1String("DERIVED"))
    return text("派生资料");
  if (s == QLatin1String("OUTPUT"))
    return text("成果");
  return s.isEmpty() || s == QLatin1String("NONE") ? text("没有版本") : s;
}
std::shared_ptr<const Index> buildIndex(const DataCatalog &cat,
                                        const Presentation &presentation,
                                        const Cancel &cancel,
                                        const Progress &progress) {
  auto index = std::make_shared<Index>();
  report(progress, 5, "读取资料关系");
  const auto entities = cat.entities();
  const auto assets = cat.assets();
  const auto versions = cat.versions();
  const auto links = cat.links();
  QHash<QString, QString> entityTypes;
  for (const auto &entity : entities) {
    if (stopped(cancel))
      return {};
    if (presentation.removedIds.contains(entity.id))
      continue;
    ++index->entityCount;
    Node n;
    n.id = entityKey(entity.id);
    n.entityId = entity.id;
    n.title = presentation.entityNames.value(entity.id, entity.name);
    n.detail = entity.entityType;
    n.kind = Kind::Entity;
    n.categories << entityCategory(entity.entityType);
    n.column = 0;
    index->nodes.insert(n.id, n);
    index->entities.append(n.id);
    entityTypes.insert(entity.id, entity.entityType);
  }
  for (auto asset : assets) {
    if (stopped(cancel))
      return {};
    if (presentation.removedIds.contains(asset.id))
      continue;
    asset.type = presentation.assetTypes.value(asset.id, asset.type);
    Node n;
    n.id = assetKey(asset.id);
    n.assetId = asset.id;
    n.title = asset.displayName;
    n.detail = asset.type;
    n.column = 1;
    const auto tags = presentation.tags.value(asset.id);
    if (!tags.isEmpty())
      n.detail += text(" · 标签：") + tags.join(QStringLiteral(" / "));
    // Explicit roles augment this type fallback; filenames never create
    // relations.
    if (asset.type == QLatin1String("well_log") ||
        asset.type == QLatin1String("well_stratification") ||
        asset.type == QLatin1String("well_head") ||
        asset.type == QLatin1String("time_depth") ||
        asset.type == QLatin1String("wells_prediction"))
      n.categories << QStringLiteral("well");
    else if (asset.type == QLatin1String("seismic") ||
             asset.type == QLatin1String("seismic_prediction"))
      n.categories << QStringLiteral("seismic");
    else if (asset.type == QLatin1String("horizon") ||
             asset.type == QLatin1String("boundary"))
      n.categories << QStringLiteral("framework");
    else if (asset.type == QLatin1String("map_product") ||
             asset.type == QLatin1String("composed_facies") ||
             asset.type == QLatin1String("facies_polygons") ||
             asset.type == QLatin1String("edited_facies") ||
             asset.type.startsWith(QLatin1String("single_factor_")) ||
             asset.type == QLatin1String("contour_lines") ||
             asset.type == QLatin1String("facies_fusion_raster"))
      n.categories << QStringLiteral("product");
    index->nodes.insert(n.id, n);
    index->assets.append(n.id);
  }
  QHash<QString, int> latest;
  for (const auto &v : versions) {
    if (stopped(cancel))
      return {};
    if (presentation.removedIds.contains(v.assetId))
      continue;
    ++index->versionCount;
    Node n;
    n.id = versionKey(v.id);
    n.versionId = v.id;
    n.assetId = v.assetId;
    n.kind = Kind::Version;
    n.stage = v.stage;
    n.path = v.path;
    n.external = !v.managed;
    n.stale = v.extra.value(QStringLiteral("stale")).toBool();
    n.column = 2;
    n.sourceUri = v.sourceUri;
    n.sha256 = v.sha256;
    n.evidence = v.extra.value(QStringLiteral("evidence")).toMap();
    if (v.extra.value(QStringLiteral("mapping_product")).toBool() &&
        index->nodes.contains(assetKey(v.assetId))) {
      auto &categories = index->nodes[assetKey(v.assetId)].categories;
      if (!categories.contains(QStringLiteral("product")))
        categories << QStringLiteral("product");
    }
    const Node a = index->nodes.value(assetKey(v.assetId));
    n.title = v.extra.value(QStringLiteral("title"), a.title).toString() +
              text(" · v%1").arg(v.versionNumber);
    n.detail = stageText(v.stage) +
               (n.external ? text(" · 外部链接") : text(" · 受管文件"));
    if (n.stale)
      n.detail += text(" · 已过期：") +
                  v.extra.value(QStringLiteral("staleReason")).toString();
    index->nodes.insert(n.id, n);
    addEdge(*index,
            {assetKey(v.assetId), n.id, text("版本"), Relation::HasVersion});
    if (!latest.contains(v.assetId) || v.versionNumber >= latest[v.assetId]) {
      latest[v.assetId] = v.versionNumber;
      index->currentVersions[v.assetId] = n.id;
    }
  }
  report(progress, 25, "构建关联索引");
  for (const auto &link : links) {
    if (stopped(cancel))
      return {};
    const QString target = assetKey(link.assetId);
    if (!index->nodes.contains(target))
      continue;
    Node &asset = index->nodes[target];
    QString group, roleDisplay = link.role;
    // 同名角色必须结合实体类型；misc 角色保持其词表归属。
    const QString family = entityCategory(link.entityType.isEmpty()
                                              ? entityTypes.value(link.entityId)
                                              : link.entityType);
    for (const auto &role : catalogRoles())
      if (role.role == link.role &&
          (role.group == family || role.group == QLatin1String("misc"))) {
        group = role.group;
        roleDisplay = role.display;
        break;
      }
    if (group.isEmpty())
      group = family;
    if (!asset.categories.contains(group))
      asset.categories << group;
    QString source = entityKey(link.entityId);
    const bool pending = link.unresolved || link.entityId.isEmpty() ||
                         !index->nodes.contains(source);
    if (pending) {
      ++index->unresolvedCount;
      asset.unresolved = true;
      source = QStringLiteral("pending:") + link.assetId;
      Node pending;
      pending.id = source;
      pending.kind = Kind::Missing;
      pending.title = text("待关联实体");
      pending.detail = link.note;
      pending.unresolved = true;
      if (presentation.removedIds.contains(link.entityId))
        pending.title = text("关联实体已移入回收清单");
      index->nodes.insert(source, pending);
    }
    addEdge(*index,
            {source, target,
             roleDisplay + (link.isPrimary ? text(" · 主资料") : QString()),
             Relation::Linked, pending});
  }
  for (const QString &id : index->assets) {
    Node &asset = index->nodes[id];
    if (asset.categories.isEmpty())
      asset.categories << QStringLiteral("misc");
    const QString current = index->currentVersions.value(asset.assetId);
    if (!current.isEmpty()) {
      Node &v = index->nodes[current];
      v.current = true;
      asset.stage = v.stage;
      asset.path = v.path;
      asset.sourceUri = v.sourceUri;
      asset.sha256 = v.sha256;
      asset.external = v.external;
      asset.stale = v.stale;
    }
  }
  for (const auto &v : versions) {
    if (stopped(cancel))
      return {};
    if (presentation.removedIds.contains(v.assetId))
      continue;
    index->nodes[versionKey(v.id)].categories =
        index->nodes.value(assetKey(v.assetId)).categories;
    const Node output = index->nodes.value(versionKey(v.id));
    const bool recorded =
        output.evidence.value(QStringLiteral("schema")).toString() ==
        QLatin1String("paleo_evidence_v1");
    const QString activityId = QStringLiteral("activity:") + v.id;
    if (recorded) {
      Node activity = output;
      activity.id = activityId;
      activity.kind = Kind::Activity;
      activity.path.clear();
      activity.column = 1;
      activity.title = text("成果生成记录 · ") + output.title;
      activity.detail = output.evidence.value(QStringLiteral("model"))
                            .toMap()
                            .value(QStringLiteral("method"))
                            .toString();
      index->nodes.insert(activityId, activity);
      addEdge(*index, {activityId, versionKey(v.id), text("生成"),
                       Relation::Generated});
    }
    const auto connectParent = [&](const QString &parent, Relation relation,
                                   const QString &label) {
      if (parent.isEmpty())
        return;
      const QString key = versionKey(parent);
      if (!index->nodes.contains(key)) {
        Node missing;
        missing.id = key;
        missing.versionId = parent;
        missing.kind = Kind::Missing;
        missing.title = text("来源版本缺失");
        missing.detail = parent;
        missing.unresolved = true;
        if (presentation.removedIds.contains(cat.versionById(parent).assetId))
          missing.title = text("来源资料已移入回收清单");
        index->nodes.insert(key, missing);
        ++index->missingParents;
      }
      addEdge(*index,
              {key,
               recorded && relation == Relation::Derived ? activityId
                                                         : versionKey(v.id),
               label,
               recorded && relation == Relation::Derived ? Relation::Used
                                                         : relation});
    };
    QStringList parents = v.parentVersionIds;
    parents.removeDuplicates();
    QSet<QString> parentSet(parents.cbegin(), parents.cend());
    if (recorded)
      for (const QVariant &input :
           output.evidence.value(QStringLiteral("inputs")).toList()) {
        const QString parent =
            input.toMap().value(QStringLiteral("version_id")).toString();
        if (!parent.isEmpty() && !parentSet.contains(parent)) {
          parents.append(parent);
          parentSet.insert(parent);
        }
      }
    for (const QString &parent : parents)
      connectParent(parent, Relation::Derived, text("衍生自"));
    connectParent(v.extra.value(QStringLiteral("editBaseline")).toString(),
                  Relation::EditBaseline, text("编辑基线"));
    connectParent(v.extra.value(QStringLiteral("rollbackTo")).toString(),
                  Relation::Rollback, text("回滚目标"));
  }
  diagnoseCycles(*index, cancel);
  if (stopped(cancel))
    return {};
  std::sort(index->assets.begin(), index->assets.end());
  std::sort(index->entities.begin(), index->entities.end());
  report(progress, 45, "关系索引就绪");
  return index;
}
Graph query(const Index &index, const Query &q, const Cancel &cancel,
            const Progress &progress) {
  Graph graph;
  graph.totalAssets = index.assets.size();
  graph.totalVersions = index.versionCount;
  graph.totalEntities = index.entityCount;
  graph.unresolved = index.unresolvedCount;
  graph.missingParents = index.missingParents;
  graph.cycles = index.cycles;
  const int budget = qBound(20, q.maxNodes, 400),
            edgeBudget = qBound(20, q.maxEdges, 1200),
            depth = qBound(1, q.depth, 6);
  QHash<QString, int> visible;
  const auto add = [&](Node n) {
    if (visible.contains(n.id))
      return true;
    if (graph.nodes.size() >= budget)
      return false;
    visible[n.id] = graph.nodes.size();
    graph.nodes.append(n);
    return true;
  };
  const auto edge = [&](const Edge &e) {
    if (graph.edges.size() < edgeBudget)
      graph.edges.append(e);
    else
      ++graph.hiddenEdges;
  };
  const bool summary = q.mode == Mode::Overview && q.category.isEmpty() &&
                       q.stage.isEmpty() && q.search.isEmpty();
  if (q.mode == Mode::Entities) {
    for (const auto &id : index.entities) {
      if (stopped(cancel))
        return {};
      Node n = index.nodes[id];
      if (!matches(n, q))
        continue;
      n.column = 0;
      if (!add(n))
        ++graph.hidden;
    }
    graph.message =
        text("双击实体展开其资料关联；未挂接文件的实体也可在这里查看。");
  } else if (q.mode == Mode::Overview) {
    if (summary && index.entityCount) {
      Node n;
      n.id = QStringLiteral("group:entities");
      n.kind = Kind::Group;
      n.column = -1;
      n.title = text("实体目录 · %1 项").arg(index.entityCount);
      n.detail = text("井、地震测区、地层框架与辅助实体；双击展开");
      add(n);
    }
    QHash<QString, int> groups;
    for (const QString &id : index.assets) {
      if (stopped(cancel))
        return {};
      const Node &asset = index.nodes[id];
      if (!matches(asset, q))
        continue;
      for (const QString &category : asset.categories) {
        if (!q.category.isEmpty() && q.category != category)
          continue;
        const QString categoryId = QStringLiteral("group:") + category;
        const QString stageId =
            categoryId + QLatin1Char(':') +
            (asset.stage.isEmpty() ? QStringLiteral("NONE") : asset.stage);
        ++groups[categoryId];
        ++groups[stageId];
      }
      if (!summary) {
        if (graph.nodes.size() + 2 > budget - 32) {
          ++graph.hidden;
          continue;
        }
        Node n = asset;
        n.column = 2;
        add(n);
        const QString vid = index.currentVersions.value(asset.assetId);
        if (!vid.isEmpty()) {
          Node v = index.nodes[vid];
          v.column = 3;
          add(v);
          edge({id, vid, text("当前版本"), Relation::HasVersion});
        }
      }
    }
    QStringList keys = groups.keys();
    std::sort(keys.begin(), keys.end());
    for (const QString &id : keys) {
      const QStringList parts = id.split(QLatin1Char(':'));
      Node group;
      group.id = id;
      group.kind = Kind::Group;
      group.categories << parts.value(1);
      group.count = groups[id];
      group.stage = parts.size() > 2 ? parts[2] : QString();
      group.column = parts.size() > 2 ? 1 : 0;
      group.title =
          (parts.size() > 2 ? stageText(group.stage) : categoryText(parts[1])) +
          text(" · %1 项").arg(group.count);
      group.detail = text("双击展开资料");
      add(group);
      if (parts.size() > 2)
        edge({QStringLiteral("group:") + parts[1], id, QString(),
              Relation::Grouped});
    }
    if (!summary)
      for (const Node &n : graph.nodes)
        if (n.kind == Kind::Asset)
          for (const QString &c : n.categories) {
            const QString group =
                QStringLiteral("group:") + c + QLatin1Char(':') +
                (n.stage.isEmpty() ? QStringLiteral("NONE") : n.stage);
            if (visible.contains(group))
              edge({group, n.id, QString(), Relation::Grouped});
          }
  } else {
    QString seed = q.focus;
    if (q.mode == Mode::Lineage && index.nodes.value(seed).kind == Kind::Asset)
      seed = index.currentVersions.value(index.nodes.value(seed).assetId);
    if (!index.nodes.contains(seed)) {
      graph.message =
          text("请从数据列表或文件预览选择资料，再打开关联或追溯。");
      return graph;
    }
    Node root = index.nodes[seed];
    root.column = 0;
    add(root);
    const bool derivedOnly = q.mode == Mode::Lineage;
    QSet<QString> hidden;
    // 上下游分别走有向边；不会经共享祖先把同级产物错误显示成下游。
    for (int direction : derivedOnly ? QList<int>{-1, 1} : QList<int>{-1}) {
      QQueue<QPair<QString, int>> queue;
      QSet<QString> visited;
      queue.enqueue({seed, 0});
      visited.insert(seed);
      while (!queue.isEmpty()) {
        if (stopped(cancel))
          return {};
        const auto [id, distance] = queue.dequeue();
        if (distance >= depth)
          continue;
        QVector<int> incident =
            direction < 0 ? index.incoming.value(id) : index.outgoing.value(id);
        if (!derivedOnly)
          incident += direction < 0 ? index.outgoing.value(id)
                                    : index.incoming.value(id);
        for (int row : incident) {
          if (stopped(cancel))
            return {};
          const Edge &e = index.edges[row];
          if (derivedOnly && !lineage(e.relation))
            continue;
          const QString other = e.source == id ? e.target : e.source;
          if (visited.contains(other))
            continue;
          visited.insert(other);
          Node n = index.nodes.value(other);
          if (!derivedOnly &&
              (n.kind == Kind::Version || n.kind == Kind::Activity) &&
              !q.history && !n.current && other != seed)
            continue;
          if (n.kind != Kind::Missing && !matches(n, q))
            continue;
          n.column = derivedOnly ? direction * (distance + 1)
                     : n.kind == Kind::Entity || n.kind == Kind::Missing ? -1
                     : n.kind == Kind::Asset                             ? 0
                     : n.kind == Kind::Activity                          ? 2
                                                                         : 1;
          if (!add(n)) {
            hidden.insert(n.id);
            continue;
          }
          queue.enqueue({other, distance + 1});
        }
      }
    }
    graph.hidden = hidden.size();
    for (const Edge &e : index.edges) {
      if (stopped(cancel))
        return {};
      if (visible.contains(e.source) && visible.contains(e.target) &&
          (!derivedOnly || lineage(e.relation)))
        edge(e);
    }
    if (derivedOnly && graph.edges.isEmpty())
      graph.message = text("该版本没有记录来源或下游；不推断未记录的血缘。");
    else if (derivedOnly && root.kind == Kind::Version &&
             root.stage != QLatin1String("RAW")) {
      bool hasInputs = false;
      for (int row : index.incoming.value(seed)) {
        const Edge &e = index.edges[row];
        if (e.relation == Relation::Derived || e.relation == Relation::Used)
          hasInputs = true;
        if (e.relation == Relation::Generated)
          for (int input : index.incoming.value(e.source))
            if (index.edges[input].relation == Relation::Used)
              hasInputs = true;
      }
      if (!hasInputs)
        graph.message =
            text("该版本未登记计算输入；图中仅显示已经记录的关系。");
    }
  }
  if (stopped(cancel))
    return {};
  report(progress, 75, "计算图谱布局");
  layout(graph, cancel);
  for (Node &node : graph.nodes) {
    if (stopped(cancel))
      return {};
    if (node.evidence.isEmpty())
      continue;
    node.evidenceText =
        QString::fromUtf8(QJsonDocument::fromVariant(node.evidence)
                              .toJson(QJsonDocument::Indented));
    constexpr int detailLimit = 64 * 1024;
    if (node.evidenceText.size() > detailLimit)
      node.evidenceText =
          node.evidenceText.left(detailLimit) +
          text(
              "\n…证据较长，图谱显示前 64K 字符；完整记录保存在该资料版本中。");
  }
  if (!stopped(cancel))
    report(progress, 100, "图谱就绪");
  return graph;
}
} // namespace paleo::datagraph
