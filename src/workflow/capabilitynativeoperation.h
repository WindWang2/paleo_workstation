// 层：功能
#pragma once
#include "capabilityrecovery.h"
#include "capabilityplan_outputs.h"
namespace paleo::capability {
QJsonObject nativeIntent(const ExecuteRequest &request);
QJsonObject nativeFrozenRequest(const ExecuteRequest &request);
bool nativeRequestFromJson(const QJsonObject &json,ExecuteRequest &request);
FrozenOperation freezeNativeOperation(const ExecuteRequest &request,const QJsonObject &original,QString *error);
FrozenOperation freezeSourcePublication(const ExecuteRequest &request,const QJsonObject &original,
                                       const OperationLog::Record &source,QString *error);
OperationWorker nativeWorker(ExecuteRequest request);
QString verifyNativeRasterInput(ExecuteRequest &request);
OperationInputValidator nativeInputValidator(const QJsonObject &frozen);
FrozenOperation freezeNativePlan(const ExecuteRequest &request, const QVector<PlanNode> &nodes,
                                 const QVector<PlanEdge> &edges, QString *error);
QJsonObject nativePlanIntent(const ExecuteRequest &request,const QVector<PlanNode> &nodes,const QVector<PlanEdge> &edges);
OperationWorker restoreNativePlan(const QJsonObject &plan, QString *error);
// H3 v2：显式 exports 的多成果计划。v1 签名与行为保持不变；restoreNativePlan
// 按冻结描述符里的版本分派（缺版本 = v1，2 = v2，其他拒绝）。
QJsonObject nativePlanIntentV2(const ExecuteRequest &request,const QVector<PlanNode> &nodes,
                               const QVector<PlanEdge> &edges,const PlanExports &exports);
FrozenOperation freezeNativePlanV2(const ExecuteRequest &request,const QVector<PlanNode> &nodes,
                                   const QVector<PlanEdge> &edges,const PlanExports &exports,QString *error);
OperationWorker restoreNativePlanV2(const QJsonObject &plan, QString *error);
}
