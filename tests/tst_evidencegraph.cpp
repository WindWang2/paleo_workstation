#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include "../src/catalog/datacatalog.h"
#include "../src/domain/predictionevidence.h"
#include "../src/services/datagraph.h"
#include "../src/workflow/datagraphworkflow.h"

using namespace paleo::datagraph;
namespace {
QVariantMap feature(const QString &uid, int revision = 1) {
  return {{"project_id", "project-A"}, {"project_session", 1}, {"layer_id", "product.H1"}, {"horizon", "H1"},
    {"asset_id", "output"}, {"version_id", "result-v1"}, {"feature_uid", uid}, {"facies_code", 2},
    {"interpretation", QVariantMap{{"id", "interpretation-" + uid}, {"assertion_id", "assertion-" + uid}, {"status", "Disputed"}, {"revision", revision}}},
    {"records", QVariantList{QVariantMap{{"id", "support"}, {"role", "SUPPORTED_BY"}, {"type", "well"}, {"source_version", "input-v1"}, {"text", "井相支持"}},
       QVariantMap{{"id", "conflict"}, {"role", "CONTRADICTED_BY"}, {"type", "seismic"}, {"source_version", "missing-v1"}, {"text", "地震预测不同"}, {"source_missing", true}}}}};
}
bool populate(DataCatalog &cat, const QString &dir) {
  if (!cat.open(dir)) return false;
  CatalogAsset input; input.id = "input"; input.type = "well_log"; input.displayName = "well log";
  CatalogAsset output; output.id = "output"; output.type = "edited_facies"; output.displayName = "map";
  if (!cat.addAsset(input) || !cat.addAsset(output)) return false;
  CatalogVersion a; a.id = "input-v1"; a.assetId = input.id; a.stage = "RAW";
  if (!cat.addVersion(a)) return false;
  CatalogVersion b; b.id = "result-v1"; b.assetId = output.id; b.stage = "DERIVED"; b.parentVersionIds = {a.id};
  b.extra = {{"horizon", "H1"}, {"evidence", paleo::evidence::build({paleo::evidence::InputRef{a.id, a.assetId}}, {{"method", "compose"}}, {})}};
  return cat.addVersion(b);
}
int count(const Graph &g, Kind kind) { int n = 0; for (const auto &node : g.nodes) n += node.kind == kind; return n; }
bool hasFeature(const Graph &g, const QString &uid) { for (const auto &n : g.nodes) if (n.kind == Kind::Feature && n.featureUid == uid) return true; return false; }
}
class TestEvidenceGraph : public QObject {
  Q_OBJECT
private slots:
  void corruptSnapshotNeverInventsInterpretation() {
    QTemporaryDir dir; DataCatalog cat; QVERIFY(populate(cat, dir.path()));
    const auto index = buildIndex(cat, {}, {}, {}); QVERIFY(index);
    Query q; q.mode = Mode::Relations; q.focus = versionKey("result-v1"); q.depth = 4;
    q.featureEvidence = feature("broken"); q.featureEvidence.insert("error", "损坏证据");
    const auto g = query(*index, q, {}, {});
    QCOMPARE(count(g, Kind::Feature), 1); QCOMPARE(count(g, Kind::Interpretation), 0);
    QCOMPARE(count(g, Kind::Assertion), 0); QCOMPARE(count(g, Kind::Evidence), 0); QVERIFY(g.unresolved > 0);
  }
  void downstreamIsDirectedAndRetainsSemanticDependencies() {
    QTemporaryDir dir; DataCatalog cat; QVERIFY(populate(cat, dir.path()));
    CatalogAsset asset; asset.id = "reviewed-map"; asset.type = "edited_facies"; QVERIFY(cat.addAsset(asset));
    CatalogVersion version; version.id = "reviewed-v1"; version.assetId = asset.id; version.stage = "DERIVED"; version.parentVersionIds = {"input-v1"};
    version.extra = {{"horizon", "H1"}, {"interpretation_reference_versions", QVariantList{QVariantMap{{"version_id", "input-v1"}, {"role", "CONTRADICTED_BY"}}}}};
    QVERIFY(cat.addVersion(version));
    const auto index = buildIndex(cat, {}, {}, {}); QVERIFY(index);
    Query q; q.mode = Mode::Impact; q.focus = versionKey("input-v1"); q.depth = 4;
    const auto g = query(*index, q, {}, {});
    bool computed = false, semantic = false;
    for (const auto &node : g.nodes) {
      computed |= node.versionId == "result-v1";
      if (node.versionId == version.id) { semantic = true; QCOMPARE(node.dependencyDepth, 1); QVERIFY(node.detail.contains(QStringLiteral("未确定空间影响"))); }
    }
    QVERIFY(computed); QVERIFY(semantic); QVERIFY(g.message.contains(QStringLiteral("潜在影响")));
    q.maxNodes = 20; const auto ordinarySize = query(*index, q, {}, {}).nodes.size();
    q.featureEvidence = feature("selected", 3);
    QCOMPARE(query(*index, q, {}, {}).nodes.size(), ordinarySize); // Impact has no local overlay reservation
    q.focus = versionKey("result-v1");
    for (const auto &node : query(*index, q, {}, {}).nodes) QVERIFY(node.versionId != version.id); // no common-ancestor sibling promotion
  }
  void highDegreeQueryHasBoundedWork_data() {
    QTest::addColumn<int>("size"); QTest::newRow("10000") << 10000; QTest::newRow("100000") << 100000;
  }
  void highDegreeQueryHasBoundedWork() {
    QFETCH(int, size); Index index;
    Node seed; seed.id = versionKey("source"); seed.versionId = "source"; seed.kind = Kind::Version; index.nodes.insert(seed.id, seed);
    for (int n = 0; n < size; ++n) {
      Node node; node.id = versionKey(QString::number(n)); node.versionId = QString::number(n); node.kind = Kind::Version; node.horizon = "H1";
      index.nodes.insert(node.id, node); index.edges.append({seed.id, node.id, "计算来源", Relation::Derived});
      index.outgoing[seed.id].append(n); index.incoming[node.id].append(n);
    }
    Query q; q.mode = Mode::Impact; q.focus = seed.id; q.depth = 6; q.maxNodes = 20; q.maxEdges = 20; q.maxTraversalEdges = 512;
    QElapsedTimer time; time.start();
    for (int n = 0; n < 100; ++n) {
      const auto g = query(index, q, {}, {});
      QVERIFY(g.nodes.size() <= 20); QVERIFY(g.edges.size() <= 20); QVERIFY(g.workTruncated); QVERIFY(g.examinedEdges <= 513);
    }
    qInfo() << "downstream scale" << size << "100 bounded queries ms" << time.elapsed();
    int calls = 0; QVERIFY(query(index, q, [&calls] { return ++calls > 40; }, {}).nodes.isEmpty());
  }
  void exportedLocalGraphKeepsIdentityRelationAndQueryOwnership() {
    QTemporaryDir dir; DataCatalog cat; QVERIFY(populate(cat, dir.path()));
    DataGraphWorkflow workflow; workflow.setContext(&cat, nullptr);
    Graph delivered; bool ready = false; connect(&workflow, &DataGraphWorkflow::ready, this, [&](const Graph &g) { delivered = g; ready = true; });
    Query q; q.mode = Mode::Relations; q.focus = versionKey("result-v1"); q.depth = 4; q.featureEvidence = feature("A"); q.horizon = "H1";
    workflow.request(q); QTRY_VERIFY_WITH_TIMEOUT(ready, 10000);
    QSignalSpy exported(&workflow, &DataGraphWorkflow::exportFinished);
    const auto path = dir.filePath("local-evidence.json"); workflow.exportGraph(delivered, path);
    Query other = q; other.featureEvidence = feature("B"); workflow.request(other);
    QTRY_COMPARE_WITH_TIMEOUT(exported.count(), 1, 10000); QVERIFY(exported[0][1].toString().isEmpty());
    QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); const auto document = QJsonDocument::fromJson(file.readAll()).toVariant().toMap();
    QCOMPARE(document.value("query").toMap().value("horizon").toString(), QString("H1"));
    bool featureA = false, conflict = false;
    for (const auto &value : document.value("nodes").toList()) { const auto node = value.toMap(); if (node.value("kind") == "feature") { QCOMPARE(node.value("feature_uid").toString(), QString("A")); featureA = true; } }
    for (const auto &value : document.value("edges").toList()) conflict |= value.toMap().value("relation") == "contradicted_by";
    QVERIFY(featureA); QVERIFY(conflict);
  }
  void localSemanticsDoNotPolluteSharedIndex() {
    QTemporaryDir dir; DataCatalog cat; QVERIFY(populate(cat, dir.path()));
    auto index = buildIndex(cat, {}, {}, {}); QVERIFY(index);
    const auto baseCount = index->nodes.size();
    Query q; q.mode = Mode::Relations; q.focus = versionKey("result-v1"); q.depth = 4; q.history = true; q.featureEvidence = feature("polygon-A");
    const auto g = query(*index, q, {}, {});
    QCOMPARE(count(g, Kind::Feature), 1); QCOMPARE(count(g, Kind::Interpretation), 1); QCOMPARE(count(g, Kind::Assertion), 1); QCOMPARE(count(g, Kind::Evidence), 2);
    bool supports = false, contradicts = false, missing = false;
    for (const auto &e : g.edges) { supports |= e.relation == Relation::Supported; contradicts |= e.relation == Relation::Contradicted; }
    for (const auto &n : g.nodes) missing |= n.kind == Kind::Missing && n.versionId == "missing-v1";
    QVERIFY(supports); QVERIFY(contradicts); QVERIFY(missing);
    QCOMPARE(index->nodes.size(), baseCount); QCOMPARE(index->nodes.value(versionKey("result-v1")).kind, Kind::Version);
    q.featureEvidence.clear(); const auto ordinary = query(*index, q, {}, {});
    QCOMPARE(count(ordinary, Kind::Feature), 0); QCOMPARE(count(ordinary, Kind::Evidence), 0);
  }
  void boundedFilteredPagedAndCancelled() {
    QTemporaryDir dir; DataCatalog cat; QVERIFY(populate(cat, dir.path()));
    const auto index = buildIndex(cat, {}, {}, {}); QVERIFY(index);
    Query q; q.mode = Mode::Relations; q.focus = versionKey("result-v1"); q.depth = 4; q.maxNodes = 20; q.maxEdges = 20; q.featureEvidence = feature("polygon-A");
    QVariantList records;
    for (int n = 0; n < 1000; ++n) records << QVariantMap{{"id", QString::number(n)}, {"type", n % 2 ? "well" : "seismic"}, {"role", "REFERENCES"}};
    q.featureEvidence.insert("records", records); q.evidenceType = "well"; q.evidenceLimit = 4;
    const auto first = query(*index, q, {}, {});
    QVERIFY(first.nodes.size() <= 20); QVERIFY(first.edges.size() <= 20); QCOMPARE(count(first, Kind::Evidence), 4); QVERIFY(first.hidden > 0);
    QSet<QString> ids; for (const auto &n : first.nodes) if (n.kind == Kind::Evidence) { QCOMPARE(n.evidenceType, QString("well")); ids.insert(n.evidenceId); }
    q.evidenceOffset = 4; const auto second = query(*index, q, {}, {});
    QCOMPARE(count(second, Kind::Evidence), 4);
    for (const auto &n : second.nodes) if (n.kind == Kind::Evidence) QVERIFY(!ids.contains(n.evidenceId));
    q.horizon = "H2"; QCOMPARE(count(query(*index, q, {}, {}), Kind::Feature), 0);
    q.horizon.clear(); q.reviewState = "Confirmed"; QCOMPARE(count(query(*index, q, {}, {}), Kind::Evidence), 0);
    QVERIFY(query(*index, q, [] { return true; }, {}).nodes.isEmpty());
    QVERIFY(!buildIndex(cat, {}, [] { return true; }, {}));
    q.reviewState.clear(); q.depth = 2; QCOMPARE(count(query(*index, q, {}, {}), Kind::Evidence), 0);
  }
  void snapshotRevisionAndRapidRequestsInvalidateOnlyLocalCache() {
    QTemporaryDir dir; DataCatalog cat; QVERIFY(populate(cat, dir.path()));
    DataGraphWorkflow workflow; workflow.setContext(&cat, nullptr);
    QVector<Graph> delivered; connect(&workflow, &DataGraphWorkflow::ready, this, [&delivered](const Graph &g) { delivered << g; });
    Query a; a.mode = Mode::Relations; a.focus = versionKey("result-v1"); a.depth = 4; a.featureEvidence = feature("A");
    Query b = a; b.featureEvidence = feature("B"); QVERIFY(!(a == b));
    workflow.request(a); workflow.request(b);
    QTRY_VERIFY_WITH_TIMEOUT(!delivered.isEmpty(), 10000);
    QVERIFY(hasFeature(delivered.last(), "B")); QVERIFY(!hasFeature(delivered.last(), "A"));
    const auto seq = cat.mutationSeq();
    b.featureEvidence = feature("B", 2); b.featureEvidence.insert("records", QVariantList{QVariantMap{{"id", "new-review"}, {"role", "REFERENCES"}, {"type", "manual"}}});
    const auto before = delivered.size(); workflow.request(b);
    QTRY_VERIFY_WITH_TIMEOUT(delivered.size() > before, 10000); QCOMPARE(cat.mutationSeq(), seq);
    bool newReview = false; for (const auto &node : delivered.last().nodes) newReview |= node.evidenceId == "new-review";
    QVERIFY(newReview);
    workflow.request(a); workflow.cancel();
    // Replacing context cancels both timer and worker and supplies an honest empty graph.
    workflow.setContext(nullptr, nullptr); QVERIFY(delivered.last().nodes.isEmpty());
  }
};
QTEST_GUILESS_MAIN(TestEvidenceGraph)
#include "tst_evidencegraph.moc"
