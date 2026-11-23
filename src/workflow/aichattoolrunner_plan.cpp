// 层：功能
#include "aichattoolrunner.h"
#include "aiplanpresentation.h"
#include "../ai/chat/planpreviewtool.h"
#include "capabilityplan.h"

// H5 — paleo.workflow_plan_preview 的 runner 入口：decode + validate + 有界
// 提案契约。纯预检零副作用（业务计算/发布路径）：不 prepare、不 start、
// 不建 operation、不读模型提供的任何路径；既有聊天消息与结果缓存按原
// 协议保存，不为凑「零文件写入」破坏回合 checkpoint。执行只发生在用户于
// 原生审阅面板显式动作之后（AiPlanPresentation → CapabilityHost 既有链）。
void AiChatToolRunner::runPlanPreview(const QJsonObject &args) {
  const auto decoded = paleo::chat::decodePlanPreviewArguments(args);
  if (!decoded.ok) {
    QJsonObject payload{{QStringLiteral("contract"), QLatin1String(paleo::chat::kPlanPreviewContract)},
                        {QStringLiteral("tool"), QLatin1String(paleo::chat::kPlanPreviewTool)},
                        {QStringLiteral("ok"), false},
                        {QStringLiteral("status"), QLatin1String(paleo::chat::kPlanPreviewNotExecuted)},
                        {QStringLiteral("code"), decoded.code}};
    if (!decoded.field.isEmpty())
      payload.insert(QStringLiteral("field"), decoded.field);
    finishActive(false, payload);
    return;
  }
  const auto plan = paleo::capability::validatePlan(decoded.nodes, decoded.edges);
  // 工具执行成功（预检完成）；payload.ok 携带图判定，issues 只含验证器
  // 真实返回的问题——不伪造节点定位或完整问题集合。
  finishActive(true, paleo::chat::planPreviewPayload(plan, decoded.nodes, decoded.edges,
                                                     decoded.title, decoded.note));
}
