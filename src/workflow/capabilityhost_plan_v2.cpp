// 层：功能
// workflow/capabilityhost_plan_v2.cpp — H3 v2 计划入口；票据路径与 v1 共享。
#include "capabilityhost.h"

namespace paleo::capability {
HostPrepared CapabilityHost::preparePlanV2(const ExecuteRequest &intent,const QVector<PlanNode> &nodes,
                                          const QVector<PlanEdge> &edges,const PlanExports &exports,
                                          host::Caller caller,const QString &trustedChatSessionId) {
  return preparePlanCommon(intent,nodes,edges,&exports,caller,trustedChatSessionId);
}
} // namespace paleo::capability
