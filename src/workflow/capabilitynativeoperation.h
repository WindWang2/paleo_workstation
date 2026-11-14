// 层：功能
#pragma once
#include "capabilityrecovery.h"
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
}
