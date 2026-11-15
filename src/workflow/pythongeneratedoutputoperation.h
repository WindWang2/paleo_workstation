// 层：功能
#pragma once
#include "pythonnumerictableoperation.h"
#include "pythonseismictraceoperation.h"
namespace paleo::python {
capability::CapabilityExecutionAdapter generatedOutputsScriptAdapter(std::shared_ptr<RuntimeProvider>,
  std::shared_ptr<SandboxProvider>,NumericTablePlanProvider,SeismicTracePlanProvider);
capability::CapabilityExecutionAdapter environmentGeneratedOutputsAdapter(std::shared_ptr<PythonEnvironmentRegistry>,
  NumericTablePlanProvider,SeismicTracePlanProvider);
}
