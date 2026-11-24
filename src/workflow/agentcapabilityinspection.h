// 层：功能
// workflow/agentcapabilityinspection.h — I5：paleo.capability_inspect 后端。
// 从既有 CapabilityRegistry / AlgorithmSpecRegistry / CommandRegistry 与
// builtinAiToolSpecs 生成只读目录、精确契约描述与纯候选参数预检。
//
// 红线（钉死）：不 prepare/start、不 Recovery::get/reconcile、不读用户路径、
// 不装载模型、不运行算法、不触发 UI、不调用 readiness 的动态分支、不发放
// 执行票据或 handle。唯一输入是工具实参与可信宿主上下文快照（runner 注入），
// 输出是纯值 JSON——候选实参里的 approved/scope/路径只是数据，不构成许可。
#pragma once
#include "services/hostcallcontext.h"

#include <QJsonObject>
#include <QString>

struct AiToolSpec;

namespace paleo::agent {

inline constexpr char kCapabilityInspectContract[] = "paleo.capability-inspect.v1";

// 可信宿主上下文快照（owner 线程上由 runner 取好传入；本服务不持宿主对象）。
// available=false：宿主未绑定——静态合同照常可读，policy 部分如实 unavailable。
struct InspectionHostContext {
  bool available = false;
  paleo::host::CallContext live;
  QString expectedDocumentId;
  quint64 expectedEpoch = 0;
};

struct InspectionResult {
  bool ok = false;
  QString code;          // 机器错误码（unsupported_version / unknown_target …）
  QString field;         // 首个违规字段（可空）
  QJsonObject payload;   // 成功：完整 v1 载荷；失败：{contract, code[, field]}
};

// list / describe / check 的纯函数入口。实参即模型工具实参（本服务自行严格
// 解码：封闭词表、有界长度/深度/元素数）；宿主上下文只来自调用方注入。
InspectionResult runCapabilityInspection(const QJsonObject &arguments,
                                         const InspectionHostContext &host);

} // namespace paleo::agent
