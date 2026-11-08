// 层：功能
#pragma once

#include "capabilitygateway.h"

class QObject;
class PaleoTask;
class PaleoTaskService;

namespace paleo::capability {

// The request contains frozen values. The worker owns neither catalog access
// nor host callbacks. Publication is reserved for the owner-side entry.
// On acceptance, finished runs on the task service's owner thread while
// completionContext is alive. Rejection is synchronous and starts no task.
PaleoTask *startCapabilityTask(PaleoTaskService *tasks, QObject *completionContext,
                              const QString &title, ExecuteRequest request,
                              std::function<void(ExecuteResult)> finished,
                              QString *rejectionCode = nullptr);

} // namespace paleo::capability
