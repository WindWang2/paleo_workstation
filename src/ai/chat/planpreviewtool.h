// 层：功能
#pragma once
#include <QString>
#include <QVector>

struct AiToolSpec;

// ai/chat — H5 计划提案工具的声明与 schema（纯数据，不含解码/执行）。
//
// paleo.workflow_plan_preview 是一个**纯预检提案**工具：模型提交 v1 有类型
// 节点/边组合，工具只做严格解码 + validatePlan 业务预检，返回有界提案契约
// （paleo.plan-preview.v1）。它不 prepare、不 start、不建 operation、不读
// 模型提供的任何路径——执行只发生在用户于原生审阅面板明确点击
// 「计算私有成果」之后（workflow/aiplanpresentation 走既有宿主链）。
//
// 严格解码器在 workflow/aichattoolrunner_plan.cpp（需要 capabilityplan 的
// 值类型）；本文件只维护工具表声明与共享常量，避免 ai↔workflow 模块环。

namespace paleo::chat {

inline constexpr char kPlanPreviewTool[] = "paleo.workflow_plan_preview";
inline constexpr char kPlanPreviewContract[] = "paleo.plan-preview.v1";
// 预检提案的执行状态恒为 not_executed：模型提案、预检通过、UI 勾选都不
// 构成执行授权；唯一改写者是用户显式动作后的 aiplanpresentation。
inline constexpr char kPlanPreviewNotExecuted[] = "not_executed";

// 边界常量（解码器与 UI 编辑器同源；不放大既有模型参数预算）：
// - 节点/边上限与 capabilityplan 的 v1 后端上限一致（32 节点/64 边）；
// - 字符串/深度/元素预算是防御深度：总参数仍受 ChatToolCall 16 KiB 既有
//   预算约束，这里不给大图开后门。
inline constexpr int kPlanMaxNodes = 32;
inline constexpr int kPlanMaxEdges = 64;
inline constexpr int kPlanMaxDepth = 8;          // parameters/inputs 嵌套深度
inline constexpr int kPlanMaxStringValue = 4096; // 单个字符串值（字符）
inline constexpr int kPlanMaxObjectKeys = 64;    // 单层对象键数
inline constexpr int kPlanMaxArrayItems = 512;   // 单个数组元素数（点表等）

// 登记进目录（domaintools::builtinAiToolSpecs 调用；顺序稳定）。
void appendAgentPlanPreviewTool(QVector<AiToolSpec> &specs);

} // namespace paleo::chat
