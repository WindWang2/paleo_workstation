// 层：测试壳
// I5 W1/W5/W6/W8：paleo.capability_inspection 服务层的反例矩阵。
//
// oracle 区分「正确与错误实现」：
//   · 四类身份不被一张 bool 表取代：contours（Callable+command surface+无独立
//     工具）、wellfacies（LegacyAdapter capability+专用工具）、AiToolSpec-only
//     查询/计划工具、UI View 命令——逐项核对输出身份与原表；
//   · 版本/别名精确：给定版本不回落、未知不猜测、AiToolSpec-only 不伪造
//     capabilityId；
//   · check 复用真实验证器：issue 即原验证器返回（missing/unknown/range/
//     enum/strict representation），effective 只来自 AlgorithmSpec 默认值；
//   · 伪造 authority/路径不授予任何权限，结果无 handle/ticket/operationId；
//   · policy（当前静态策略）与 runtime（恒 not_checked）分离；无 Host 时
//     policy unavailable 而静态合同照常可读。
#include <QtTest>
#include "../src/ai/chat/domaintools.h"
#include "../src/services/capabilityspec.h"
#include "../src/services/commandspec.h"
#include "../src/services/hostcallcontext.h"
#include "../src/workflow/agentcapabilityinspection.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {
paleo::agent::InspectionHostContext readOnlyHost() {
  paleo::agent::InspectionHostContext host;
  host.available = true;
  host.live.open = true;
  host.live.documentId = QStringLiteral("doc-i5");
  host.live.epoch = 7;
  host.live.persistentIdentity = true;
  host.live.readOnly = true; // Read-only 工程：Presentation/PrivateStage 必拒
  host.live.ownsWrites = true;
  // 效果面全开、但工程只读：拒绝原因必须是 project_read_only 而非 effect_denied
  // ——与真实 Read-only 工程（h5 夹具 writable=false 口径）一致。
  host.live.allowedEffects = paleo::host::Read | paleo::host::PrivateStage |
      paleo::host::CatalogPublish | paleo::host::Presentation |
      paleo::host::FileExport | paleo::host::ExternalTransfer;
  host.expectedDocumentId = host.live.documentId;
  host.expectedEpoch = host.live.epoch;
  return host;
}
paleo::agent::InspectionResult inspect(const QJsonObject &args,
                                      const paleo::agent::InspectionHostContext &host = {}) {
  return paleo::agent::runCapabilityInspection(args, host);
}
QJsonObject payloadJson(const paleo::agent::InspectionResult &result) {
  return result.payload;
}

QJsonObject toolCheckArgs(const QString &tool, const QJsonObject &arguments) {
  return QJsonObject{{"mode", "check"},
                     {"candidate", QJsonObject{{"tool", tool}, {"arguments", arguments}}}};
}
QJsonObject algorithmCheckArgs(const QString &algorithm, int version, const QJsonObject &parameters) {
  return QJsonObject{{"mode", "check"},
                     {"candidate", QJsonObject{{"algorithm", algorithm},
                                               {"contract_version", version},
                                               {"parameters", parameters}}}};
}
} // namespace

class TestI5CapabilityInspection : public QObject {
  Q_OBJECT
private slots:
  void listIsStableSortedPaginatedAndCursorBound();
  void commandPagesRespectWholeReplyBudgetWithoutLosingEntries();
  void commandsOnlyWithExplicitFilter();
  void keywordSearchMatchesLocatorAndBindsPagination();
  void describeCapabilityExactVersionAliasAndNoFallback();
  void describeAlgorithmVersionsWithoutMerging();
  void describeToolReflectsRealContractAndHonestRelations();
  void counterExampleContoursCallableWithoutDedicatedTool();
  void counterExampleWellFaciesLegacyAdapterWithDedicatedTool();
  void viewCommandsAreDeclarationFactsOnly();
  void checkToolCandidateReturnsRealValidatorIssues();
  void checkToolCandidateStrictRepresentation();
  void checkAlgorithmCandidateReturnsContractIssuesAndDefaults();
  void fakeAuthorityAndPathsGrantNothing();
  void policyIsStaticAndSeparatedFromRuntime();
  void missingHostKeepsStaticContractAndMarksPolicyUnavailable();
  void describeBudgetDropsWholeSectionsWithCoverage();
  void cursorAndSectionGuardsAreStrict();
  void topLevelClosedFieldSet();
  void unknownModesAndTargetsAreMachineErrors();
};

void TestI5CapabilityInspection::listIsStableSortedPaginatedAndCursorBound() {
  const auto page = inspect({{"mode", "list"}, {"page_size", 5}});
  QVERIFY(page.ok);
  const auto payload = payloadJson(page);
  QCOMPARE(payload.value("contract").toString(), QStringLiteral("paleo.capability-inspect.v1"));
  QCOMPARE(payload.value("mode").toString(), QStringLiteral("list"));
  QVERIFY(payload.value("hasMore").toBool());
  const auto entries = payload.value("entries").toArray();
  QCOMPARE(entries.size(), 5);
  QVERIFY(payload.value("totalCount").toInt() >
          payload.value("returnedCount").toInt());
  // 排序稳定：kind → id → version。
  QString previous;
  for (const auto &value : entries) {
    const auto entry = value.toObject();
    const QString key = entry.value("kind").toString() + ':' + entry.value("id").toString();
    QVERIFY(previous.isEmpty() || previous <= key);
    previous = key;
  }
  // default 过滤不含 command 全集（W2：UI 命令不默认混入）。
  for (const auto &value : entries)
    QVERIFY(value.toObject().value("kind").toString() != QLatin1String("command"));

  // 续页绑定 filter+目录摘要：换条件续页 → cursor_stale。
  const QString cursor = payload.value("nextCursor").toString();
  QVERIFY(!cursor.isEmpty());
  const auto next = inspect({{"mode", "list"}, {"page_size", 5}, {"cursor", cursor}});
  QVERIFY(next.ok);
  QVERIFY(payloadJson(next).value("entries").toArray().size() >= 1);
  const auto stale = inspect({{"mode", "list"}, {"page_size", 5}, {"cursor", cursor},
                              {"filter", "tools"}});
  QVERIFY(!stale.ok);
  QCOMPARE(stale.code, QStringLiteral("cursor_stale"));
}

void TestI5CapabilityInspection::commandPagesRespectWholeReplyBudgetWithoutLosingEntries() {
  QStringList expected;
  for (const auto &spec : paleo::command::CommandRegistry::all())
    expected.append(spec.commandId);
  expected.sort();

  QJsonObject args{{"mode", "list"}, {"filter", "commands"}, {"page_size", 32}, {"max_bytes", 2048}};
  QStringList returned;
  do {
    const auto result = inspect(args);
    QVERIFY(result.ok);
    const auto &page = result.payload;
    QVERIFY(QJsonDocument(page).toJson(QJsonDocument::Compact).size() <= 2048);
    const auto entries = page.value("entries").toArray();
    QVERIFY(!entries.isEmpty());
    QCOMPARE(page.value("returnedCount").toInt(), entries.size());
    for (const auto row : entries)
      returned.append(row.toObject().value("id").toString());
    QVERIFY(returned.size() <= expected.size());
    QCOMPARE(page.value("hasMore").toBool(), returned.size() < expected.size());
    QCOMPARE(page.value("coverage").toObject().value("complete").toBool(), returned.size() == expected.size());
    QCOMPARE(page.contains("nextCursor"), returned.size() < expected.size());
    args.insert("cursor", page.value("nextCursor"));
  } while (returned.size() < expected.size());
  QCOMPARE(returned, expected);
}

void TestI5CapabilityInspection::commandsOnlyWithExplicitFilter() {
  const auto page = inspect({{"mode", "list"}, {"filter", "commands"}, {"page_size", 32}});
  QVERIFY(page.ok);
  const auto entries = payloadJson(page).value("entries").toArray();
  QVERIFY(!entries.isEmpty());
  for (const auto &value : entries) {
    const auto entry = value.toObject();
    QCOMPARE(entry.value("kind").toString(), QStringLiteral("command"));
    // View 命令在清单里只是声明事实，不带执行入口。
    QVERIFY(!entry.contains(QStringLiteral("target")));
  }
}

void TestI5CapabilityInspection::keywordSearchMatchesLocatorAndBindsPagination() {
  QJsonObject args{{"mode", "list"}, {"filter", "commands"}, {"query", QStringLiteral("井位  导入")}};
  auto result = inspect(args);
  QVERIFY(result.ok);
  const auto rows = payloadJson(result).value("entries").toArray();
  bool found = false;
  for (const auto row : rows)
    if (row.toObject().value("id") == QLatin1String("data.import.wells")) {
      found = true;
      QVERIFY(!row.toObject().value("title").toString().isEmpty());
    }
  QVERIFY(found);
  args["query"] = QStringLiteral("井位 无此命令");
  QCOMPARE(payloadJson(inspect(args)).value("totalCount").toInt(), 0);
  args["query"] = QStringLiteral("project");
  args["page_size"] = 1;
  const QString cursor = payloadJson(inspect(args)).value("nextCursor").toString();
  QVERIFY(!cursor.isEmpty());
  args["cursor"] = cursor;
  args["query"] = QStringLiteral("  PROJECT  ");
  QVERIFY(inspect(args).ok);
  args["query"] = QStringLiteral("projec");
  QCOMPARE(inspect(args).code, QStringLiteral("cursor_stale"));
  args.remove("cursor");
  for (const QJsonValue &invalid : {QJsonValue(17), QJsonValue(QString(129, u'x'))}) {
    args["query"] = invalid;
    QVERIFY(!inspect(args).ok);
  }
}

void TestI5CapabilityInspection::describeCapabilityExactVersionAliasAndNoFallback() {
  // 别名解析：paleo.constraint_idw 是 constraint.idw 的注册别名。
  const auto byAlias = inspect({{"mode", "describe"}, {"target_kind", "capability"},
                                {"target_id", "paleo.constraint_idw"}});
  QVERIFY(byAlias.ok);
  const auto sections = payloadJson(byAlias).value("sections").toObject();
  const auto identity = sections.value("identity").toObject();
  QCOMPARE(identity.value("id").toString(), QStringLiteral("constraint.idw"));
  QCOMPARE(identity.value("requested_alias").toString(), QStringLiteral("paleo.constraint_idw"));
  QCOMPARE(identity.value("exposure").toString(), QStringLiteral("callable"));
  // 精确版本：constraint.idw 只有 v1——请求 v2 明确拒绝，不回落（field 归因
  // 版本字段，与 algorithm/command 同口径）。
  const auto wrong = inspect({{"mode", "describe"}, {"target_kind", "capability"},
                              {"target_id", "constraint.idw"}, {"contract_version", 2}});
  QVERIFY(!wrong.ok);
  QCOMPARE(wrong.code, QStringLiteral("unsupported_version"));
  QCOMPARE(wrong.field, QStringLiteral("contract_version"));
  // F5：失败帧携带 mode（自身 schema required=[contract,ok] 之外的纪律补齐）。
  QCOMPARE(wrong.payload.value("mode").toString(), QStringLiteral("describe"));
  // wellfacies.predict 登记 v2：默认（0）取 v2 并列出 versions。
  const auto facies = inspect({{"mode", "describe"}, {"target_kind", "capability"},
                               {"target_id", "wellfacies.predict"}});
  QVERIFY(facies.ok);
  const auto faciesIdentity = payloadJson(facies).value("sections").toObject()
                                  .value("identity").toObject();
  QCOMPARE(faciesIdentity.value("contract_version").toInt(), 2);
  QCOMPARE(faciesIdentity.value("versions").toArray().first().toString(), QStringLiteral("2"));
  // 未知能力不猜测。
  const auto unknown = inspect({{"mode", "describe"}, {"target_kind", "capability"},
                                {"target_id", "no.such_capability"}});
  QVERIFY(!unknown.ok);
  QCOMPARE(unknown.code, QStringLiteral("unknown_target"));
}

void TestI5CapabilityInspection::describeAlgorithmVersionsWithoutMerging() {
  const auto tile = inspect({{"mode", "describe"}, {"target_kind", "algorithm"},
                             {"target_id", "onnx:tile_classification"}});
  QVERIFY(tile.ok);
  const auto sections = payloadJson(tile).value("sections").toObject();
  QCOMPARE(sections.value("identity").toObject().value("contract_version").toInt(), 2);
  QCOMPARE(sections.value("identity").toObject().value("versions").toArray().size(), 1);
  QCOMPARE(sections.value("contract").toObject().value("engine").toString(),
           QStringLiteral("onnx"));
  // v1 未登记：明确拒绝，不回落最新版。
  const auto v1 = inspect({{"mode", "describe"}, {"target_kind", "algorithm"},
                           {"target_id", "onnx:tile_classification"}, {"contract_version", 1}});
  QVERIFY(!v1.ok);
  QCOMPARE(v1.code, QStringLiteral("unsupported_version"));
  // complete ≠ callable：IDW 算法 complete 且 exposure callable；processing
  // 未开放算法 complete 仍 internal/legacy（逐项如实）。
  const auto idw = inspect({{"mode", "describe"}, {"target_kind", "algorithm"},
                            {"target_id", "paleo:paleo_constraint_idw"}});
  QVERIFY(idw.ok);
  QVERIFY(payloadJson(idw).value("sections").toObject().value("identity").toObject()
              .value("complete").toBool());
  const auto related = payloadJson(idw).value("sections").toObject().value("related").toObject();
  QCOMPARE(related.value("exposure_for_algorithm").toString(), QStringLiteral("callable"));
  QVERIFY(!related.value("complete_is_not_callable").toBool());
}

void TestI5CapabilityInspection::describeToolReflectsRealContractAndHonestRelations() {
  // query_project 的完整声明 schema 超过单页内联预算（F1 路径在
  // describeBudgetDropsWholeSections 里以它做超限反例）；schema 内容断言用
  // 较小的 operation_list，route/related 断言用 query_project。
  const auto operations = inspect({{"mode", "describe"}, {"target_kind", "tool"},
                                   {"target_id", "paleo.operation_list"},
                                   {"sections", QJsonArray{QStringLiteral("contract")}},
                                   {"max_bytes", 8192}});
  QVERIFY(operations.ok);
  const auto operationsSections = payloadJson(operations).value("sections").toObject();
  QVERIFY(operationsSections.contains(QStringLiteral("declaration")));
  const auto operationsDeclaration = operationsSections.value("declaration").toObject();
  // schema 来自真实 AiToolSpec 生成器：view enum 与注册表同源。
  const auto view = operationsDeclaration.value("parameters_schema").toObject()
                        .value("properties").toObject().value("view").toObject();
  QVERIFY(view.value("enum").toArray().contains(QStringLiteral("relations")));
  QVERIFY(operationsDeclaration.contains(QStringLiteral("model_envelope_schema")));
  QVERIFY(payloadJson(operations).value("coverage").toObject().value("complete").toBool());

  const auto query = inspect({{"mode", "describe"}, {"target_kind", "tool"},
                              {"target_id", "paleo.query_project"},
                              {"sections", QJsonArray{QStringLiteral("route"),
                                                      QStringLiteral("related")}},
                              {"max_bytes", 8192}});
  QVERIFY(query.ok);
  const auto sections = payloadJson(query).value("sections").toObject();
  const auto route = sections.value("route").toObject();
  QCOMPARE(route.value("status").toString(), QStringLiteral("static_route_registered"));
  // 空探针实参走 dispatch 早退分支时不虚构 target（target_note 说明取法）。
  QVERIFY(route.contains(QStringLiteral("target")) ||
          route.contains(QStringLiteral("target_note")));
  // AiToolSpec-only：不伪造 capabilityId（resolveAlias 无命中——真实冲突
  // 时同一路径报 alias_collision，注册表当前无冲突项，见 aliasCollisions）。
  const auto related = sections.value("related").toObject();
  QCOMPARE(related.value("status").toString(), QStringLiteral("ai_tool_spec_only"));
  QVERIFY(related.value("capability_id").isNull());
  QVERIFY(CapabilityRegistry::aliasCollisions().isEmpty());
  // coverage 完整（默认节在预算内）。
  QVERIFY(payloadJson(query).value("coverage").toObject().value("complete").toBool());

  // 自反：目录也能描述自己（纯工具，不递归执行）。
  const auto self = inspect({{"mode", "describe"}, {"target_kind", "tool"},
                             {"target_id", "paleo.capability_inspect"},
                             {"sections", QJsonArray{QStringLiteral("identity")}}});
  QVERIFY(self.ok);
  QCOMPARE(payloadJson(self).value("sections").toObject().value("identity").toObject()
               .value("id").toString(), QStringLiteral("paleo.capability_inspect"));

  // AiToolSpec-only 反例：read_result 无任何 CapabilitySpec/别名关系。
  const auto read = inspect({{"mode", "describe"}, {"target_kind", "tool"},
                             {"target_id", "paleo.read_result"},
                             {"sections", QJsonArray{QStringLiteral("related")}}});
  QVERIFY(read.ok);
  const auto readRelated = payloadJson(read).value("sections").toObject()
                               .value("related").toObject();
  QCOMPARE(readRelated.value("status").toString(), QStringLiteral("ai_tool_spec_only"));
  QVERIFY(readRelated.value("capability_id").isNull());
}

void TestI5CapabilityInspection::counterExampleContoursCallableWithoutDedicatedTool() {
  // 反例 1：singlefactor.contours Callable 且有 Agent command surface，
  // 但没有独立 AiToolSpec alias——工具关联必须为空，命令关联必须真实。
  const auto contours = inspect({{"mode", "describe"}, {"target_kind", "capability"},
                                 {"target_id", "singlefactor.contours"}});
  QVERIFY(contours.ok);
  const auto related = payloadJson(contours).value("sections").toObject()
                           .value("related").toObject();
  QCOMPARE(related.value("tools").toArray().size(), 0);
  const auto commands = related.value("commands").toArray();
  bool sawAgentSurface = false;
  for (const auto &value : commands) {
    const auto command = value.toObject();
    QCOMPARE(command.value("id").toString(), QStringLiteral("singlefactor.contours"));
    sawAgentSurface = sawAgentSurface || command.value("agent_surface").toBool();
  }
  QVERIFY(sawAgentSurface);
}

void TestI5CapabilityInspection::counterExampleWellFaciesLegacyAdapterWithDedicatedTool() {
  // 反例 2：wellfacies.predict 是 typed-v2 LegacyAdapter，实际工具走专用生产路径。
  const auto facies = inspect({{"mode", "describe"}, {"target_kind", "capability"},
                               {"target_id", "wellfacies.predict"}});
  QVERIFY(facies.ok);
  const auto sections = payloadJson(facies).value("sections").toObject();
  QCOMPARE(sections.value("identity").toObject().value("exposure").toString(),
           QStringLiteral("legacy-adapter"));
  QCOMPARE(sections.value("identity").toObject().value("contract_version").toInt(), 2);
  const auto related = sections.value("related").toObject();
  const auto tools = related.value("tools").toArray();
  QCOMPARE(tools.size(), 1);
  QCOMPARE(tools.first().toString(), QStringLiteral("paleo.well_facies_prediction"));
  // 反向：工具 describe 关联到该 capability（alias 关系，非标题推断）。
  const auto tool = inspect({{"mode", "describe"}, {"target_kind", "tool"},
                             {"target_id", "paleo.well_facies_prediction"}});
  QVERIFY(tool.ok);
  const auto toolRelated = payloadJson(tool).value("sections").toObject()
                               .value("related").toObject();
  QCOMPARE(toolRelated.value("status").toString(), QStringLiteral("registered"));
  QCOMPARE(toolRelated.value("capability_id").toString(), QStringLiteral("wellfacies.predict"));
}

void TestI5CapabilityInspection::viewCommandsAreDeclarationFactsOnly() {
  // UI View/普通 QAction：仅静态声明，不开放执行、不触发 UI。
  const auto commands = paleo::command::CommandRegistry::all();
  QString viewId;
  for (const auto &command : commands)
    if (command.kind == paleo::command::CommandKind::View) {
      viewId = command.commandId;
      break;
    }
  QVERIFY(!viewId.isEmpty());
  const auto view = inspect({{"mode", "describe"}, {"target_kind", "command"},
                             {"target_id", viewId}});
  QVERIFY(view.ok);
  const auto sections = payloadJson(view).value("sections").toObject();
  QCOMPARE(sections.value("related").toObject().value("rule").toString(),
           QStringLiteral("view commands never reference capabilities"));
  QVERIFY(!sections.value("related").toObject().contains(QStringLiteral("identity")));
  // Agent surface 事实按注册表呈现（View 恒 false——validate() 钉住）。
  QVERIFY(!sections.value("contract").toObject().value("agent_surface").toBool());
}

void TestI5CapabilityInspection::checkToolCandidateReturnsRealValidatorIssues() {
  paleo::agent::InspectionHostContext host = readOnlyHost();
  // 缺必填：query_project 需要 topic。
  const auto missing = inspect(toolCheckArgs("paleo.query_project", QJsonObject{}), host);
  QVERIFY(missing.ok); // 查询成功；结论分层
  const auto check = payloadJson(missing).value("check").toObject();
  QCOMPARE(check.value("route").toObject().value("status").toString(),
           QStringLiteral("routed_arguments_invalid"));
  QCOMPARE(check.value("parameterAssessment").toObject().value("status").toString(),
           QStringLiteral("invalid"));
  const auto issues = check.value("parameterAssessment").toObject().value("issues").toArray();
  QVERIFY(!issues.isEmpty());
  QCOMPARE(issues.first().toObject().value("validator").toString(),
           QStringLiteral("AiToolSpec.validateParameters"));

  // 合法候选：topic=wells + 分页参数原生数值。
  const QJsonObject validArgs{{"mode", "check"},
      {"candidate", QJsonObject{{"tool", "paleo.query_project"},
                                {"arguments", QJsonObject{{"topic", "wells"},
                                                          {"page_size", 10}}}}}};
  const auto valid = inspect(validArgs, host);
  QVERIFY(valid.ok);
  const auto validCheck = payloadJson(valid).value("check").toObject();
  QCOMPARE(validCheck.value("route").toObject().value("status").toString(),
           QStringLiteral("routed"));
  QCOMPARE(validCheck.value("parameterAssessment").toObject().value("status").toString(),
           QStringLiteral("valid"));
  // 只读工程 + Read 效果 → policy allowed；runtime 恒 not_checked（并存不折算）。
  QCOMPARE(validCheck.value("policyAssessment").toObject().value("status").toString(),
           QStringLiteral("allowed"));
  QCOMPARE(validCheck.value("runtimeAssessment").toObject().value("status").toString(),
           QStringLiteral("not_checked"));
  QCOMPARE(validCheck.value("no_execution_ticket").toBool(), true);
  QVERIFY(check.contains(QStringLiteral("meaning")));
}

void TestI5CapabilityInspection::checkToolCandidateStrictRepresentation() {
  paleo::agent::InspectionHostContext host = readOnlyHost();
  // "5" 文本整数（lineage 的 upstream_depth，无主题域拦截）：严格外部档拒绝
  // （invalid_integer_representation + 字段定位）。
  const auto textInteger = inspect(toolCheckArgs("paleo.asset_lineage",
      QJsonObject{{"asset", "any"}, {"upstream_depth", QStringLiteral("5")}}), host);
  QVERIFY(textInteger.ok);
  const auto issues = payloadJson(textInteger).value("check").toObject()
                          .value("parameterAssessment").toObject().value("issues").toArray();
  bool sawStrict = false;
  for (const auto &value : issues)
    if (value.toObject().value("validator").toString() ==
        QLatin1String("AiToolSpec.validateExternalArguments")) {
      sawStrict = true;
      QCOMPARE(value.toObject().value("code").toString(),
               QStringLiteral("invalid_integer_representation"));
      QCOMPARE(value.toObject().value("field").toString(), QStringLiteral("upstream_depth"));
    }
  QVERIFY(sawStrict);
  // query_project 的主题预算域（projectquerytopics 单源）先于通用整数档：
  // page_size 文本 "16" 命中 query_budget_invalid（原验证器真实行为）。
  const auto budget = inspect(toolCheckArgs("paleo.query_project",
      QJsonObject{{"topic", "wells"}, {"page_size", QStringLiteral("16")}}), host);
  QVERIFY(budget.ok);
  const auto budgetIssues = payloadJson(budget).value("check").toObject()
                                .value("parameterAssessment").toObject()
                                .value("issues").toArray();
  bool sawBudget = false;
  for (const auto &value : budgetIssues)
    if (value.toObject().value("code").toString() == QLatin1String("query_budget_invalid")) {
      sawBudget = true;
      QCOMPARE(value.toObject().value("field").toString(), QStringLiteral("page_size"));
    }
  QVERIFY(sawBudget);
  // 越界：lineage 的 upstream_depth 声明域 0–12（boundedParameter 进 schema、
  // 严格外部档拒绝）。
  const auto range = inspect(toolCheckArgs("paleo.asset_lineage", QJsonObject{{"asset", "any"},
                                                          {"upstream_depth", 99}}), host);
  QVERIFY(range.ok);
  const auto rangeIssues = payloadJson(range).value("check").toObject()
                               .value("parameterAssessment").toObject()
                               .value("issues").toArray();
  bool sawRange = false;
  for (const auto &value : rangeIssues)
    if (value.toObject().value("code").toString() == QLatin1String("argument_out_of_range")) {
      sawRange = true;
      QCOMPARE(value.toObject().value("field").toString(), QStringLiteral("upstream_depth"));
    }
  QVERIFY(sawRange);
  // 枚举外取值：topic=nope。
  const auto enumMiss = inspect(toolCheckArgs("paleo.query_project", QJsonObject{{"topic", "nope"}}), host);
  QVERIFY(enumMiss.ok);
  QCOMPARE(payloadJson(enumMiss).value("check").toObject().value("parameterAssessment")
               .toObject().value("status").toString(), QStringLiteral("invalid"));
}

void TestI5CapabilityInspection::checkAlgorithmCandidateReturnsContractIssuesAndDefaults() {
  // 缺必填：depth_batch_convert 需要 TIME_SEMANTIC 等——与 tst_algorithm_contract
  // 同一 oracle（missing_required）。
  const auto missing = inspect(algorithmCheckArgs("native:depth_batch_convert", 1, QJsonObject{}));
  QVERIFY(missing.ok);
  const auto check = payloadJson(missing).value("check").toObject();
  QCOMPARE(check.value("parameterAssessment").toObject().value("status").toString(),
           QStringLiteral("invalid"));
  QCOMPARE(check.value("parameterAssessment").toObject().value("code").toString(),
           QStringLiteral("missing_required"));
  const auto issues = check.value("parameterAssessment").toObject().value("issues").toArray();
  QVERIFY(!issues.isEmpty());
  QCOMPARE(issues.first().toObject().value("field").toString(), QStringLiteral("TIME_SEMANTIC"));

  // 合法参数集：effective 只含 AlgorithmSpec 真实默认值（TEST 与
  // tst_algorithm_contract 的 ok 分支同参）。
  const auto valid = inspect(algorithmCheckArgs("native:depth_batch_convert", 1, QJsonObject{
                                    {"TIME_SEMANTIC", "TWT"},
                                    {"DEPTH_SEMANTIC", "TVD"},
                                    {"EXTRAPOLATION", "reject_outside"},
                                    {"MODEL_REVISION", "m1"}}));
  QVERIFY(valid.ok);
  const auto validCheck = payloadJson(valid).value("check").toObject();
  QCOMPARE(validCheck.value("parameterAssessment").toObject().value("status").toString(),
           QStringLiteral("valid"));
  const auto effective = validCheck.value("parameterAssessment").toObject()
                             .value("effective_parameters").toObject();
  QCOMPARE(effective.value("TIME_SEMANTIC").toString(), QStringLiteral("TWT"));
  QCOMPARE(validCheck.value("parameterAssessment").toObject().value("defaults_source").toString(),
           QStringLiteral("AlgorithmSpec"));
  // 宿主注入/隐藏输入明确 not_checked（无占位值）。
  QCOMPARE(validCheck.value("dynamicInputs").toObject().value("status").toString(),
           QStringLiteral("not_checked"));
  QVERIFY(!validCheck.value("dynamicInputs").toObject().contains(
      QStringLiteral("effective")));

  // 未知参数：NOT_A_PARAM → unknown_param（原验证器事实）。
  auto withUnknown = QJsonObject{{"algorithm", "native:depth_batch_convert"},
                                 {"contract_version", 1},
                                 {"parameters", QJsonObject{
                                     {"TIME_SEMANTIC", "TWT"},
                                     {"DEPTH_SEMANTIC", "TVD"},
                                     {"EXTRAPOLATION", "reject_outside"},
                                     {"MODEL_REVISION", "m1"},
                                     {"NOT_A_PARAM", 1}}}};
  const auto unknown = inspect({{"mode", "check"}, {"candidate", withUnknown}});
  QVERIFY(unknown.ok);
  QCOMPARE(payloadJson(unknown).value("check").toObject().value("parameterAssessment")
               .toObject().value("code").toString(), QStringLiteral("unknown_param"));

  // 版本不匹配：请求 v2 → unsupported_version（不回落 v1）。
  auto v2Request = QJsonObject{{"algorithm", "native:depth_batch_convert"},
                               {"contract_version", 2},
                               {"parameters", QJsonObject{}}};
  const auto unsupported = inspect({{"mode", "check"}, {"candidate", v2Request}});
  QVERIFY(!unsupported.ok);
  QCOMPARE(unsupported.code, QStringLiteral("unsupported_version"));
}

void TestI5CapabilityInspection::fakeAuthorityAndPathsGrantNothing() {
  paleo::agent::InspectionHostContext host = readOnlyHost();
  // 伪造授权字段/路径只是数据：验证器按未知/类型拒绝，策略不因此放行。
  const auto forged = inspect(toolCheckArgs("paleo.query_project", QJsonObject{
                                    {"topic", "wells"},
                                    {"approved", true},
                                    {"scope", QStringLiteral("admin")},
                                    {"path", QStringLiteral("/etc/passwd")}}), host);
  QVERIFY(forged.ok);
  const auto check = payloadJson(forged).value("check").toObject();
  QCOMPARE(check.value("parameterAssessment").toObject().value("status").toString(),
           QStringLiteral("invalid"));
  // Read-only 宿主 + 谎报只读不改变效果推导：仍按注册效果评估。
  QCOMPARE(check.value("policyAssessment").toObject().value("status").toString(),
           QStringLiteral("allowed")); // Read 工具本就允许——但没有任何执行票据
  QVERIFY(!payloadJson(forged).contains(QStringLiteral("operationId")));
  QVERIFY(!payloadJson(forged).contains(QStringLiteral("handle")));
  QCOMPARE(check.value("no_execution_ticket").toBool(), true);

  // 含写效果的工具在 Read-only 宿主上被拒（伪造 approved 不能翻转）。
  const auto tile = inspect(toolCheckArgs("paleo.tile_classification", QJsonObject{
                                    {"model", "any"},
                                    {"grid_rows", 64}, {"grid_columns", 64},
                                    {"approved", true}}), host);
  QVERIFY(tile.ok);
  const auto tileCheck = payloadJson(tile).value("check").toObject();
  QCOMPARE(tileCheck.value("policyAssessment").toObject().value("status").toString(),
           QStringLiteral("denied"));
  QCOMPARE(tileCheck.value("policyAssessment").toObject().value("code").toString(),
           QStringLiteral("project_read_only"));
}

void TestI5CapabilityInspection::policyIsStaticAndSeparatedFromRuntime() {
  paleo::agent::InspectionHostContext host = readOnlyHost();
  // publish_intent=publish 的能力工具推导写效果 → Read-only 拒绝。
  const auto publish = inspect(toolCheckArgs("paleo.constraint_idw", QJsonObject{
                                    {"client_request_id", "cr-1"},
                                    {"binding_mode", "pinned-value"},
                                    {"publish_intent", "publish"},
                                    {"parameters", QJsonObject{}},
                                    {"inputs", QJsonObject{}}}), host);
  QVERIFY(publish.ok);
  const auto publishCheck = payloadJson(publish).value("check").toObject();
  QCOMPARE(publishCheck.value("policyAssessment").toObject().value("status").toString(),
           QStringLiteral("denied"));
  QCOMPARE(publishCheck.value("policyAssessment").toObject().value("code").toString(),
           QStringLiteral("project_read_only"));
  QCOMPARE(publishCheck.value("runtimeAssessment").toObject().value("status").toString(),
           QStringLiteral("not_checked"));

  // 同一候选 compute-only：效果推导为 Read|PrivateStage → 仍拒（PrivateStage
  // 属工程写族）；invalid intent → undetermined。
  auto computeOnly = QJsonObject{{"tool", "paleo.constraint_idw"},
                                 {"arguments", QJsonObject{
                                     {"client_request_id", "cr-2"},
                                     {"binding_mode", "pinned-value"},
                                     {"publish_intent", "compute-only"},
                                     {"parameters", QJsonObject{}},
                                     {"inputs", QJsonObject{}}}}};
  const auto compute = inspect({{"mode", "check"}, {"candidate", computeOnly}}, host);
  QVERIFY(compute.ok);
  QCOMPARE(payloadJson(compute).value("check").toObject().value("policyAssessment")
               .toObject().value("status").toString(), QStringLiteral("denied"));

  // epoch 漂移：期待身份与宿主不符 → stale_session（与 runner 同口径）。
  auto drifted = readOnlyHost();
  drifted.expectedEpoch = 8;
  const auto stale = inspect(toolCheckArgs(QStringLiteral("paleo.query_project"),
                                           QJsonObject{{"topic", "wells"}}), drifted);
  QVERIFY(stale.ok);
  QCOMPARE(payloadJson(stale).value("check").toObject().value("policyAssessment")
               .toObject().value("code").toString(), QStringLiteral("stale_session"));
}

void TestI5CapabilityInspection::missingHostKeepsStaticContractAndMarksPolicyUnavailable() {
  // 无 Host/未开工程：公开静态合同照常可读，policy 不伪造。
  const auto describe = inspect({{"mode", "describe"}, {"target_kind", "capability"},
                                 {"target_id", "constraint.idw"}});
  QVERIFY(describe.ok);
  const auto check = inspect(toolCheckArgs(QStringLiteral("paleo.query_project"),
                                           QJsonObject{{"topic", "wells"}}));
  QVERIFY(check.ok);
  const auto policy = payloadJson(check).value("check").toObject()
                          .value("policyAssessment").toObject();
  QCOMPARE(policy.value("status").toString(), QStringLiteral("unavailable"));
  QCOMPARE(policy.value("code").toString(), QStringLiteral("host_unavailable"));
  QCOMPARE(payloadJson(check).value("check").toObject().value("runtimeAssessment")
               .toObject().value("status").toString(), QStringLiteral("not_checked"));
}

void TestI5CapabilityInspection::describeBudgetDropsWholeSectionsWithCoverage() {
  // 缩紧预算：整节省略 + coverage 如实；指定节可续读（不截半个 schema）。
  const auto squeezed = inspect({{"mode", "describe"}, {"target_kind", "algorithm"},
                                 {"target_id", "paleo:paleo_constraint_idw"},
                                 {"max_bytes", 2048}});
  QVERIFY(squeezed.ok);
  const auto coverage = payloadJson(squeezed).value("coverage").toObject();
  const auto omitted = coverage.value("omittedSections").toArray();
  QVERIFY(!omitted.isEmpty());
  QCOMPARE(coverage.value("complete").toBool(), false);
  QVERIFY(!coverage.value("continuation").toString().isEmpty());
  const auto sections = payloadJson(squeezed).value("sections").toObject();
  for (const auto &value : omitted)
    QVERIFY(!sections.contains(value.toString()));
  // 补读被省的节：单独请求可在预算内取回（同一合同可重组）。
  const auto refetch = inspect({{"mode", "describe"}, {"target_kind", "algorithm"},
                                {"target_id", "paleo:paleo_constraint_idw"},
                                {"sections", QJsonArray{omitted.first().toString()}}});
  QVERIFY(refetch.ok);
  QVERIFY(payloadJson(refetch).value("sections").toObject()
              .contains(omitted.first().toString()));
  QVERIFY(payloadJson(refetch).value("coverage").toObject().value("complete").toBool());

  // F1（评审修复）：单个节自身超预算同样省略——不超限交付、不谎报 complete。
  const auto single = inspect({{"mode", "describe"}, {"target_kind", "tool"},
                               {"target_id", "paleo.query_project"},
                               {"sections", QJsonArray{QStringLiteral("contract")}},
                               {"max_bytes", 2048}});
  QVERIFY(single.ok);
  const auto singleCoverage = payloadJson(single).value("coverage").toObject();
  QCOMPARE(singleCoverage.value("complete").toBool(), false);
  QVERIFY(singleCoverage.value("omittedSections").toArray().contains(
      QStringLiteral("declaration")));
  QVERIFY(!payloadJson(single).value("sections").toObject().contains(
      QStringLiteral("declaration")));
}

void TestI5CapabilityInspection::cursorAndSectionGuardsAreStrict() {
  // F3（评审修复）：游标 offset 非原生整数（文本 "1"）→ cursor_stale，
  // 不被 toInt 静默解释成第 1 页。
  const auto page = inspect({{"mode", "list"}, {"page_size", 5}});
  QVERIFY(page.ok);
  const QString cursor = payloadJson(page).value("nextCursor").toString();
  QVERIFY(!cursor.isEmpty());
  const auto forged = QByteArray::fromBase64(cursor.toUtf8(),
      QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
  auto document = QJsonDocument::fromJson(forged);
  auto object = document.object();
  object.insert(QStringLiteral("offset"), QStringLiteral("1")); // 伪造文本 offset
  const auto reforged = QString::fromLatin1(QJsonDocument(object).toJson(QJsonDocument::Compact)
      .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
  const auto staleOffset = inspect({{"mode", "list"}, {"page_size", 5},
                                    {"cursor", reforged}});
  QVERIFY(!staleOffset.ok);
  QCOMPARE(staleOffset.code, QStringLiteral("cursor_stale"));

  // F4（评审修复）：目标类型不适用的节名 → invalid_section（不静默缺失）。
  const auto toolParams = inspect({{"mode", "describe"}, {"target_kind", "tool"},
                                   {"target_id", "paleo.query_project"},
                                   {"sections", QJsonArray{QStringLiteral("params")}}});
  QVERIFY(!toolParams.ok);
  QCOMPARE(toolParams.code, QStringLiteral("invalid_section"));
  QCOMPARE(toolParams.field, QStringLiteral("params"));
  const auto commandRoute = inspect({{"mode", "describe"}, {"target_kind", "command"},
                                     {"target_id", "view.open.data"},
                                     {"sections", QJsonArray{QStringLiteral("route")}}});
  QVERIFY(!commandRoute.ok);
  QCOMPARE(commandRoute.code, QStringLiteral("invalid_section"));
  // capability 的 params 节按请求名交付（含来源 AlgorithmSpec 身份）。
  const auto capabilityParams = inspect({{"mode", "describe"}, {"target_kind", "capability"},
                                         {"target_id", "constraint.idw"},
                                         {"sections", QJsonArray{QStringLiteral("params")}}});
  QVERIFY(capabilityParams.ok);
  const auto params = payloadJson(capabilityParams).value("sections").toObject()
                          .value("params").toObject();
  QCOMPARE(params.value("source").toString(),
           QStringLiteral("linked AlgorithmSpec at the capability's algorithm_contract_version"));
  QVERIFY(params.value("fields").toArray().size() >= 1);
  QVERIFY(payloadJson(capabilityParams).value("coverage").toObject()
              .value("deliveredSections").toArray().contains(QStringLiteral("params")));
}

void TestI5CapabilityInspection::topLevelClosedFieldSet() {
  // 顶层未知字段（授权语义）在服务档即拒（防御深度；dispatch 档另有覆盖）。
  const auto forged = inspect({{"mode", "list"}, {"approved", true}});
  QVERIFY(!forged.ok);
  QCOMPARE(forged.code, QStringLiteral("unknown_field"));
  QCOMPARE(forged.field, QStringLiteral("approved"));
}

void TestI5CapabilityInspection::unknownModesAndTargetsAreMachineErrors() {
  QCOMPARE(inspect({{"mode", "execute"}}).code, QStringLiteral("invalid_mode"));
  QCOMPARE(inspect({}).code, QStringLiteral("missing_required"));
  QCOMPARE(inspect({{"mode", "describe"}, {"target_kind", "tool"},
                    {"target_id", "paleo.not_a_tool"}}).code,
           QStringLiteral("unknown_target"));
  QCOMPARE(inspect({{"mode", "describe"}, {"target_kind", "banana"}, {"target_id", "x"}}).code,
           QStringLiteral("invalid_target_kind"));
  QCOMPARE(inspect({{"mode", "describe"}}).code, QStringLiteral("missing_required"));
  QCOMPARE(inspect({{"mode", "check"}}).code, QStringLiteral("missing_candidate"));
  QCOMPARE(inspect({{"mode", "check"}, {"candidate", QJsonObject{{"tool", "x"},
                                                                 {"algorithm", "y"}}}}).code,
           QStringLiteral("ambiguous_candidate"));
  QCOMPARE(inspect({{"mode", "list"}, {"filter", "everything"}}).code,
           QStringLiteral("invalid_filter"));
  // 候选含超深嵌套 → 形状预算拒绝。
  QJsonObject deep;
  for (int i = 0; i < 12; ++i)
    deep = QJsonObject{{"nested", deep}};
  QCOMPARE(inspect({{"mode", "check"},
                    {"candidate", QJsonObject{{"tool", "paleo.query_project"},
                                              {"arguments", deep}}}}).code,
           QStringLiteral("candidate_budget_exceeded"));
}

QTEST_MAIN(TestI5CapabilityInspection)
#include "tst_i5_capabilityinspection.moc"
