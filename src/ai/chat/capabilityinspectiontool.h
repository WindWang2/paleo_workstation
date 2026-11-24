// 层：功能
#pragma once
#include <QString>
#include <QVector>

struct AiToolSpec;

// ai/chat — I5 能力目录检查工具的声明与共享常量（纯数据，不含解码/执行）。
//
// paleo.capability_inspect 是一个**只读目录查询**工具：list/describe/check
// 三模式封闭词表。它投影既有 CapabilityRegistry / AlgorithmSpecRegistry /
// CommandRegistry / builtinAiToolSpecs 的静态合同，并对候选实参调用既有
// 纯验证器给出可定位反馈。它不 prepare/start、不建 operation、不读模型
// 提供的任何路径、不触发 UI——执行只发生在模型随后显式调用原工具、且
// 原有 gate（冻结工具表/readiness/Host prepare）全部复核之后。
//
// 解码与执行在 workflow/agentcapabilityinspection.cpp（需要注册表投影）；
// 本文件只维护工具表声明与共享常量，避免 ai↔workflow 模块环。

namespace paleo::chat {

inline constexpr char kCapabilityInspectTool[] = "paleo.capability_inspect";

// 登记进目录（domaintools::builtinAiToolSpecs 调用；顺序稳定）。
void appendCapabilityInspectionTool(QVector<AiToolSpec> &specs);

} // namespace paleo::chat
