// 层：功能
#pragma once
#include "pythonnumerictableoperation.h"
namespace paleo::python {
capability::CapabilityExecutionAdapter tableOutputsScriptAdapter(std::shared_ptr<RuntimeProvider>,
  std::shared_ptr<SandboxProvider>,NumericTablePlanProvider);
capability::CapabilityExecutionAdapter environmentTableOutputsAdapter(std::shared_ptr<PythonEnvironmentRegistry>,NumericTablePlanProvider);
}
