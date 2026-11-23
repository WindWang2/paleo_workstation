// 层：测试壳
// H3 v2 计划多成果：exports 值类型/闭包校验/类型化执行与真实 Host 主链。
// 与 v1（tst_capability_plan / tst_capability_recovery_native）并存，不重复
// v1 已覆盖的整图预检口径。
#include <QtTest>
#include <qgsapplication.h>
#include "workflow/capabilityhost.h"
#include "workflow/capabilitynativeoperation.h"
#include "workflow/capabilityplan_outputs.h"
#include "workflow/capabilityplanexecution.h"
#include "catalog/datacatalog.h"
#include "metadata/projectlock.h"
#include "services/paleotaskservice.h"
#include <QDeadlineTimer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>

using namespace paleo;
using namespace paleo::capability;

namespace {
struct Host {
  QTemporaryDir root{QDir::temp().filePath(QStringLiteral("h3-outputs-XXXXXX"))};
  DataCatalog catalog;
  ProjectDirLock lock{root.path()};
  PaleoTaskService tasks;
  CapabilityHost service;
  bool providerOnline = true, writable = true;
  quint64 epoch = 1;
  bool init() {
    if (!lock.tryLock() || !catalog.open(root.path()))
      return false;
    service.setTaskService(&tasks);
    service.setCallerScope(host::Caller::Agent, "agent");
    service.setBindingProvider([this] {
      HostBinding b;
      b.projectDir = root.path();
      b.operationDir = root.path() + "/.paleo/capability-ops";
      b.catalog = &catalog;
      b.writeClaim = lock.claim();
      b.context.documentId = "doc-h3";
      b.context.epoch = epoch;
      b.context.open = true;
      b.context.persistentIdentity = true;
      b.context.readOnly = !writable;
      b.context.ownsWrites = writable;
      b.context.allowedEffects = host::Read | host::PrivateStage | host::CatalogPublish | host::Presentation;
      b.freeze = [this](ExecuteRequest &r) {
        if (!providerOnline)
          return QString("provider_offline");
        r.bindingMode = "pinned-value";
        return QString();
      };
      b.validateInputs = [this](const ExecuteRequest &) { return providerOnline ? QString() : QString("provider_offline"); };
      return b;
    });
    return true;
  }
  bool run(const HostPrepared &prepared, const ExecuteRequest &intent, QObject *subscriber, const QString &title,
           ExecuteResult *out, QString *error) {
    bool done = false;
    if (!service.start(prepared.handle, intent, subscriber, title,
                       [&](ExecuteResult result) { *out = result; done = true; }, error))
      return false;
    // bool 返回的辅助函数不能用 QTRY 宏（内含 return）；事件循环自旋等待。
    const QDeadlineTimer deadline(120000);
    while (!done && !deadline.hasExpired())
      QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    return done;
  }
};

QVariantList idwPoints() {
  QVariantList points;
  for (int i = 0; i < 4; ++i)
    points.append(QVariantMap{{"x", (i % 2) * 100.0}, {"y", (i / 2) * 100.0}, {"z", (i / 2) * 10.0},
                               {"well_id", QString::number(i)}});
  return points;
}

PlanNode idwNode(const QString &id = QStringLiteral("idw")) {
  return {id, QStringLiteral("constraint.idw"), 1, QStringLiteral("OUTPUT"), QStringLiteral("compute-only"),
          {{QStringLiteral("FIELD"), QStringLiteral("z")}, {QStringLiteral("CELL_SIZE"), 25.0}},
          {{QStringLiteral("points"), idwPoints()}, {QStringLiteral("points_ref"), QStringLiteral("v-points")},
           {QStringLiteral("subinterval_id"), QStringLiteral("S1")}}};
}

PlanNode contourNode(const QString &id, double level) {
  return {id, QStringLiteral("singlefactor.contours"), 1, QStringLiteral("LINES"), QStringLiteral("compute-only"),
          {{QStringLiteral("CONTOUR_LEVEL"), level}}, {}};
}

PlanExports forkExports() {
  // IDW 主栅格 + 两个不同等值线成果；别名是父端口稳定身份。
  return {{{QStringLiteral("idw_raster"), QStringLiteral("idw"), QStringLiteral("OUTPUT")},
           {QStringLiteral("contours_a"), QStringLiteral("contour_a"), QStringLiteral("LINES")},
           {QStringLiteral("contours_b"), QStringLiteral("contour_b"), QStringLiteral("LINES")}}};
}

QVector<PlanEdge> forkEdges() {
  return {{QStringLiteral("idw"), QStringLiteral("OUTPUT"), QStringLiteral("contour_a"), QStringLiteral("RASTER")},
          {QStringLiteral("idw"), QStringLiteral("OUTPUT"), QStringLiteral("contour_b"), QStringLiteral("RASTER")}};
}
} // namespace

class H3PlanOutputsTests : public QObject {
  Q_OBJECT
private slots:
  void exportsCodecRejectsUnknownVersionsAndFields();
  void v2ValidationAcceptsForkAndIntermediateExport();
  void v2ValidationClosureFailures_data();
  void v2ValidationClosureFailures();
  void v1SingleTerminalRuleStillRejectsTheSameFork();
  void firstBatchScopeRejectsNonFileAndNonPlanAdapters();
  void executorStopsBetweenNodesOnCancel();
  void hostPublishesThreeRealArtifactsAndReadsBackEveryPort();
  void hostIdempotencyDistinguishesExportChanges();
  void v2TicketsDieWithEpochChange();
};

void H3PlanOutputsTests::exportsCodecRejectsUnknownVersionsAndFields() {
  const auto base = QJsonObject{{"nodes", QJsonArray{}}, {"edges", QJsonArray{}}, {"version", 2},
                                {"exports", QJsonArray{QJsonObject{{"alias", "a"}, {"nodeId", "n"}, {"portId", "p"}}}}};
  QString code;
  PlanExports exports;
  QVERIFY(planExportsFromJson(base, exports, &code));
  QCOMPARE(exports.entries.size(), 1);
  auto noVersion = base;
  noVersion.remove("version");
  QVERIFY(!planExportsFromJson(noVersion, exports, &code));
  QCOMPARE(code, QStringLiteral("missing_plan_version"));
  auto future = base;
  future.insert("version", 3);
  QVERIFY(!planExportsFromJson(future, exports, &code));
  QCOMPARE(code, QStringLiteral("unsupported_plan_version"));
  auto extraField = base;
  extraField.insert("schemaHint", "x");
  QVERIFY(!planExportsFromJson(extraField, exports, &code));
  QCOMPARE(code, QStringLiteral("unknown_plan_field"));
  auto missingExports = base;
  missingExports.remove("exports");
  QVERIFY(!planExportsFromJson(missingExports, exports, &code));
  QCOMPARE(code, QStringLiteral("missing_export_field"));
  auto extraEntryField = base;
  auto rows = extraEntryField.value("exports").toArray();
  auto first = rows.at(0).toObject();
  first.insert("required", true);
  rows.replace(0, first);
  extraEntryField.insert("exports", rows);
  QVERIFY(!planExportsFromJson(extraEntryField, exports, &code));
  QCOMPARE(code, QStringLiteral("unknown_export_field"));
  auto unsafeAlias = base;
  unsafeAlias.insert("exports", QJsonArray{QJsonObject{{"alias", "a/b"}, {"nodeId", "n"}, {"portId", "p"}}});
  QVERIFY(!planExportsFromJson(unsafeAlias, exports, &code));
  QCOMPARE(code, QStringLiteral("invalid_export_alias"));
  auto empty = base;
  empty.insert("exports", QJsonArray{});
  QVERIFY(!planExportsFromJson(empty, exports, &code));
  QCOMPARE(code, QStringLiteral("invalid_export_count"));
  // 无版本且带 exports：拒绝，而不是静默按 v1 恢复。
  auto orphan = base;
  orphan.remove("version");
  orphan.insert("exports", base.value("exports"));
  QVERIFY(!planExportsFromJson(orphan, exports, &code));
}

void H3PlanOutputsTests::v2ValidationAcceptsForkAndIntermediateExport() {
  const QVector<PlanNode> nodes{idwNode(), contourNode("contour_a", 5.0), contourNode("contour_b", 2.5)};
  const auto validated = validatePlanV2(nodes, forkEdges(), forkExports());
  QVERIFY2(validated.ok, qPrintable(validated.issues.isEmpty() ? QString() : validated.issues.first().code));
  QCOMPARE(validated.exports.entries.size(), 3);
  QCOMPARE(validated.nodeIndex, QVector<int>({0, 1, 2}));
  QCOMPARE(validated.entryByAlias.value(QStringLiteral("contours_b")), 2);
  // 中间 raster 被下游消费的同时额外导出是合法形态。
  const auto spec = compositePlanSpec(validated);
  QVERIFY(spec.complete);
  QCOMPARE(spec.id, QStringLiteral("workflow.plan"));
  QCOMPARE(spec.implementationVersion, QStringLiteral("2"));
  QCOMPARE(spec.outputs.size(), 3);
  QCOMPARE(spec.outputs.at(0).id, QStringLiteral("idw_raster"));
  QCOMPARE(spec.outputs.at(0).kind, QStringLiteral("raster"));
  QCOMPARE(spec.outputs.at(1).id, QStringLiteral("contours_a"));
  QCOMPARE(spec.outputs.at(1).kind, QStringLiteral("vector"));
  QCOMPARE(spec.outputs.at(1).geometryHint, QStringLiteral("line"));
  QCOMPARE(spec.outputs.at(2).id, QStringLiteral("contours_b"));
  QVERIFY(spec.outputs.at(2).required);
  // Unicode/分隔样节点 id 不进入任何端口身份。
  const QVector<PlanNode> unicode{idwNode(QStringLiteral("井|,idw")), contourNode(QStringLiteral("等值|线A"), 5.0),
                                  contourNode(QStringLiteral("等值/线B"), 2.5)};
  PlanExports exports = forkExports();
  exports.entries[0].nodeId = unicode.at(0).id;
  exports.entries[1].nodeId = unicode.at(1).id;
  exports.entries[2].nodeId = unicode.at(2).id;
  QVector<PlanEdge> edges = forkEdges();
  edges[0].from = unicode.at(0).id;
  edges[1].from = unicode.at(0).id;
  edges[0].to = unicode.at(1).id;
  edges[1].to = unicode.at(2).id;
  const auto unicodeValidated = validatePlanV2(unicode, edges, exports);
  QVERIFY2(unicodeValidated.ok, qPrintable(unicodeValidated.issues.isEmpty() ? QString() : unicodeValidated.issues.first().code));
}

void H3PlanOutputsTests::v2ValidationClosureFailures_data() {
  QTest::addColumn<QString>("mutation");
  QTest::addColumn<QString>("expected");
  QTest::newRow("no-exports") << "no-exports" << "missing_exports";
  QTest::newRow("too-many") << "too-many" << "plan_too_large";
  QTest::newRow("unknown-node") << "unknown-node" << "unknown_node";
  QTest::newRow("unselected-port") << "unselected-port" << "unselected_output";
  QTest::newRow("duplicate-alias") << "duplicate-alias" << "duplicate_export_alias";
  QTest::newRow("duplicate-source") << "duplicate-source" << "duplicate_export_source";
  QTest::newRow("unsafe-alias") << "unsafe-alias" << "invalid_export_alias";
  QTest::newRow("missing-terminal") << "missing-terminal" << "missing_export";
  QTest::newRow("uncontributing") << "uncontributing" << "uncontributing_node";
}

void H3PlanOutputsTests::v2ValidationClosureFailures() {
  QFETCH(QString, mutation);
  QFETCH(QString, expected);
  QVector<PlanNode> nodes{idwNode(), contourNode("contour_a", 5.0), contourNode("contour_b", 2.5)};
  QVector<PlanEdge> edges = forkEdges();
  PlanExports exports = forkExports();
  if (mutation == QLatin1String("no-exports"))
    exports.entries.clear();
  if (mutation == QLatin1String("too-many")) {
    for (int i = 0; i < 9; ++i)
      exports.entries.append({QStringLiteral("extra%1").arg(i), QStringLiteral("idw"), QStringLiteral("OUTPUT")});
  }
  if (mutation == QLatin1String("unknown-node"))
    exports.entries[1].nodeId = QStringLiteral("absent");
  if (mutation == QLatin1String("unselected-port"))
    exports.entries[0].portId = QStringLiteral("CONSTRAINT_RECEIPTS");
  if (mutation == QLatin1String("duplicate-alias"))
    exports.entries[2].alias = exports.entries.at(1).alias;
  if (mutation == QLatin1String("duplicate-source")) {
    exports.entries[2].nodeId = exports.entries.at(1).nodeId;
    exports.entries[2].portId = exports.entries.at(1).portId;
  }
  if (mutation == QLatin1String("unsafe-alias"))
    exports.entries[0].alias = QStringLiteral("bad/alias");
  if (mutation == QLatin1String("missing-terminal"))
    exports.entries.removeAt(2); // contour_b 仍是终端但未被导出
  if (mutation == QLatin1String("uncontributing")) {
    // 一段与任何导出无关的两节点链：中间节点对导出无贡献
    //（uncontributing_node），链尾终端未导出（missing_export）；
    // 按节点序首个 issue 报中间节点。
    nodes.append(idwNode(QStringLiteral("extra")));
    nodes.append(contourNode(QStringLiteral("extra_end"), 7.0));
    edges.append({QStringLiteral("extra"), QStringLiteral("OUTPUT"), QStringLiteral("extra_end"),
                  QStringLiteral("RASTER")});
    exports.entries.removeAt(0); // idw 仍经两个 contour 导出有贡献
  }
  const auto validated = validatePlanV2(nodes, edges, exports);
  QVERIFY(!validated.ok);
  QCOMPARE(validated.issues.first().code, expected);
}

void H3PlanOutputsTests::v1SingleTerminalRuleStillRejectsTheSameFork() {
  const QVector<PlanNode> nodes{idwNode(), contourNode("contour_a", 5.0), contourNode("contour_b", 2.5)};
  // v1：多终端仍按原规则拒绝；v2 通过显式 exports 接受同一张图。
  QCOMPARE(checkPlan(nodes, forkEdges()).code, QStringLiteral("unsupported_plan_shape"));
  QVERIFY(validatePlanV2(nodes, forkEdges(), forkExports()).ok);
  // v2 不能靠省略 exports 偷回“最后一个节点”。
  QCOMPARE(checkPlan(nodes, forkEdges()).code, QStringLiteral("unsupported_plan_shape"));
}

void H3PlanOutputsTests::firstBatchScopeRejectsNonFileAndNonPlanAdapters() {
  QVector<PlanNode> nodes{idwNode(), contourNode("contour_a", 5.0)};
  QVector<PlanEdge> edges = forkEdges();
  edges.removeLast();
  PlanExports exports = forkExports();
  exports.entries.removeAt(2);
  // 可选 string 收据端口不能冒充文件工件（引用未选定端口即拒绝）。
  exports.entries[0].portId = QStringLiteral("CONSTRAINT_RECEIPTS");
  QCOMPARE(validatePlanV2(nodes, edges, exports).issues.first().code, QStringLiteral("unselected_output"));
  // seismic/depth 节点不在首批 plan adapter 范围内。
  auto seismic = idwNode();
  seismic.capabilityId = QStringLiteral("seismic.attributes.compute");
  seismic.outputPort = QStringLiteral("RMS");
  QCOMPARE(validatePlanV2({seismic}, {}, {{{QStringLiteral("seis"), QStringLiteral("seismic"), QStringLiteral("RMS")}}})
               .issues.first().code,
           QStringLiteral("unsupported_plan_adapter"));
  auto depth = idwNode();
  depth.capabilityId = QStringLiteral("depth.convert_batch");
  depth.outputPort = QStringLiteral("DEPTH");
  QCOMPARE(validatePlanV2({depth}, {}, {{{QStringLiteral("depth"), QStringLiteral("depth"), QStringLiteral("DEPTH")}}})
               .issues.first().code,
           QStringLiteral("unsupported_plan_adapter"));
}

void H3PlanOutputsTests::executorStopsBetweenNodesOnCancel() {
  Host h;
  QVERIFY(h.init());
  ExecuteRequest host;
  host.documentId = QStringLiteral("doc-h3");
  host.callerScope = QStringLiteral("agent");
  host.clientRequestId = QStringLiteral("h3-cancel");
  host.publishIntent = QStringLiteral("compute-only");
  host.operationDir = h.root.path() + "/.paleo/capability-ops";
  host.projectDir = h.root.path();
  host.bindingMode = QStringLiteral("pinned-value");
  // 节点间取消：执行器在每个节点启动前恰好查询一次取消；第二次查询（idw
  // 完成、contour_a 启动前）翻转为 true，后续节点不再启动。
  int cancelQueries = 0;
  const auto cancelled = [&cancelQueries] { return ++cancelQueries > 1; };
  const QVector<PlanNode> nodes{idwNode(), contourNode("contour_a", 5.0)};
  QVector<PlanEdge> edges = forkEdges();
  edges.removeLast();
  PlanExports exports = forkExports();
  exports.entries.removeAt(2);
  const auto collected = executePlanCollected(host, nodes, edges, exports, cancelled);
  QVERIFY(!collected.ok);
  QCOMPARE(collected.code, QStringLiteral("cancelled"));
  QCOMPARE(collected.failedNode, QStringLiteral("contour_a"));
  QCOMPARE(collected.produced.size(), 1); // 成功前缀有界保留，不当完整结果
  QCOMPARE(collected.produced.first().nodeId, QStringLiteral("idw"));
  QVERIFY(collected.childOperations.size() <= 2);
}

void H3PlanOutputsTests::hostPublishesThreeRealArtifactsAndReadsBackEveryPort() {
  Host h;
  QVERIFY(h.init());
  ExecuteRequest host;
  host.clientRequestId = QStringLiteral("h3-main");
  host.publishIntent = QStringLiteral("publish");
  const QVector<PlanNode> nodes{idwNode(QStringLiteral("idw,|节点")), contourNode(QStringLiteral("等值|线A"), 5.0),
                                contourNode(QStringLiteral("等值/线B"), 2.5)};
  QVector<PlanEdge> edges = forkEdges();
  edges[0].from = nodes.at(0).id;
  edges[1].from = nodes.at(0).id;
  edges[0].to = nodes.at(1).id;
  edges[1].to = nodes.at(2).id;
  PlanExports exports = forkExports();
  exports.entries[0].nodeId = nodes.at(0).id;
  exports.entries[1].nodeId = nodes.at(1).id;
  exports.entries[2].nodeId = nodes.at(2).id;
  const auto prepared = h.service.preparePlanV2(host, nodes, edges, exports, host::Caller::Agent);
  QVERIFY2(prepared.ok, qPrintable(prepared.code));
  ExecuteResult result;
  QString error;
  QVERIFY(h.run(prepared, host, this, QStringLiteral("H3 主链"), &result, &error));
  QVERIFY2(result.ok, qPrintable(result.code));
  QCOMPARE(result.commitState, QStringLiteral("committed"));
  // 三个真实成果（一个父 operation、一个整组 receipt）。
  QCOMPARE(h.catalog.versions().size(), 3);
  QCOMPARE(result.summary.value("child_operations").toList().size(), 3);
  const auto state = h.service.recovery()->get("agent", result.operationId);
  QCOMPARE(state.value("computeCount").toInt(), 1);
  // 三个子操作对应三个节点（分叉下 idw 只执行一次——逐分支重算会产生第 4 个）。
  // 子步骤按 G4 隔离是父尝试私有目录里的 executor 日志，不是主 journal
  // 中的可独立 get/resume v2 对象——这里显式验证两种身份的边界。
  OperationLog mainLog(h.root.path() + "/.paleo/capability-ops");
  QVERIFY(mainLog.recordById(result.operationId).capabilityId == QStringLiteral("workflow.plan"));
  const QDir parentPriv(h.root.path() + "/.paleo/capability-ops/" + result.operationId);
  QVERIFY(parentPriv.exists());
  QSet<QString> childIds;
  const auto children = result.summary.value("child_operations").toList();
  QCOMPARE(children.size(), 3);
  for (const auto &attempt : parentPriv.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    OperationLog log(parentPriv.filePath(attempt));
    for (const auto &child : children) {
      const auto row = child.toMap();
      const auto id = row.value("operationId").toString();
      QVERIFY(!row.value("nodeId").toString().isEmpty());
      QVERIFY(!row.value("requestHash").toString().isEmpty());
      const auto record = log.recordById(id);
      QVERIFY(!record.operationId.isEmpty());
      QVERIFY(record.capabilityId.isEmpty()); // legacy executor 步骤记录不带能力身份
      QVERIFY(mainLog.recordById(id).operationId.isEmpty()); // 主 journal 中不存在
      childIds.insert(id);
    }
  }
  QCOMPARE(childIds.size(), 3);
  // operation_results 列出精确父端口，kind 来自组合 spec。
  const auto results = h.service.recovery()->results("agent", result.operationId);
  QVERIFY(results.value("ok").toBool());
  QCOMPARE(results.value("ports").toArray().size(), 3);
  QHash<QString, QJsonObject> ports;
  for (const auto &value : results.value("ports").toArray())
    ports.insert(value.toObject().value("portId").toString(), value.toObject());
  QCOMPARE(ports.value("idw_raster").value("kind").toString(), QStringLiteral("raster"));
  QCOMPARE(ports.value("contours_a").value("kind").toString(), QStringLiteral("vector"));
  QCOMPARE(ports.value("contours_b").value("kind").toString(), QStringLiteral("vector"));
  QVERIFY(!ports.value("idw_raster").value("sha256").toString().isEmpty());
  // 逐端口认证读取：真实字节、哈希一致；三个端口都能读。
  for (const auto &alias : {QStringLiteral("idw_raster"), QStringLiteral("contours_a"), QStringLiteral("contours_b")}) {
    bool done = false;
    QJsonObject page;
    QString readError;
    QVERIFY(h.service.recovery()->read("agent", result.operationId, alias, 0, 64, this,
                                       [&](QJsonObject out) { page = out; done = true; }, &readError));
    QTRY_VERIFY_WITH_TIMEOUT(done, 30000);
    QVERIFY2(page.value("ok").toBool(), qPrintable(page.value("code").toString()));
    QCOMPARE(page.value("portId").toString(), alias);
    QVERIFY(page.value("data").toString().size() > 0);
  }
  // 数值/几何差异：不同等值线级别的成果内容不同（真实产物而非占位）。
  const auto carrierOf = [&] {
    for (const auto &version : h.catalog.versions())
      if (version.extra.contains("outputSet") &&
          version.extra.value("outputSet").toMap().value("members").toList().size() == 3)
        return version;
    return CatalogVersion{};
  };
  const auto carrier = carrierOf();
  QVERIFY(!carrier.id.isEmpty());
  QString shaA, shaB, rasterSha;
  for (const auto &member : carrier.extra.value("outputSet").toMap().value("members").toList()) {
    const auto row = member.toMap();
    if (row.value("portId").toString() == QStringLiteral("contours_a"))
      shaA = row.value("sha256").toString();
    if (row.value("portId").toString() == QStringLiteral("contours_b"))
      shaB = row.value("sha256").toString();
    if (row.value("portId").toString() == QStringLiteral("idw_raster"))
      rasterSha = row.value("sha256").toString();
  }
  QVERIFY(!rasterSha.isEmpty());
  QVERIFY(shaA != shaB);
  // 组内血缘：contour 成员的 catalog parentVersionIds 指向同一组的 raster
  // 版本（parentPortIds → commitSet 兄弟版本解析），raster 自身无外部父版本。
  QHash<QString, QString> versionByPort;
  QString rasterVersionId;
  for (const auto &member : carrier.extra.value("outputSet").toMap().value("members").toList()) {
    const auto row = member.toMap();
    versionByPort.insert(row.value("versionId").toString(), row.value("portId").toString());
    if (row.value("portId").toString() == QStringLiteral("idw_raster"))
      rasterVersionId = row.value("versionId").toString();
  }
  QVERIFY(!rasterVersionId.isEmpty());
  QCOMPARE(versionByPort.size(), 3);
  for (const auto &version : h.catalog.versions()) {
    const auto port = versionByPort.value(version.id);
    QVERIFY(!port.isEmpty()); // 每个版本对应组内一个成员端口
    const auto expected = port == QStringLiteral("idw_raster") ? QStringList{} : QStringList{rasterVersionId};
    QCOMPARE(version.parentVersionIds, expected);
  }
  // GPKG 真实文件特征 + 与 receipt 哈希一致。
  for (const auto &version : h.catalog.versions()) {
    QFile file(h.root.path() + '/' + version.path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto magic = file.read(16);
    if (version.fileName.endsWith(QLatin1String(".tif")))
      QVERIFY(magic.startsWith(QByteArray("II")) || magic.startsWith(QByteArray("MM")));
    if (version.fileName.endsWith(QLatin1String(".gpkg")))
      QVERIFY(magic.startsWith(QByteArray("SQLite format 3")));
  }
}

void H3PlanOutputsTests::hostIdempotencyDistinguishesExportChanges() {
  Host h;
  QVERIFY(h.init());
  ExecuteRequest host;
  host.clientRequestId = QStringLiteral("h3-idem");
  host.publishIntent = QStringLiteral("publish");
  const QVector<PlanNode> nodes{idwNode(), contourNode("contour_a", 5.0), contourNode("contour_b", 2.5)};
  PlanExports exports = forkExports();
  const auto prepared = h.service.preparePlanV2(host, nodes, forkEdges(), exports, host::Caller::Agent);
  QVERIFY2(prepared.ok, qPrintable(prepared.code));
  ExecuteResult result;
  QString error;
  QVERIFY(h.run(prepared, host, this, QStringLiteral("H3 幂等"), &result, &error));
  QVERIFY2(result.ok, qPrintable(result.code));
  // 同 key 改 exports（增删/别名/顺序）都判 idempotency_conflict，不建第二父操作。
  PlanExports changed = exports;
  changed.entries.removeAt(2);
  QCOMPARE(h.service.preparePlanV2(host, nodes, forkEdges(), changed, host::Caller::Agent).code,
           QStringLiteral("idempotency_conflict"));
  PlanExports renamed = exports;
  renamed.entries[1].alias = QStringLiteral("contours_renamed");
  QCOMPARE(h.service.preparePlanV2(host, nodes, forkEdges(), renamed, host::Caller::Agent).code,
           QStringLiteral("idempotency_conflict"));
  PlanExports reordered = exports;
  std::swap(reordered.entries[0], reordered.entries[2]);
  QCOMPARE(h.service.preparePlanV2(host, nodes, forkEdges(), reordered, host::Caller::Agent).code,
           QStringLiteral("idempotency_conflict"));
  // 等价重放返回原 operation。
  const auto replay = h.service.preparePlanV2(host, nodes, forkEdges(), exports, host::Caller::Agent);
  QVERIFY2(replay.ok, qPrintable(replay.code));
  QCOMPARE(replay.code, QStringLiteral("replay"));
  ExecuteResult replayed;
  QVERIFY(h.run(replay, host, this, QStringLiteral("H3 重放"), &replayed, &error));
  QVERIFY(replayed.ok);
  QCOMPARE(replayed.operationId, result.operationId);
  QCOMPARE(replayed.commitState, QStringLiteral("committed"));
  QCOMPARE(h.catalog.versions().size(), 3);
  QCOMPARE(h.service.recovery()->get("agent", result.operationId).value("computeCount").toInt(), 1);
  // v1 入口对同一 key 的不同意图同样冲突（跨版本不互认）。
  QCOMPARE(h.service.preparePlan(host, nodes, forkEdges(), host::Caller::Agent).code,
           QStringLiteral("idempotency_conflict"));
}

void H3PlanOutputsTests::v2TicketsDieWithEpochChange() {
  Host h;
  QVERIFY(h.init());
  ExecuteRequest host;
  host.clientRequestId = QStringLiteral("h3-ticket");
  host.publishIntent = QStringLiteral("publish");
  const QVector<PlanNode> nodes{idwNode(), contourNode("contour_a", 5.0), contourNode("contour_b", 2.5)};
  const auto exports = forkExports();
  const auto prepared = h.service.preparePlanV2(host, nodes, forkEdges(), exports, host::Caller::Agent);
  QVERIFY2(prepared.ok, qPrintable(prepared.code));
  // 票据只在取证上下文内有效：epoch 改变后不能跨上下文执行。
  ++h.epoch;
  QString error;
  QVERIFY(!h.service.start(prepared.handle, host, this, QStringLiteral("stale"), [](ExecuteResult) {}, &error));
  QCOMPARE(error, QStringLiteral("stale_session"));
}

int main(int argc, char **argv) {
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH"), true);
  app.initQgis();
  H3PlanOutputsTests test;
  const auto code = QTest::qExec(&test, argc, argv);
  QgsApplication::exitQgis();
  return code;
}
#include "tst_h3_plan_outputs.moc"
