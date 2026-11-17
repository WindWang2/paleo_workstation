// 层：功能
#include "pythonseismictraceoperation_internal.h"
#include "pythonenvironmentoperation_internal.h"
namespace paleo::python {
capability::CapabilityExecutionAdapter environmentSeismicTraceAdapter(std::shared_ptr<PythonEnvironmentRegistry> registry,SeismicTracePlanProvider selector) {
  EnvironmentOperationContract contract;contract.allowLegacyPlans=false;
  contract.captureAdapter=[selector](const EnvironmentSelection &selected){return seismicTraceScriptAdapter(selected.runtime,selected.sandbox,selector);};
  contract.checkRecord=[](const auto &record,const EnvironmentSelection &selected){return seismic_trace_operation::checkRecord(record,selected.runtime,selected.sandbox,false);};
  contract.inputValidator=[](const auto &plan,const EnvironmentSelection &selected){return seismic_trace_operation::inputValidator(plan,selected.runtime,selected.sandbox);};
  contract.worker=[](const auto &plan,const EnvironmentSelection &selected){return seismic_trace_operation::worker(plan,selected.runtime,selected.sandbox);};
  return environmentOperationAdapter(EnvironmentMode::Generated,std::move(registry),std::move(contract));
}
}
