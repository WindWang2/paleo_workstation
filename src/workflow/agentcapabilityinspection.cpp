// 层：功能
// workflow/agentcapabilityinspection.cpp — 见 agentcapabilityinspection.h。
// list/describe/check 三模式的纯函数实现：注册表快照投影 + 既有纯验证器
// （AiToolSpec 校验 / validateAndFillParams / checkContext）复用，零业务副作用。
#include "agentcapabilityinspection.h"
#include "agentoperationdirectory.h"

#include "../ai/chat/capabilityinspectiontool.h"
#include "../ai/chat/domaintools.h"
#include "../ai/chat/geostatictools.h"
#include "../services/algorithmcontract.h"
#include "../services/algorithmspec.h"
#include "../services/capabilitydescription.h"
#include "../services/capabilityspec.h"
#include "../services/commandspec.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>

namespace paleo::agent {
namespace {

// —— 有界输入（防御深度；总实参仍受 ChatToolCall 16 KiB 既有预算约束）——
constexpr int kMaxTargetIdLength = 128;
constexpr int kMaxSections = 16;
constexpr int kMaxCursorBytes = 512;
constexpr int kMaxCandidateDepth = 8;
constexpr int kMaxCandidateKeys = 64;
constexpr int kMaxCandidateArrayItems = 512;
constexpr int kMaxCandidateStringChars = 4096;
constexpr int kDefaultPageSize = 16;
constexpr int kMaxPageSize = 32;
constexpr int kMinPageBytes = 2048;
// F6（评审）：与传输层内联页上限对齐（toolresultprotocol kResultPageBytes）；
// 超过该值的请求不会内联送达（envelope 转 result_referenced）——声明上限
// 不放大到 24 KiB，大结果经 H7 read_result 引用续读。
constexpr int kMaxPageBytes = 8192;
constexpr int kDefaultPageBytes = 8192;
// F1（评审）：payload 外层固定开销余量（tool/mode/code/coverage.continuation
// 等键 + target_id 最长 128 字符 UTF-8）——测量值 + 余量须落在预算内。
constexpr int kEnvelopeMarginBytes = 512;
constexpr int kMaxIssues = 16;
constexpr int kMaxContractVersion = 99;

// JSON 原生整数（与严格外部档同口径：非 double / 非有限 / 非整值一律无效）。
bool strictIntArg(const QJsonObject &args, const char *key, int minimum, int maximum,
                  int fallback, int *value) {
  *value = fallback;
  if (!args.contains(QLatin1String(key)))
    return true;
  const QJsonValue raw = args.value(QLatin1String(key));
  if (!raw.isDouble())
    return false;
  const double number = raw.toDouble();
  if (number != std::floor(number) || number < minimum || number > maximum)
    return false;
  *value = static_cast<int>(number);
  return true;
}

int compactBytes(const QJsonObject &value) {
  return QJsonDocument(value).toJson(QJsonDocument::Compact).size();
}

// 候选实参的形状预算（与 H5 planpreview 同族防御：不改既有 16 KiB 总预算）。
bool candidateShapeWithinBudget(const QJsonValue &value, int depth) {
  if (depth > kMaxCandidateDepth)
    return false;
  if (value.isString())
    return value.toString().size() <= kMaxCandidateStringChars;
  if (value.isArray()) {
    const auto array = value.toArray();
    if (array.size() > kMaxCandidateArrayItems)
      return false;
    for (const auto &item : array)
      if (!candidateShapeWithinBudget(item, depth + 1))
        return false;
    return true;
  }
  if (value.isObject()) {
    const auto object = value.toObject();
    if (object.size() > kMaxCandidateKeys)
      return false;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
      if (!candidateShapeWithinBudget(it.value(), depth + 1))
        return false;
    return true;
  }
  return true;
}

QVariantMap jsonToVariantMap(const QJsonObject &object) {
  QVariantMap out;
  for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
    const QJsonValue value = it.value();
    if (value.isDouble())
      out.insert(it.key(), value.toDouble());
    else if (value.isString())
      out.insert(it.key(), value.toString());
    else if (value.isBool())
      out.insert(it.key(), value.toBool());
    else if (value.isArray())
      out.insert(it.key(), value.toArray().toVariantList());
    else if (value.isObject())
      out.insert(it.key(), jsonToVariantMap(value.toObject()));
  }
  return out;
}

InspectionResult failure(const QString &code, const QString &field = {}) {
  InspectionResult out;
  out.code = code;
  out.field = field;
  QJsonObject payload{{"contract", QLatin1String(kCapabilityInspectContract)},
                      {"tool", QLatin1String(paleo::chat::kCapabilityInspectTool)},
                      {"ok", false},
                      {"code", code}};
  if (!field.isEmpty())
    payload.insert(QStringLiteral("field"), field);
  out.payload = payload;
  return out;
}

QString directoryDigest(const QVector<DirectoryEntry> &entries, const QString &filter) {
  QByteArray basis;
  for (const DirectoryEntry &entry : entries)
    basis += entry.id.toUtf8() + ':' + QByteArray::number(entry.version) + ';';
  basis += filter.toUtf8();
  return QString::fromLatin1(
      QCryptographicHash::hash(basis, QCryptographicHash::Sha256).toHex().left(16));
}

// 游标绑定查询身份（filter + 全序摘要 + offset）；无密钥——本目录是全局
// 静态注册内容，伪造游标最多换来同一公开投影的另一页，不构成任何权限。
bool decodeListCursor(const QString &cursor, const QString &filter,
                      const QVector<DirectoryEntry> &entries, const QString &digestKey, int *offset) {
  if (cursor.toUtf8().size() > kMaxCursorBytes)
    return false;
  const auto payload = QByteArray::fromBase64(cursor.toUtf8(),
      QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
  const auto document = QJsonDocument::fromJson(payload);
  if (!document.isObject())
    return false;
  const auto object = document.object();
  if (object.value("v").toInt() != 1 ||
      object.value("filter").toString() != filter ||
      object.value("d").toString() != directoryDigest(entries, digestKey))
    return false;
  // F3（评审）：offset 严格整数解码——文本/分数/布尔不被 QJsonValue::toInt
  // 静默解释成 0（与 strictIntArg 同口径，伪造游标按 cursor_stale 拒）。
  const auto offsetValue = object.value("offset");
  if (!offsetValue.isDouble() || !std::isfinite(offsetValue.toDouble()) ||
      std::floor(offsetValue.toDouble()) != offsetValue.toDouble())
    return false;
  const int position = int(offsetValue.toDouble());
  if (position < 0 || position > entries.size())
    return false;
  *offset = position;
  return true;
}

QString encodeListCursor(const QString &filter, const QVector<DirectoryEntry> &entries,
                         const QString &digestKey, int offset) {
  const QJsonObject object{{"v", 1},
                           {"filter", filter},
                           {"d", directoryDigest(entries, digestKey)},
                           {"offset", offset}};
  return QString::fromLatin1(QJsonDocument(object).toJson(QJsonDocument::Compact).toBase64(
      QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QStringList registeredCapabilityVersions(const QVector<CapabilitySpec> &table,
                                         const QString &capabilityId) {
  QStringList versions;
  for (const CapabilitySpec &spec : table)
    if (spec.capabilityId == capabilityId &&
        !versions.contains(QString::number(spec.contractVersion)))
      versions.append(QString::number(spec.contractVersion));
  return versions;
}

// 「当前登记版本」= 该 id 的最高 contractVersion（wellfacies.predict 只有 v2，
// spec(id,1) 会得到空条目——与 describe 的 0=当前语义同源）。
CapabilitySpec capabilityAtCurrentVersion(const QString &capabilityId) {
  const QVector<CapabilitySpec> table = CapabilityRegistry::all();
  const CapabilitySpec *best = nullptr;
  for (const CapabilitySpec &spec : table)
    if (spec.capabilityId == capabilityId &&
        (!best || spec.contractVersion > best->contractVersion))
      best = &spec;
  return best ? *best : CapabilitySpec{};
}

// 关联工具：以注册表声明的 alias/ID 关系为唯一依据（不按标题/前缀推断）。
QStringList toolsLinkedToCapability(const QString &capabilityId) {
  QStringList names;
  for (const AiToolSpec &spec : builtinAiToolSpecs()) {
    QString code;
    const QString resolved = CapabilityRegistry::resolveAlias(spec.name, &code);
    if (code.isEmpty() && resolved == capabilityId)
      names.append(spec.name);
  }
  return names;
}

QJsonObject describeToolDeclaration(const AiToolSpec &spec) {
  QJsonObject declaration;
  declaration.insert(QStringLiteral("parameters_schema"), spec.toJsonSchema());
  declaration.insert(QStringLiteral("native_result_schema"),
                     QJsonDocument::fromJson(spec.resultSchemaJson.toUtf8()).object());
  declaration.insert(QStringLiteral("model_envelope_schema"), spec.modelResultSchema());
  declaration.insert(QStringLiteral("schema_source"),
                     QStringLiteral("AiToolSpec::toJsonSchema/modelResultSchema"));
  return declaration;
}

// 静态路由事实：dispatchAiTool 的 NotFound 之外都说明存在静态路由分支；
// 空实参触发的 Disabled（缺必填）在这里只证明「有路由」，不是参数结论——
// 该早退分支不填 target，如实留空并指引 check 携真实实参取 target。
QJsonObject describeToolRoute(const QString &name) {
  const AiToolDispatch dispatch = dispatchAiTool(name, QJsonObject{});
  QJsonObject route;
  const bool registered = dispatch.status != AiToolDispatchStatus::NotFound;
  route.insert(QStringLiteral("status"), registered
      ? QStringLiteral("static_route_registered")
      : QStringLiteral("declaration_only"));
  if (!dispatch.target.isEmpty())
    route.insert(QStringLiteral("target"), dispatch.target);
  else if (registered)
    route.insert(QStringLiteral("target_note"),
        QStringLiteral("empty probe arguments hit the validation early-return; use check with real arguments for the route target"));
  route.insert(QStringLiteral("argument_validation"), QStringLiteral("per_call_or_check"));
  route.insert(QStringLiteral("turn_negotiation"),
      QStringLiteral("directory presence does not add this tool to the current turn's frozen tool table"));
  return route;
}

QJsonObject describeToolEffects(const AiToolSpec &spec) {
  unsigned effects = spec.effects;
  // 无实参的 describe 不能把按 publish_intent 派生写效果的工具报成只读。
  // 与 effectsForArguments / computeEffects 同一组相位，取并集。
  if (paleo::geostat_atom::isGeostatAtomTool(spec.name)) {
    effects = 0;
    const QStringList phases{QStringLiteral("prepare"), QStringLiteral("compute-only"),
                             QStringLiteral("publish")};
    for (const QString &intent : phases) {
      effects |= spec.effectsForArguments(
          QJsonObject{{QStringLiteral("publish_intent"), intent}});
    }
  }
  return QJsonObject{{"declared_mask", QString::number(effects)},
                     {"declared_names",
                      QJsonArray::fromStringList(paleo::capability::hostEffectNames(effects))},
                     {"derivation",
                      QStringLiteral("effectsForArguments derives per-call effects from the current parameter rules (e.g. publish_intent)")}};
}

// F4（评审）：capability 的 params/ports 节按请求名单独交付（fields/items
// 顶层平铺，请求名与交付名对齐；默认值单一来源不复制；链接算法缺失时
// 如实报缺口——不因静态元信息不足而修补成看似可调用）。
QJsonObject capabilityParamSection(const CapabilitySpec &spec) {
  const AlgorithmSpec algorithm = AlgorithmSpecRegistry::specAtVersion(
      spec.algorithmId, spec.algorithmContractVersion);
  QJsonObject section{{"source", QStringLiteral("linked AlgorithmSpec at the capability's algorithm_contract_version")},
                      {"algorithm_id", spec.algorithmId}};
  if (algorithm.id.isEmpty()) {
    section.insert(QStringLiteral("status"),
                   AlgorithmSpecRegistry::knows(spec.algorithmId)
                       ? QStringLiteral("algorithm_version_not_registered")
                       : QStringLiteral("algorithm_not_registered"));
    return section;
  }
  const auto description = paleo::capability::algorithmParamsDescription(algorithm);
  section.insert(QStringLiteral("status"), QStringLiteral("registered"));
  section.insert(QStringLiteral("fields"), description.value("fields"));
  section.insert(QStringLiteral("defaults_are_single_source"),
                description.value("defaults_are_single_source"));
  return section;
}
QJsonObject capabilityPortSection(const CapabilitySpec &spec) {
  const AlgorithmSpec algorithm = AlgorithmSpecRegistry::specAtVersion(
      spec.algorithmId, spec.algorithmContractVersion);
  QJsonObject section{{"source", QStringLiteral("linked AlgorithmSpec at the capability's algorithm_contract_version")},
                      {"algorithm_id", spec.algorithmId}};
  if (algorithm.id.isEmpty()) {
    section.insert(QStringLiteral("status"),
                   AlgorithmSpecRegistry::knows(spec.algorithmId)
                       ? QStringLiteral("algorithm_version_not_registered")
                       : QStringLiteral("algorithm_not_registered"));
    return section;
  }
  const auto description = paleo::capability::algorithmPortsDescription(algorithm);
  section.insert(QStringLiteral("status"), QStringLiteral("registered"));
  section.insert(QStringLiteral("inputs"), description.value("inputs"));
  section.insert(QStringLiteral("outputs"), description.value("outputs"));
  return section;
}

QJsonObject capabilityRelatedSection(const CapabilitySpec &spec) {
  QJsonArray commands;
  for (const auto &command : paleo::command::CommandRegistry::all())
    if (command.capabilityId == spec.capabilityId)
      commands.append(paleo::capability::commandSummaryCard(command));
  QJsonArray tools;
  for (const QString &name : toolsLinkedToCapability(spec.capabilityId))
    tools.append(name);
  return QJsonObject{{"commands", commands},
                     {"tools", tools},
                     {"command_required_context_kept_separate", true}};
}

QJsonObject commandCapabilitySection(const paleo::command::CommandSpec &spec) {
  if (spec.capabilityId.isEmpty())
    return QJsonObject{{"rule", QStringLiteral("view commands never reference capabilities")}};
  const CapabilitySpec capability =
      CapabilityRegistry::spec(spec.capabilityId, spec.contractVersion);
  if (capability.capabilityId.isEmpty())
    return QJsonObject{{"capability_id", spec.capabilityId},
                       {"status", QStringLiteral("capability_version_not_registered")}};
  QJsonObject section{{"status", QStringLiteral("registered")}};
  section.insert(QStringLiteral("identity"), paleo::capability::capabilityIdentityDescription(capability));
  return section;
}

QJsonObject algorithmRelatedSection(const AlgorithmSpec &spec) {
  QJsonArray capabilities;
  QStringList capabilityIds;
  for (const CapabilitySpec &capability : CapabilityRegistry::all())
    if (capability.algorithmId == spec.id) {
      capabilities.append(paleo::capability::capabilitySummaryCard(capability));
      capabilityIds.append(capability.capabilityId);
    }
  QJsonArray tools;
  for (const QString &capabilityId : capabilityIds)
    for (const QString &name : toolsLinkedToCapability(capabilityId))
      tools.append(name);
  QString reason;
  const CapabilityExposure exposure = CapabilityRegistry::exposureForAlgorithm(spec.id, &reason);
  return QJsonObject{{"capabilities", capabilities},
                     {"tools", tools},
                     {"exposure_for_algorithm", capabilityExposureName(exposure)},
                     {"exposure_reason", reason},
                     {"complete_is_not_callable",
                      spec.complete && exposure != CapabilityExposure::Callable}};
}

QJsonObject toolRelatedSection(const AiToolSpec &spec) {
  QString code;
  const QString resolved = CapabilityRegistry::resolveAlias(spec.name, &code);
  QJsonObject section;
  if (code == QLatin1String("alias_collision")) {
    // 多个能力同时声明该名：冲突如实呈现，不猜测归属。
    section.insert(QStringLiteral("status"), QStringLiteral("alias_collision"));
    section.insert(QStringLiteral("code"), code);
    return section;
  }
  if (resolved.isEmpty()) {
    // AiToolSpec-only 条目：没有 CapabilitySpec 就不伪造 capabilityId。
    section.insert(QStringLiteral("status"), QStringLiteral("ai_tool_spec_only"));
    section.insert(QStringLiteral("capability_id"), QJsonValue());
    return section;
  }
  const CapabilitySpec capability = capabilityAtCurrentVersion(resolved);
  section.insert(QStringLiteral("status"), QStringLiteral("registered"));
  section.insert(QStringLiteral("capability_id"), resolved);
  section.insert(QStringLiteral("identity"),
                 paleo::capability::capabilityIdentityDescription(capability));
  return section;
}

// —— check 辅助 ——
QJsonObject checkPolicy(unsigned effects, const InspectionHostContext &host) {
  QJsonObject policy;
  policy.insert(QStringLiteral("effects_mask"), QString::number(effects));
  if (effects == 0) {
    policy.insert(QStringLiteral("status"), QStringLiteral("undetermined"));
    policy.insert(QStringLiteral("code"), QStringLiteral("invalid_publish_intent"));
    return policy;
  }
  if (!host.available) {
    // Host 缺失/关闭：静态合同照常可读，当前策略部分保持 unavailable。
    policy.insert(QStringLiteral("status"), QStringLiteral("unavailable"));
    policy.insert(QStringLiteral("code"), QStringLiteral("host_unavailable"));
    return policy;
  }
  auto expected = host.live;
  expected.documentId = host.expectedDocumentId;
  expected.epoch = host.expectedEpoch;
  const QString denied = paleo::host::checkContext(host.live, expected, effects);
  policy.insert(QStringLiteral("status"), denied.isEmpty() ? QStringLiteral("allowed")
                                                           : QStringLiteral("denied"));
  if (!denied.isEmpty())
    policy.insert(QStringLiteral("code"), denied);
  return policy;
}

QJsonObject runtimeAssessment(bool requiresLocalOrt) {
  QJsonObject runtime;
  runtime.insert(QStringLiteral("status"), QStringLiteral("not_checked"));
  runtime.insert(QStringLiteral("needs"), QStringLiteral("execution_preflight"));
  runtime.insert(QStringLiteral("note"),
      QStringLiteral("dynamic prerequisites (provider/model/selection/frozen inputs/view permit) "
                     "are only checked by the original readiness gates at execution time; "
                     "this inspection never calls them"));
#if !PALEO_HAVE_ORT
  if (requiresLocalOrt)
    runtime.insert(QStringLiteral("build_ort"), QStringLiteral("unavailable"));
#else
  Q_UNUSED(requiresLocalOrt); // 构建含 ORT：该静态事实仅在无 ORT 构建呈现
#endif
  return runtime;
}

QJsonObject checkToolCandidate(const QJsonObject &candidate, const InspectionHostContext &host) {
  const QString name = candidate.value("tool").toString();
  const AiToolSpec spec = aiToolSpec(name);
  if (spec.name.isEmpty())
    return QJsonObject{{"ok", false}, {"code", QStringLiteral("unknown_tool")},
                       {"field", QStringLiteral("candidate.tool")}};
  const QJsonObject arguments = candidate.value("arguments").toObject();

  QJsonObject out;
  out.insert(QStringLiteral("target"), QStringLiteral("tool"));
  out.insert(QStringLiteral("tool"), name);

  QJsonObject declaration;
  declaration.insert(QStringLiteral("status"), QStringLiteral("registered"));
  declaration.insert(QStringLiteral("exposure"), capabilityExposureName(spec.exposure));
  declaration.insert(QStringLiteral("legacy_adapter"), spec.legacyAdapter);
  declaration.insert(QStringLiteral("requires_local_ort"), spec.requiresLocalOrt);
  out.insert(QStringLiteral("declaration"), declaration);

  // 路由评估：用真实候选实参问 dispatch——Disabled 的 note 是「这组实参为何
  // 调用不成」，不是「没有路由」；NotFound 才是没有登记。
  const AiToolDispatch dispatch = dispatchAiTool(name, arguments);
  QJsonObject route;
  route.insert(QStringLiteral("status"), dispatch.status == AiToolDispatchStatus::NotFound
      ? QStringLiteral("not_registered")
      : (dispatch.status == AiToolDispatchStatus::Routed ? QStringLiteral("routed")
                                                         : QStringLiteral("routed_arguments_invalid")));
  if (!dispatch.target.isEmpty())
    route.insert(QStringLiteral("target"), dispatch.target);
  if (!dispatch.note.isEmpty())
    route.insert(QStringLiteral("note"), dispatch.note);
  out.insert(QStringLiteral("route"), route);

  // 参数评估：复用两档既有权威验证器，不手写第三套规则。
  // 候选实参只是数据——其中的 approved/scope/路径不构成任何许可。
  QJsonObject parameters;
  QJsonArray issues;
  const QString shared = spec.validateParameters(arguments);
  if (!shared.isEmpty())
    issues.append(QJsonObject{{"validator", QStringLiteral("AiToolSpec.validateParameters")},
                              {"message", shared}});
  QString strictField;
  const QString strict = spec.validateExternalArguments(arguments, &strictField);
  if (!strict.isEmpty())
    issues.append(QJsonObject{{"validator", QStringLiteral("AiToolSpec.validateExternalArguments")},
                              {"code", strict},
                              {"field", strictField}});
  parameters.insert(QStringLiteral("status"), issues.isEmpty() ? QStringLiteral("valid")
                                                               : QStringLiteral("invalid"));
  if (!issues.isEmpty())
    parameters.insert(QStringLiteral("issues"), issues);
  parameters.insert(QStringLiteral("validates"),
      QStringLiteral("static argument contract only; source refs, paths, model weights, CRS, "
                     "units and input cardinality are resolved by the host at execution"));
  out.insert(QStringLiteral("parameterAssessment"), parameters);

  out.insert(QStringLiteral("policyAssessment"),
             checkPolicy(spec.effectsForArguments(arguments), host));
  out.insert(QStringLiteral("runtimeAssessment"), runtimeAssessment(spec.requiresLocalOrt));
  return out;
}

QJsonObject checkAlgorithmCandidate(const QJsonObject &candidate) {
  const QString id = candidate.value("algorithm").toString();
  QVector<AlgorithmSpec> matches;
  for (const AlgorithmSpec &spec : AlgorithmSpecRegistry::allSpecsProjection())
    if (spec.id == id)
      matches.append(spec);
  if (matches.isEmpty())
    return QJsonObject{{"ok", false}, {"code", QStringLiteral("unknown_algorithm")},
                       {"field", QStringLiteral("candidate.algorithm")}};
  std::sort(matches.begin(), matches.end(),
            [](const AlgorithmSpec &a, const AlgorithmSpec &b) {
              return a.contractVersion < b.contractVersion;
            });
  // 候选版本严格解码：缺席 → 0（当前登记最高版）；在场必须是 JSON 原生
  // 整数（文本 "1"、分数明确拒绝，不静默解释成默认版）。
  int requestedVersion = 0;
  const auto versionValue = candidate.value("contract_version");
  if (!versionValue.isUndefined()) {
    if (!versionValue.isDouble() || !std::isfinite(versionValue.toDouble()) ||
        std::floor(versionValue.toDouble()) != versionValue.toDouble() ||
        versionValue.toDouble() < 0 || versionValue.toDouble() > kMaxContractVersion)
      return QJsonObject{{"ok", false},
                         {"code", QStringLiteral("contract_version_invalid")},
                         {"field", QStringLiteral("candidate.contract_version")}};
    requestedVersion = int(versionValue.toDouble());
  }
  const AlgorithmSpec *spec = nullptr;
  for (const AlgorithmSpec &item : matches)
    if (item.contractVersion == requestedVersion)
      spec = &item;
  if (!spec && requestedVersion == 0)
    spec = &matches.last();
  if (!spec)
    return QJsonObject{{"ok", false}, {"code", QStringLiteral("unsupported_version")},
                       {"field", QStringLiteral("candidate.contract_version")}};
  const QVariantMap requested = jsonToVariantMap(candidate.value("parameters").toObject());

  QJsonObject out;
  out.insert(QStringLiteral("target"), QStringLiteral("algorithm"));
  out.insert(QStringLiteral("algorithm"), id);
  out.insert(QStringLiteral("contract_version"), spec->contractVersion);

  QJsonObject declaration;
  declaration.insert(QStringLiteral("status"), QStringLiteral("registered"));
  declaration.insert(QStringLiteral("complete"), spec->complete);
  QString reason;
  const CapabilityExposure exposure = CapabilityRegistry::exposureForAlgorithm(id, &reason);
  declaration.insert(QStringLiteral("exposure_for_algorithm"), capabilityExposureName(exposure));
  declaration.insert(QStringLiteral("exposure_reason"), reason);
  declaration.insert(QStringLiteral("complete_is_not_callable"),
                     spec->complete && exposure != CapabilityExposure::Callable);
  out.insert(QStringLiteral("declaration"), declaration);

  // 权威纯验证器：effective 只含 AlgorithmSpec 单一来源的真实默认值；issues
  // 是验证器真实返回（有界呈现，不虚构完整错误集合）。
  const auto validated = validateAndFillParams(id, spec->contractVersion, requested);
  QJsonObject parameters;
  parameters.insert(QStringLiteral("status"), validated.ok ? QStringLiteral("valid")
                                                           : QStringLiteral("invalid"));
  if (!validated.code.isEmpty())
    parameters.insert(QStringLiteral("code"), validated.code);
  if (!validated.effective.isEmpty())
    parameters.insert(QStringLiteral("effective_parameters"),
                      QJsonObject::fromVariantMap(validated.effective));
  if (!validated.issues.isEmpty()) {
    QJsonArray issues;
    const int reported = qMin(validated.issues.size(), kMaxIssues);
    for (int index = 0; index < reported; ++index) {
      const ContractIssue &issue = validated.issues.at(index);
      issues.append(QJsonObject{{"code", issue.code},
                                {"field", issue.field},
                                {"message", issue.message}});
    }
    parameters.insert(QStringLiteral("issues"), issues);
    parameters.insert(QStringLiteral("issue_count"), validated.issues.size());
    if (validated.issues.size() > reported)
      parameters.insert(QStringLiteral("issues_truncated"), true);
  }
  parameters.insert(QStringLiteral("defaults_source"), QStringLiteral("AlgorithmSpec"));
  parameters.insert(QStringLiteral("validates"),
      QStringLiteral("scalar parameter contract only; host-injected inputs (hidden ports, "
                     "sampling, source resolution, model weights) remain not_checked"));
  out.insert(QStringLiteral("parameterAssessment"), parameters);

  QJsonObject dynamicInputs;
  dynamicInputs.insert(QStringLiteral("status"), QStringLiteral("not_checked"));
  dynamicInputs.insert(QStringLiteral("hidden_ports"),
                       QJsonArray::fromStringList(spec->hiddenParameterNames()));
  dynamicInputs.insert(QStringLiteral("note"),
      QStringLiteral("host resolve/freeze happens in CapabilityHost::prepare at execution; "
                     "this check never calls it and never fills placeholder values"));
  out.insert(QStringLiteral("dynamicInputs"), dynamicInputs);

  // 算法目标的效果声明在 CapabilitySpec/工具上——没有唯一对应规则就保持
  // 未确定，不把 legacy 字符串转成权限掩码。
  QJsonObject policy;
  policy.insert(QStringLiteral("status"), QStringLiteral("undetermined"));
  policy.insert(QStringLiteral("note"),
      QStringLiteral("effects are declared on CapabilitySpec/AiToolSpec; check the capability or tool target for a policy verdict"));
  out.insert(QStringLiteral("policyAssessment"), policy);
  out.insert(QStringLiteral("runtimeAssessment"), runtimeAssessment(false));
  return out;
}

// —— list / describe / check 主流程 ——
InspectionResult runList(const QJsonObject &args) {
  const QString filter = args.value("filter").toString(QStringLiteral("default"));
  const QStringList filters{QStringLiteral("default"), QStringLiteral("capabilities"),
                            QStringLiteral("algorithms"), QStringLiteral("tools"),
                            QStringLiteral("commands")};
  if (!filters.contains(filter))
    return failure(QStringLiteral("invalid_filter"), QStringLiteral("filter"));
  int pageSize = kDefaultPageSize;
  if (!strictIntArg(args, "page_size", 1, kMaxPageSize, kDefaultPageSize, &pageSize))
    return failure(QStringLiteral("argument_out_of_range"), QStringLiteral("page_size"));
  int maxBytes = kDefaultPageBytes;
  if (!strictIntArg(args, "max_bytes", kMinPageBytes, kMaxPageBytes, kDefaultPageBytes, &maxBytes))
    return failure(QStringLiteral("argument_out_of_range"), QStringLiteral("max_bytes"));

  const QJsonValue queryArg = args.value("query");
  if (!queryArg.isUndefined() && (!queryArg.isString() || queryArg.toString().size() > 128))
    return failure(QStringLiteral("argument_out_of_range"), QStringLiteral("query"));
  const OperationSearch search(queryArg.toString());
  const QVector<DirectoryEntry> entries = buildDirectory(filter, search);
  const QString digestKey = search.phrase.isEmpty() ? filter : filter + u'/' + search.phrase;
  int offset = 0;
  if (args.contains("cursor")) {
    const QString cursor = args.value("cursor").toString();
    if (!decodeListCursor(cursor, filter, entries, digestKey, &offset))
      return failure(QStringLiteral("cursor_stale"), QStringLiteral("cursor"));
  }
  QJsonArray cards;
  int position = offset;
  for (; position < entries.size() && cards.size() < pageSize; ++position)
    cards.append(entries.at(position).card);
  // 字节预算：超限时从尾部整条移除（不截半个条目），coverage 如实汇报。
  while (compactBytes(QJsonObject{{"entries", cards}}) > maxBytes && cards.size() > 1) {
    cards.removeLast();
    --position;
  }
  // F2（评审）：首条即超预算时不能返回「空页 + 原游标」的可重复成功——
  // 模型会无限重翻同一页；明确报 page_budget_exceeded（换更小筛选/更大
  // max_bytes 或改用 describe）。
  if (!cards.isEmpty() && compactBytes(QJsonObject{{"entries", cards}}) > maxBytes)
    return failure(QStringLiteral("page_budget_exceeded"), QStringLiteral("max_bytes"));

  QJsonObject payload{{"contract", QLatin1String(kCapabilityInspectContract)},
                      {"tool", QLatin1String(paleo::chat::kCapabilityInspectTool)},
                      {"mode", QStringLiteral("list")},
                      {"ok", true},
                      {"code", QStringLiteral("ok")},
                      {"filter", filter},
                      {"entries", cards},
                      {"returnedCount", cards.size()},
                      {"totalCount", entries.size()}};
  const bool hasMore = position < entries.size();
  payload.insert(QStringLiteral("hasMore"), hasMore);
  if (hasMore)
    payload.insert(QStringLiteral("nextCursor"), encodeListCursor(filter, entries, digestKey, position));
  payload.insert(QStringLiteral("coverage"), QJsonObject{
      {"complete", !hasMore},
      {"selection", search.phrase.isEmpty() ? QStringLiteral("stable id/version order; continuation binds filter and directory digest")
                                            : QStringLiteral("relevance, then stable id/version order; continuation binds query")}});
  InspectionResult out;
  out.ok = true;
  out.payload = payload;
  return out;
}

InspectionResult runDescribe(const QJsonObject &args) {
  const QString kind = args.value("target_kind").toString();
  const QString id = args.value("target_id").toString();
  if (id.isEmpty())
    return failure(QStringLiteral("missing_required"), QStringLiteral("target_id"));
  if (id.size() > kMaxTargetIdLength)
    return failure(QStringLiteral("argument_out_of_range"), QStringLiteral("target_id"));
  int requestedVersion = 0;
  if (!strictIntArg(args, "contract_version", 0, kMaxContractVersion, 0, &requestedVersion))
    return failure(QStringLiteral("argument_out_of_range"), QStringLiteral("contract_version"));
  int maxBytes = kDefaultPageBytes;
  if (!strictIntArg(args, "max_bytes", kMinPageBytes, kMaxPageBytes, kDefaultPageBytes, &maxBytes))
    return failure(QStringLiteral("argument_out_of_range"), QStringLiteral("max_bytes"));

  // sections 封闭词表（F4：按目标类型校验适用子集——不适用的节名明确
  // invalid_section，不静默吞掉；未请求的节不出现在载荷里）。
  const QStringList vocabulary{QStringLiteral("identity"), QStringLiteral("contract"),
                               QStringLiteral("params"), QStringLiteral("ports"),
                               QStringLiteral("lifecycle"), QStringLiteral("related"),
                               QStringLiteral("route")};
  QStringList requested;
  const auto sections = args.value("sections").toArray();
  if (sections.size() > kMaxSections)
    return failure(QStringLiteral("argument_out_of_range"), QStringLiteral("sections"));
  for (const auto &value : sections) {
    const QString section = value.toString();
    if (!vocabulary.contains(section))
      return failure(QStringLiteral("invalid_section"), section);
    if (!requested.contains(section))
      requested.append(section);
  }
  const auto kindSections = [&kind] {
    if (kind == QLatin1String("command"))
      return QStringList{QStringLiteral("identity"), QStringLiteral("contract"),
                         QStringLiteral("related")};
    if (kind == QLatin1String("tool"))
      return QStringList{QStringLiteral("identity"), QStringLiteral("contract"),
                         QStringLiteral("route"), QStringLiteral("related"),
                         QStringLiteral("lifecycle")};
    return QStringList{QStringLiteral("identity"), QStringLiteral("contract"),
                       QStringLiteral("params"), QStringLiteral("ports"),
                       QStringLiteral("lifecycle"), QStringLiteral("related")};
  }();
  for (const QString &section : requested)
    if (!kindSections.contains(section))
      return failure(QStringLiteral("invalid_section"), section);
  const bool allSections = requested.isEmpty();
  const auto wants = [&](const QString &section) {
    return allSections || requested.contains(section);
  };

  // 装配（固定节顺序；identity 恒在最前，预算超限时从尾部整节移除）。
  QVector<QPair<QString, QJsonObject>> assembled;
  const auto appendSection = [&assembled](const QString &name, const QJsonObject &value) {
    assembled.append({name, value});
  };

  if (kind == QLatin1String("capability")) {
    const QVector<CapabilitySpec> table = CapabilityRegistry::all();
    QString canonical = id;
    if (std::none_of(table.cbegin(), table.cend(),
                     [&canonical](const CapabilitySpec &spec) {
                       return spec.capabilityId == canonical;
                     })) {
      // 精确 id 未命中才解析 alias；冲突不猜测，未知如实报错。
      QString code;
      const QString resolved = CapabilityRegistry::resolveAlias(id, &code);
      if (code == QLatin1String("alias_collision"))
        return failure(code, QStringLiteral("target_id"));
      if (resolved.isEmpty())
        return failure(QStringLiteral("unknown_target"), QStringLiteral("target_id"));
      canonical = resolved;
    }
    const CapabilitySpec *spec = nullptr;
    for (const CapabilitySpec &item : table)
      if (item.capabilityId == canonical &&
          (requestedVersion == 0 || item.contractVersion == requestedVersion)) {
        if (!spec || item.contractVersion > spec->contractVersion)
          spec = &item;
      }
    if (!spec)
      return failure(requestedVersion > 0 ? QStringLiteral("unsupported_version")
                                          : QStringLiteral("unknown_target"),
                     requestedVersion > 0 ? QStringLiteral("contract_version")
                                          : QStringLiteral("target_id"));
    if (wants(QStringLiteral("identity"))) {
      auto identity = paleo::capability::capabilityIdentityDescription(*spec);
      identity.insert(QStringLiteral("id"), canonical);
      if (canonical != id)
        identity.insert(QStringLiteral("requested_alias"), id);
      identity.insert(QStringLiteral("versions"),
                      QJsonArray::fromStringList(registeredCapabilityVersions(table, canonical)));
      appendSection(QStringLiteral("identity"), identity);
    }
    if (wants(QStringLiteral("contract")))
      appendSection(QStringLiteral("contract"), QJsonObject{
          {"exposure", capabilityExposureName(spec->exposure)},
          {"exposure_reason", spec->exposureReason},
          {"aliases", QJsonArray::fromStringList(spec->aliases)},
          {"publish_effect", spec->publishEffect},
          {"operator_shapes", QJsonArray::fromStringList(spec->operatorShapes)}});
    if (wants(QStringLiteral("lifecycle")))
      appendSection(QStringLiteral("lifecycle"), QJsonObject{
          {"cancel_supported", spec->cancelSupported},
          {"resume_supported", spec->resumeSupported},
          {"restartable", spec->restartable}});
    if (wants(QStringLiteral("params")))
      appendSection(QStringLiteral("params"), capabilityParamSection(*spec));
    if (wants(QStringLiteral("ports")))
      appendSection(QStringLiteral("ports"), capabilityPortSection(*spec));
    if (wants(QStringLiteral("related")))
      appendSection(QStringLiteral("related"), capabilityRelatedSection(*spec));
  } else if (kind == QLatin1String("algorithm")) {
    QVector<AlgorithmSpec> matches;
    for (const AlgorithmSpec &spec : AlgorithmSpecRegistry::allSpecsProjection())
      if (spec.id == id)
        matches.append(spec);
    if (matches.isEmpty())
      return failure(QStringLiteral("unknown_target"), QStringLiteral("target_id"));
    std::sort(matches.begin(), matches.end(),
              [](const AlgorithmSpec &a, const AlgorithmSpec &b) {
                return a.contractVersion < b.contractVersion;
              });
    const AlgorithmSpec *spec = nullptr;
    for (const AlgorithmSpec &item : matches)
      if (item.contractVersion == requestedVersion)
        spec = &item;
    if (!spec && requestedVersion == 0)
      spec = &matches.last();
    if (!spec)
      return failure(QStringLiteral("unsupported_version"), QStringLiteral("contract_version"));
    if (wants(QStringLiteral("identity"))) {
      auto identity = paleo::capability::algorithmIdentityDescription(*spec);
      QStringList versions;
      for (const AlgorithmSpec &item : matches)
        versions.append(QString::number(item.contractVersion));
      identity.insert(QStringLiteral("versions"), QJsonArray::fromStringList(versions));
      appendSection(QStringLiteral("identity"), identity);
    }
    if (wants(QStringLiteral("contract")))
      appendSection(QStringLiteral("contract"), QJsonObject{
          {"engine", spec->engine},
          {"complete", spec->complete},
          {"complete_is_not_callable_note",
           QStringLiteral("Spec.complete describes contract completeness only; agent callability is exposure+route+policy+runtime")}});
    if (wants(QStringLiteral("params")))
      appendSection(QStringLiteral("params"), paleo::capability::algorithmParamsDescription(*spec));
    if (wants(QStringLiteral("ports")))
      appendSection(QStringLiteral("ports"), paleo::capability::algorithmPortsDescription(*spec));
    if (wants(QStringLiteral("lifecycle")))
      appendSection(QStringLiteral("lifecycle"), QJsonObject{
          {"cancel_supported", spec->cancelSupported},
          {"resume_supported", spec->resumeSupported}});
    if (wants(QStringLiteral("related")))
      appendSection(QStringLiteral("related"), algorithmRelatedSection(*spec));
  } else if (kind == QLatin1String("command")) {
    // 与 capability/algorithm 同一「0=当前登记最高版」语义。
    const auto table = paleo::command::CommandRegistry::all();
    const paleo::command::CommandSpec *command = nullptr;
    for (const auto &item : table)
      if (item.commandId == id &&
          (requestedVersion == 0 || item.contractVersion == requestedVersion)) {
        if (!command || item.contractVersion > command->contractVersion)
          command = &item;
      }
    if (!command)
      return failure(requestedVersion > 0 ? QStringLiteral("unsupported_version")
                                          : QStringLiteral("unknown_target"),
                     requestedVersion > 0 ? QStringLiteral("contract_version")
                                          : QStringLiteral("target_id"));
    if (wants(QStringLiteral("identity")))
      appendSection(QStringLiteral("identity"),
                    paleo::capability::commandIdentityDescription(*command));
    if (wants(QStringLiteral("contract")))
      appendSection(QStringLiteral("contract"), QJsonObject{
          {"command_kind", paleo::command::commandKindName(command->kind)},
          {"effect", command->effect},
          {"agent_surface", command->hasSurface(paleo::command::SurfaceAgent)},
          {"required_context", QJsonArray::fromStringList(command->requiredContext)}});
    if (wants(QStringLiteral("related")))
      appendSection(QStringLiteral("related"), commandCapabilitySection(*command));
  } else if (kind == QLatin1String("tool")) {
    const AiToolSpec spec = aiToolSpec(id);
    if (spec.name.isEmpty())
      return failure(QStringLiteral("unknown_target"), QStringLiteral("target_id"));
    if (wants(QStringLiteral("identity")))
      appendSection(QStringLiteral("identity"), QJsonObject{
          {"kind", QStringLiteral("tool")},
          {"id", spec.name},
          {"title", spec.title},
          {"description", spec.description},
          {"requires_local_ort", spec.requiresLocalOrt},
          {"legacy_adapter", spec.legacyAdapter},
          {"exposure", capabilityExposureName(spec.exposure)}});
    if (wants(QStringLiteral("contract")))
      appendSection(QStringLiteral("declaration"), describeToolDeclaration(spec));
    if (wants(QStringLiteral("route")))
      appendSection(QStringLiteral("route"), describeToolRoute(spec.name));
    if (wants(QStringLiteral("related")))
      appendSection(QStringLiteral("related"), toolRelatedSection(spec));
    if (wants(QStringLiteral("lifecycle")))
      appendSection(QStringLiteral("effects"), describeToolEffects(spec));
  } else {
    return failure(QStringLiteral("invalid_target_kind"), QStringLiteral("target_kind"));
  }

  // 预算：从尾部整节移除并记录 omittedSections——不截半个 schema 再称完整。
  // F1（评审）：单个节自身超预算同样省略（sections 可为空 + complete=false +
  // continuation 指引），绝不在超限时交付并谎报 complete。
  QStringList delivered, omitted;
  while (!assembled.isEmpty()) {
    QJsonObject measure;
    for (const auto &section : assembled)
      measure.insert(section.first, section.second);
    if (compactBytes(measure) + kEnvelopeMarginBytes <= maxBytes)
      break;
    omitted.prepend(assembled.last().first);
    assembled.removeLast();
  }
  QJsonObject sectionValues;
  for (const auto &section : assembled) {
    sectionValues.insert(section.first, section.second);
    delivered.append(section.first);
  }
  InspectionResult out;
  out.ok = true;
  out.payload = QJsonObject{
      {"contract", QLatin1String(kCapabilityInspectContract)},
      {"tool", QLatin1String(paleo::chat::kCapabilityInspectTool)},
      {"mode", QStringLiteral("describe")},
      {"ok", true},
      {"code", QStringLiteral("ok")},
      {"target_kind", kind},
      {"target_id", id},
      {"sections", sectionValues},
      {"coverage", QJsonObject{{"complete", omitted.isEmpty()},
                               {"deliveredSections", QJsonArray::fromStringList(delivered)},
                               {"omittedSections", QJsonArray::fromStringList(omitted)},
                               {"continuation",
                                QStringLiteral("re-request with sections=[...] to read omitted sections within budget")}}}};
  return out;
}

InspectionResult runCheck(const QJsonObject &args, const InspectionHostContext &host) {
  if (!args.contains("candidate") || !args.value("candidate").isObject())
    return failure(QStringLiteral("missing_candidate"), QStringLiteral("candidate"));
  const QJsonObject candidate = args.value("candidate").toObject();
  if (!candidateShapeWithinBudget(candidate, 0))
    return failure(QStringLiteral("candidate_budget_exceeded"), QStringLiteral("candidate"));
  const bool hasTool = candidate.contains("tool");
  const bool hasAlgorithm = candidate.contains("algorithm");
  if (hasTool == hasAlgorithm)
    return failure(hasTool ? QStringLiteral("ambiguous_candidate") : QStringLiteral("missing_candidate"),
                   QStringLiteral("candidate"));
  QJsonObject verdict =
      hasTool ? checkToolCandidate(candidate, host) : checkAlgorithmCandidate(candidate);
  const QString code = verdict.value("code").toString();
  if (code == QLatin1String("unknown_tool") || code == QLatin1String("unknown_algorithm") ||
      code == QLatin1String("unsupported_version"))
    return failure(code, verdict.value("field").toString());
  // 检查成功的准确含义是「哪些层已通过」，不是「能够执行」。
  verdict.insert(QStringLiteral("meaning"),
      QStringLiteral("assessment layers only: declaration/route/parameter/policy/runtime are "
                     "independent; a valid+allowed verdict does not waive the original "
                     "execution gates (frozen tool table, readiness, host prepare)"));
  verdict.insert(QStringLiteral("no_execution_ticket"), true);
  InspectionResult out;
  out.ok = true;
  out.payload = QJsonObject{
      {"contract", QLatin1String(kCapabilityInspectContract)},
      {"tool", QLatin1String(paleo::chat::kCapabilityInspectTool)},
      {"mode", QStringLiteral("check")},
      {"ok", true},
      {"code", QStringLiteral("ok")},
      {"check", verdict}};
  return out;
}

} // namespace

InspectionResult runCapabilityInspection(const QJsonObject &arguments,
                                        const InspectionHostContext &host) {
  // 顶层封闭字段集（与工具 schema additionalProperties=false 同源）：实参里的
  // approved/scope/authority 等伪造授权字段在此即拒，不进入任何评估。
  static const QStringList kTopLevelFields{QStringLiteral("mode"),
      QStringLiteral("target_kind"), QStringLiteral("target_id"),
      QStringLiteral("contract_version"), QStringLiteral("sections"),
      QStringLiteral("filter"), QStringLiteral("query"), QStringLiteral("candidate"),
      QStringLiteral("cursor"), QStringLiteral("page_size"), QStringLiteral("max_bytes")};
  for (auto it = arguments.constBegin(); it != arguments.constEnd(); ++it)
    if (!kTopLevelFields.contains(it.key()))
      return failure(QStringLiteral("unknown_field"), it.key());
  const QString mode = arguments.value("mode").toString();
  InspectionResult result;
  if (mode == QLatin1String("list"))
    result = runList(arguments);
  else if (mode == QLatin1String("describe"))
    result = runDescribe(arguments);
  else if (mode == QLatin1String("check"))
    result = runCheck(arguments, host);
  else
    result = failure(arguments.contains("mode") ? QStringLiteral("invalid_mode")
                                                : QStringLiteral("missing_required"),
                     QStringLiteral("mode"));
  // F5（评审）：失败帧也携带 mode（H5 失败路径携带完整 required 键的同纪律；
  // mode 本身非法时为空串——schema required 只钉 contract/ok）。
  if (!result.ok)
    result.payload.insert(QStringLiteral("mode"), mode);
  return result;
}

} // namespace paleo::agent
