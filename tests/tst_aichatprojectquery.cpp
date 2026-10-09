// 层：测试壳
#include <QtTest>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <functional>

#include "../src/ai/chat/chatmessage.h"
#include "../src/ai/chat/domaintools.h"
#include "../src/catalog/datacatalog.h"
#include "../src/services/derivationgraph.h"
#include "../src/workflow/aichatcontroller.h"
#include "../src/workflow/aichattoolrunner.h"

// 方向77：AI 工程上下文注入——query_project / asset_lineage 两只读工具。
//
// 覆盖（Oracle 1/2/4/5 的单测面；端到端假端点在 tst_aichattoolloop）：
//   · 工具表 3→5：schema（enum 入参）+ 分发 Routed（无 ORT 依赖）。
//   · 假 catalog 下的实体枚举 / 角色过滤 / 列表截断 / 汇总计数。
//   · 只读红线：工具执行前后 catalog 快照逐字节一致（exportCatalogJson 对拍）。
//   · 血缘对拍：selectionClosure 出参 == 测试侧直算（同一 Service）。
//   · 版本引用：节点/条目带 version path（可追溯口径）。
//   · 诚实面：未绑 catalog → 如实 error，不冒充成功。
//   · 预算：projectBrief ≤300 字符；注入 systemPrompt 后仍常驻首位。
class TestAiChatProjectQuery : public QObject {
  Q_OBJECT
private slots:
  void toolTableGainsQueryAndLineageSpecs();
  void dispatchRoutesQueryToolsWithoutOrt();
  void queryWellsEnumeratesFromCatalog();
  void queryWellDetailsFiltersByRoleAndCitesVersions();
  void queryWellsTruncatesListAtCap();
  void querySummaryCountsFreshFromCatalog();
  void toolsAreReadOnlyCatalogSnapshotUnchanged();
  void lineageClosureMatchesDirectComputation();
  void unboundCatalogIsHonestError();
  void unknownWellIsHonestError();
  void projectBriefSummarizesAndStaysSmall();
  void controllerInjectsBriefIntoSystemPrompt();

private:
  struct Fixture {
    QTemporaryDir dir;
    DataCatalog catalog;
  };
  bool initFixture(Fixture &f);
  void seedProject(Fixture &f);
  static bool spinUntil(const std::function<bool()> &done, int timeoutMs = 10000);
  // 跑一只读工具并取回首帧结果（ok + 解析后的 payload）。
  bool runTool(AiChatToolRunner &runner, const QString &name,
               const QJsonObject &args, bool *ok, QJsonObject *payload);
  ChatToolCall makeCall(const QString &id, const QString &name,
                        const QJsonObject &args) const;
};

namespace {
constexpr int kWellsListCap = 50; // 出参列表上限（ledger 定案）
} // namespace

bool TestAiChatProjectQuery::initFixture(Fixture &f) {
  return f.dir.isValid() && f.catalog.open(f.dir.path());
}

// 假工程：两井（一井带 well_log + tops 两角色关联）、一层位资产、
// 三级派生链（RAW → DERIVED → DERIVED²）供血缘对拍。
void TestAiChatProjectQuery::seedProject(Fixture &f) {
  auto addWell = [&](const QString &id, const QString &name, double td) {
    CatalogEntity e;
    e.id = id;
    e.entityType = QStringLiteral("well");
    e.name = name;
    e.td = td;
    e.hasSurface = false;
    return f.catalog.addEntity(e);
  };
  QVERIFY(addWell(QStringLiteral("well-1"), QStringLiteral("W-1"), 2100.0));
  QVERIFY(addWell(QStringLiteral("well-2"), QStringLiteral("W-2"), 1850.0));
  CatalogEntity planned;
  planned.id = QStringLiteral("planned-1");
  planned.entityType = QStringLiteral("planned");
  planned.name = QStringLiteral("P-1");
  QVERIFY(f.catalog.addEntity(planned));
  // 层位资产挂地震测线实体（角色词表：horizon 属 seismic_survey）。
  CatalogEntity survey;
  survey.id = QStringLiteral("srv-1");
  survey.entityType = QStringLiteral("seismic_survey");
  survey.name = QStringLiteral("S1");
  QVERIFY(f.catalog.addEntity(survey));

  auto addAsset = [&](const QString &id, const QString &type,
                      const QString &name, const QString &format) {
    CatalogAsset a;
    a.id = id;
    a.type = type;
    a.displayName = name;
    a.format = format;
    return f.catalog.addAsset(a);
  };
  QVERIFY(addAsset(QStringLiteral("ast-log"), QStringLiteral("well_log"),
                   QStringLiteral("W-1_final.las"), QStringLiteral("las")));
  QVERIFY(addAsset(QStringLiteral("ast-tops"), QStringLiteral("tops"),
                   QStringLiteral("W-1_tops.txt"), QStringLiteral("txt")));
  QVERIFY(addAsset(QStringLiteral("ast-hor"), QStringLiteral("horizon"),
                   QStringLiteral("T2_horizon.grd"), QStringLiteral("grd")));
  QVERIFY(addAsset(QStringLiteral("ast-raw"), QStringLiteral("single_factor"),
                   QStringLiteral("factor_grain.grd"), QStringLiteral("grd")));
  QVERIFY(addAsset(QStringLiteral("ast-mid"), QStringLiteral("single_factor"),
                   QStringLiteral("factor_smooth.grd"), QStringLiteral("grd")));
  QVERIFY(addAsset(QStringLiteral("ast-top"), QStringLiteral("facies"),
                   QStringLiteral("facies_raster.tif"), QStringLiteral("tif")));

  auto addVersion = [&](const QString &id, const QString &assetId,
                        const QString &stage, int number, const QString &path,
                        const QStringList &parents = {}) {
    CatalogVersion v;
    v.id = id;
    v.assetId = assetId;
    v.stage = stage;
    v.versionNumber = number;
    v.path = path;
    v.fileName = path.section(QLatin1Char('/'), -1);
    v.parentVersionIds = parents;
    return f.catalog.addVersion(v);
  };
  QVERIFY(addVersion(QStringLiteral("ver-log-1"), QStringLiteral("ast-log"),
                     QStringLiteral("RAW"), 1,
                     QStringLiteral("RAW/ast-log/ver-log-1/W-1_final.las")));
  QVERIFY(addVersion(QStringLiteral("ver-log-2"), QStringLiteral("ast-log"),
                     QStringLiteral("RAW"), 2,
                     QStringLiteral("RAW/ast-log/ver-log-2/W-1_final.las")));
  QVERIFY(addVersion(QStringLiteral("ver-tops-1"), QStringLiteral("ast-tops"),
                     QStringLiteral("RAW"), 1,
                     QStringLiteral("RAW/ast-tops/ver-tops-1/W-1_tops.txt")));
  QVERIFY(addVersion(QStringLiteral("ver-hor-1"), QStringLiteral("ast-hor"),
                     QStringLiteral("RAW"), 1,
                     QStringLiteral("RAW/ast-hor/ver-hor-1/T2_horizon.grd")));
  QVERIFY(addVersion(QStringLiteral("ver-raw-1"), QStringLiteral("ast-raw"),
                     QStringLiteral("RAW"), 1,
                     QStringLiteral("RAW/ast-raw/ver-raw-1/factor_grain.grd")));
  QVERIFY(addVersion(QStringLiteral("ver-mid-1"), QStringLiteral("ast-mid"),
                     QStringLiteral("DERIVED"), 1,
                     QStringLiteral("DERIVED/ast-mid/ver-mid-1/factor_smooth.grd"),
                     {QStringLiteral("ver-raw-1")}));
  QVERIFY(addVersion(QStringLiteral("ver-top-1"), QStringLiteral("ast-top"),
                     QStringLiteral("DERIVED"), 1,
                     QStringLiteral("DERIVED/ast-top/ver-top-1/facies_raster.tif"),
                     {QStringLiteral("ver-mid-1")}));

  auto addLink = [&](const QString &entityId, const QString &assetId,
                     const QString &role, bool primary) {
    EntityAssetLink l;
    l.entityType = QStringLiteral("well");
    l.entityId = entityId;
    l.assetId = assetId;
    l.role = role;
    l.isPrimary = primary;
    return f.catalog.addLink(l);
  };
  QVERIFY(addLink(QStringLiteral("well-1"), QStringLiteral("ast-log"),
                  QStringLiteral("well_log"), true));
  QVERIFY(addLink(QStringLiteral("well-1"), QStringLiteral("ast-tops"),
                  QStringLiteral("tops"), true));
  QVERIFY(f.catalog.addLink([&] {
    EntityAssetLink l;
    l.entityType = QStringLiteral("seismic_survey");
    l.entityId = QStringLiteral("srv-1");
    l.assetId = QStringLiteral("ast-hor");
    l.role = QStringLiteral("horizon");
    l.isPrimary = true;
    return l;
  }()));
}

bool TestAiChatProjectQuery::spinUntil(const std::function<bool()> &done,
                                       int timeoutMs) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < timeoutMs)
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  return done();
}

ChatToolCall TestAiChatProjectQuery::makeCall(const QString &id,
                                              const QString &name,
                                              const QJsonObject &args) const {
  ChatToolCall call;
  call.id = id;
  call.name = name;
  call.argumentsJson = QString::fromUtf8(
    QJsonDocument(args).toJson(QJsonDocument::Compact));
  return call;
}

bool TestAiChatProjectQuery::runTool(AiChatToolRunner &runner,
                                     const QString &name,
                                     const QJsonObject &args, bool *ok,
                                     QJsonObject *payload) {
  QSignalSpy results(&runner, &AiChatToolRunner::toolFinished);
  runner.run(makeCall(QStringLiteral("call-q"), name, args));
  if (!spinUntil([&results] { return results.size() >= 1; }))
    return false;
  *ok = results.at(0).at(1).toBool();
  *payload = QJsonDocument::fromJson(results.at(0).at(2).toString().toUtf8())
               .object();
  return true;
}

// ---- 工具表（Oracle 1 的 schema 面）----

void TestAiChatProjectQuery::toolTableGainsQueryAndLineageSpecs() {
  const QVector<AiToolSpec> tools = builtinAiToolSpecs();
  QCOMPARE(tools.size(), 5);
  QStringList names;
  for (const AiToolSpec &spec : tools)
    names.append(spec.name);
  QVERIFY(names.contains(QStringLiteral("paleo.query_project")));
  QVERIFY(names.contains(QStringLiteral("paleo.asset_lineage")));

  const AiToolSpec query = aiToolSpec(QStringLiteral("paleo.query_project"));
  QVERIFY(!query.name.isEmpty());
  QVERIFY(!query.requiresLocalOrt);
  QVERIFY(!query.resultSchemaJson.isEmpty());
  QVERIFY(!QJsonDocument::fromJson(query.resultSchemaJson.toUtf8()).isNull());
  // topic 是 enum 入参：schema 出 enum，词表外取值被实参校验拒绝。
  const QJsonObject schema = query.toJsonSchema();
  const QJsonObject topic =
    schema.value(QStringLiteral("properties")).toObject().value(
      QStringLiteral("topic")).toObject();
  const QJsonArray topicEnum = topic.value(QStringLiteral("enum")).toArray();
  QVERIFY(topicEnum.contains(QLatin1String("wells")));
  QVERIFY(topicEnum.contains(QLatin1String("well_details")));
  QJsonObject badTopic;
  badTopic.insert(QStringLiteral("topic"), QStringLiteral("banana"));
  QVERIFY(!query.validateParameters(badTopic).isEmpty());

  const AiToolSpec lineage = aiToolSpec(QStringLiteral("paleo.asset_lineage"));
  QVERIFY(!lineage.name.isEmpty());
  QVERIFY(!lineage.requiresLocalOrt);
  QVERIFY(!lineage.resultSchemaJson.isEmpty());
}

void TestAiChatProjectQuery::dispatchRoutesQueryToolsWithoutOrt() {
  // 只读工具不依赖 ORT/远端配置：分发恒 Routed（运行期缺上下文由执行面如实报错）。
  QJsonObject args;
  args.insert(QStringLiteral("topic"), QStringLiteral("wells"));
  const AiToolDispatch query =
    dispatchAiTool(QStringLiteral("paleo.query_project"), args);
  QCOMPARE(query.status, AiToolDispatchStatus::Routed);

  QJsonObject lineageArgs;
  lineageArgs.insert(QStringLiteral("asset"), QStringLiteral("facies_raster.tif"));
  const AiToolDispatch lineage =
    dispatchAiTool(QStringLiteral("paleo.asset_lineage"), lineageArgs);
  QCOMPARE(lineage.status, AiToolDispatchStatus::Routed);
}

// ---- query_project 执行面 ----

void TestAiChatProjectQuery::queryWellsEnumeratesFromCatalog() {
  Fixture f;
  QVERIFY(initFixture(f));
  seedProject(f);
  AiChatToolRunner runner;
  AiChatToolContext context;
  context.catalog = &f.catalog;
  context.projectDir = f.dir.path();
  runner.setContext(context);

  QJsonObject args;
  args.insert(QStringLiteral("topic"), QStringLiteral("wells"));
  bool ok = false;
  QJsonObject payload;
  QVERIFY(runTool(runner, QStringLiteral("paleo.query_project"), args, &ok,
                  &payload));
  QVERIFY2(ok, qPrintable(payload.value(QStringLiteral("error")).toString()));
  QCOMPARE(payload.value(QStringLiteral("topic")).toString(),
           QStringLiteral("wells"));
  QCOMPARE(payload.value(QStringLiteral("count")).toInt(), 3); // 两口井 + 一计划井
  const QJsonArray wells = payload.value(QStringLiteral("wells")).toArray();
  QCOMPARE(wells.size(), 3);
  QStringList names;
  for (const QJsonValue &value : wells)
    names.append(value.toObject().value(QStringLiteral("name")).toString());
  QVERIFY(names.contains(QStringLiteral("W-1")));
  QVERIFY(names.contains(QStringLiteral("W-2")));
  QVERIFY(names.contains(QStringLiteral("P-1"))); // 计划井如实可枚举（type 区分）
  for (const QJsonValue &value : wells)
    QVERIFY(!value.toObject().value(QStringLiteral("id")).toString().isEmpty());
}

void TestAiChatProjectQuery::queryWellDetailsFiltersByRoleAndCitesVersions() {
  Fixture f;
  QVERIFY(initFixture(f));
  seedProject(f);
  AiChatToolRunner runner;
  AiChatToolContext context;
  context.catalog = &f.catalog;
  context.projectDir = f.dir.path();
  runner.setContext(context);

  QJsonObject args;
  args.insert(QStringLiteral("topic"), QStringLiteral("well_details"));
  args.insert(QStringLiteral("well_name"), QStringLiteral("W-1"));
  args.insert(QStringLiteral("role"), QStringLiteral("well_log"));
  bool ok = false;
  QJsonObject payload;
  QVERIFY(runTool(runner, QStringLiteral("paleo.query_project"), args, &ok,
                  &payload));
  QVERIFY2(ok, qPrintable(payload.value(QStringLiteral("error")).toString()));
  QCOMPARE(payload.value(QStringLiteral("well")).toString(),
           QStringLiteral("W-1"));
  const QJsonArray links = payload.value(QStringLiteral("links")).toArray();
  QCOMPARE(links.size(), 1); // 角色过滤后只剩曲线关联（tops 被滤掉）
  const QJsonObject link = links.at(0).toObject();
  QCOMPARE(link.value(QStringLiteral("role")).toString(),
           QStringLiteral("well_log"));
  QCOMPARE(link.value(QStringLiteral("asset")).toString(),
           QStringLiteral("W-1_final.las"));
  // 可追溯口径：引用带版本路径；主版本 = 最高 versionNumber（ver-log-2）。
  QCOMPARE(link.value(QStringLiteral("version_id")).toString(),
           QStringLiteral("ver-log-2"));
  QCOMPARE(link.value(QStringLiteral("stage")).toString(),
           QStringLiteral("RAW"));
  QVERIFY(link.value(QStringLiteral("path")).toString().endsWith(
    QStringLiteral("W-1_final.las")));
}

void TestAiChatProjectQuery::queryWellsTruncatesListAtCap() {
  Fixture f;
  QVERIFY(initFixture(f));
  for (int i = 0; i < kWellsListCap + 10; ++i) {
    CatalogEntity e;
    e.id = QStringLiteral("well-%1").arg(i);
    e.entityType = QStringLiteral("well");
    e.name = QStringLiteral("W-%1").arg(i);
    QVERIFY(f.catalog.addEntity(e));
  }
  AiChatToolRunner runner;
  AiChatToolContext context;
  context.catalog = &f.catalog;
  context.projectDir = f.dir.path();
  runner.setContext(context);

  QJsonObject args;
  args.insert(QStringLiteral("topic"), QStringLiteral("wells"));
  bool ok = false;
  QJsonObject payload;
  QVERIFY(runTool(runner, QStringLiteral("paleo.query_project"), args, &ok,
                  &payload));
  QVERIFY2(ok, qPrintable(payload.value(QStringLiteral("error")).toString()));
  QCOMPARE(payload.value(QStringLiteral("count")).toInt(),
           kWellsListCap + 10); // count 如实报全量
  QCOMPARE(payload.value(QStringLiteral("wells")).toArray().size(),
           kWellsListCap);      // 列表截到上限
  QVERIFY2(payload.contains(QStringLiteral("truncated")),
           "截断必须如实标记（字段带总数说明）");
}

void TestAiChatProjectQuery::querySummaryCountsFreshFromCatalog() {
  Fixture f;
  QVERIFY(initFixture(f));
  seedProject(f);
  AiChatToolRunner runner;
  AiChatToolContext context;
  context.catalog = &f.catalog;
  context.projectDir = f.dir.path();
  runner.setContext(context);

  QJsonObject args;
  args.insert(QStringLiteral("topic"), QStringLiteral("summary"));
  bool ok = false;
  QJsonObject payload;
  QVERIFY(runTool(runner, QStringLiteral("paleo.query_project"), args, &ok,
                  &payload));
  QVERIFY2(ok, qPrintable(payload.value(QStringLiteral("error")).toString()));
  const QJsonObject entities =
    payload.value(QStringLiteral("entities")).toObject();
  QCOMPARE(entities.value(QStringLiteral("well")).toInt(), 2);
  QCOMPARE(entities.value(QStringLiteral("planned")).toInt(), 1);
  const QJsonObject assets =
    payload.value(QStringLiteral("assets_by_type")).toObject();
  QCOMPARE(assets.value(QStringLiteral("well_log")).toInt(), 1);
  QCOMPARE(assets.value(QStringLiteral("horizon")).toInt(), 1);
  QVERIFY(payload.value(QStringLiteral("revision")).toInt() > 0);
}

// ---- 只读红线（Oracle 4）----

void TestAiChatProjectQuery::toolsAreReadOnlyCatalogSnapshotUnchanged() {
  Fixture f;
  QVERIFY(initFixture(f));
  seedProject(f);
  const QString before =
    f.dir.filePath(QStringLiteral("snapshot-before.json"));
  const QString after = f.dir.filePath(QStringLiteral("snapshot-after.json"));
  {
    QString error;
    QVERIFY2(f.catalog.exportCatalogJson(before, &error),
             qPrintable(error));
  }
  const quint64 seqBefore = f.catalog.mutationSeq();
  const int revisionBefore = f.catalog.catalogRevision();

  AiChatToolRunner runner;
  AiChatToolContext context;
  context.catalog = &f.catalog;
  context.projectDir = f.dir.path();
  runner.setContext(context);
  const QVector<QJsonObject> queries = {
    QJsonObject{{QStringLiteral("topic"), QStringLiteral("summary")}},
    QJsonObject{{QStringLiteral("topic"), QStringLiteral("wells")}},
    QJsonObject{{QStringLiteral("topic"), QStringLiteral("horizons")}},
    QJsonObject{{QStringLiteral("topic"), QStringLiteral("assets")}},
    QJsonObject{{QStringLiteral("topic"), QStringLiteral("well_details")},
                {QStringLiteral("well_name"), QStringLiteral("W-1")}},
    QJsonObject{{QStringLiteral("asset"), QStringLiteral("facies_raster.tif")}},
  };
  const QStringList names = {
    QStringLiteral("paleo.query_project"), QStringLiteral("paleo.query_project"),
    QStringLiteral("paleo.query_project"), QStringLiteral("paleo.query_project"),
    QStringLiteral("paleo.query_project"),
    QStringLiteral("paleo.asset_lineage"),
  };
  for (int i = 0; i < queries.size(); ++i) {
    bool ok = false;
    QJsonObject payload;
    QVERIFY(runTool(runner, names[i], queries[i], &ok, &payload));
    QVERIFY2(ok, qPrintable(payload.value(QStringLiteral("error")).toString()));
  }

  QCOMPARE(f.catalog.mutationSeq(), seqBefore);    // 零 mutator
  QCOMPARE(f.catalog.catalogRevision(), revisionBefore); // 零落盘
  {
    QString error;
    QVERIFY2(f.catalog.exportCatalogJson(after, &error), qPrintable(error));
  }
  QFile beforeFile(before), afterFile(after);
  QVERIFY(beforeFile.open(QIODevice::ReadOnly));
  QVERIFY(afterFile.open(QIODevice::ReadOnly));
  QCOMPARE(beforeFile.readAll(), afterFile.readAll()); // 快照逐字节一致
}

// ---- 血缘（Oracle 2）----

void TestAiChatProjectQuery::lineageClosureMatchesDirectComputation() {
  Fixture f;
  QVERIFY(initFixture(f));
  seedProject(f);
  AiChatToolRunner runner;
  AiChatToolContext context;
  context.catalog = &f.catalog;
  context.projectDir = f.dir.path();
  runner.setContext(context);

  QJsonObject args;
  args.insert(QStringLiteral("asset"), QStringLiteral("facies_raster.tif"));
  bool ok = false;
  QJsonObject payload;
  QVERIFY(runTool(runner, QStringLiteral("paleo.asset_lineage"), args, &ok,
                  &payload));
  QVERIFY2(ok, qPrintable(payload.value(QStringLiteral("error")).toString()));
  QVERIFY(payload.value(QStringLiteral("available")).toBool());
  QCOMPARE(payload.value(QStringLiteral("version_id")).toString(),
           QStringLiteral("ver-top-1"));

  // 对拍：测试侧用同一服务直算，闭包集必须一致。
  paleo::derivation::Query query;
  query.versionId = QStringLiteral("ver-top-1");
  query.upstreamDepth = 3;
  query.downstreamDepth = 1;
  const paleo::derivation::Graph graph =
    paleo::derivation::Service::build(&f.catalog, query);
  const QSet<QString> expected =
    paleo::derivation::Service::selectionClosure(&f.catalog, graph,
                                                 query.versionId);
  QStringList expectedIds = expected.values();
  expectedIds.sort();
  QStringList actualIds;
  const QJsonArray selection =
    payload.value(QStringLiteral("selection")).toObject().value(
      QStringLiteral("version_ids")).toArray();
  for (const QJsonValue &value : selection)
    actualIds.append(value.toString());
  actualIds.sort();
  QCOMPARE(actualIds, expectedIds);
  QVERIFY(actualIds.contains(QStringLiteral("ver-raw-1"))); // 上游闭包到源头
  QVERIFY(actualIds.contains(QStringLiteral("ver-top-1"))); // 含种子

  // 引用带版本路径（可追溯）。
  const QJsonArray nodes = payload.value(QStringLiteral("nodes")).toArray();
  QVERIFY(nodes.size() >= 3);
  for (const QJsonValue &value : nodes) {
    const QJsonObject node = value.toObject();
    QVERIFY2(!node.value(QStringLiteral("path")).toString().isEmpty(),
             "血缘节点必须带版本路径");
    QVERIFY(!node.value(QStringLiteral("stage")).toString().isEmpty());
  }
}

// ---- 诚实面 ----

void TestAiChatProjectQuery::unboundCatalogIsHonestError() {
  AiChatToolRunner runner; // 不 setContext：catalog 未绑
  QJsonObject args;
  args.insert(QStringLiteral("topic"), QStringLiteral("wells"));
  bool ok = false;
  QJsonObject payload;
  QVERIFY(runTool(runner, QStringLiteral("paleo.query_project"), args, &ok,
                  &payload));
  QVERIFY(!ok);
  QVERIFY2(payload.value(QStringLiteral("error")).toString().contains(
             QStringLiteral("工程")),
           qPrintable(payload.value(QStringLiteral("error")).toString()));

  QJsonObject lineageArgs;
  lineageArgs.insert(QStringLiteral("asset"), QStringLiteral("anything"));
  QVERIFY(runTool(runner, QStringLiteral("paleo.asset_lineage"), lineageArgs,
                  &ok, &payload));
  QVERIFY(!ok);
  QVERIFY2(payload.value(QStringLiteral("error")).toString().contains(
             QStringLiteral("工程")),
           qPrintable(payload.value(QStringLiteral("error")).toString()));
}

void TestAiChatProjectQuery::unknownWellIsHonestError() {
  Fixture f;
  QVERIFY(initFixture(f));
  seedProject(f);
  AiChatToolRunner runner;
  AiChatToolContext context;
  context.catalog = &f.catalog;
  context.projectDir = f.dir.path();
  runner.setContext(context);

  QJsonObject args;
  args.insert(QStringLiteral("topic"), QStringLiteral("well_details"));
  args.insert(QStringLiteral("well_name"), QStringLiteral("no-such-well"));
  bool ok = false;
  QJsonObject payload;
  QVERIFY(runTool(runner, QStringLiteral("paleo.query_project"), args, &ok,
                  &payload));
  QVERIFY(!ok);
  QVERIFY2(payload.value(QStringLiteral("error")).toString().contains(
             QStringLiteral("W-")),
           "报错应提示可查井清单（井名实证）");
}

// ---- 摘要与预算（Oracle 5 单测面）----

void TestAiChatProjectQuery::projectBriefSummarizesAndStaysSmall() {
  Fixture f;
  QVERIFY(initFixture(f));
  seedProject(f);
  const QString brief =
    AiChatToolRunner::projectBrief(&f.catalog, f.dir.path());
  QVERIFY2(brief.contains(QStringLiteral("3")), qPrintable(brief)); // 井 3（含计划井）
  QVERIFY(brief.contains(QStringLiteral("well_log")));
  QVERIFY(brief.size() <= 300); // 预算纪律：常驻段硬上限
  QVERIFY(!brief.contains(QLatin1Char('\n')) ||
           brief.count(QLatin1Char('\n')) <= 2); // ≤3 行

  const QString none = AiChatToolRunner::projectBrief(nullptr, QString());
  QVERIFY2(none.contains(QStringLiteral("未打开")), qPrintable(none));
}

void TestAiChatProjectQuery::controllerInjectsBriefIntoSystemPrompt() {
  AiChatController controller;
  const QString plain = controller.systemPrompt();
  QVERIFY(!plain.contains(QStringLiteral("当前工程概况")));
  controller.setProjectBrief(QStringLiteral(
    "当前工程概况：井 3 口（含计划井 1），资产 well_log 1、horizon 1。"));
  const QString withBrief = controller.systemPrompt();
  QVERIFY(withBrief.contains(QStringLiteral("当前工程概况")));
  QVERIFY(withBrief.contains(QStringLiteral("well_log")));
  QVERIFY(withBrief.size() > plain.size());
  // 摘要常驻不破预算口径：估算 token 增量必须远小于历史预算。
  QVERIFY(AiChatController::estimateTokens(withBrief) -
            AiChatController::estimateTokens(plain) <
          AiChatController::kHistoryTokenBudget / 10);
}

int main(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);
  TestAiChatProjectQuery tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_aichatprojectquery.moc"
