// 层：测试壳
// H3 v2 成组发布与耐久窗口：compute-only→重启→source publish、混合来源
// 拒绝、prepared 后破坏字节、取消窗口与迟到取消、预算上界。
#include <QtTest>
#include <qgsapplication.h>
#include "workflow/capabilityhost.h"
#include "workflow/capabilitynativeoperation.h"
#include "workflow/capabilityplan_outputs.h"
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
  QString rootPath; // 由外部 QTemporaryDir 持有，跨“重启”复用
  DataCatalog catalog;
  std::unique_ptr<ProjectDirLock> lock;
  PaleoTaskService tasks;
  CapabilityHost service;
  bool writable = true;
  quint64 epoch = 1;
  explicit Host(const QString &root) : rootPath(root), lock(std::make_unique<ProjectDirLock>(root)), service() {}
  bool init() {
    if (!lock->tryLock() || !catalog.open(rootPath))
      return false;
    service.setTaskService(&tasks);
    service.setCallerScope(host::Caller::Agent, "agent");
    service.setBindingProvider([this] {
      HostBinding b;
      b.projectDir = rootPath;
      b.operationDir = rootPath + "/.paleo/capability-ops";
      b.catalog = &catalog;
      b.writeClaim = writable ? lock->claim() : nullptr;
      b.context.documentId = "doc-h3";
      b.context.epoch = epoch;
      b.context.open = true;
      b.context.persistentIdentity = true;
      b.context.readOnly = !writable;
      b.context.ownsWrites = writable;
      b.context.allowedEffects = host::Read | host::PrivateStage | host::CatalogPublish | host::Presentation;
      b.freeze = [](ExecuteRequest &r) {
        r.bindingMode = "pinned-value";
        return QString();
      };
      b.validateInputs = [](const ExecuteRequest &) { return QString(); };
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

void forkPlan(QVector<PlanNode> *nodes, QVector<PlanEdge> *edges, PlanExports *exports) {
  *nodes = {{QStringLiteral("idw"), QStringLiteral("constraint.idw"), 1, QStringLiteral("OUTPUT"),
             QStringLiteral("compute-only"),
             {{QStringLiteral("FIELD"), QStringLiteral("z")}, {QStringLiteral("CELL_SIZE"), 25.0}},
             {{QStringLiteral("points"), idwPoints()}, {QStringLiteral("points_ref"), QStringLiteral("v-points")},
              {QStringLiteral("subinterval_id"), QStringLiteral("S1")}}},
            {QStringLiteral("contour_a"), QStringLiteral("singlefactor.contours"), 1, QStringLiteral("LINES"),
             QStringLiteral("compute-only"), {{QStringLiteral("CONTOUR_LEVEL"), 5.0}}, {}},
            {QStringLiteral("contour_b"), QStringLiteral("singlefactor.contours"), 1, QStringLiteral("LINES"),
             QStringLiteral("compute-only"), {{QStringLiteral("CONTOUR_LEVEL"), 2.5}}, {}}};
  *edges = {{QStringLiteral("idw"), QStringLiteral("OUTPUT"), QStringLiteral("contour_a"), QStringLiteral("RASTER")},
            {QStringLiteral("idw"), QStringLiteral("OUTPUT"), QStringLiteral("contour_b"), QStringLiteral("RASTER")}};
  *exports = {{{QStringLiteral("idw_raster"), QStringLiteral("idw"), QStringLiteral("OUTPUT")},
               {QStringLiteral("contours_a"), QStringLiteral("contour_a"), QStringLiteral("LINES")},
               {QStringLiteral("contours_b"), QStringLiteral("contour_b"), QStringLiteral("LINES")}}};
}
} // namespace

class H3PlanPublicationTests : public QObject {
  Q_OBJECT
private slots:
  void computeOnlyThenRestartedSourcePublishDoesNotRecompute();
  void mixedSourcePublicationRequestIsRejected();
  void tamperedPreparedOutputFailsTheWholeSet();
  void lateCancelAfterCommitKeepsReceiptAndArtifacts();
  void exportsRespectTheirOwnUpperBounds();
};

void H3PlanPublicationTests::computeOnlyThenRestartedSourcePublishDoesNotRecompute() {
  QTemporaryDir root{QDir::temp().filePath(QStringLiteral("h3-pub-XXXXXX"))};
  QString parentOperationId;
  QStringList childOperationIds;
  {
    Host h(root.path());
    QVERIFY(h.init());
    ExecuteRequest host;
    host.clientRequestId = QStringLiteral("h3-restart");
    host.publishIntent = QStringLiteral("compute-only");
    QVector<PlanNode> nodes;
    QVector<PlanEdge> edges;
    PlanExports exports;
    forkPlan(&nodes, &edges, &exports);
    const auto prepared = h.service.preparePlanV2(host, nodes, edges, exports, host::Caller::Agent);
    QVERIFY2(prepared.ok, qPrintable(prepared.code));
    ExecuteResult result;
    QString error;
    QVERIFY(h.run(prepared, host, this, QStringLiteral("H3 compute-only"), &result, &error));
    QVERIFY2(result.ok, qPrintable(result.code));
    QVERIFY(result.commitState != QStringLiteral("committed")); // compute-only：不发布
    QCOMPARE(h.catalog.versions().size(), 0); // compute-only：不进可见目录
    parentOperationId = result.operationId;
    for (const auto &child : result.summary.value("child_operations").toList())
      childOperationIds.append(child.toMap().value("operationId").toString());
    QCOMPARE(childOperationIds.size(), 3);
    // compute-only 的父 manifest 已包含全部三个选中端口。
    const auto results = h.service.recovery()->results("agent", parentOperationId);
    QCOMPARE(results.value("ports").toArray().size(), 3);
  }
  // 进程重建：全新 Host/Catalog 读同一工程磁盘状态。
  {
    Host h(root.path());
    QVERIFY(h.init());
    const auto state = h.service.recovery()->get("agent", parentOperationId);
    QCOMPARE(state.value("execution").toString(), QStringLiteral("succeeded"));
    ExecuteRequest publish;
    publish.capabilityId = QStringLiteral("workflow.plan");
    publish.clientRequestId = QStringLiteral("h3-restart-publish");
    publish.publishIntent = QStringLiteral("publish");
    publish.inputs = {{QStringLiteral("source_operation_id"), parentOperationId}};
    const auto prepared = h.service.prepare(publish, host::Caller::Agent);
    QVERIFY2(prepared.ok, qPrintable(prepared.code));
    ExecuteResult result;
    QString error;
    QVERIFY(h.run(prepared, publish, this, QStringLiteral("H3 source publish"), &result, &error));
    QVERIFY2(result.ok, qPrintable(result.code));
    QCOMPARE(result.commitState, QStringLiteral("committed"));
    // 三个成员一次整组 receipt；父 computeCount 不变（copySource 不重算）。
    QCOMPARE(h.catalog.versions().size(), 3);
    const auto after = h.service.recovery()->get("agent", parentOperationId);
    QCOMPARE(after.value("computeCount").toInt(), 1);
    // 子核未重算：三个子操作 computeCount 仍各为 1，且没有新的子操作 id。
    // 子步骤日志在父尝试私有目录（G4 隔离），跨重启仍在磁盘上。
    OperationLog mainLog(h.rootPath + "/.paleo/capability-ops");
    QVERIFY(mainLog.recordById(parentOperationId).capabilityId == QStringLiteral("workflow.plan"));
    const QDir parentPriv(h.rootPath + "/.paleo/capability-ops/" + parentOperationId);
    QVERIFY(parentPriv.exists());
    int childTotal = 0;
    for (const auto &attempt : parentPriv.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
      OperationLog log(parentPriv.filePath(attempt));
      for (const auto &id : childOperationIds) {
        const auto record = log.recordById(id);
        QVERIFY(!record.operationId.isEmpty());
        QCOMPARE(record.computeCount, 1);
        ++childTotal;
      }
    }
    QCOMPARE(childTotal, 3);
    const auto results = h.service.recovery()->results("agent", parentOperationId);
    QCOMPARE(results.value("ports").toArray().size(), 3);
    QCOMPARE(results.value("childOperations").toArray().size(), 3);
    // receipt 是一个整组 outputSet：每个成员版本 extra 携带同一 manifest。
    QSet<QString> outputSets;
    for (const auto &version : h.catalog.versions()) {
      const auto manifest = version.extra.value("outputSet").toMap();
      QCOMPARE(manifest.value("members").toList().size(), 3);
      outputSets.insert(manifest.value("id").toString());
    }
    QCOMPARE(outputSets.size(), 1);
  }
}

void H3PlanPublicationTests::mixedSourcePublicationRequestIsRejected() {
  QTemporaryDir root{QDir::temp().filePath(QStringLiteral("h3-mixed-XXXXXX"))};
  Host h(root.path());
  QVERIFY(h.init());
  ExecuteRequest host;
  host.clientRequestId = QStringLiteral("h3-mixed");
  host.publishIntent = QStringLiteral("compute-only");
  QVector<PlanNode> nodes;
  QVector<PlanEdge> edges;
  PlanExports exports;
  forkPlan(&nodes, &edges, &exports);
  const auto prepared = h.service.preparePlanV2(host, nodes, edges, exports, host::Caller::Agent);
  QVERIFY2(prepared.ok, qPrintable(prepared.code));
  ExecuteResult result;
  QString error;
  QVERIFY(h.run(prepared, host, this, QStringLiteral("H3 mixed base"), &result, &error));
  QVERIFY2(result.ok, qPrintable(result.code));
  // 混合来源：source publish 请求同时携带参数/额外输入 → 明确拒绝，
  // 已计算的成果与其身份不受影响。
  ExecuteRequest mixed;
  mixed.capabilityId = QStringLiteral("workflow.plan");
  mixed.clientRequestId = QStringLiteral("h3-mixed-publish");
  mixed.publishIntent = QStringLiteral("publish");
  mixed.parameters = {{QStringLiteral("FIELD"), QStringLiteral("z")}};
  mixed.inputs = {{QStringLiteral("source_operation_id"), result.operationId}};
  QCOMPARE(h.service.prepare(mixed, host::Caller::Agent).code, QStringLiteral("mixed_source_operation_inputs"));
  ExecuteRequest extraInput = mixed;
  extraInput.parameters.clear();
  extraInput.inputs.insert(QStringLiteral("invented"), 1);
  QCOMPARE(h.service.prepare(extraInput, host::Caller::Agent).code, QStringLiteral("mixed_source_operation_inputs"));
  QCOMPARE(h.catalog.versions().size(), 0);
}

void H3PlanPublicationTests::tamperedPreparedOutputFailsTheWholeSet() {
  QTemporaryDir root{QDir::temp().filePath(QStringLiteral("h3-tamper-XXXXXX"))};
  Host h(root.path());
  QVERIFY(h.init());
  ExecuteRequest host;
  host.clientRequestId = QStringLiteral("h3-tamper");
  host.publishIntent = QStringLiteral("publish");
  QVector<PlanNode> nodes;
  QVector<PlanEdge> edges;
  PlanExports exports;
  forkPlan(&nodes, &edges, &exports);
  const auto prepared = h.service.preparePlanV2(host, nodes, edges, exports, host::Caller::Agent);
  QVERIFY2(prepared.ok, qPrintable(prepared.code));
  // prepared 检查点：worker 已把输出集拷贝进受管 staging 并落 preparedManifest，
  // Catalog 提交尚未发生。此时破坏一个成员字节 → 整组拒绝，不留部分发布。
  // 检查点时序本身保证可见：store(prepared) 先于 checkpoint("prepared")。
  h.service.recovery()->setCheckpointHook([&](const QString &point) {
    if (point != QLatin1String("prepared"))
      return;
    OperationLog log(h.rootPath + "/.paleo/capability-ops");
    const auto found = log.lookup(QStringLiteral("doc-h3"), QStringLiteral("agent"), QStringLiteral("h3-tamper"));
    if (found.code != QLatin1String("found"))
      return;
    const auto rows = found.record.stagePlan.value("targets").toArray();
    if (rows.isEmpty())
      return;
    QFile target(h.rootPath + '/' + rows.at(0).toObject().value("staging").toObject().value("path").toString());
    if (target.open(QIODevice::Append)) {
      target.write("tampered");
      target.close();
    }
  });
  ExecuteResult result;
  QString error;
  bool started = h.service.start(prepared.handle, host, this, QStringLiteral("H3 tamper"),
                                 [&](ExecuteResult out) { result = out; }, &error);
  QVERIFY2(started, qPrintable(error));
  QTRY_VERIFY_WITH_TIMEOUT(!result.operationId.isEmpty(), 120000);
  QVERIFY(!result.ok);
  QVERIFY(result.commitState != QStringLiteral("committed"));
  QCOMPARE(h.catalog.versions().size(), 0); // 整组拒绝，无部分发布
}

void H3PlanPublicationTests::lateCancelAfterCommitKeepsReceiptAndArtifacts() {
  QTemporaryDir root{QDir::temp().filePath(QStringLiteral("h3-late-XXXXXX"))};
  Host h(root.path());
  QVERIFY(h.init());
  ExecuteRequest host;
  host.clientRequestId = QStringLiteral("h3-late");
  host.publishIntent = QStringLiteral("publish");
  QVector<PlanNode> nodes;
  QVector<PlanEdge> edges;
  PlanExports exports;
  forkPlan(&nodes, &edges, &exports);
  const auto prepared = h.service.preparePlanV2(host, nodes, edges, exports, host::Caller::Agent);
  QVERIFY2(prepared.ok, qPrintable(prepared.code));
  ExecuteResult result;
  QString error;
  QVERIFY(h.run(prepared, host, this, QStringLiteral("H3 late cancel"), &result, &error));
  QVERIFY2(result.ok, qPrintable(result.code));
  // 提交后的迟到取消不删除已提交成果，receipt 保留。
  const auto cancelled = h.service.recovery()->cancel("agent", result.operationId);
  QCOMPARE(cancelled.value("code").toString(), QStringLiteral("cancel_after_commit"));
  QCOMPARE(h.catalog.versions().size(), 3);
}

void H3PlanPublicationTests::exportsRespectTheirOwnUpperBounds() {
  QTemporaryDir root{QDir::temp().filePath(QStringLiteral("h3-budget-XXXXXX"))};
  Host h(root.path());
  QVERIFY(h.init());
  ExecuteRequest host;
  host.clientRequestId = QStringLiteral("h3-budget");
  host.publishIntent = QStringLiteral("publish");
  QVector<PlanNode> nodes;
  QVector<PlanEdge> edges;
  PlanExports exports;
  forkPlan(&nodes, &edges, &exports);
  // 超过 export 数量上界：在创建任何子操作/目录产物前拒绝。
  PlanExports tooMany = exports;
  for (int i = 0; i < 6; ++i)
    tooMany.entries.append({QStringLiteral("extra%1").arg(i), QStringLiteral("idw"), QStringLiteral("OUTPUT")});
  QCOMPARE(h.service.preparePlanV2(host, nodes, edges, tooMany, host::Caller::Agent).code,
           QStringLiteral("plan_too_large"));
  // 别名非法（路径分隔符）：同样整图前拒绝。
  PlanExports unsafe = exports;
  unsafe.entries[0].alias = QStringLiteral("idw/../raster");
  QCOMPARE(h.service.preparePlanV2(host, nodes, edges, unsafe, host::Caller::Agent).code,
           QStringLiteral("invalid_export_alias"));
  // 两种拒绝都发生在任何子操作/目录产物之前：journal 里没有父操作记录。
  QCOMPARE(h.catalog.versions().size(), 0);
  QCOMPARE(h.service.recovery()->lookup(QStringLiteral("agent"), QStringLiteral("h3-budget")).code,
           QStringLiteral("not_found"));
}

int main(int argc, char **argv) {
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH"), true);
  app.initQgis();
  H3PlanPublicationTests test;
  const auto code = QTest::qExec(&test, argc, argv);
  QgsApplication::exitQgis();
  return code;
}
#include "tst_h3_plan_publication.moc"
