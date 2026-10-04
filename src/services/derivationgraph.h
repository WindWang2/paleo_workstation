// 层：数据
#pragma once

#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

class DataCatalog;

namespace paleo::derivation
{
enum class Kind { Raw, Derived, External };
struct Node
{
  QString versionId, assetId, versionName, assetName, stage, staleReason;
  Kind kind = Kind::Raw;
  bool stale = false;
  int column = 0;
};
struct Edge
{
  QString parentId, childId; // 箭头口径固定：parent → child，源流向下游。
};
struct Choice { QString id, name; };
struct Query
{
  QString entityId, assetId, versionId; // 当前上下文；显式版本优先于资产/实体。
  QString entityFilter, assetFilter;
  int kindFilter = -1; // -1 = 全部，否则 Kind
  bool staleOnly = false; // stale + 在当前谱系内的源版本（解释过期原因）
  int upstreamDepth = 2, downstreamDepth = 2, maxNodes = 80;
};
struct Graph
{
  QVector<Node> nodes;
  QVector<Edge> edges;
  QVector<Choice> entities;
  QString message;
  bool available = false;
  int collapsedUpstream = 0, collapsedDownstream = 0, collapsedSeeds = 0;
  int filtered = 0, missingParents = 0;
};

// 只读服务；所有事实经 DataCatalog API。闭包、深度、过滤、选中闭包在
// 服务层算；视图只消费值对象。无新增 catalog 接口或第三方布局依赖。
class Service
{
public:
  static Graph build(const DataCatalog *catalog, const Query &query);
  static QSet<QString> selectionClosure(const DataCatalog *catalog, const Graph &graph, const QString &versionId);
};
} // namespace paleo::derivation
