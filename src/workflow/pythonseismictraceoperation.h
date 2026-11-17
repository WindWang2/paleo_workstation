// 层：功能
#pragma once
#include "capabilityhost.h"
#include "services/pythonseismictrace.h"
#include "services/pythonsandboxprovider.h"
namespace paleo::python {
class PythonEnvironmentRegistry;
using SeismicTracePlanProvider=std::function<QString(const capability::ExecuteRequest &,
  SeismicTraceReadPlan &,std::function<QString()> &)>;
capability::CapabilityExecutionAdapter seismicTraceScriptAdapter(std::shared_ptr<RuntimeProvider>,
  std::shared_ptr<SandboxProvider>,SeismicTracePlanProvider);
capability::CapabilityExecutionAdapter environmentSeismicTraceAdapter(std::shared_ptr<PythonEnvironmentRegistry>,SeismicTracePlanProvider);
}
