// 层：功能
#pragma once
#include "pythonnumerictableoperation.h"
namespace paleo::python::numeric_table_operation {
QString checkRecord(const capability::OperationLog::Record &,const std::shared_ptr<RuntimeProvider> &,
  const std::shared_ptr<SandboxProvider> &,bool requireBackend=true);
capability::OperationInputValidator inputValidator(const QJsonObject &,std::shared_ptr<RuntimeProvider>,std::shared_ptr<SandboxProvider>);
capability::OperationWorker worker(const QJsonObject &,std::shared_ptr<RuntimeProvider>,std::shared_ptr<SandboxProvider>);
}
