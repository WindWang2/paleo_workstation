// 层：功能
#pragma once
#include "capabilityhost.h"
#include "services/pythonnumerictable.h"
#include "services/pythonsandboxprovider.h"
namespace paleo::python {
class PythonEnvironmentRegistry;
using NumericTablePlanProvider=std::function<QString(const capability::ExecuteRequest &,
  NumericTableReadPlan &,std::function<QString()> &)>;
capability::CapabilityExecutionAdapter numericTableScriptAdapter(std::shared_ptr<RuntimeProvider>,
  std::shared_ptr<SandboxProvider>,NumericTablePlanProvider);
capability::CapabilityExecutionAdapter environmentNumericTableAdapter(std::shared_ptr<PythonEnvironmentRegistry>,NumericTablePlanProvider);
}
