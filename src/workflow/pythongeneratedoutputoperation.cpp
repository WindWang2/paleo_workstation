// 层：功能
#include "pythongeneratedoutputoperation_internal.h"
#include "pythonenvironmentoperation_internal.h"
#include "services/pythongeneratedoutputspec.h"
#include <utility>
namespace paleo::python {
namespace {
using namespace generated_outputs_operation;
capability::CapabilityPreparation capture(const capability::ExecuteRequest &request,const QJsonObject &intent,
    const std::shared_ptr<RuntimeProvider> &runtime,const std::shared_ptr<SandboxProvider> &sandbox,
    const NumericTablePlanProvider &tables,const SeismicTracePlanProvider &traces,QString *error) {
  const auto fail=[&](const QString &code){if(error)*error=code;return capability::CapabilityPreparation{};};
  if(request.capabilityId!=QLatin1String(GeneratedOutputsCapability)||request.contractVersion!=1)return fail("execution_adapter_unavailable");
  const auto parameters=QJsonObject::fromVariantMap(request.parameters);auto inputs=QJsonObject::fromVariantMap(request.inputs);
  const auto kind=inputs.take("input_kind").toString();if(kind!="numeric_table"&&kind!="seismic_trace")return fail("python_output_input_kind_invalid");
  if(!parameters.value("output_table").isObject()||!parameters.value("output_image").isObject())return fail("invalid_python_outputs_profile");
  const auto profile=generatedOutputsProfile(parameters.value("output_table").toObject(),parameters.value("output_image").toObject());
  const auto code=validateGeneratedOutputsProfile(profile,runtime?runtime->policy():ExecutionPolicy{},true);if(!code.isEmpty())return fail(code);
  auto original=request;original.capabilityId=kind=="numeric_table"?QLatin1String(NumericTableCapability):QLatin1String(SeismicTraceCapability);
  original.inputs=inputs.toVariantMap();original.parameters.remove("output_table");original.parameters.remove("output_image");
  auto adapter=kind=="numeric_table"?numericTableScriptAdapter(runtime,sandbox,tables):seismicTraceScriptAdapter(runtime,sandbox,traces);
  auto preparation=adapter.capture(original,intent,error);if(!preparation.worker)return preparation;const auto inner=preparation.worker;
  preparation.worker=[inner,profile,kind,runtime,sandbox](const auto &cancelled,QString *error) {
    auto operation=inner(cancelled,error);if(operation.capabilityId.isEmpty())return operation;
    operation.capabilityId=QLatin1String(GeneratedOutputsCapability);operation.outputSpec=generatedOutputsAlgorithmSpec();
    operation.outputPorts={"REPORT","TABLE","IMAGE"};operation.implementationVersion=operation.outputSpec.implementationVersion;
    operation.frozenPlan["schema"]="paleo.python-generated-outputs-operation.v1";operation.frozenPlan["capability"]=operation.capabilityId;
    operation.frozenPlan["inputKind"]=kind;operation.frozenPlan["outputProfile"]=profile;
    operation.worker=worker(operation.frozenPlan,runtime,sandbox);operation.inputValidator=inputValidator(operation.frozenPlan,runtime,sandbox);return operation;
  };
  return preparation;
}
}
capability::CapabilityExecutionAdapter generatedOutputsScriptAdapter(std::shared_ptr<RuntimeProvider> runtime,std::shared_ptr<SandboxProvider> sandbox,
    NumericTablePlanProvider tables,SeismicTracePlanProvider traces) {
  capability::CapabilityExecutionAdapter out;
  out.capture=[runtime,sandbox,tables,traces](const auto &request,const auto &intent,QString *error){return capture(request,intent,runtime,sandbox,tables,traces,error);};
  out.validateFrozen=[runtime,sandbox](const auto &record){return generated_outputs_operation::checkRecord(record,runtime,sandbox,true);};
  out.restoreWorker=[runtime,sandbox](const auto &record,QString *error) {
    const auto code=generated_outputs_operation::checkRecord(record,runtime,sandbox,true);if(error)*error=code;
    return code.isEmpty()?generated_outputs_operation::worker(record.frozenRequest.value("plan").toObject(),runtime,sandbox):capability::OperationWorker{};
  };
  out.restoreInputValidator=[runtime,sandbox](const auto &record,QString *error) {
    const auto code=generated_outputs_operation::checkRecord(record,runtime,sandbox,true);if(error)*error=code;
    return code.isEmpty()?generated_outputs_operation::inputValidator(record.frozenRequest.value("plan").toObject(),runtime,sandbox):capability::OperationInputValidator{};
  };
  return out;
}
capability::CapabilityExecutionAdapter environmentGeneratedOutputsAdapter(std::shared_ptr<PythonEnvironmentRegistry> registry,
    NumericTablePlanProvider tables,SeismicTracePlanProvider traces) {
  EnvironmentOperationContract contract;
  contract.captureAdapter=[tables,traces](const EnvironmentSelection &selected){return generatedOutputsScriptAdapter(selected.runtime,selected.sandbox,tables,traces);};
  contract.checkRecord=[](const auto &record,const EnvironmentSelection &selected){return generated_outputs_operation::checkRecord(record,selected.runtime,selected.sandbox,false);};
  contract.inputValidator=[](const auto &plan,const EnvironmentSelection &selected){return generated_outputs_operation::inputValidator(plan,selected.runtime,selected.sandbox);};
  contract.worker=[](const auto &plan,const EnvironmentSelection &selected){return generated_outputs_operation::worker(plan,selected.runtime,selected.sandbox);};
  return environmentOperationAdapter(EnvironmentMode::Generated,std::move(registry),std::move(contract));
}
}
