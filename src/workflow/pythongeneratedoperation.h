// 层：功能
#pragma once
#include "pythonanalysisoperation.h"
#include "services/pythonsandboxprovider.h"
namespace paleo::python {
capability::CapabilityExecutionAdapter generatedScriptAdapter(std::shared_ptr<RuntimeProvider> runtime,
    std::shared_ptr<SandboxProvider> sandbox,AnalysisPlanProvider selector);
}
