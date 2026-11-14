// 层：功能
#pragma once
#include "pythonanalysisoperation.h"

namespace paleo::python::analysis_operation {
QString checkPlan(const QJsonObject &plan,AnalysisKind kind,const std::shared_ptr<RuntimeProvider> &provider);
QString checkRecord(const capability::OperationLog::Record &record,AnalysisKind kind,
    const std::shared_ptr<RuntimeProvider> &provider);
capability::OperationInputValidator inputValidator(const QJsonObject &plan,AnalysisKind kind,
    std::shared_ptr<RuntimeProvider> provider);
capability::OperationWorker worker(const QJsonObject &plan,AnalysisKind kind,std::shared_ptr<RuntimeProvider> provider);
} // namespace paleo::python::analysis_operation
