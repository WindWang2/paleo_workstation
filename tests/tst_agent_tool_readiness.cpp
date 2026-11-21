// 层：测试壳
// G3/tst_agent_tool_readiness — 资格判断与工具枚举的可重入快照边界、
// 主题可发现性收敛与新鲜度闭环（W6/W7）。
#include <QtTest>
#include <QJsonDocument>
#include <QSemaphore>
#include <QTemporaryDir>
#include "../src/ai/chat/domaintools.h"
#include "../src/workflow/aichattoolrunner.h"
#include "../src/workflow/capabilityhost.h"
#include "../src/catalog/datacatalog.h"
#include "../src/services/paleotaskservice.h"
#if PALEO_HAVE_ORT
#include "../src/ai/onnxfixture.h"
#include "../src/ai/onnxpredictionservice.h"
#endif

using namespace paleo;

namespace {
capability::HostBinding readBinding() {
  capability::HostBinding binding;
  binding.context.open = true;
  binding.context.documentId = QStringLiteral("doc");
  binding.context.readOnly = true;
  binding.context.allowedEffects = host::Read | host::ExternalTransfer;
  return binding;
}
} // namespace

class TestAgentToolReadiness : public QObject {
  Q_OBJECT

private slots:
#if PALEO_HAVE_ORT
  // ---- W6：注入回调内同步重绑 → 本次判断按 context_rebound 失败 ----
  //（本用例要走到 hosted 层位建议的 trackingInput 检查，需真 ORT 小模型；
  // 无 ORT 构建跳过，快照机制另由 runtimeReadiness 枚举用例覆盖。）
  void rebindInsideInjectedCallbackFailsClosed() {
    AiChatToolRunner runner;
    AiAssistWorkflow workflow(nullptr);
    runner.setWorkflow(&workflow);
    QTemporaryDir projectA, projectB;
    QVERIFY(projectA.isValid() && projectB.isValid());
    DataCatalog catalogA, catalogB;
    QString error;
    QVERIFY(catalogA.open(projectA.path(), &error));
    QVERIFY(catalogB.open(projectB.path(), &error));
    // 层位建议在 hosted 模式要走到 trackingInput 检查，须 ORT 模型就绪
    // （口径同 tst_agent_readiness 的 modelProviderAndSemanticFailures）。
    PaleoOnnxService onnx;
    onnx.setModelRoot(projectA.path());
    QVERIFY(OnnxFixtureWriter::writeTraceScorer(projectA.filePath("model.onnx"), 1, 0));
    capability::HostBinding binding = readBinding();
    capability::CapabilityHost host;
    host.setBindingProvider([&] { return binding; });
    int providerCalls = 0;
    AiChatToolContext context;
    context.catalog = &catalogA;
    context.documentId = QStringLiteral("doc");
    context.onnx = &onnx;
    context.capabilityHost = &host;
    context.requireCapabilityHost = true;
    context.trackingInput = [&](const auto &, auto *, QString *) {
      ++providerCalls;
      // 同步重绑：替换 catalog 绑定（setContext 判定变化 → 世代前进）。
      AiChatToolContext rebound = context;
      rebound.catalog = &catalogB;
      runner.setContext(rebound);
      return false;
    };
    runner.setContext(context);
    const auto arguments = QJsonObject{{"model", QStringLiteral("model")}};
    const auto ready = runner.readiness(QStringLiteral("paleo.horizon_tracking_suggestion"), arguments);
    QVERIFY(!ready.ready);
    QCOMPARE(ready.code, QStringLiteral("context_rebound"));
    QCOMPARE(providerCalls, 1);
    // 旧绑定的 ready 没有投递；下一次判断按新快照给出提供者语义结论。
    AiChatToolContext next = context;
    next.catalog = &catalogB;
    next.trackingInput = [](const auto &, auto *, QString *) { return false; };
    runner.setContext(next);
    QCOMPARE(runner.readiness(QStringLiteral("paleo.horizon_tracking_suggestion"), arguments).code,
             QStringLiteral("tracking_semantics_unavailable"));
  }

#endif

  // ---- W6：一次枚举绑定同一快照；回调重绑后下一次枚举才用新上下文 ----
  void enumerationBindsOneSnapshotAndRecoversNextCall() {
    AiChatToolRunner runner;
    QTemporaryDir projectA, projectB;
    QVERIFY(projectA.isValid() && projectB.isValid());
    DataCatalog catalogA, catalogB;
    QString error;
    QVERIFY(catalogA.open(projectA.path(), &error));
    QVERIFY(catalogB.open(projectB.path(), &error));
    capability::HostBinding binding = readBinding();
    capability::CapabilityHost host;
    host.setBindingProvider([&] { return binding; });
    PaleoTaskService tasks;
    runner.setTaskService(&tasks);
    int callbackCalls = 0;
    AiChatToolContext context;
    context.catalog = &catalogA;
    context.documentId = QStringLiteral("doc");
    context.capabilityHost = &host;
    context.requireCapabilityHost = true;
    context.runtimeReadiness = [&](const QString &, const QJsonObject &, bool) {
      if (++callbackCalls == 1) {
        // 枚举途中同步重绑到 catalogB。
        AiChatToolContext rebound = context;
        rebound.catalog = &catalogB;
        runner.setContext(rebound);
      }
      return QString();
    };
    runner.setContext(context);
    const auto tools = runner.availableTools();
    QStringList names;
    for (const auto &spec : tools)
      names.append(spec.name);
    // 快照判定：不返回一半旧一半新——依赖 runtimeReadiness 的工具（重绑
    // 后复核失败）本次不出现；read_result 等不触回调的照常。
    QVERIFY(names.contains(QStringLiteral("paleo.read_result")));
    QVERIFY(!names.contains(QStringLiteral("paleo.query_project")));
    // 下一次枚举自然使用新快照（catalogB）——有界策略，无重试堆积。
    callbackCalls = 100; // 之后不再重绑
    const auto again = runner.availableTools();
    QStringList namesAgain;
    for (const auto &spec : again)
      namesAgain.append(spec.name);
    QVERIFY(namesAgain.contains(QStringLiteral("paleo.query_project")));
    QVERIFY(tasks.tasks().isEmpty());
  }

  // ---- W6：provider 接入/撤除推进世代；排队调用不再按旧绑定执行 ----
  void providerRemovalAdvancesGenerationAndStalesQueuedCalls() {
    QTemporaryDir projectA, projectB;
    QVERIFY(projectA.isValid() && projectB.isValid());
    PaleoTaskService tasks;
    tasks.setMaxWorkerThreads(1);
    QSemaphore gate;
    tasks.start(QStringLiteral("blocker"), [&](PaleoTask *) { gate.acquire(); return QString(); });
    AiChatToolRunner runner;
    runner.setTaskService(&tasks);
    AiAssistWorkflow workflow(nullptr);
    runner.setWorkflow(&workflow);
    AiChatToolContext context;
    context.projectDir = projectA.path();
    context.documentId = QStringLiteral("doc-a");
    context.trackingInput = [](const auto &, auto *, QString *) { return false; };
    runner.setContext(context);
    QSignalSpy done(&runner, &AiChatToolRunner::toolFinished);
    // 活动调用占住串行执行器；层位建议排队。
    runner.run({"active", "paleo.constraint_idw",
                QString::fromUtf8(QJsonDocument(QJsonObject{
                    {"client_request_id", QStringLiteral("g-1")},
                    {"binding_mode", QStringLiteral("pinned-value")},
                    {"publish_intent", QStringLiteral("prepare")},
                    {"parameters", QJsonObject{{"FIELD", QStringLiteral("z")}, {"CELL_SIZE", 10.0}}},
                    {"inputs", QJsonObject{{"points_ref", QStringLiteral("v")}, {"subinterval_id", QStringLiteral("s")}}}})
                                  .toJson(QJsonDocument::Compact))});
    runner.run({"queued", "paleo.horizon_tracking_suggestion",
                R"({"model":"m","window_samples":4,"radius":1,"seeds":[{"inline":1,"xline":1,"sample_index":1}]})"});
    // 撤除 trackingInput（其余不变）：世代前进，排队调用按 stale_session 失败，
    // 不再按旧绑定放行执行（readiness 的 hosted 门见上一用例与
    // tst_agent_readiness 既有覆盖）。
    AiChatToolContext removed = context;
    removed.trackingInput = nullptr;
    runner.setContext(removed);
    gate.release();
    QTRY_COMPARE(done.size(), 2);
    const auto queued = QJsonDocument::fromJson(done.last()[2].toString().toUtf8()).object();
    QCOMPARE(queued.value("code").toString(), QStringLiteral("stale_session"));
  }

  // ---- W7：主题可发现性在 spec 副本中收敛，不改全局注册表 ----
  void topicAvailabilityNarrowsInSpecCopyOnly() {
    QTemporaryDir project;
    QVERIFY(project.isValid());
    DataCatalog catalog;
    QString error;
    QVERIFY(catalog.open(project.path(), &error));
    capability::HostBinding binding = readBinding();
    capability::CapabilityHost host;
    host.setBindingProvider([&] { return binding; });
    host.setCallerScope(host::Caller::Agent, QStringLiteral("principal"));
    AiChatToolRunner runner;
    PaleoTaskService tasks;
    runner.setTaskService(&tasks);
    AiChatToolContext context;
    context.catalog = &catalog;
    context.documentId = QStringLiteral("doc");
    context.capabilityHost = &host;
    context.requireCapabilityHost = true;
    runner.setContext(context);

    const auto topicEnum = [](const QVector<AiToolSpec> &tools) {
      QStringList topics;
      for (const auto &spec : tools)
        if (spec.name == QLatin1String("paleo.query_project"))
          for (const auto &parameter : spec.parameters)
            if (parameter.name == QLatin1String("topic"))
              topics = parameter.enumValues;
      return topics;
    };

    // 无端点配置：well_facies_models（要端点）与 local_models（要 onnx）
    // 从副本 enum 收窄；目录查询主题保留——单个不可用主题不抹掉整只工具。
    const auto narrowed = topicEnum(runner.availableTools());
    QVERIFY(!narrowed.isEmpty());
    QVERIFY(narrowed.contains(QStringLiteral("wells")));
    QVERIFY(narrowed.contains(QStringLiteral("summary")));
    QVERIFY(!narrowed.contains(QStringLiteral("well_facies_models")));
    QVERIFY(!narrowed.contains(QStringLiteral("local_models")));
    // 全局注册表未动：完整 enum 仍在。
    const auto global = aiToolSpec(QStringLiteral("paleo.query_project"));
    QStringList globalTopics;
    for (const auto &parameter : global.parameters)
      if (parameter.name == QLatin1String("topic"))
        globalTopics = parameter.enumValues;
    QVERIFY(globalTopics.contains(QStringLiteral("well_facies_models")));
    QVERIFY(globalTopics.contains(QStringLiteral("local_models")));
    // 资格枚举零业务副作用：无任务、无 operation 目录、无 catalog 写。
    QCOMPARE(tasks.tasks().size(), 0);
    QVERIFY(!QFileInfo::exists(project.filePath(QStringLiteral(".paleo/capability-ops"))));

    // 配好回环端点后：well_facies_models 恢复可发现。
    WellFaciesConfig config;
    config.baseUrl = QUrl(QStringLiteral("http://127.0.0.1:1"));
    config.apiKey = QByteArrayLiteral("local-test");
    config.allowInsecureHttp = true;
    runner.setFaciesConfig(config);
    const auto restored = topicEnum(runner.availableTools());
    QVERIFY(restored.contains(QStringLiteral("well_facies_models")));
    QVERIFY(restored.contains(QStringLiteral("wells")));
    // 无 TaskService 时 well_facies_inputs 也收窄。
    AiChatToolRunner bare;
    AiChatToolContext bareContext;
    bareContext.catalog = &catalog;
    bareContext.documentId = QStringLiteral("doc");
    bareContext.capabilityHost = &host;
    bareContext.requireCapabilityHost = true;
    bare.setContext(bareContext);
    const auto withoutTasks = topicEnum(bare.availableTools());
    QVERIFY(!withoutTasks.contains(QStringLiteral("well_facies_inputs")));
    QVERIFY(withoutTasks.contains(QStringLiteral("wells")));
  }

  // ---- W7：清单生成后换工程，旧参数不能在新工程产生执行 ----
  void staleArgumentsCannotExecuteInNewProject() {
    QTemporaryDir projectA, projectB;
    QVERIFY(projectA.isValid() && projectB.isValid());
    DataCatalog catalogA, catalogB;
    QString error;
    QVERIFY(catalogA.open(projectA.path(), &error));
    QVERIFY(catalogB.open(projectB.path(), &error));
    for (int i = 0; i < 3; ++i) {
      CatalogEntity entity;
      entity.id = QStringLiteral("a-%1").arg(i);
      entity.entityType = QStringLiteral("well");
      entity.name = QStringLiteral("A-%1").arg(i);
      QVERIFY(catalogA.addEntity(entity));
    }
    PaleoTaskService tasks;
    tasks.setMaxWorkerThreads(1);
    QSemaphore gate;
    tasks.start(QStringLiteral("blocker"), [&](PaleoTask *) { gate.acquire(); return QString(); });
    AiChatToolRunner runner;
    runner.setTaskService(&tasks);
    AiChatToolContext context;
    context.projectDir = projectA.path();
    context.documentId = QStringLiteral("doc-a");
    context.catalog = &catalogA;
    runner.setContext(context);
    QSignalSpy done(&runner, &AiChatToolRunner::toolFinished);
    runner.run({"active", "paleo.constraint_idw",
                QString::fromUtf8(QJsonDocument(QJsonObject{
                    {"client_request_id", QStringLiteral("ab-1")},
                    {"binding_mode", QStringLiteral("pinned-value")},
                    {"publish_intent", QStringLiteral("prepare")},
                    {"parameters", QJsonObject{{"FIELD", QStringLiteral("z")}, {"CELL_SIZE", 10.0}}},
                    {"inputs", QJsonObject{{"points_ref", QStringLiteral("v")}, {"subinterval_id", QStringLiteral("s")}}}})
                                  .toJson(QJsonDocument::Compact))});
    runner.run({"queued", "paleo.query_project", R"({"topic":"wells"})"});
    const quint64 mutationB = catalogB.mutationSeq();
    AiChatToolContext swapped = context;
    swapped.projectDir = projectB.path();
    swapped.documentId = QStringLiteral("doc-b");
    swapped.catalog = &catalogB;
    runner.setContext(swapped);
    gate.release();
    QTRY_COMPARE(done.size(), 2);
    const auto queued = QJsonDocument::fromJson(done.last()[2].toString().toUtf8()).object();
    QCOMPARE(queued.value("code").toString(), QStringLiteral("stale_session"));
    QCOMPARE(catalogB.mutationSeq(), mutationB);
    QVERIFY(!QFileInfo::exists(projectB.filePath(QStringLiteral(".paleo/capability-ops"))));
  }

  // ---- W7：listing 不当真实调用校验（带必填的工具仍可发现） ----
  void listingKeepsToolsWithRequiredParameters() {
    QTemporaryDir project;
    QVERIFY(project.isValid());
    DataCatalog catalog;
    QString error;
    QVERIFY(catalog.open(project.path(), &error));
    AiChatToolRunner runner;
    AiChatToolContext context;
    context.catalog = &catalog;
    context.documentId = QStringLiteral("doc");
    runner.setContext(context);
    const auto tools = runner.availableTools();
    QStringList names;
    for (const auto &spec : tools)
      names.append(spec.name);
    QVERIFY(names.contains(QStringLiteral("paleo.query_project")));   // topic 必填
    QVERIFY(names.contains(QStringLiteral("paleo.asset_lineage")));   // asset 必填
    QVERIFY(names.contains(QStringLiteral("paleo.read_result")));     // result_ref 必填
  }
};

QTEST_GUILESS_MAIN(TestAgentToolReadiness)
#include "tst_agent_tool_readiness.moc"
