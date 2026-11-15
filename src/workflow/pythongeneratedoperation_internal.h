// 层：功能
#pragma once
#include "pythongeneratedoperation.h"
namespace paleo::python::generated_operation {
QString checkRecord(const capability::OperationLog::Record &,const std::shared_ptr<RuntimeProvider> &,
    const std::shared_ptr<SandboxProvider> &,bool requireBackend=true);
QString checkPlan(const QJsonObject &,const std::shared_ptr<RuntimeProvider> &,const std::shared_ptr<SandboxProvider> &,bool requireBackend);
QString checkBundle(const InputBundle &,const QJsonObject &plan);
capability::OperationInputValidator inputValidator(const QJsonObject &,std::shared_ptr<RuntimeProvider>,std::shared_ptr<SandboxProvider>);
capability::OperationWorker worker(const QJsonObject &,std::shared_ptr<RuntimeProvider>,std::shared_ptr<SandboxProvider>);
}
