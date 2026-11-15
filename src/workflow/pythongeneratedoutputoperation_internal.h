// 层：功能
#pragma once
#include "pythongeneratedoutputoperation.h"
#include "pythongeneratedworker.h"
namespace paleo::python::generated_outputs_operation {
QJsonObject inputPlan(QJsonObject);
QString checkPlan(const QJsonObject &,const std::shared_ptr<RuntimeProvider> &,const std::shared_ptr<SandboxProvider> &,bool backend);
QString checkRecord(const capability::OperationLog::Record &,const std::shared_ptr<RuntimeProvider> &,const std::shared_ptr<SandboxProvider> &,bool backend);
capability::OperationInputValidator inputValidator(const QJsonObject &,std::shared_ptr<RuntimeProvider>,std::shared_ptr<SandboxProvider>);
capability::OperationWorker worker(const QJsonObject &,std::shared_ptr<RuntimeProvider>,std::shared_ptr<SandboxProvider>);
}
