// 层：测试壳
// G3/tst_agent_tool_contracts — 工具声明、参数准入与解码一致性
//（行为差异测试：修复前红、修复后绿；矩阵覆盖 schema/dispatch/runner/
// service 四层与两条真实解码链）。
#include <QtTest>
#include <QJsonDocument>
#include <QFileInfo>
#include <QTemporaryDir>
#include <algorithm>
#include "../src/ai/chat/domaintools.h"
#include "../src/ai/chat/chatmessage.h"
#include "../src/workflow/aichattoolrunner.h"
#include "../src/workflow/capabilitygateway.h"
#include "../src/workflow/capabilityhost.h"
#include "../src/catalog/datacatalog.h"
#include "../src/services/paleotaskservice.h"
#include "../src/services/projectquerycapability.h"
#include "../src/services/projectquerytopics.h"

using namespace paleo;

namespace {
// 单帧工具结果等待：runner 的 toolFinished 一律异步（#281）。
bool runOne(AiChatToolRunner &runner, const QString &id, const QString &name,
            const QString &argumentsJson, bool *ok, QJsonObject *payload) {
  QSignalSpy done(&runner, &AiChatToolRunner::toolFinished);
  runner.run({id, name, argumentsJson});
  if (!done.wait(10000))
    return false;
  *ok = done.first()[1].toBool();
  *payload = QJsonDocument::fromJson(done.first()[2].toString().toUtf8()).object();
  return true;
}
bool runOne(AiChatToolRunner &runner, const QString &id, const QString &name,
            const QJsonObject &args, bool *ok, QJsonObject *payload) {
  return runOne(runner, id, name,
                QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)), ok, payload);
}
} // namespace

class TestAgentToolContracts : public QObject {
  Q_OBJECT

private slots:
  // ---- W1/W4：声明与运行期边界一致 ----
  void schemaDeclarationsExposeRuntimeBounds() {
    const auto horizon = aiToolSpec(QStringLiteral("paleo.horizon_tracking_suggestion"));
    const auto seeds = horizon.toJsonSchema().value("properties").toObject().value("seeds").toObject();
    QCOMPARE(seeds.value("maxItems").toInt(), 64);
    const auto items = seeds.value("items").toObject();
    QCOMPARE(items.value("required").toArray(),
             (QJsonArray{"inline", "xline", "sample_index"}));
    const auto properties = items.value("properties").toObject();
    for (const char *key : {"inline", "xline"})
      QCOMPARE(properties.value(QLatin1String(key)).toObject().value("type").toString(),
               QStringLiteral("integer"));
    QCOMPARE(properties.value("sample_index").toObject().value("minimum").toInt(), 0);
    const auto horizonParams = horizon.toJsonSchema().value("properties").toObject();
    QCOMPARE(horizonParams.value("window_samples").toObject().value("maximum").toInt(), 4096);
    QCOMPARE(horizonParams.value("radius").toObject().value("maximum").toInt(), 256);

    const auto read = aiToolSpec(QStringLiteral("paleo.read_result"));
    const auto readParams = read.toJsonSchema().value("properties").toObject();
    QCOMPARE(readParams.value("page_size").toObject().value("minimum").toInt(), 1);
    QCOMPARE(readParams.value("page_size").toObject().value("maximum").toInt(), 128);
    QCOMPARE(readParams.value("max_bytes").toObject().value("minimum").toInt(), 768);
    QCOMPARE(readParams.value("max_bytes").toObject().value("maximum").toInt(), 65536);

    const auto events = aiToolSpec(QStringLiteral("paleo.operation_events"));
    const auto eventParams = events.toJsonSchema().value("properties").toObject();
    QCOMPARE(eventParams.value("page_size").toObject().value("maximum").toInt(), 64);
    QCOMPARE(eventParams.value("max_bytes").toObject().value("minimum").toInt(), 1024);
    QCOMPARE(eventParams.value("max_bytes").toObject().value("maximum").toInt(), 32768);
    const auto operationRead = aiToolSpec(QStringLiteral("paleo.operation_read"));
    QCOMPARE(operationRead.toJsonSchema().value("properties").toObject()
                 .value("max_bytes").toObject().value("maximum").toInt(), 24576);
    const auto offset = operationRead.toJsonSchema().value("properties").toObject().value("offset").toObject();
    QVERIFY2(offset.value("description").toString().contains(QStringLiteral("十进制")),
             qPrintable(QString::fromUtf8(QJsonDocument(operationRead.toJsonSchema()).toJson(QJsonDocument::Compact))));

    const auto query = aiToolSpec(QStringLiteral("paleo.query_project"));
    const auto allOf = query.toJsonSchema().value("allOf").toArray();
    bool faciesInputsCondition = false, localModelsCondition = false;
    for (const auto &entry : allOf) {
      const auto condition = entry.toObject().value("if").toObject()
                                 .value("properties").toObject().value("topic").toObject();
      if (condition.value("const").toString() == QLatin1String("well_facies_inputs"))
        faciesInputsCondition = entry.toObject().value("then").toObject()
                                    .value("required").toArray().contains(QLatin1String("well_ref"));
      if (condition.value("const").toString() == QLatin1String("local_models")) {
        const auto then = entry.toObject().value("then").toObject().value("properties").toObject();
        localModelsCondition = then.value("page_size").toObject().value("maximum").toInt() == 64 &&
                               then.value("max_bytes").toObject().value("minimum").toInt() == 1024;
      }
    }
    QVERIFY(faciesInputsCondition);
    QVERIFY(localModelsCondition);

    // 声明展开仍在工具声明预算内（远小于 16 KiB 实参预算的量级）。
    for (const auto &spec : builtinAiToolSpecs()) {
      const auto serialized = QJsonDocument(spec.toChatFunction()).toJson(QJsonDocument::Compact);
      QVERIFY2(serialized.size() < 16384, qPrintable(spec.name));
    }
  }

  // ---- W3：顶层 JSON 必须是 object ----
  void nonObjectTopLevelFailsBeforeAnyProvider() {
    AiChatToolRunner runner;
    int providerCalls = 0;
    AiChatToolContext context;
    context.trackingInput = [&](const auto &, auto *, QString *) { ++providerCalls; return false; };
    context.gridFetch = [&](int, int, int, int, QVector<float> &, QString &) { ++providerCalls; return false; };
    runner.setContext(context);
    QSignalSpy done(&runner, &AiChatToolRunner::toolFinished);
    int calls = 0;
    for (const QString &argumentsJson : {QStringLiteral("[1,2]"), QStringLiteral("\"text\""),
                                         QStringLiteral("null"), QStringLiteral("17")}) {
      runner.run({QStringLiteral("top-%1").arg(++calls),
                  QStringLiteral("paleo.python_environment"), argumentsJson});
      QTRY_COMPARE(done.size(), calls);
      QVERIFY(!done.last()[1].toBool());
      // 两级明确失败均可（不进无参工具/provider）：
      //  · 合法 JSON 但非 object → invalid_arguments_type 机器码
      //  · Qt 解析器本身拒绝的顶层标量/字符串（illegal value）→ 既有
      //    「实参不是合法 JSON」诚实错误（envelope 保持兼容）
      const auto payload = QJsonDocument::fromJson(done.last()[2].toString().toUtf8()).object();
      QVERIFY2(payload.value("code").toString() == QStringLiteral("invalid_arguments_type") ||
                   payload.value("error").toString().contains(QStringLiteral("实参不是合法 JSON")),
               qPrintable(argumentsJson + QStringLiteral(" -> ") + done.last()[2].toString()));
    }
    QCOMPARE(providerCalls, 0);
    // 空串 = 无参（与 ChatToolCall::hasValidArguments 口径一致）：解析层
    // 放行到校验层，报「缺必填」而不是 JSON 解析错误。
    runner.run({"empty", "paleo.query_project", QString()});
    QTRY_COMPARE(done.size(), calls + 1);
    const auto empty = QJsonDocument::fromJson(done.last()[2].toString().toUtf8()).object();
    QVERIFY(empty.value("code").toString() != QStringLiteral("invalid_arguments_type"));
  }

  // ---- W3：严格外部档 vs 宽松内部档 ----
  void integerTextIsInternalCompatOnly() {
    const auto tile = aiToolSpec(QStringLiteral("paleo.tile_classification"));
    QJsonObject arguments{{"model", QStringLiteral("m")},
                          {"grid_rows", QStringLiteral("16")},
                          {"grid_columns", 8}};
    // 宽松档（宿主内部 API）：PW-060 文本整数仍被接受（tst_aichat 钉住）。
    QVERIFY(tile.validateParameters(arguments).isEmpty());
    QCOMPARE(dispatchAiTool(QStringLiteral("paleo.tile_classification"), arguments).status,
             AiToolDispatchStatus::Routed);
    // 严格外部档：同一实参在 runner 入口（新模型请求）明确失败。
    QString field;
    QCOMPARE(tile.validateExternalArguments(arguments, &field),
             QStringLiteral("invalid_integer_representation"));
    QCOMPARE(field, QStringLiteral("grid_rows"));

    AiChatToolRunner runner;
    int providerCalls = 0;
    AiChatToolContext context;
    context.gridFetch = [&](int, int, int, int, QVector<float> &, QString &) { ++providerCalls; return false; };
    runner.setContext(context);
    bool ok = true;
    QJsonObject payload;
    QVERIFY(runOne(runner, "text-int", "paleo.tile_classification", arguments, &ok, &payload));
    QVERIFY(!ok);
    QCOMPARE(payload.value("code").toString(), QStringLiteral("invalid_integer_representation"));
    QCOMPARE(payload.value("field").toString(), QStringLiteral("grid_rows"));
    QCOMPARE(providerCalls, 0); // 失败发生在任何取数/推理 provider 之前

    // 伪造 legacy 标记 / host_ 前缀调用号不能解锁宽松档。
    QJsonObject forged = arguments;
    forged.insert("legacy", true);
    QVERIFY(!tile.validateParameters(forged).isEmpty()); // 未知参数
    QJsonObject prefixId = arguments;
    prefixId.insert("call_id", QStringLiteral("host_trusted"));
    QCOMPARE(tile.validateExternalArguments(prefixId), QStringLiteral("invalid_integer_representation"));
  }

  void strictGateRejectsFractionsAndOverflowBeforeProviders() {
    const auto tile = aiToolSpec(QStringLiteral("paleo.tile_classification"));
    const auto withRows = [&](const QJsonValue &rows) {
      return QJsonObject{{"model", QStringLiteral("m")}, {"grid_rows", rows}, {"grid_columns", 8}};
    };
    QCOMPARE(tile.validateExternalArguments(withRows(1.5)),
             QStringLiteral("invalid_integer_representation"));
    QCOMPARE(tile.validateExternalArguments(withRows(1e300)),
             QStringLiteral("invalid_integer_representation"));
    QCOMPARE(tile.validateExternalArguments(withRows(-1.0)),
             QStringLiteral("argument_out_of_range"));
    AiChatToolRunner runner;
    QSignalSpy done(&runner, &AiChatToolRunner::toolFinished);
    runner.run({"frac", "paleo.tile_classification",
                QString::fromUtf8(QJsonDocument(withRows(1.5)).toJson(QJsonDocument::Compact))});
    QTRY_COMPARE(done.size(), 1);
    // 1.5 被宽松分派层先行拒绝（「类型应为 integer」诚实错误）——分档
    // 均为业务 provider 前的明确失败；表示级机器码由严格档直断言覆盖。
    QVERIFY(!done.first()[1].toBool());
    QVERIFY(!QJsonDocument::fromJson(done.first()[2].toString().toUtf8()).object()
                 .value("code").toString().isEmpty() ||
            !QJsonDocument::fromJson(done.first()[2].toString().toUtf8()).object()
                 .value("error").toString().isEmpty());
  }

  // ---- W3：contract_version 精确解码（两条真实链） ----
  void contractVersionDecodesExactlyOnDecodeChain() {
    using namespace paleo::capability;
    ExecuteRequest request;
    // 缺席 → 默认 1；显式 1 → 1；不支持的整值 3 原样进入（网关
    // unsupported_version 精确拒绝，不在此回退成 1）。
    QVERIFY(prepareChatToolRequest(request, "paleo.depth_convert_batch",
                                   {{"client_request_id", QStringLiteral("r1")},
                                    {"binding_mode", QStringLiteral("pinned-value")}},
                                   "scope", "doc", "/tmp/ops", {}).isEmpty());
    QCOMPARE(request.contractVersion, 1);
    QVERIFY(prepareChatToolRequest(request, "paleo.depth_convert_batch",
                                   {{"contract_version", 3},
                                    {"client_request_id", QStringLiteral("r2")},
                                    {"binding_mode", QStringLiteral("pinned-value")}},
                                   "scope", "doc", "/tmp/ops", {}).isEmpty());
    QCOMPARE(request.contractVersion, 3);
    // 表示不精确 → 明确参数错误，不静默解释成 1。
    for (const QJsonValue &bad : {QJsonValue(QStringLiteral("2")), QJsonValue(2.5), QJsonValue(0),
                                  QJsonValue(-1), QJsonValue(2147483648.0)}) {
      const auto rejected = prepareChatToolRequest(
          request, "paleo.depth_convert_batch",
          {{"contract_version", bad}, {"client_request_id", QStringLiteral("r3")},
           {"binding_mode", QStringLiteral("pinned-value")}},
          "scope", "doc", "/tmp/ops", {});
      QCOMPARE(rejected.value("code").toString(), QStringLiteral("contract_version_invalid"));
    }
    // parameters/inputs 形状防御：非 object 明确拒绝（此前静默取空 map）。
    QCOMPARE(prepareChatToolRequest(request, "paleo.depth_convert_batch",
                                    {{"parameters", QStringLiteral("x")}},
                                    "scope", "doc", "/tmp/ops", {}).value("code").toString(),
             QStringLiteral("invalid_request_shape"));
    QCOMPARE(prepareHostedChatToolRequest(request, "paleo.depth_convert_batch",
                                          {{"inputs", QJsonArray{1}}}).value("code").toString(),
             QStringLiteral("invalid_request_shape"));
  }

  void contractVersionOnRunnerChainFailsBeforeOperation() {
    QTemporaryDir project;
    QVERIFY(project.isValid());
    PaleoTaskService tasks;
    AiChatToolRunner runner;
    runner.setTaskService(&tasks);
    AiChatToolContext context;
    context.projectDir = project.path();
    context.documentId = QStringLiteral("doc");
    runner.setContext(context);
    const auto prepare = [](const QJsonValue &version) {
      return QJsonObject{{"contract_version", version},
                         {"client_request_id", QStringLiteral("v-1")},
                         {"binding_mode", QStringLiteral("pinned-value")},
                         {"publish_intent", QStringLiteral("prepare")},
                         {"parameters", QJsonObject{{"FIELD", QStringLiteral("z")}, {"CELL_SIZE", 10.0}}},
                         {"inputs", QJsonObject{{"points_ref", QStringLiteral("ver-points")},
                                                {"subinterval_id", QStringLiteral("sub-1")}}}};
    };
    bool ok = true;
    QJsonObject payload;
    // "2" 文本：严格档在 integer 表示处拒绝——无 Task、无 operation 目录。
    QVERIFY(runOne(runner, "v-text", "paleo.constraint_idw", prepare(QStringLiteral("2")), &ok, &payload));
    QVERIFY(!ok);
    QVERIFY2(payload.value("code").toString() == QStringLiteral("invalid_integer_representation"),
             qPrintable(QJsonDocument(payload).toJson(QJsonDocument::Compact)));
    // 2.5：同上。
    QVERIFY(runOne(runner, "v-frac", "paleo.constraint_idw", prepare(2.5), &ok, &payload));
    QVERIFY(!ok);
    // 2.5 同样由宽松分派层先行拒绝；严格档表示由 v-text 与直断言覆盖。
    QVERIFY(!payload.value("code").toString().isEmpty() || !payload.value("error").toString().isEmpty());
    QCOMPARE(tasks.tasks().size(), 0);
    QVERIFY(!QFileInfo::exists(project.filePath(QStringLiteral(".paleo/capability-ops"))));
    // 不支持的整值版本 9：解码原样通过（不回退成 1），网关在 OperationLog
    // 之前按 unsupported_version 拒绝——不写 operation 目录。
    QVERIFY(runOne(runner, "v-unsupported", "paleo.constraint_idw", prepare(9), &ok, &payload));
    QVERIFY(!ok);
    QCOMPARE(payload.value("code").toString(), QStringLiteral("unsupported_version"));
    QVERIFY(!QFileInfo::exists(project.filePath(QStringLiteral(".paleo/capability-ops"))));
  }

  // ---- W2：query_project 主题字段矩阵 ----
  void queryTopicFieldMatrixAtValidatorAndDispatch() {
    QTemporaryDir project;
    QVERIFY(project.isValid());
    DataCatalog catalog;
    QString openError;
    QVERIFY(catalog.open(project.path(), &openError));
    query::ProjectQueryCapability service;
    service.bind(&catalog, {QStringLiteral("doc"), 1, 1, QStringLiteral("chat")});
    const auto query = aiToolSpec(QStringLiteral("paleo.query_project"));
    const QJsonObject wellRef{{"contract_version", 1},
                              {"document_id", QStringLiteral("doc")},
                              {"catalog_entity_id", QStringLiteral("well-1")},
                              {"model_store_id", QStringLiteral("area-model")},
                              {"model_available", true},
                              {"model_well_id", QStringLiteral("7")},
                              {"model_head", QStringLiteral("0")},
                              {"model_store_identity", QStringLiteral("store")},
                              {"model_generation", QStringLiteral("g1")}};
    struct Row {
      const char *description;
      QJsonObject arguments;
      QString expectedCode; // 空 = 通过
      QString expectedField;
    };
    const QList<Row> rows{
        {"wells minimal", {{"topic", "wells"}}, QString(), QString()},
        {"wells rejects version fields", {{"topic", "wells"}, {"version_id", "v1"}},
         "unsupported_topic_argument", "version_id"},
        {"wells rejects well_ref", {{"topic", "wells"}, {"well_ref", wellRef}},
         "unsupported_topic_argument", "well_ref"},
        {"summary rejects page_size", {{"topic", "summary"}, {"page_size", 20}},
         "unsupported_topic_argument", "page_size"},
        {"summary rejects cursor", {{"topic", "summary"}, {"cursor", "c"}},
         "unsupported_topic_argument", "cursor"},
        {"summary allows max_bytes", {{"topic", "summary"}, {"max_bytes", 4096}}, QString(), QString()},
        {"horizons rejects asset_type", {{"topic", "horizons"}, {"asset_type", "well_log"}},
         "unsupported_topic_argument", "asset_type"},
        {"assets allows asset_type", {{"topic", "assets"}, {"asset_type", "well_log"}}, QString(), QString()},
        {"well_details allows role", {{"topic", "well_details"}, {"well_name", "W-1"}, {"role", "well_log"}},
         QString(), QString()},
        {"version missing version_id", {{"topic", "version"}}, "missing_required_field", "version_id"},
        {"version rejects cursor", {{"topic", "version"}, {"version_id", "v"}, {"cursor", "c"}},
         "unsupported_topic_argument", "cursor"},
        {"version_parents minimal", {{"topic", "version_parents"}, {"version_id", "v"}}, QString(), QString()},
        {"well_facies_models rejects cursor",
         {{"topic", "well_facies_models"}, {"cursor", "c"}}, "unsupported_topic_argument", "cursor"},
        {"well_facies_inputs missing well_ref", {{"topic", "well_facies_inputs"}},
         "missing_required_field", "well_ref"},
        {"well_facies_inputs rejects model_id",
         {{"topic", "well_facies_inputs"}, {"well_ref", wellRef}, {"model_id", "m"}},
         "unsupported_topic_argument", "model_id"},
        {"local_models rejects well_name", {{"topic", "local_models"}, {"well_name", "W-1"}},
         "unsupported_topic_argument", "well_name"},
        {"local_models budget over max", {{"topic", "local_models"}, {"page_size", 65}},
         "query_budget_invalid", "page_size"},
        {"assets budget fraction", {{"topic", "assets"}, {"page_size", 1.5}},
         "query_budget_invalid", "page_size"},
        {"wells budget text", {{"topic", "wells"}, {"page_size", QStringLiteral("50")}},
         "query_budget_invalid", "page_size"},
    };
    for (const auto &row : rows) {
      // 宽松档（validateParameters/dispatchAiTool）不承载主题字段契约——
      // 分派层口径保持既有行为；同一契约在严格外部档与服务入口执行。
      QString field;
      const auto strict = query.validateExternalArguments(row.arguments, &field);
      QVERIFY2(strict == row.expectedCode,
               qPrintable(QStringLiteral("%1: got %2 want %3")
                              .arg(row.description, strict, row.expectedCode)));
      QCOMPARE(field, row.expectedField);
      const auto serviceCode = service.query(row.arguments).value("code").toString();
      if (row.expectedCode == QLatin1String("unsupported_topic_argument") ||
          row.expectedCode == QLatin1String("missing_required_field") ||
          row.expectedCode == QLatin1String("query_budget_invalid"))
        QCOMPARE(serviceCode, row.expectedCode);
    }
  }

  void queryTopicContractAtRunnerKeepsCatalogSnapshot() {
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
    const quint64 mutationBefore = catalog.mutationSeq();
    bool ok = true;
    QJsonObject payload;
    QVERIFY(runOne(runner, "cross", "paleo.query_project",
                   {{"topic", "wells"}, {"version_id", QStringLiteral("v1")}}, &ok, &payload));
    QVERIFY(!ok);
    QCOMPARE(payload.value("code").toString(), QStringLiteral("unsupported_topic_argument"));
    QCOMPARE(payload.value("field").toString(), QStringLiteral("version_id"));
    QCOMPARE(catalog.mutationSeq(), mutationBefore);
    QVERIFY(runOne(runner, "summary-page", "paleo.query_project",
                   {{"topic", "summary"}, {"page_size", 20}}, &ok, &payload));
    QVERIFY(!ok);
    QCOMPARE(payload.value("code").toString(), QStringLiteral("unsupported_topic_argument"));
  }

  // ---- W4：seeds / read_result / operation 分页声明与执行 ----
  void seedsShapeEnforcedBeforeTrackingProvider() {
    AiAssistWorkflow workflow(nullptr);
    PaleoTaskService tasks;
    AiChatToolRunner runner;
    runner.setWorkflow(&workflow);
    runner.setTaskService(&tasks);
    int providerCalls = 0, fetchCalls = 0;
    AiChatToolContext context;
    context.trackingInput = [&](const auto &, auto *, QString *) { ++providerCalls; return false; };
    context.traceFetch = [&](int, int, int, int, QVector<float> &, QString &) { ++fetchCalls; return false; };
    runner.setContext(context);
    QSignalSpy done(&runner, &AiChatToolRunner::toolFinished);
    // 元素坐标为文本整数：严格档拒绝，provider 不被调用。
    const QJsonObject textSeed{{"inline", QStringLiteral("10")}, {"xline", 20}, {"sample_index", 5}};
    runner.run({"seed-text", "paleo.horizon_tracking_suggestion",
                QString::fromUtf8(QJsonDocument(QJsonObject{{"model", QStringLiteral("m")},
                                                            {"window_samples", 4}, {"radius", 1},
                                                            {"seeds", QJsonArray{textSeed}}})
                                      .toJson(QJsonDocument::Compact))});
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(!done.first()[1].toBool());
    QCOMPARE(QJsonDocument::fromJson(done.first()[2].toString().toUtf8()).object()
                 .value("code").toString(), QStringLiteral("invalid_integer_representation"));
    QVERIFY(done.first()[2].toString().contains(QStringLiteral("seeds.0.inline")));
    QCOMPARE(providerCalls, 0);
    QCOMPARE(fetchCalls, 0);
    // 缺坐标：运行期 PW-013 边界检查保留（schema required 之外的第二道）。
    const QJsonObject missingXline{{"inline", 10}, {"sample_index", 5}};
    runner.run({"seed-missing", "paleo.horizon_tracking_suggestion",
                QString::fromUtf8(QJsonDocument(QJsonObject{{"model", QStringLiteral("m")},
                                                            {"window_samples", 4}, {"radius", 1},
                                                            {"seeds", QJsonArray{missingXline}}})
                                      .toJson(QJsonDocument::Compact))});
    QTRY_COMPARE(done.size(), 2);
    QVERIFY(!done.last()[1].toBool());
    QCOMPARE(providerCalls, 0); // runHorizon 在取数前拒绝
    QCOMPARE(fetchCalls, 0);
    // 种子数超限：运行期预算检查保留。
    QJsonArray many;
    for (int i = 0; i < 65; ++i)
      many.append(QJsonObject{{"inline", i}, {"xline", i}, {"sample_index", 1}});
    runner.run({"seed-count", "paleo.horizon_tracking_suggestion",
                QString::fromUtf8(QJsonDocument(QJsonObject{{"model", QStringLiteral("m")},
                                                            {"window_samples", 4}, {"radius", 1},
                                                            {"seeds", many}})
                                      .toJson(QJsonDocument::Compact))});
    QTRY_COMPARE(done.size(), 3);
    QVERIFY(!done.last()[1].toBool());
    QCOMPARE(providerCalls, 0);
    QCOMPARE(fetchCalls, 0);
    // 未知种子字段：严格档与 schema items 的 additionalProperties:false 一致。
    const QJsonObject stray{{"inline", 1}, {"xline", 1}, {"sample_index", 1}, {"depth", 2.5}};
    runner.run({"seed-stray", "paleo.horizon_tracking_suggestion",
                QString::fromUtf8(QJsonDocument(QJsonObject{{"model", QStringLiteral("m")},
                                                            {"window_samples", 4}, {"radius", 1},
                                                            {"seeds", QJsonArray{stray}}})
                                      .toJson(QJsonDocument::Compact))});
    QTRY_COMPARE(done.size(), 4);
    QVERIFY(!done.last()[1].toBool());
    const auto strayPayload = QJsonDocument::fromJson(done.last()[2].toString().toUtf8()).object();
    QCOMPARE(strayPayload.value("code").toString(), QStringLiteral("unsupported_seed_field"));
    QCOMPARE(strayPayload.value("field").toString(), QStringLiteral("seeds.0.depth"));
    QCOMPARE(providerCalls, 0);
    QCOMPARE(fetchCalls, 0);
    QVERIFY(tasks.tasks().isEmpty());
  }

  void readResultBudgetEnforcedAtGateAndRuntime() {
    const auto read = aiToolSpec(QStringLiteral("paleo.read_result"));
    QString field;
    QCOMPARE(read.validateExternalArguments({{"result_ref", QStringLiteral("chat-result-v1:a:b")},
                                             {"page_size", 200}}, &field),
             QStringLiteral("argument_out_of_range"));
    QCOMPARE(field, QStringLiteral("page_size"));
    QCOMPARE(read.validateExternalArguments({{"result_ref", QStringLiteral("chat-result-v1:a:b")},
                                             {"max_bytes", 100}}, &field),
             QStringLiteral("argument_out_of_range"));
    QCOMPARE(field, QStringLiteral("max_bytes"));
    AiChatToolRunner runner;
    runner.setResultSession(QStringLiteral("session"));
    bool ok = true;
    QJsonObject payload;
    QVERIFY(runOne(runner, "read-page", "paleo.read_result",
                   {{"result_ref", QStringLiteral("chat-result-v1:a:b")}, {"page_size", 200}},
                   &ok, &payload));
    QVERIFY(!ok);
    QCOMPARE(payload.value("code").toString(), QStringLiteral("argument_out_of_range"));
    // 范围外的引用走原有 store 语义（result_scope_changed），协议不变。
    QVERIFY(runOne(runner, "read-unknown", "paleo.read_result",
                   {{"result_ref", QStringLiteral("chat-result-v1:a:b")}}, &ok, &payload));
    QVERIFY(!ok);
    QCOMPARE(payload.value("code").toString(), QStringLiteral("result_scope_changed"));
  }

  void operationPageBudgetEnforcedBeforeRecovery() {
    AiChatToolRunner runner;
    QSignalSpy done(&runner, &AiChatToolRunner::toolFinished);
    runner.run({"events-page", "paleo.operation_events",
                R"({"operation_id":"op-1","page_size":65})"});
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(!done.first()[1].toBool());
    QCOMPARE(QJsonDocument::fromJson(done.first()[2].toString().toUtf8()).object()
                 .value("code").toString(), QStringLiteral("argument_out_of_range"));
    runner.run({"read-bytes", "paleo.operation_read",
                R"({"operation_id":"op-1","port_id":"p","max_bytes":32769})"});
    QTRY_COMPARE(done.size(), 2);
    QVERIFY(!done.last()[1].toBool());
    QCOMPARE(QJsonDocument::fromJson(done.last()[2].toString().toUtf8()).object()
                 .value("code").toString(), QStringLiteral("argument_out_of_range"));
    // 十进制串协议不变：合法 offset 继续由运行期 decimal() 把守（此处
    // 无宿主 → operation_host_or_session_unavailable，而非参数错误）。
    runner.run({"read-offset", "paleo.operation_read",
                R"({"operation_id":"op-1","port_id":"p","offset":"12"})"});
    QTRY_COMPARE(done.size(), 3);
    QCOMPARE(QJsonDocument::fromJson(done.last()[2].toString().toUtf8()).object()
                 .value("code").toString(), QStringLiteral("operation_host_or_session_unavailable"));
  }

  // ---- W8：真实正向链 ----
  void legalQueryPagesThroughRealCatalog() {
    QTemporaryDir project;
    QVERIFY(project.isValid());
    DataCatalog catalog;
    QString error;
    QVERIFY(catalog.open(project.path(), &error));
    for (int i = 0; i < 60; ++i) {
      CatalogEntity entity;
      entity.id = QStringLiteral("well-%1").arg(i);
      entity.entityType = QStringLiteral("well");
      entity.name = QStringLiteral("W-%1").arg(i);
      entity.td = 2000.0 + i;
      QVERIFY(catalog.addEntity(entity));
    }
    AiChatToolRunner runner;
    AiChatToolContext context;
    context.catalog = &catalog;
    context.documentId = QStringLiteral("doc");
    runner.setContext(context);
    QStringList seen;
    QString cursor;
    int pages = 0;
    do {
      QJsonObject arguments{{"topic", "wells"}, {"page_size", 30}};
      if (!cursor.isEmpty())
        arguments.insert("cursor", cursor);
      bool ok = false;
      QJsonObject payload;
      QVERIFY(runOne(runner, QStringLiteral("wells-%1").arg(pages), "paleo.query_project",
                     arguments, &ok, &payload));
      QVERIFY(ok);
      QCOMPARE(payload.value("count").toInt(), 60);
      for (const auto &value : payload.value("wells").toArray())
        seen.append(value.toObject().value("id").toString());
      cursor = payload.value("nextCursor").toString();
      ++pages;
      QVERIFY(pages < 10);
    } while (!cursor.isEmpty());
    // 页数不钉死（服务端按 max_bytes 预算可能拆得更细），钉住的是：
    // 多页续完、无重复、总数正确。
    QVERIFY(pages >= 2);
    seen.sort();
    seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
    QCOMPARE(seen.size(), 60);
  }

  void capabilityPrepareReplayKeepsOperationIdentity() {
    QTemporaryDir project;
    QVERIFY(project.isValid());
    PaleoTaskService tasks;
    AiChatToolRunner runner;
    runner.setTaskService(&tasks);
    AiChatToolContext context;
    context.projectDir = project.path();
    context.documentId = QStringLiteral("doc");
    runner.setContext(context);
    const auto prepare = [] {
      return QJsonObject{{"client_request_id", QStringLiteral("same-request")},
                         {"binding_mode", QStringLiteral("pinned-value")},
                         {"publish_intent", QStringLiteral("prepare")},
                         {"parameters", QJsonObject{{"FIELD", QStringLiteral("z")}, {"CELL_SIZE", 10.0}}},
                         {"inputs", QJsonObject{{"points_ref", QStringLiteral("ver-points")},
                                                {"subinterval_id", QStringLiteral("sub-1")}}}};
    };
    bool ok = false;
    QJsonObject first, second, third;
    QVERIFY(runOne(runner, "prepare-1", "paleo.constraint_idw", prepare(), &ok, &first));
    QVERIFY(ok);
    QCOMPARE(first.value("code").toString(), QStringLiteral("prepared"));
    const auto operationId = first.value("operationId").toString();
    QVERIFY(!operationId.isEmpty());
    // 同 client_request_id 幂等重放：同一 operation，不产生第二个业务操作。
    QVERIFY(runOne(runner, "prepare-2", "paleo.constraint_idw", prepare(), &ok, &second));
    QCOMPARE(second.value("operationId").toString(), operationId);
    QVERIFY(QFileInfo::exists(project.filePath(QStringLiteral(".paleo/capability-ops"))));
    // 表示不同的同义重试（"1" 文本）不会先规范化再重放——在入口拒绝，
    // 既不冲突也不生成替代 operation。
    auto textual = prepare();
    textual.insert("contract_version", QStringLiteral("1"));
    QJsonObject rejected;
    QVERIFY(runOne(runner, "prepare-3", "paleo.constraint_idw", textual, &ok, &rejected));
    QVERIFY(!ok);
    QCOMPARE(rejected.value("code").toString(), QStringLiteral("invalid_integer_representation"));
    // 被拒的尝试没有产生任何新 operation：原请求幂等重放仍是原 operation。
    QVERIFY(runOne(runner, "prepare-4", "paleo.constraint_idw", prepare(), &ok, &third));
    QCOMPARE(third.value("operationId").toString(), operationId);
  }

  void operationGetReportsPersistedOperationFacts() {
    QTemporaryDir project;
    QVERIFY(project.isValid());
    DataCatalog catalog;
    QString error;
    QVERIFY(catalog.open(project.path(), &error));
    capability::HostBinding binding;
    binding.context.open = true;
    binding.context.documentId = QStringLiteral("doc");
    binding.context.readOnly = true;
    binding.context.allowedEffects = host::Read;
    capability::CapabilityHost host;
    host.setBindingProvider([&] { return binding; });
    host.setCallerScope(host::Caller::Agent, QStringLiteral("principal"));
    PaleoTaskService tasks;
    host.setTaskService(&tasks);
    AiChatToolRunner runner;
    runner.setTaskService(&tasks);
    runner.setResultSession(QStringLiteral("chat-A"));
    AiChatToolContext context;
    context.catalog = &catalog;
    context.documentId = QStringLiteral("doc");
    context.capabilityHost = &host;
    context.requireCapabilityHost = true;
    runner.setContext(context);
    // 未知 id：作用域内如实 not_found（读取面零副作用）。
    bool ok = true;
    QJsonObject payload;
    QVERIFY(runOne(runner, "op-get", "paleo.operation_get",
                   {{"operation_id", QStringLiteral("nope")}}, &ok, &payload));
    QVERIFY(!ok);
    QVERIFY(payload.contains(QStringLiteral("code")));
    QCOMPARE(tasks.tasks().size(), 0);
  }
};

QTEST_GUILESS_MAIN(TestAgentToolContracts)
#include "tst_agent_tool_contracts.moc"
