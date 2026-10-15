// 层：数据
#pragma once
#include <QHash>
#include <QPointF>
#include <QSet>
#include <QStringList>
#include <QVariantMap>
#include <QVector>
#include <functional>
#include <memory>

class DataCatalog;
namespace paleo::datagraph {
enum class Kind { Entity, Asset, Version, Activity, Group, Missing, Evidence, Assertion, Interpretation, Feature };
enum class Relation {
  Linked,
  HasVersion,
  Derived,
  Used,
  Generated,
  EditBaseline,
  Rollback,
  Grouped,
  Supported, Contradicted, Constrained, References, Informs, Materializes
};
enum class Mode { Overview, Relations, Lineage, Entities, Impact };
struct Node {
  QString id, title, detail, entityId, assetId, versionId, stage, path,
      sourceUri, sha256, evidenceText;
  QStringList categories;
  QVariantMap evidence;
  QString featureUid, evidenceId, assertionId, interpretationId, horizon,
      evidenceType, reviewState, layerId;
  Kind kind = Kind::Asset;
  bool current = false, external = false, stale = false, unresolved = false,
       cycle = false;
  QPointF position;
  int column = 0, count = 0;
  int dependencyDepth = 0;
};
struct Edge {
  QString source, target, label;
  Relation relation = Relation::Linked;
  bool unresolved = false;
};
struct Index {
  QHash<QString, Node> nodes;
  QVector<Edge> edges;
  QHash<QString, QVector<int>> incoming, outgoing;
  QStringList assets, entities;
  QHash<QString, QString> currentVersions;
  int entityCount = 0, versionCount = 0, unresolvedCount = 0,
      missingParents = 0, cycles = 0;
};
struct Query {
  Mode mode = Mode::Overview;
  QString focus, category, stage, search;
  bool history = false;
  int depth = 2, maxNodes = 200, maxEdges = 600;
  QString horizon, evidenceType, reviewState;
  int evidenceOffset = 0, evidenceLimit = 50;
  int maxTraversalEdges = 0; // 0 = unlimited (#443)
  // Selected feature's immutable owner-thread snapshot only. It participates
  // in equality/cache keys; never added to the shared project Index.
  QVariantMap featureEvidence;
  bool operator==(const Query &) const = default;
};
struct Graph {
  QVector<Node> nodes;
  QVector<Edge> edges;
  // 产出该图的查询条件（含被钳制前的原始字段）：导出/回执与图内容
  // 同源，不取工作流当前待算的新查询。
  Query query;
  int totalAssets = 0, totalVersions = 0, totalEntities = 0;
  int unresolved = 0, missingParents = 0, cycles = 0, hidden = 0,
      hiddenEdges = 0;
  QString message;
  bool workTruncated = false;
  int examinedEdges = 0;
};
using Cancel = std::function<bool()>;
using Progress = std::function<void(double, const QString &)>;
// 现有文件管理 sidecar 的纯值快照；图谱不自行读取/写入第二份管理状态。
struct Presentation {
  QSet<QString> removedIds;
  QHash<QString, QString> assetTypes, entityNames;
  QHash<QString, QStringList> tags;
  bool operator==(const Presentation &) const = default;
};
QString entityKey(const QString &id);
QString assetKey(const QString &id);
QString versionKey(const QString &id);
QString categoryText(const QString &category);
QString stageText(const QString &stage);
std::shared_ptr<const Index> buildIndex(const DataCatalog &snapshot,
                                        const Presentation &presentation,
                                        const Cancel &cancel,
                                        const Progress &progress);
Graph query(const Index &index, const Query &q, const Cancel &cancel,
            const Progress &progress);
// 数据服务仅输出值对象与坐标；布局和查询均由后台任务调用。
} // namespace paleo::datagraph
