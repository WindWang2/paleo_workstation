// 层：功能
// ai/chat/capabilityinspectiontool.cpp — I5 paleo.capability_inspect 声明。
#include "capabilityinspectiontool.h"
#include "domaintools.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace paleo::chat {

void appendCapabilityInspectionTool(QVector<AiToolSpec> &specs) {
  AiToolSpec spec;
  spec.name = QLatin1String(kCapabilityInspectTool);
  spec.title = QObject::tr("能力目录与契约检查");
  spec.description = QObject::tr(
      "只读查询 Agent 可用的业务能力与工具目录：list 分页列出已登记的 "
      "capability/algorithm/tool（UI 命令全集仅在 filter=commands 时给出）；"
      "describe 按精确 ID+版本读取参数/端口/效果合同（sections 可选节，"
      "超预算时整节省略并在 coverage 声明）；check 对候选实参复用既有纯验证器"
      "返回分层结论（declaration/route/parameter/policy/runtime 相互独立，"
      "policy 只反映当前宿主静态策略，动态前提恒为 not_checked）。"
      "本工具不执行任何能力、不发放执行票据——检查结论不豁免原工具调用时"
      "的既有门禁。");
  spec.requiresLocalOrt = false;
  spec.effects = paleo::host::Read;
  spec.parameters = {
      {QStringLiteral("mode"), QStringLiteral("string"),
       QObject::tr("查询模式：list=分页目录；describe=精确合同；check=候选参数预检"),
       true,
       {QStringLiteral("list"), QStringLiteral("describe"), QStringLiteral("check")}},
      {QStringLiteral("target_kind"), QStringLiteral("string"),
       QObject::tr("describe 目标类型：capability/algorithm/command/tool"), false,
       {QStringLiteral("capability"), QStringLiteral("algorithm"),
        QStringLiteral("command"), QStringLiteral("tool")}},
      {QStringLiteral("target_id"), QStringLiteral("string"),
       QObject::tr("精确注册表 ID（capability/command 支持 registry 声明的别名；"
                   "别名冲突时不猜测）"),
       false},
      {QStringLiteral("contract_version"), QStringLiteral("integer"),
       QObject::tr("目标契约版本（0=当前登记最高版，缺省 0；给定版本精确匹配不回落）"),
       false},
      {QStringLiteral("sections"), QStringLiteral("array"),
       QObject::tr("describe 节选择（≤16）：identity/contract/params/ports/lifecycle/"
                   "related/route；缺省全节"),
       false},
      {QStringLiteral("filter"), QStringLiteral("string"),
       QObject::tr("list 筛选：default=capability+algorithm+tool；另有 capabilities/"
                   "algorithms/tools/commands 精确子集"),
       false,
       {QStringLiteral("default"), QStringLiteral("capabilities"),
        QStringLiteral("algorithms"), QStringLiteral("tools"),
        QStringLiteral("commands")}},
      {QStringLiteral("candidate"), QStringLiteral("object"),
       QObject::tr("check 候选（二选一）：{tool,arguments} 或 "
                   "{algorithm,contract_version,parameters}；实参只是数据，"
                   "approved/scope/路径不构成许可"), false},
      {QStringLiteral("cursor"), QStringLiteral("string"),
       QObject::tr("上一页 nextCursor；绑定筛选与目录摘要，注册表演进后报 cursor_stale"),
       false},
      {QStringLiteral("page_size"), QStringLiteral("integer"),
       QObject::tr("list 每页条数（1–32，缺省 16）"), false},
      {QStringLiteral("max_bytes"), QStringLiteral("integer"),
       QObject::tr("本页 JSON 字节预算（2048–24576，缺省 8192）"), false},
  };
  // 结果 schema 程序化构造（与 query/lineage 工具同款做法，避免手写长串括号错）。
  QJsonObject schema{{"type", "object"}, {"required", QJsonArray{"contract", "mode", "ok"}}};
  QJsonObject properties{
      {"contract", QJsonObject{{"const", "paleo.capability-inspect.v1"}}},
      {"mode", QJsonObject{{"enum", QJsonArray{"list", "describe", "check"}}}},
      {"ok", QJsonObject{{"type", "boolean"}}},
      {"code", QJsonObject{{"type", "string"}}},
      {"field", QJsonObject{{"type", "string"}}},
      {"filter", QJsonObject{{"type", "string"}}},
      {"entries", QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "object"}}}}},
      {"returnedCount", QJsonObject{{"type", "integer"}}},
      {"totalCount", QJsonObject{{"type", "integer"}}},
      {"hasMore", QJsonObject{{"type", "boolean"}}},
      {"nextCursor", QJsonObject{{"type", "string"}}},
      {"target_kind", QJsonObject{{"type", "string"}}},
      {"target_id", QJsonObject{{"type", "string"}}},
      {"sections", QJsonObject{{"type", "object"}}},
      {"coverage", QJsonObject{{"type", "object"}}}};
  QJsonObject checkProperties;
  for (const char *key : {"target", "tool", "algorithm"})
    checkProperties.insert(QLatin1String(key), QJsonObject{{"type", "string"}});
  checkProperties.insert("contract_version", QJsonObject{{"type", "integer"}});
  for (const char *key : {"declaration", "route", "parameterAssessment", "dynamicInputs",
                          "policyAssessment", "runtimeAssessment"})
    checkProperties.insert(QLatin1String(key), QJsonObject{{"type", "object"}});
  checkProperties.insert("meaning", QJsonObject{{"type", "string"}});
  checkProperties.insert("no_execution_ticket", QJsonObject{{"const", true}});
  properties.insert("check",
                    QJsonObject{{"type", "object"}, {"properties", checkProperties}});
  schema.insert("properties", properties);
  spec.resultSchemaJson =
      QString::fromUtf8(QJsonDocument(schema).toJson(QJsonDocument::Compact));
  specs.append(spec);
}

} // namespace paleo::chat
