// 层：功能
#pragma once
#include "capabilityhost.h"
#include "services/pythonruntime.h"

namespace paleo::python {
capability::CapabilityExecutionAdapter trustedScriptAdapter(std::shared_ptr<RuntimeProvider> runtime,const QString &templateRoot);
capability::FrozenOperation freezeTrustedScript(const capability::ExecuteRequest &request,const QJsonObject &intent,
    std::shared_ptr<RuntimeProvider> runtime,const QString &templateRoot,QString *error);
} // namespace paleo::python
