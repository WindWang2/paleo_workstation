// 层：功能
#include "capabilityhost.h"
#include "capabilitynativeoperation.h"
namespace paleo::capability {
namespace {
CapabilityExecutionAdapter nativeAdapter(const HostBinding &binding) {
  CapabilityExecutionAdapter adapter;
  adapter.intent=nativeIntent;
  adapter.freeze=freezeNativeOperation;
  adapter.normalize=[binding](ExecuteRequest &request,QVariantMap &effective) {
    QString code;if(binding.freeze)code=binding.freeze(request);
    if(code.isEmpty() && binding.validateInputs)code=binding.validateInputs(request);
    if(!code.isEmpty())return code;
    const auto spec=CapabilityRegistry::spec(request.capabilityId,request.contractVersion);
    auto parameters=request.parameters;
    const bool resolvedInterval=request.seismicTrace && !parameters.contains(QStringLiteral("SAMPLE_INTERVAL_MS"));
    if(resolvedInterval)parameters.insert(QStringLiteral("SAMPLE_INTERVAL_MS"),1.0);
    effective=effectiveScalarParams(spec.algorithmId,spec.algorithmContractVersion,parameters,&code);
    if(code!=QLatin1String("ok"))return code;
    // SDK preflight supplies the real interval; never display a placeholder.
    if(resolvedInterval)effective.remove(QStringLiteral("SAMPLE_INTERVAL_MS"));
    request.parameters=effective;return QString();
  };
  adapter.restoreInputValidator=[](const OperationLog::Record &record,QString *) {
    return nativeInputValidator(record.frozenRequest.value("plan").toObject());
  };
  adapter.validateFrozen=[binding](const OperationLog::Record &record) {
    ExecuteRequest request;
    if(!nativeRequestFromJson(record.frozenRequest.value("plan").toObject(),request))return QString();
    if(!request.inputs.contains("seismic_selection"))return QString();
    request.projectDir=binding.projectDir;
    QString code;if(binding.freeze)code=binding.freeze(request);
    if(code.isEmpty() && binding.validateInputs)code=binding.validateInputs(request);
    if(code.isEmpty() && nativeFrozenRequest(request).value("sourceVerification")!=record.frozenRequest.value("plan").toObject().value("sourceVerification"))code="source_revision_changed";
    return code;
  };
  adapter.restoreWorker=[binding](const OperationLog::Record &record,QString *error) {
    if(record.capabilityId==QLatin1String("workflow.plan"))return restoreNativePlan(record.frozenRequest.value("plan").toObject(),error);
    ExecuteRequest request;
    if(!nativeRequestFromJson(record.frozenRequest.value("plan").toObject(),request)){if(error)*error="invalid_frozen_plan";return OperationWorker{};}
    request.projectDir=binding.projectDir;request.operationDir=binding.operationDir;
    QString code;if(binding.freeze)code=binding.freeze(request);
    if(code.isEmpty() && binding.validateInputs)code=binding.validateInputs(request);
    if(!code.isEmpty()){if(error)*error=code;return OperationWorker{};}
    return nativeWorker(std::move(request));
  };
  return adapter;
}
}
CapabilityHost::CapabilityHost(QObject *parent) : CapabilityHost(nativeAdapter,parent) {}
}
