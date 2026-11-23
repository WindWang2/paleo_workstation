// 层：数据
// services/capabilitydescription.h — I5：能力/算法/命令三注册表的只读纯值投影。
// 只做「表里登记了什么」的忠实描述 DTO：不复制可变注册表、不另建支持清单、
// 不维护第二份参数默认值表；不 include ai/workflow/ui，不执行任何业务。
// Agent 可调用性是多层数实（exposure/route/policy/runtime），本文件只回答
// 声明层事实，调用资格由 workflow/agentcapabilityinspection 分层汇报。
#pragma once
#include "algorithmspec.h"
#include "capabilityspec.h"
#include "commandspec.h"

#include <QJsonObject>

namespace paleo::capability {

// —— 名字投影（与各注册表原语义一致，不发明新词）——
QString capabilityKindName(CapabilityKind kind);
QString algorithmReproducibilityName(AlgorithmReproducibility value);
QString algorithmParamTypeName(AlgorithmParamField::Type type);

// —— CapabilitySpec 投影 ——
// 清单卡片（list 页条目）：身份/版本/kind/exposure + 摘要事实。
QJsonObject capabilitySummaryCard(const CapabilitySpec &spec);
// 精确描述（describe 用）：aliases、publishEffect、cancel/resume/restartable、
// operatorShapes、executionEffects 与 algorithmId/algorithmContractVersion 引用。
QJsonObject capabilityIdentityDescription(const CapabilitySpec &spec);

// —— AlgorithmSpec 投影 ——
QJsonObject algorithmSummaryCard(const AlgorithmSpec &spec);
// 身份/引擎/版本：displayName、engine、complete（≠可调用）、contractVersion、
// implementationVersion、paramSchemaVersion、executionShape、reproducibility、
// cancel/resume。空单位、未声明轴、unspecified 一律原样输出，不由显示名猜测。
QJsonObject algorithmIdentityDescription(const AlgorithmSpec &spec);
// 参数合同：key/label/type/required/applyDefaultWhenAbsent/defaultValue/
// min/max/decimals/suffix/options——默认值单一来源即本表，原样投影。
QJsonObject algorithmParamDescription(const AlgorithmParamField &field);
QJsonObject algorithmParamsDescription(const AlgorithmSpec &spec);
// 端口合同：kind/role/geometryHint/required/hidden/cardinality/unit/axis/
// verticalDatum/constraints（输入与输出分开）。
QJsonObject algorithmPortDescription(const AlgoPort &port);
QJsonObject algorithmPortsDescription(const AlgorithmSpec &spec);

// —— CommandSpec 投影 ——
QJsonObject commandSummaryCard(const paleo::command::CommandSpec &spec);
// 身份/kind/surfaces/stage/effect/shortcutId/requiredContext/docKinds 与
// capabilityId+contractVersion 精确引用（View 命令无 capability 原样为空）。
QJsonObject commandIdentityDescription(const paleo::command::CommandSpec &spec);

} // namespace paleo::capability
