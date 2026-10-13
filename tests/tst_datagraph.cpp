// 任务 A（数据/资产/图谱深化 2026-10-13）：资料图谱服务/工作流契约回归。
// #424 交付的 datagraph 家族此前零直接测试（DATA_GRAPH.md:101 自证）；
// 本套件覆盖索引构建、四种查询模式、预算诚实性、缓存/导出/取消契约与
// 合成规模行为。改前红用例见 docs/progress/data-asset-graph-deepening-20261013.md。
#include <QtTest>
#include <cmath>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "../src/catalog/datacatalog.h"
#include "../src/io/perffixtures.h"
#include "../src/services/datagraph.h"
#include "../src/workflow/datagraphworkflow.h"

using namespace paleo::datagraph;

namespace {
CatalogEntity entity(const QString &id, const QString &type,
                     const QString &name = QString()) {
  CatalogEntity e;
  e.id = id;
  e.entityType = type;
  e.name = name.isEmpty() ? id : name;
  e.hasSurface = true;
  e.surfaceX = 500000.0;
  e.surfaceY = 4000000.0;
  return e;
}
CatalogAsset asset(const QString &id, const QString &type) {
  CatalogAsset a;
  a.id = id;
  a.type = type;
  a.format = QStringLiteral("las");
  a.displayName = id + QStringLiteral(".las");
  return a;
}
CatalogVersion version(const QString &id, const QString &assetId,
                       const QStringList &parents = {}, int no = 1,
                       const QString &stage = QString()) {
  CatalogVersion v;
  v.id = id;
  v.assetId = assetId;
  v.versionNumber = no;
  v.stage = stage.isEmpty() ? (parents.isEmpty() ? QStringLiteral("RAW")
                                                 : QStringLiteral("DERIVED"))
                            : stage;
  v.parentVersionIds = parents;
  v.managed = true;
  v.fileName = id + QStringLiteral(".las");
  v.path = DataCatalog::managedPath(v.stage, assetId, id, v.fileName);
  return v;
}
EntityAssetLink link(const QString &entityId, const QString &assetId,
                     const QString &role = QStringLiteral("well_log")) {
  EntityAssetLink l;
  l.entityType = QStringLiteral("well");
  l.entityId = entityId;
  l.assetId = assetId;
  l.role = role;
  l.isPrimary = true;
  return l;
}
QVariantMap evidenceV1(const QStringList &inputVersions,
                       const QString &method = QStringLiteral("test_method")) {
  QVariantList inputs;
  for (const QString &id : inputVersions)
    inputs.append(QVariantMap{{"version_id", id}, {"role", "source"}});
  return QVariantMap{
      {"schema", QStringLiteral("paleo_evidence_v1")},
      {"model", QVariantMap{{"method", method}}},
      {"inputs", inputs}};
}
int countKind(const Graph &g, Kind kind) {
  int out = 0;
  for (const Node &n : g.nodes)
    if (n.kind == kind)
      ++out;
  return out;
}
bool hasEdge(const QVector<Edge> &edges, const QString &source,
             const QString &target, Relation relation) {
  for (const Edge &e : edges)
    if (e.source == source && e.target == target && e.relation == relation)
      return true;
  return false;
}
} // namespace

class TestDataGraph : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() {
    qRegisterMetaType<paleo::datagraph::Graph>();
  }
  void indexStructureAndEvidenceProjection();
  void unresolvedAndMissingSourcesAreHonest();
  void cycleDiagnosisFlagsComputationalLoop();
  void overviewSummaryAndCategoryExpansion();
  void overviewSmallBudgetKeepsAssetsAndCountsHidden();
  void overviewGroupOverflowCountsHidden();
  void entitiesModeFilterAndSearch();
  void relationsNeighborhoodIncludesSiblings();
  void lineageDoesNotPromoteSharedAncestorSiblings();
  void lineageWithoutInputsIsHonest();
  void rollbackAndEditBaselineReferenceEdges();
  void assetRollbackIsReferenceNotDerived();
  void evidenceTextTruncatedAt64K();
  void cancelReturnsEmptyResults();
  void layoutIsDeterministicAndColumnar();
  void searchMatchesTitleDetailAndStageText();

  void workflowRequestReadyAndCacheReuse();
  void workflowCacheIsLruNotFifo();
  void workflowMutationInvalidatesCacheAndIndex();
  void workflowPresentationInvalidatesIndex();
  void workflowCancelSuppressesLaunch();
  void workflowContextSwitchResets();
  void workflowExportDescribesRenderedQuery();
  void workflowExportReceiptSurvivesNewRequest();
  void workflowExportDroppedAfterContextSwitch();

  void scaleSyntheticLineageCatalog();

private:
  // setContext() 本身会先发一张空图；这里等待“下一张”图到达。
  // QTRY 宏的失败路径是裸 return，不能放在带返回值的函数里。
  void waitNextReady(QSignalSpy &spy, int previousCount) {
    QTRY_COMPARE(spy.count(), previousCount + 1);
  }
  paleo::datagraph::Graph lastGraph(QSignalSpy &spy) {
    return spy.last().first().value<paleo::datagraph::Graph>();
  }
};

// ---- 服务层：索引结构 / 证据投影 / 诚实呈现 ----

void TestDataGraph::indexStructureAndEvidenceProjection() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  QVERIFY(cat.addEntity(entity(QStringLiteral("w1"), QStringLiteral("well"))));
  QVERIFY(cat.addAsset(asset(QStringLiteral("raw"), QStringLiteral("well_log"))));
  const auto v1 = version(QStringLiteral("v1"), QStringLiteral("raw"));
  QVERIFY(cat.addVersion(v1));
  QVERIFY(cat.addLink(link(QStringLiteral("w1"), QStringLiteral("raw"))));
  QVERIFY(cat.addAsset(asset(QStringLiteral("out"), QStringLiteral("map_product"))));
  auto out = version(QStringLiteral("v2"), QStringLiteral("out"), {v1.id}, 2);
  out.extra.insert(QStringLiteral("evidence"),
                   evidenceV1({v1.id}, QStringLiteral("kriging")));
  QVERIFY(cat.addVersion(out));

  const auto index = buildIndex(cat, {}, nullptr, nullptr);
  QVERIFY(index);
  QCOMPARE(index->entityCount, 1);
  QCOMPARE(index->versionCount, 2);
  QCOMPARE(index->unresolvedCount, 0);
  QCOMPARE(index->missingParents, 0);
  // 证据投影：成果版本带 paleo_evidence_v1 → Activity 节点 + Generated 边；
  // 输入版本经 Used 边挂到 Activity，而不是直连成果版本。
  QVERIFY(index->nodes.contains(QStringLiteral("activity:v2")));
  QCOMPARE(index->nodes.value(QStringLiteral("activity:v2")).kind, Kind::Activity);
  QVERIFY(hasEdge(index->edges, QStringLiteral("activity:v2"),
                  QStringLiteral("version:v2"), Relation::Generated));
  QVERIFY(hasEdge(index->edges, QStringLiteral("version:v1"),
                  QStringLiteral("activity:v2"), Relation::Used));
  QCOMPARE(index->nodes.value(QStringLiteral("version:v2")).evidence
               .value(QStringLiteral("schema")).toString(),
           QStringLiteral("paleo_evidence_v1"));
  QCOMPARE(index->currentVersions.value(QStringLiteral("out")),
           QStringLiteral("version:v2"));
}

void TestDataGraph::unresolvedAndMissingSourcesAreHonest() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  QVERIFY(cat.addEntity(entity(QStringLiteral("w1"), QStringLiteral("well"))));
  QVERIFY(cat.addAsset(asset(QStringLiteral("a"), QStringLiteral("well_log"))));
  QVERIFY(cat.addVersion(version(QStringLiteral("v1"), QStringLiteral("a"))));
  auto pending = link(QString(), QStringLiteral("a"));
  pending.unresolved = true;
  pending.note = QStringLiteral("导入时未匹配实体");
  QVERIFY(cat.addLink(pending));
  QVERIFY(cat.addVersion(version(QStringLiteral("v2"), QStringLiteral("a"),
                                 {QStringLiteral("ghost")}, 2)));

  const auto index = buildIndex(cat, {}, nullptr, nullptr);
  QVERIFY(index);
  QCOMPARE(index->unresolvedCount, 1);
  QCOMPARE(index->missingParents, 1);
  QVERIFY(index->nodes.contains(QStringLiteral("pending:a")));
  QCOMPARE(index->nodes.value(QStringLiteral("pending:a")).kind, Kind::Missing);
  QVERIFY(index->nodes.value(QStringLiteral("pending:a")).unresolved);
  QVERIFY(index->nodes.value(QStringLiteral("asset:a")).unresolved);
  QVERIFY(index->nodes.contains(QStringLiteral("version:ghost")));
  QCOMPARE(index->nodes.value(QStringLiteral("version:ghost")).kind, Kind::Missing);
}

void TestDataGraph::cycleDiagnosisFlagsComputationalLoop() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  QVERIFY(cat.addAsset(asset(QStringLiteral("a"), QStringLiteral("well_log"))));
  auto v1 = version(QStringLiteral("v1"), QStringLiteral("a"));
  auto v2 = version(QStringLiteral("v2"), QStringLiteral("a"), {v1.id}, 2);
  v1.parentVersionIds = {v2.id}; // 人造环：仅测试用
  QVERIFY(cat.addVersion(v1));
  QVERIFY(cat.addVersion(v2));

  const auto index = buildIndex(cat, {}, nullptr, nullptr);
  QVERIFY(index);
  QCOMPARE(index->cycles, 1);
  QVERIFY(index->nodes.value(QStringLiteral("version:v1")).cycle);
  QVERIFY(index->nodes.value(QStringLiteral("version:v2")).cycle);
  // editBaseline 参考环不计入计算血缘环。
  auto v3 = version(QStringLiteral("v3"), QStringLiteral("a"), {}, 3);
  v3.extra.insert(QStringLiteral("editBaseline"), v3.id); // 自参考
  QVERIFY(cat.addVersion(v3));
  const auto index2 = buildIndex(cat, {}, nullptr, nullptr);
  QVERIFY(index2);
  QCOMPARE(index2->cycles, 1);
}

void TestDataGraph::overviewSummaryAndCategoryExpansion() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  QVERIFY(cat.addEntity(entity(QStringLiteral("w1"), QStringLiteral("well"))));
  QVERIFY(cat.addAsset(asset(QStringLiteral("a"), QStringLiteral("well_log"))));
  QVERIFY(cat.addVersion(version(QStringLiteral("v1"), QStringLiteral("a"))));
  QVERIFY(cat.addLink(link(QStringLiteral("w1"), QStringLiteral("a"))));

  const auto index = buildIndex(cat, {}, nullptr, nullptr);
  QVERIFY(index);
  Query summary;
  const auto head = query(*index, summary, nullptr, nullptr);
  QVERIFY(head.nodes.size() <= summary.maxNodes);
  QVERIFY(countKind(head, Kind::Group) > 0);

  Query expanded;
  expanded.category = QStringLiteral("well");
  const auto list = query(*index, expanded, nullptr, nullptr);
  QCOMPARE(countKind(list, Kind::Asset), 1);
  QCOMPARE(countKind(list, Kind::Version), 1);
  QVERIFY(hasEdge(list.edges, QStringLiteral("asset:a"),
                  QStringLiteral("version:v1"), Relation::HasVersion));
  bool wellGroup = false;
  for (const Node &n : list.nodes)
    if (n.kind == Kind::Group && n.id == QStringLiteral("group:well")) {
      wellGroup = true;
      QCOMPARE(n.count, 1);
    }
  QVERIFY(wellGroup);
}

void TestDataGraph::overviewSmallBudgetKeepsAssetsAndCountsHidden() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  for (int i = 1; i <= 6; ++i) {
    const QString id = QString::number(i);
    QVERIFY(cat.addAsset(asset(QStringLiteral("a") + id,
                               QStringLiteral("well_log"))));
    QVERIFY(cat.addVersion(version(QStringLiteral("v") + id,
                                   QStringLiteral("a") + id)));
  }
  const auto index = buildIndex(cat, {}, nullptr, nullptr);
  QVERIFY(index);
  Query q;
  q.category = QStringLiteral("well"); // 进入非 summary 展开分支
  q.maxNodes = 20;                     // 钳制下限：保留槽不得吃光预算
  const auto g = query(*index, q, nullptr, nullptr);
  // 修前（budget-32 恒小于 0）：6 份资料全部隐藏、画布只剩分组。
  QCOMPARE(countKind(g, Kind::Asset), 1);
  QCOMPARE(g.hidden, 5);
  QVERIFY(g.nodes.size() <= 20);
}

void TestDataGraph::overviewGroupOverflowCountsHidden() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  for (int i = 0; i < 20; ++i) {
    const QString id = QString::number(i + 1);
    const QString stage =
        QStringLiteral("S") + QString::number(i + 1).rightJustified(2, QLatin1Char('0'));
    QVERIFY(cat.addAsset(asset(QStringLiteral("a") + id,
                               QStringLiteral("well_log"))));
    QVERIFY(cat.addVersion(version(QStringLiteral("v") + id,
                                   QStringLiteral("a") + id, {}, 1, stage)));
  }
  const auto index = buildIndex(cat, {}, nullptr, nullptr);
  QVERIFY(index);
  Query q;
  q.search = QStringLiteral("a"); // 非 summary；1 类别分组 + 20 阶段分组
  q.maxNodes = 20;
  const auto g = query(*index, q, nullptr, nullptr);
  QCOMPARE(g.nodes.size(), 20);
  QCOMPARE(countKind(g, Kind::Asset), 1);
  // 19 份被预留槽挡下的资料 + 3 个挤不进预算的分组，都要计入 hidden。
  QCOMPARE(g.hidden, 22);
}

void TestDataGraph::entitiesModeFilterAndSearch() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  QVERIFY(cat.addEntity(entity(QStringLiteral("w1"), QStringLiteral("well"),
                               QStringLiteral("Well-Alpha"))));
  QVERIFY(cat.addEntity(entity(QStringLiteral("w2"), QStringLiteral("well"),
                               QStringLiteral("Well-Beta"))));
  QVERIFY(cat.addEntity(entity(QStringLiteral("s1"),
                               QStringLiteral("seismic_survey"),
                               QStringLiteral("Survey-3D"))));
  const auto index = buildIndex(cat, {}, nullptr, nullptr);
  QVERIFY(index);
  Query q;
  q.mode = Mode::Entities;
  const auto all = query(*index, q, nullptr, nullptr);
  QCOMPARE(countKind(all, Kind::Entity), 3);
  q.search = QStringLiteral("ALPHA"); // 大小写不敏感
  const auto alpha = query(*index, q, nullptr, nullptr);
  QCOMPARE(countKind(alpha, Kind::Entity), 1);
  QCOMPARE(alpha.nodes.first().entityId, QStringLiteral("w1"));
}

void TestDataGraph::relationsNeighborhoodIncludesSiblings() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  QVERIFY(cat.addAsset(asset(QStringLiteral("src"), QStringLiteral("well_log"))));
  QVERIFY(cat.addVersion(version(QStringLiteral("r1"), QStringLiteral("src"))));
  QVERIFY(cat.addAsset(asset(QStringLiteral("a"), QStringLiteral("map_product"))));
  QVERIFY(cat.addVersion(version(QStringLiteral("va"), QStringLiteral("a"),
                                 {QStringLiteral("r1")}, 2)));
  QVERIFY(cat.addAsset(asset(QStringLiteral("b"), QStringLiteral("map_product"))));
  QVERIFY(cat.addVersion(version(QStringLiteral("vb"), QStringLiteral("b"),
                                 {QStringLiteral("r1")}, 2)));
  const auto index = buildIndex(cat, {}, nullptr, nullptr);
  QVERIFY(index);
  Query q;
  q.mode = Mode::Relations;
  q.focus = QStringLiteral("version:va");
  const auto g = query(*index, q, nullptr, nullptr);
  // 关联视图是有界邻域：经共享父版本可达同级产物（按邻域呈现，非下游）。
  bool siblingSeen = false;
  for (const Node &n : g.nodes)
    siblingSeen |= n.id == QStringLiteral("version:vb");
  QVERIFY(siblingSeen);
}

void TestDataGraph::lineageDoesNotPromoteSharedAncestorSiblings() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  QVERIFY(cat.addAsset(asset(QStringLiteral("src"), QStringLiteral("well_log"))));
  QVERIFY(cat.addVersion(version(QStringLiteral("r1"), QStringLiteral("src"))));
  QVERIFY(cat.addAsset(asset(QStringLiteral("a"), QStringLiteral("map_product"))));
  QVERIFY(cat.addVersion(version(QStringLiteral("va"), QStringLiteral("a"),
                                 {QStringLiteral("r1")}, 2)));
  QVERIFY(cat.addAsset(asset(QStringLiteral("b"), QStringLiteral("map_product"))));
  QVERIFY(cat.addVersion(version(QStringLiteral("vb"), QStringLiteral("b"),
                                 {QStringLiteral("r1")}, 2)));
  const auto index = buildIndex(cat, {}, nullptr, nullptr);
  QVERIFY(index);
  Query q;
  q.mode = Mode::Lineage;
  q.focus = QStringLiteral("version:va");
  const auto g = query(*index, q, nullptr, nullptr);
  for (const Node &n : g.nodes)
    QVERIFY2(n.id != QStringLiteral("version:vb"),
             "共享祖先的同级产物不得冒充下游");
  QVERIFY(hasEdge(g.edges, QStringLiteral("version:r1"),
                  QStringLiteral("version:va"), Relation::Derived));
}

void TestDataGraph::lineageWithoutInputsIsHonest() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  QVERIFY(cat.addAsset(asset(QStringLiteral("a"), QStringLiteral("well_log"))));
  QVERIFY(cat.addVersion(version(QStringLiteral("v1"), QStringLiteral("a"))));
  const auto index = buildIndex(cat, {}, nullptr, nullptr);
  QVERIFY(index);
  Query q;
  q.mode = Mode::Lineage;
  q.focus = QStringLiteral("version:v1");
  const auto g = query(*index, q, nullptr, nullptr);
  QVERIFY(!g.message.isEmpty()); // 无记录血缘 → 明确文案，不推断
}

void TestDataGraph::rollbackAndEditBaselineReferenceEdges() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  QVERIFY(cat.addAsset(asset(QStringLiteral("a"), QStringLiteral("well_log"))));
  QVERIFY(cat.addVersion(version(QStringLiteral("v1"), QStringLiteral("a"))));
  QVERIFY(cat.addVersion(version(QStringLiteral("v2"), QStringLiteral("a"),
                                 {QStringLiteral("v1")}, 2)));
  auto v3 = version(QStringLiteral("v3"), QStringLiteral("a"), {}, 3);
  v3.extra.insert(QStringLiteral("editBaseline"), QStringLiteral("v1"));
  v3.extra.insert(QStringLiteral("rollbackTo"), QStringLiteral("v2"));
  QVERIFY(cat.addVersion(v3));
  const auto index = buildIndex(cat, {}, nullptr, nullptr);
  QVERIFY(index);
  QVERIFY(hasEdge(index->edges, QStringLiteral("version:v1"),
                  QStringLiteral("version:v3"), Relation::EditBaseline));
  QVERIFY(hasEdge(index->edges, QStringLiteral("version:v2"),
                  QStringLiteral("version:v3"), Relation::Rollback));
}

void TestDataGraph::assetRollbackIsReferenceNotDerived() {
  // 数据列表通用回滚（assetops）写 parentVersionIds={target} +
  // extra.rollbackOf：图谱按回滚参考边呈现，不冒充衍生。
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  QVERIFY(cat.addAsset(asset(QStringLiteral("a"), QStringLiteral("well_log"))));
  QVERIFY(cat.addVersion(version(QStringLiteral("v1"), QStringLiteral("a"))));
  QVERIFY(cat.addVersion(version(QStringLiteral("v2"), QStringLiteral("a"),
                                 {QStringLiteral("v1")}, 2)));
  auto rolled = version(QStringLiteral("v3"), QStringLiteral("a"),
                        {QStringLiteral("v1")}, 3);
  rolled.extra.insert(QStringLiteral("rollbackOf"), QStringLiteral("v1"));
  QVERIFY(cat.addVersion(rolled));
  const auto index = buildIndex(cat, {}, nullptr, nullptr);
  QVERIFY(index);
  QVERIFY(hasEdge(index->edges, QStringLiteral("version:v1"),
                  QStringLiteral("version:v3"), Relation::Rollback));
  QVERIFY(!hasEdge(index->edges, QStringLiteral("version:v1"),
                   QStringLiteral("version:v3"), Relation::Derived));
  // 血缘查询仍可达回滚链（Rollback 属血缘关系族）。
  Query q;
  q.mode = Mode::Lineage;
  q.focus = QStringLiteral("version:v3");
  const auto g = query(*index, q, nullptr, nullptr);
  bool sawV1 = false;
  for (const Node &n : g.nodes)
    sawV1 |= n.id == QStringLiteral("version:v1");
  QVERIFY(sawV1);
}

void TestDataGraph::evidenceTextTruncatedAt64K() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  QVERIFY(cat.addAsset(asset(QStringLiteral("a"), QStringLiteral("well_log"))));
  QVERIFY(cat.addVersion(version(QStringLiteral("v1"), QStringLiteral("a"))));
  auto v2 = version(QStringLiteral("v2"), QStringLiteral("a"),
                    {QStringLiteral("v1")}, 2);
  QVariantMap big = evidenceV1({QStringLiteral("v1")});
  big.insert(QStringLiteral("notes"), QString(200 * 1024, QLatin1Char('x')));
  v2.extra.insert(QStringLiteral("evidence"), big);
  QVERIFY(cat.addVersion(v2));
  const auto index = buildIndex(cat, {}, nullptr, nullptr);
  QVERIFY(index);
  Query q;
  q.mode = Mode::Relations;
  q.focus = QStringLiteral("version:v2");
  const auto g = query(*index, q, nullptr, nullptr);
  const Node *activity = nullptr;
  for (const Node &n : g.nodes)
    if (n.kind == Kind::Activity)
      activity = &n;
  QVERIFY(activity);
  QVERIFY(activity->evidenceText.size() > 64 * 1024);
  QVERIFY(activity->evidenceText.size() < 200 * 1024);
  QVERIFY(activity->evidenceText.contains(
      QStringLiteral("完整记录保存在该资料版本中")));
}

void TestDataGraph::cancelReturnsEmptyResults() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  QVERIFY(cat.addAsset(asset(QStringLiteral("a"), QStringLiteral("well_log"))));
  QVERIFY(cat.addVersion(version(QStringLiteral("v1"), QStringLiteral("a"))));
  const auto cancel = [] { return true; };
  QVERIFY(!buildIndex(cat, {}, cancel, nullptr));
  const auto index = buildIndex(cat, {}, nullptr, nullptr);
  QVERIFY(index);
  Query q;
  const auto g = query(*index, q, cancel, nullptr);
  QVERIFY(g.nodes.isEmpty());
  QVERIFY(g.edges.isEmpty());
}

void TestDataGraph::layoutIsDeterministicAndColumnar() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  QVERIFY(cat.addAsset(asset(QStringLiteral("src"), QStringLiteral("well_log"))));
  QVERIFY(cat.addVersion(version(QStringLiteral("r1"), QStringLiteral("src"))));
  QVERIFY(cat.addAsset(asset(QStringLiteral("a"), QStringLiteral("map_product"))));
  QVERIFY(cat.addVersion(version(QStringLiteral("va"), QStringLiteral("a"),
                                 {QStringLiteral("r1")}, 2)));
  const auto index = buildIndex(cat, {}, nullptr, nullptr);
  QVERIFY(index);
  Query q;
  q.mode = Mode::Lineage;
  q.focus = QStringLiteral("version:va");
  const auto a = query(*index, q, nullptr, nullptr);
  const auto b = query(*index, q, nullptr, nullptr);
  QCOMPARE(a.nodes.size(), b.nodes.size());
  for (int i = 0; i < a.nodes.size(); ++i) {
    QCOMPARE(a.nodes[i].id, b.nodes[i].id);
    QCOMPARE(a.nodes[i].position, b.nodes[i].position);
    // 列距固定 310：x 必须落在网格上（第 0 列第 0 行的 (0,0) 合法）。
    QCOMPARE(std::fmod(a.nodes[i].position.x(), 310.0), 0.0);
  }
  // 列号单调映射到 x 坐标：列大者在右侧。
  const Node *left = nullptr, *right = nullptr;
  for (const Node &n : a.nodes) {
    if (n.id == QStringLiteral("version:r1"))
      left = &n;
    if (n.id == QStringLiteral("version:va"))
      right = &n;
  }
  QVERIFY(left && right);
  QVERIFY(left->position.x() < right->position.x());
}

void TestDataGraph::searchMatchesTitleDetailAndStageText() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  QVERIFY(cat.addEntity(entity(QStringLiteral("w1"), QStringLiteral("well"),
                               QStringLiteral("Well-Alpha"))));
  QVERIFY(cat.addAsset(asset(QStringLiteral("a1"), QStringLiteral("well_log"))));
  QVERIFY(cat.addVersion(version(QStringLiteral("v1"), QStringLiteral("a1"))));
  QVERIFY(cat.addLink(link(QStringLiteral("w1"), QStringLiteral("a1"))));
  QVERIFY(cat.addAsset(asset(QStringLiteral("a2"), QStringLiteral("well_log"))));
  QVERIFY(cat.addVersion(version(QStringLiteral("v2"), QStringLiteral("a2"),
                                 {}, 1, QStringLiteral("DERIVED"))));
  const auto index = buildIndex(cat, {}, nullptr, nullptr);
  QVERIFY(index);
  // title/detail（含版本阶段文本）的命中语义；大小写不敏感。
  Query byTitle;
  byTitle.mode = Mode::Entities;
  byTitle.search = QStringLiteral("ALPHA");
  const auto t = query(*index, byTitle, nullptr, nullptr);
  QCOMPARE(countKind(t, Kind::Entity), 1);
  Query byStage;
  byStage.mode = Mode::Relations;
  byStage.focus = QStringLiteral("asset:a2");
  byStage.search = QStringLiteral("派生");
  const auto s = query(*index, byStage, nullptr, nullptr);
  QCOMPARE(countKind(s, Kind::Version), 1); // 版本 detail 的“派生资料”可检索
}

// ---- 工作流层：合并 / 缓存 / 失效 / 取消 / 导出 ----

void TestDataGraph::workflowRequestReadyAndCacheReuse() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  QVERIFY(cat.addAsset(asset(QStringLiteral("a"), QStringLiteral("well_log"))));
  QVERIFY(cat.addVersion(version(QStringLiteral("v1"), QStringLiteral("a"))));
  DataGraphWorkflow wf;
  QSignalSpy ready(&wf, &DataGraphWorkflow::ready);
  QSignalSpy status(&wf, &DataGraphWorkflow::statusChanged);
  wf.setContext(&cat, nullptr);
  const int base = ready.count(); // setContext 先发一张空图
  Query q;
  wf.request(q);
  waitNextReady(ready, base);
    const auto g = lastGraph(ready);
  QCOMPARE(g.totalAssets, 1);
  QCOMPARE(g.totalVersions, 1);
  // 同查询再发：命中 8 槽缓存，直接复用（状态含「复用」且不再起任务）。
  const int statusBefore = status.count();
  wf.request(q);
  QTRY_COMPARE(ready.count(), base + 2);
  bool reused = false;
  for (int i = statusBefore; i < status.count(); ++i)
    reused |= status.at(i).first().toString()
                  .contains(QStringLiteral("复用当前目录缓存"));
  QVERIFY(reused);
}

void TestDataGraph::workflowCacheIsLruNotFifo() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  QVERIFY(cat.addAsset(asset(QStringLiteral("a"), QStringLiteral("well_log"))));
  QVERIFY(cat.addVersion(version(QStringLiteral("v1"), QStringLiteral("a"))));
  DataGraphWorkflow wf;
  QSignalSpy ready(&wf, &DataGraphWorkflow::ready);
  QSignalSpy status(&wf, &DataGraphWorkflow::statusChanged);
  wf.setContext(&cat, nullptr);
  int seen = ready.count();
  Query base;
  Query first = base;
  first.maxNodes = 100;
  // 填满 8 槽缓存（maxNodes 互异 → 查询键互异）。
  for (int i = 0; i < 8; ++i) {
    Query q = base;
    q.maxNodes = 100 + i;
    wf.request(q);
    waitNextReady(ready, seen);
    ++seen;
  }
  // 命中 q(maxNodes=100) 把它变成最近使用；随后第 9 个查询挤掉的是
  // q(maxNodes=101)（LRU）而不是它（FIFO 会挤掉队头）。
  wf.request(first);
  waitNextReady(ready, seen);
  ++seen;
  Query ninth = base;
  ninth.maxNodes = 108;
  wf.request(ninth);
  waitNextReady(ready, seen);
  ++seen;
  const int statusBefore = status.count();
  wf.request(first);
  waitNextReady(ready, seen);
  bool reused = false;
  for (int i = statusBefore; i < status.count(); ++i)
    reused |= status.at(i).first().toString()
                  .contains(QStringLiteral("复用当前目录缓存"));
  QVERIFY2(reused, "缓存淘汰应为 LRU：最近命中的查询不得先于未命中者被逐出");
}

void TestDataGraph::workflowMutationInvalidatesCacheAndIndex() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  QVERIFY(cat.addAsset(asset(QStringLiteral("a"), QStringLiteral("well_log"))));
  QVERIFY(cat.addVersion(version(QStringLiteral("v1"), QStringLiteral("a"))));
  DataGraphWorkflow wf;
  QSignalSpy ready(&wf, &DataGraphWorkflow::ready);
  wf.setContext(&cat, nullptr);
  int seen = ready.count();
  Query q;
  wf.request(q);
  waitNextReady(ready, seen);
  QCOMPARE(lastGraph(ready).totalVersions, 1);
  ++seen;
  // 目录变更（mutationSeq 前进）：缓存与索引必须失效，图反映新版本。
  QVERIFY(cat.addVersion(version(QStringLiteral("v2"), QStringLiteral("a"),
                                 {QStringLiteral("v1")}, 2)));
  wf.request(q);
  waitNextReady(ready, seen);
  QCOMPARE(lastGraph(ready).totalVersions, 2);
}

void TestDataGraph::workflowPresentationInvalidatesIndex() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  QVERIFY(cat.addAsset(asset(QStringLiteral("a"), QStringLiteral("well_log"))));
  QVERIFY(cat.addVersion(version(QStringLiteral("v1"), QStringLiteral("a"))));
  QVERIFY(cat.addAsset(asset(QStringLiteral("b"), QStringLiteral("well_log"))));
  QVERIFY(cat.addVersion(version(QStringLiteral("v2"), QStringLiteral("b"))));
  DataGraphWorkflow wf;
  QSignalSpy ready(&wf, &DataGraphWorkflow::ready);
  wf.setContext(&cat, nullptr);
  int seen = ready.count();
  Query q;
  wf.request(q);
  waitNextReady(ready, seen);
  QCOMPARE(lastGraph(ready).totalAssets, 2);
  ++seen;
  Presentation p;
  p.removedIds.insert(QStringLiteral("b"));
  wf.setPresentation(p);
  wf.request(q);
  waitNextReady(ready, seen);
  QCOMPARE(lastGraph(ready).totalAssets, 1);
}

void TestDataGraph::workflowCancelSuppressesLaunch() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  QVERIFY(cat.addAsset(asset(QStringLiteral("a"), QStringLiteral("well_log"))));
  QVERIFY(cat.addVersion(version(QStringLiteral("v1"), QStringLiteral("a"))));
  DataGraphWorkflow wf;
  QSignalSpy ready(&wf, &DataGraphWorkflow::ready);
  wf.setContext(&cat, nullptr);
  const int base = ready.count();
  Query q;
  wf.request(q);
  wf.cancel(); // 同步取消：180ms 合并窗口内撤回，launch 不得再发图
  QTest::qWait(400);
  QCOMPARE(ready.count(), base);
}

void TestDataGraph::workflowContextSwitchResets() {
  QTemporaryDir dirA;
  QTemporaryDir dirB;
  DataCatalog a;
  QVERIFY(a.open(dirA.path(), nullptr));
  QVERIFY(a.addAsset(asset(QStringLiteral("a1"), QStringLiteral("well_log"))));
  QVERIFY(a.addVersion(version(QStringLiteral("v1"), QStringLiteral("a1"))));
  DataCatalog b;
  QVERIFY(b.open(dirB.path(), nullptr));
  QVERIFY(b.addAsset(asset(QStringLiteral("b1"), QStringLiteral("well_log"))));
  QVERIFY(b.addVersion(version(QStringLiteral("v2"), QStringLiteral("b1"))));
  DataGraphWorkflow wf;
  QSignalSpy ready(&wf, &DataGraphWorkflow::ready);
  wf.setContext(&a, nullptr);
  int seen = ready.count();
  Query q;
  q.category = QStringLiteral("well"); // 展开（非 summary）才能看到资产节点
  wf.request(q);
  waitNextReady(ready, seen);
  QCOMPARE(lastGraph(ready).totalAssets, 1);
  ++seen;
  // 换工区：清缓存/索引，先发空图；后续查询使用新目录。
  wf.setContext(&b, nullptr);
  waitNextReady(ready, seen);
  const auto blank = lastGraph(ready);
  QVERIFY(blank.nodes.isEmpty());
  ++seen;
  wf.request(q);
  waitNextReady(ready, seen);
  const auto g = lastGraph(ready);
  QCOMPARE(g.totalAssets, 1);
  bool hasB = false;
  for (const Node &n : g.nodes)
    hasB |= n.id == QStringLiteral("asset:b1");
  QVERIFY(hasB);
}

void TestDataGraph::workflowExportDescribesRenderedQuery() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  QVERIFY(cat.addAsset(asset(QStringLiteral("a"), QStringLiteral("well_log"))));
  QVERIFY(cat.addVersion(version(QStringLiteral("v1"), QStringLiteral("a"))));
  QVERIFY(cat.addAsset(asset(QStringLiteral("b"), QStringLiteral("well_log"))));
  QVERIFY(cat.addVersion(version(QStringLiteral("v2"), QStringLiteral("b"))));
  DataGraphWorkflow wf;
  QSignalSpy ready(&wf, &DataGraphWorkflow::ready);
  wf.setContext(&cat, nullptr);
  int seen = ready.count();
  Query first;
  first.mode = Mode::Relations;
  first.focus = QStringLiteral("asset:a");
  wf.request(first);
  waitNextReady(ready, seen);
    const auto graphA = lastGraph(ready);
  QVERIFY(!graphA.nodes.isEmpty());
  QVERIFY(graphA.query.focus == QStringLiteral("asset:a"));
  // 180ms 合并窗口内又请求了新查询：导出仍必须描述已渲染的图。
  Query second = first;
  second.focus = QStringLiteral("asset:b");
  wf.request(second);
  const QString path = dir.filePath(QStringLiteral("export.json"));
  QSignalSpy exported(&wf, &DataGraphWorkflow::exportFinished);
  wf.exportGraph(graphA, path);
  QTRY_COMPARE(exported.count(), 1);
  QCOMPARE(exported.first().first().toString(), path);
  QVERIFY(exported.first().at(1).toString().isEmpty());
  QFile file(path);
  QVERIFY(file.open(QIODevice::ReadOnly));
  const auto doc = QJsonDocument::fromJson(file.readAll());
  QVERIFY(!doc.isNull());
  QCOMPARE(doc.object().value(QStringLiteral("schema")).toString(),
           QStringLiteral("paleo_data_graph_v1"));
  QCOMPARE(doc.object().value(QStringLiteral("scope")).toString(),
           QStringLiteral("visible_subgraph"));
  const auto queryObj = doc.object().value(QStringLiteral("query")).toObject();
  QCOMPARE(queryObj.value(QStringLiteral("focus")).toString(),
           QStringLiteral("asset:a"));
  QCOMPARE(queryObj.value(QStringLiteral("mode")).toString(),
           QStringLiteral("relations"));
  QCOMPARE(doc.object().value(QStringLiteral("nodes")).toArray().size(),
           graphA.nodes.size());
  QTRY_COMPARE(ready.count(), seen + 2); // 第二个查询随后正常返回
}

void TestDataGraph::workflowExportReceiptSurvivesNewRequest() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  QVERIFY(cat.addAsset(asset(QStringLiteral("a"), QStringLiteral("well_log"))));
  QVERIFY(cat.addVersion(version(QStringLiteral("v1"), QStringLiteral("a"))));
  DataGraphWorkflow wf;
  QSignalSpy ready(&wf, &DataGraphWorkflow::ready);
  wf.setContext(&cat, nullptr);
  int seen = ready.count();
  Query q;
  q.mode = Mode::Relations;
  q.focus = QStringLiteral("asset:a");
  wf.request(q);
  waitNextReady(ready, seen);
    const auto g = lastGraph(ready);
  const QString path = dir.filePath(QStringLiteral("export2.json"));
  QSignalSpy exported(&wf, &DataGraphWorkflow::exportFinished);
  wf.exportGraph(g, path);
  // 导出进行期间来了新查询（代次前进）：导出回执不得被吞。
  Query next = q;
  next.focus = QStringLiteral("version:v1");
  wf.request(next);
  QTRY_COMPARE(exported.count(), 1);
  QVERIFY(exported.first().at(1).toString().isEmpty());
  QVERIFY(QFile::exists(path));
  waitNextReady(ready, seen);
}

void TestDataGraph::workflowExportDroppedAfterContextSwitch() {
  QTemporaryDir dirA;
  QTemporaryDir dirB;
  DataCatalog a;
  QVERIFY(a.open(dirA.path(), nullptr));
  QVERIFY(a.addAsset(asset(QStringLiteral("a1"), QStringLiteral("well_log"))));
  QVERIFY(a.addVersion(version(QStringLiteral("v1"), QStringLiteral("a1"))));
  DataCatalog b;
  QVERIFY(b.open(dirB.path(), nullptr));
  QVERIFY(b.addAsset(asset(QStringLiteral("b1"), QStringLiteral("well_log"))));
  QVERIFY(b.addVersion(version(QStringLiteral("v2"), QStringLiteral("b1"))));
  DataGraphWorkflow wf;
  QSignalSpy ready(&wf, &DataGraphWorkflow::ready);
  wf.setContext(&a, nullptr);
  int seen = ready.count();
  Query q;
  q.mode = Mode::Relations;
  q.focus = QStringLiteral("asset:a1");
  wf.request(q);
  waitNextReady(ready, seen);
    const auto g = lastGraph(ready);
  const QString path = dirA.filePath(QStringLiteral("export3.json"));
  QSignalSpy exported(&wf, &DataGraphWorkflow::exportFinished);
  wf.exportGraph(g, path);
  // 换工区：旧目录的导出回执不再投递（任务被取消/上下文已换）。
  wf.setContext(&b, nullptr);
  QTest::qWait(400);
  QCOMPARE(exported.count(), 0);
}

// ---- 规模层：合成血缘目录上的查询契约 ----

void TestDataGraph::scaleSyntheticLineageCatalog() {
  const int scale = qEnvironmentVariableIntValue("PALEO_DATAGRAPH_SCALE");
  const int n = scale > 0 ? scale : 4000;
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  QElapsedTimer populate;
  populate.start();
  QVERIFY(PerfFixtures::populateSyntheticDerivedCatalog(&cat, n, nullptr));
  const qint64 populateMs = populate.elapsed();

  QElapsedTimer indexing;
  indexing.start();
  const auto index = buildIndex(cat, {}, nullptr, nullptr);
  const qint64 indexMs = indexing.elapsed();
  QVERIFY(index);
  const int derived = n + (n + 9) / 10;
  QCOMPARE(index->versionCount, n + derived);
  QCOMPARE(index->entityCount, n);

  QElapsedTimer querying;
  querying.start();
  Query overview;
  overview.category = QStringLiteral("well");
  const auto head = query(*index, overview, nullptr, nullptr);
  const qint64 overviewMs = querying.restart();
  // 检索型概览：matches() 在全部候选资料上执行（预计算检索串的主场景）。
  Query overviewSearch;
  overviewSearch.category = QStringLiteral("well");
  overviewSearch.search = QStringLiteral("SYNTH_9");
  const auto searched = query(*index, overviewSearch, nullptr, nullptr);
  const qint64 searchMs = querying.restart();
  // 宽邻域 + 预算拉满：收边与重心布局的压力面。
  Query relationsWide;
  relationsWide.mode = Mode::Relations;
  relationsWide.focus = QStringLiteral("version:dver-1");
  relationsWide.depth = 6;
  relationsWide.maxNodes = 400;
  relationsWide.maxEdges = 1200;
  const auto wide = query(*index, relationsWide, nullptr, nullptr);
  const qint64 wideMs = querying.restart();
  Query lineage;
  lineage.mode = Mode::Lineage;
  lineage.focus = QStringLiteral("version:dver-1");
  const auto chain = query(*index, lineage, nullptr, nullptr);
  const qint64 lineageMs = querying.elapsed();
  QVERIFY(searched.nodes.size() <= overviewSearch.maxNodes);
  QVERIFY(wide.nodes.size() <= relationsWide.maxNodes);
  QVERIFY(wide.edges.size() <= relationsWide.maxEdges);
  // 预算得到尊重；隐藏如实计数：类别展开下每份资料占 2 槽（资产+当前
  // 版本），预留 32 槽给分组——默认 200 预算恰好容纳 84 份。
  const int visible = qMin(n, (overview.maxNodes - 32) / 2);
  QCOMPARE(countKind(head, Kind::Asset), visible);
  QCOMPARE(head.hidden, n - visible);
  QCOMPARE(head.nodes.size(), visible * 2 + 2); // + group:well(:DERIVED)
  // 血缘链包含：DERIVED v2 的活动投影与 RAW v1 输入。
  bool sawActivity = false, sawRaw = false;
  for (const Node &node : chain.nodes) {
    sawActivity |= node.kind == Kind::Activity;
    sawRaw |= node.id == QStringLiteral("version:ver-1");
  }
  QVERIFY(sawActivity);
  QVERIFY(sawRaw);
  qInfo("SCALE datagraph n=%d populate=%lldms index=%lldms "
        "overview=%lldms search=%lldms relationsWide=%lldms lineage=%lldms "
        "nodes=%d hidden=%d",
        n, populateMs, indexMs, overviewMs, searchMs, wideMs, lineageMs,
        head.nodes.size(), head.hidden);
}

QTEST_MAIN(TestDataGraph)
#include "tst_datagraph.moc"
