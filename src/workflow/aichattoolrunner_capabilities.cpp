// 层：功能
// workflow/aichattoolrunner_capabilities.cpp — I5：paleo.capability_inspect 的
// runner 分支。宿主上下文在 owner 线程上快照后交给纯检查服务；同步查询也经
// finishActive 统一异步回报（#281 纪律）。零业务副作用：不建任务、不占 Host
// 票据、不触发 provider/模型/文件读取。
#include "aichattoolrunner.h"
#include "agentcapabilityinspection.h"
#include "capabilityhost.h"

void AiChatToolRunner::runCapabilityInspection(const QJsonObject &args) {
  paleo::agent::InspectionHostContext host;
  host.available = bool(m_context.capabilityHost);
  if (host.available) {
    // owner 线程上取宿主当前上下文（context() 是宿主公开只读快照，无副作用）；
    // 期待身份来自 runner 绑定，模型不可注入。
    host.live = m_context.capabilityHost->context();
    host.expectedDocumentId = m_context.documentId;
    host.expectedEpoch = m_context.documentEpoch;
  }
  const auto result = paleo::agent::runCapabilityInspection(args, host);
  finishActive(result.ok, result.payload);
}
