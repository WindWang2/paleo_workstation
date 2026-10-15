// 层：数据
#include "datagraph.h"
#include "../catalog/catalogroles.h"
#include "../catalog/datacatalog.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QQueue>
#include <QSet>
#include <algorithm>
#include <limits>

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
  // 即时检索沿用上游量化决定；增加层位过滤不建立重复检索索引。
  return (q.horizon.isEmpty() || n.horizon.isEmpty() || n.horizon == q.horizon) &&
         (q.category.isEmpty() || n.categories.contains(q.category)) &&
         (q.stage.isEmpty() || n.stage == q.stage ||
          (q.stage == QLatin1String("NONE") && n.stage.isEmpty())) &&
         (q.search.isEmpty() ||
          (n.title + QLatin1Char(' ') + n.detail + QLatin1Char(' ') + n.path)
              .contains(q.search, Qt::CaseInsensitive));
}
void layout(Graph &graph, const Cancel &cancel) {
  // 有界子图：稳定列布局，双向重心排序减少交叉；同名节点以稳定 ID 打破平局。
  // 边端点一次性解析成节点行号并建邻接表：重心各趟只看相邻边，
  // 不再对每节点全量扫边做 QString 等值比较。
  QHash<int, QVector<int>> columns;
  for (int i = 0; i < graph.nodes.size(); ++i)
    columns[graph.nodes[i].column].append(i);
  QList<int> keys = columns.keys();
  std::sort(keys.begin(), keys.end());
  const int count = graph.nodes.size();
  QHash<QString, int> indexOf;
  indexOf.reserve(count);
  for (int i = 0; i < count; ++i)
    indexOf.insert(graph.nodes[i].id, i);
  QVector<QPair<int, int>> edgeRows;
  QVector<QVector<int>> incident(count);
  edgeRows.reserve(graph.edges.size());
  for (const Edge &e : graph.edges) {
    const auto s = indexOf.constFind(e.source);
    const auto t = indexOf.constFind(e.target);
    const int a = s == indexOf.cend() ? -1 : s.value();
    const int b = t == indexOf.cend() ? -1 : t.value();
    edgeRows.append({a, b});
    if (a < 0 || b < 0)
      continue;
    incident[a].append(edgeRows.size() - 1);
    if (a != b)
      incident[b].append(edgeRows.size() - 1);
  }
  QVector<int> rows(count, 0);
  QVector<double> scores(count, 0);
  for (int c : keys) {
    auto &items = columns[c];
    std::sort(items.begin(), items.end(), [&](int a, int b) {
      return graph.nodes[a].title == graph.nodes[b].title
                 ? graph.nodes[a].id < graph.nodes[b].id
                 : graph.nodes[a].title < graph.nodes[b].title;
    });
    for (int i = 0; i < items.size(); ++i)
      rows[items[i]] = i;
  }
  for (int pass = 0; pass < 4 && !stopped(cancel); ++pass) {
    QList<int> order = keys;
    if (pass % 2)
      std::reverse(order.begin(), order.end());
    for (int c : order) {
      auto &items = columns[c];
      for (int i : items) {
        double sum = 0;
        int degree = 0;
        for (int row : incident[i]) {
          const auto endpoints = edgeRows[row];
          const int other = endpoints.first == i ? endpoints.second
                                                : endpoints.first;
          if (other < 0)
            continue;
          sum += rows[other];
          ++degree;
        }
        scores[i] = degree ? sum / degree : rows[i];
      }
      std::stable_sort(items.begin(), items.end(),
                       [&](int a, int b) { return scores[a] < scores[b]; });
      for (int i = 0; i < items.size(); ++i)
        rows[items[i]] = i;
    }
  }
  for (int c : keys)
    for (int i : columns[c])
      graph.nodes[i].position = QPointF(c * 310.0, rows[i] * 112.0);
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
std::shared_ptr<const Index> buildIndex(const DataCatalog &snapshot,
                                        const Presentation &presentation,
                                        const Cancel &cancel,
                                        const Progress &progress) {
  auto index = std::make_shared<Index>();
  report(progress, 5, "读取资料关系");
  const auto entities = snapshot.entities();
  const auto assets = snapshot.assets();
  const auto versions = snapshot.versions();
  const auto links = snapshot.links();
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
    n.horizon = v.extra.value("horizon").toString();
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
        if (presentation.removedIds.contains(snapshot.versionById(parent).assetId))
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
    // 数据列表的通用回滚（assetops）把目标同时记进 parentVersionIds 与
    // extra.rollbackOf：图谱按回滚参考边呈现（与 welltops rollbackTo 同口径），
    // 不把回滚冒充成衍生。
    const QString rollbackOf =
        v.extra.value(QStringLiteral("rollbackOf")).toString();
    if (!rollbackOf.isEmpty())
      parents.removeAll(rollbackOf);
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
    QHash<QString, QString> inputRoles;
    for (const auto &input : output.evidence.value("inputs").toList())
      inputRoles.insert(input.toMap().value("version_id").toString(), input.toMap().value("role").toString());
    QHash<QString, QVariantList> references;
    for (const auto &entry : v.extra.value("interpretation_reference_versions").toList())
      references[entry.toMap().value("version_id").toString()].append(entry);
    for (const QString &parent : parents) {
      const auto role = inputRoles.value(parent);
      if (references.contains(parent)) {
        QSet<QString> seen;
        for (const auto &entry : references.value(parent)) {
          const auto ref = entry.toMap(); const auto r = ref.value("role", "REFERENCES").toString();
          const auto key = r + (ref.value("tombstone").toBool() ? ":history" : ":active");
          if (seen.contains(key)) continue; seen.insert(key);
          const auto relation = r == "SUPPORTED_BY" ? Relation::Supported : r == "CONTRADICTED_BY" ? Relation::Contradicted :
              r == "CONSTRAINED_BY" ? Relation::Constrained : Relation::References;
          QString label = r == "SUPPORTED_BY" ? text("支撑解释") : r == "CONTRADICTED_BY" ? text("反驳解释") :
              r == "CONSTRAINED_BY" ? text("约束解释") : text("解释引用（非计算输入）");
          if (ref.value("tombstone").toBool()) label += text(" · 历史已解除");
          connectParent(parent, relation, label);
        }
        if (role == "DERIVED_FROM") connectParent(parent, Relation::Derived, text("计算来源"));
      } else if (role == "CONTRADICTED_BY") connectParent(parent, Relation::Contradicted, text("反驳解释"));
      else if (role == "SUPPORTED_BY") connectParent(parent, Relation::Supported, text("支撑解释"));
      else if (role == "CONSTRAINED_BY") connectParent(parent, Relation::Constrained, text("约束解释"));
      else if (role == "REFERENCES") connectParent(parent, Relation::References, text("解释引用（非计算输入）"));
      else connectParent(parent, Relation::Derived, text("衍生自"));
    }
    connectParent(v.extra.value(QStringLiteral("editBaseline")).toString(),
                  Relation::EditBaseline, text("编辑基线"));
    connectParent(v.extra.value(QStringLiteral("rollbackTo")).toString(),
                  Relation::Rollback, text("回滚目标"));
    connectParent(rollbackOf, Relation::Rollback, text("回滚目标"));
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
  graph.query = q;
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
  bool overlay = false;
  const int localCount = q.featureEvidence.value("records").toList().size() + q.featureEvidence.value("generation_records").toList().size();
  const bool hasOverlay = q.mode != Mode::Impact && !q.featureEvidence.value("feature_uid").toString().isEmpty() &&
      (q.horizon.isEmpty() || q.featureEvidence.value("horizon").toString() == q.horizon);
  const int baseBudget = !hasOverlay ? budget :
      qMax(1, budget - qMin(budget - 1, 3 + qMin(localCount, 50) * 2));
  const auto add = [&](Node n) {
    if (visible.contains(n.id))
      return true;
    if (graph.nodes.size() >= (overlay ? budget : baseBudget))
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
        // 分组节点（类别 + 类别:阶段，理论上限 32 个）后置入画布；
        // 预算下限 20 时至少给首份资料留 2 槽——不再把全部资料藏掉。
        const int reserve = qMin(32, qMax(0, budget - 2));
        if (graph.nodes.size() + 2 > budget - reserve) {
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
      if (!add(group))
        ++graph.hidden;
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
    if (!index.nodes.contains(seed) && !q.featureEvidence.isEmpty()) seed = versionKey(q.featureEvidence.value("version_id").toString());
    if ((q.mode == Mode::Lineage || q.mode == Mode::Impact) && index.nodes.value(seed).kind == Kind::Asset)
      seed = index.currentVersions.value(index.nodes.value(seed).assetId);
    if (!index.nodes.contains(seed)) {
      graph.message =
          text("请从数据列表或文件预览选择资料，再打开关联或追溯。");
      return graph;
    }
    Node root = index.nodes[seed];
    root.column = 0;
    add(root);
    const bool impact = q.mode == Mode::Impact;
    const bool derivedOnly = q.mode == Mode::Lineage || impact;
    // #443：默认 0 = 不封顶；调用方显式设置正数时再钳制到 [512, 20000]。
    const bool unlimitedTraversal = q.maxTraversalEdges <= 0;
    const int traversalBudget = unlimitedTraversal ? std::numeric_limits<int>::max()
                                                   : qBound(512, q.maxTraversalEdges, 20000);
    const auto traversable = [&](Relation relation) {
      return lineage(relation) || (impact && (relation == Relation::Supported || relation == Relation::Contradicted ||
          relation == Relation::Constrained || relation == Relation::References));
    };
    QSet<QString> hidden;
    // 上下游分别走有向边；不会经共享祖先把同级产物错误显示成下游。
    for (int direction : impact ? QList<int>{1} : derivedOnly ? QList<int>{-1, 1} : QList<int>{-1}) {
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
        // Implicitly shared adjacency ranges avoid detaching/copying every
        // edge of a high-degree node before the work budget can be checked.
        QVector<QVector<int>> incident{direction < 0 ? index.incoming.value(id) : index.outgoing.value(id)};
        if (!derivedOnly) incident << (direction < 0 ? index.outgoing.value(id) : index.incoming.value(id));
        for (const auto &range : incident) {
        for (int row : range) {
          if (stopped(cancel))
            return {};
          if (++graph.examinedEdges > traversalBudget) { graph.workTruncated = true; break; }
          const Edge &e = index.edges[row];
          if (derivedOnly && !traversable(e.relation))
            continue;
          const QString other = e.source == id ? e.target : e.source;
          if (visited.contains(other))
            continue;
          visited.insert(other);
          Node n = index.nodes.value(other);
          if (impact) {
            n.dependencyDepth = distance + 1;
            n.detail += distance == 0 ? text(" · 直接依赖（来源关系，未确定空间影响）") :
                text(" · 下游潜在影响（未确定空间范围）");
          }
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
        if (graph.workTruncated) break;
        }
        if (graph.workTruncated) break;
      }
      if (graph.workTruncated) break;
    }
    graph.hidden = hidden.size();
    // Reuse the upstream visible-source adjacency collection and stable row
    // ordering, bounded for high-degree nodes and semantic downstream queries.
    QVector<int> rows;
    int edgeWork = 0;
    for (const Node &n : graph.nodes) {
      for (int row : index.outgoing.value(n.id)) {
        if (stopped(cancel)) return {};
        if (++edgeWork > traversalBudget) { graph.workTruncated = true; break; }
        const Edge &e = index.edges[row];
        if (visible.contains(e.target) && (!derivedOnly || traversable(e.relation))) rows.append(row);
      }
      if (edgeWork > traversalBudget) break;
    }
    std::sort(rows.begin(), rows.end());
    for (int row : rows) edge(index.edges[row]);
    if (impact) graph.message = text("来源版本的直接依赖与派生下游；仅表示需要复核的潜在影响，不代表确定的地质空间影响区。");
    else if (derivedOnly && graph.edges.isEmpty())
      graph.message = text("该版本没有记录来源或下游；不推断未记录的血缘。");
    else if (derivedOnly && !graph.workTruncated && root.kind == Kind::Version &&
             root.stage != QLatin1String("RAW")) {
      bool hasInputs = false;
      for (int row : index.incoming.value(seed)) {
        if (stopped(cancel)) return {};
        if (++graph.examinedEdges > traversalBudget) { graph.workTruncated = true; break; }
        const Edge &e = index.edges[row];
        if (e.relation == Relation::Derived || e.relation == Relation::Used)
          hasInputs = true;
        if (e.relation == Relation::Generated)
          for (int input : index.incoming.value(e.source)) {
            if (stopped(cancel)) return {};
            if (++graph.examinedEdges > traversalBudget) { graph.workTruncated = true; break; }
            if (index.edges[input].relation == Relation::Used) { hasInputs = true; break; }
          }
        if (hasInputs || graph.workTruncated) break;
      }
      if (!hasInputs && !graph.workTruncated)
        graph.message =
            text("该版本未登记计算输入；图中仅显示已经记录的关系。");
    }
  }
  overlay = true;
  const auto snapshot = q.featureEvidence;
  const auto featureUid = snapshot.value("feature_uid").toString();
  if (!featureUid.isEmpty() && q.mode != Mode::Impact && (q.horizon.isEmpty() || snapshot.value("horizon").toString() == q.horizon)) {
    const auto interpretation = snapshot.value("interpretation").toMap();
    Node feature;
    feature.id = "feature:" + snapshot.value("project_id").toString() + ":" + snapshot.value("version_id").toString() + ":" + featureUid;
    feature.kind = Kind::Feature; feature.title = text("地图图斑 · ") + featureUid.left(12);
    feature.featureUid = featureUid; feature.horizon = snapshot.value("horizon").toString();
    feature.versionId = snapshot.value("version_id").toString(); feature.assetId = snapshot.value("asset_id").toString();
    feature.layerId = snapshot.value("layer_id").toString(); feature.column = 3;
    feature.reviewState = interpretation.value("status", "Proposed").toString();
    feature.detail = text("当前图斑解释：%1；来源仅按已登记关系展示").arg(feature.reviewState);
    add(feature);
    if (visible.contains(versionKey(feature.versionId))) edge({versionKey(feature.versionId), feature.id, text("空间成果"), Relation::Materializes});
    if (!snapshot.value("error").toString().isEmpty()) {
      Node problem = feature; problem.id = "missing-evidence:" + feature.id; problem.kind = Kind::Missing;
      problem.title = text("图斑证据损坏"); problem.detail = snapshot.value("error").toString(); problem.unresolved = true;
      if (add(problem)) edge({problem.id, feature.id, text("证据不可读取"), Relation::References, true});
      ++graph.unresolved;
    } else {
    Node i = feature; i.kind = Kind::Interpretation; i.interpretationId = interpretation.value("id").toString();
    i.id = "interpretation:" + feature.id; i.title = text("地质解释 · ") + i.reviewState; i.column = 2; i.evidence = interpretation;
    if (depth >= 1 && add(i)) edge({i.id, feature.id, text("呈现为"), Relation::Materializes});
    Node assertion = i; assertion.kind = Kind::Assertion; assertion.assertionId = interpretation.value("assertion_id").toString();
    assertion.id = "assertion:" + feature.id; assertion.title = text("相类型判断 · %1").arg(snapshot.value("facies_code").toString()); assertion.column = 1;
    assertion.evidence.clear();
    if (depth >= 2 && add(assertion)) edge({assertion.id, i.id, text("形成解释"), Relation::Informs});
    auto records = snapshot.value("records").toList();
    records += snapshot.value("spatial_candidates").toList();
    records += snapshot.value("generation_records").toList();
    QVector<int> matches;
    for (int n = 0; n < records.size(); ++n) {
      if (stopped(cancel)) return {};
      const auto r = records[n].toMap();
      if (!r.value("unlinked").toBool() && (q.evidenceType.isEmpty() || q.evidenceType == r.value("type", "reference").toString()) &&
          (q.reviewState.isEmpty() || q.reviewState == feature.reviewState)) matches << n;
    }
    const int offset = qBound(0, q.evidenceOffset, int(matches.size()));
    const int end = qMin(int(matches.size()), offset + qBound(1, q.evidenceLimit, 100));
    graph.hidden += qMax(0, int(matches.size()) - (end - offset));
    for (int row = offset; row < end; ++row) {
      if (stopped(cancel)) return {};
      const auto r = records[matches[row]].toMap();
      if (r.value("unlinked").toBool()) continue;
      const auto type = r.value("type", "reference").toString();
      if (depth < 3) { ++graph.hidden; continue; }
      Node e = feature; e.kind = Kind::Evidence; e.evidenceId = r.value("evidence_id", r.value("id")).toString();
      if (e.evidenceId.isEmpty()) { ++graph.unresolved; continue; }
      e.id = "evidence:" + feature.id + ":" + r.value("id", e.evidenceId).toString(); e.evidenceType = type; e.column = 0;
      e.title = text("证据 · ") + r.value("source_title", r.value("name", r.value("text", type))).toString();
      e.detail = r.value("text").toString(); e.evidence = r;
      e.unresolved = r.value("source_missing", r.value("missing")).toBool();
      const auto role = r.value("role", r.value("relation", "REFERENCES")).toString();
      Relation relation = role == "SUPPORTED_BY" ? Relation::Supported : role == "CONTRADICTED_BY" ? Relation::Contradicted :
          role == "CONSTRAINED_BY" ? Relation::Constrained : Relation::References;
      const auto label = role == "SUPPORTED_BY" ? text("支持") : role == "CONTRADICTED_BY" ? text("反驳") :
          role == "CONSTRAINED_BY" ? text("约束") : role == "DERIVED_FROM" ? text("生成来源（未推断支持）") : text("参考");
      if (!add(e)) { ++graph.hidden; continue; }
      if (visible.contains(assertion.id)) edge({e.id, assertion.id, label, relation, e.unresolved});
      const auto source = r.value("source_version", r.value("version_id")).toString();
      if (source.isEmpty() || depth < 4) continue;
      Node sourceNode = index.nodes.value(versionKey(source));
      if (sourceNode.id.isEmpty()) { sourceNode.id = versionKey(source); sourceNode.versionId = source; sourceNode.kind = Kind::Missing;
        sourceNode.title = text("证据来源版本缺失"); sourceNode.unresolved = true; ++graph.missingParents; }
      sourceNode.column = -1;
      if (add(sourceNode)) edge({sourceNode.id, e.id, text("来源版本"), Relation::References, sourceNode.unresolved});
      else ++graph.hidden;
    }
    if (graph.hidden) graph.message = text("局部证据图谱已裁剪；可缩小筛选或按证据页继续查询");
    }
  }
  if (graph.workTruncated) graph.message += text(" 查询达到工作量上限，结果为局部子图；请选择邻接节点继续展开或缩小范围。");
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
