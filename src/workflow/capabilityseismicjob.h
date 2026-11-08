// 层：功能
#pragma once

#include "capabilitygateway.h"
#include "services/capabilityseismicinput.h"

namespace paleo::capability {

struct SeismicTraceExecutionPlan {
  SeismicTraceInputPlan input;
  QStringList attributes;
  int rmsHalf = 0;
  qint64 estimatedBytes = 0;
};

QJsonObject seismicTraceIdentity(const ResolvedSeismicTraceInput &input);
bool restoreSeismicTraceResult(const ExecuteRequest &request, ExecuteResult &result);
QString prepareSeismicTraceJob(const ExecuteRequest &request, ValidatedParams &params,
                              SeismicTraceExecutionPlan &plan);
ExecuteResult executeSeismicTraceJob(const ExecuteRequest &request, const SeismicTraceExecutionPlan &plan,
                                     const QString &operationId, const std::function<bool()> &cancelled);

} // namespace paleo::capability
