// 层：功能
#pragma once
#include "capabilityrecovery.h"
namespace paleo::capability {
// Common, non-seismic intent fields shared by registered and native adapters.
QJsonObject capabilityIntent(const ExecuteRequest &request);
FrozenOperation freezeSourcePublication(const ExecuteRequest &request,const QJsonObject &original,
                                       const OperationLog::Record &source,QString *error);
}
