// 层：功能
#pragma once
#include "llmdiagnosticcontract.h"
#include <QString>

namespace paleo::chat {

// H6-W6：诊断报告的有界文本渲染（纯函数；不含秘密——候选快照本就无凭据，
// detail 出口已在收口点脱敏，此处再作有界截断的纵深防御）。
//
// 口径：
//   · 分项独立呈现（目录/文本/流式/工具往返），未测试/无法确定/被拒绝/
//     失败不合并成「已连接」。
//   · 真实 SSE 单列一行恒标「未独立确认」——公开信号不能区分真实 SSE 与
//     普通 JSON 兼容应答。
//   · usage 标注「服务端报告值」；缺省不得显示成精确零。
//   · 历史报告只说明当时快照，不构成当前配置/服务健康证明（页脚注明）。
QString renderLlmDiagnosticSummary(const LlmDiagnosticReport &report);

// 只指出证据支持的下一步（不自动修改配置/不替换密钥/不打开任意 URL）。
QString suggestNextStep(const LlmDiagnosticReport &report);

// 稳定分类标签（面向用户；测试按枚举断言，不匹配文案）。
QString llmDiagnosticStatusLabel(LlmDiagnosticStatus status);
// 凭据来源的诚实标注（本次输入/沿用当前内存值/无——不猜测存储出处）。
QString llmKeySourceLabel(LlmDiagnosticCandidate::KeySource source);

} // namespace paleo::chat
