// 层：数据
#include "pythonanalysiscapabilityspec.h"
#include "pythonanalysis.h"
#include "hostcallcontext.h"

namespace paleo::python {
QString analysisCapabilityId(AnalysisKind kind) {
  return kind==AnalysisKind::WellQuality?QLatin1String(WellQualityCapability):QLatin1String(CorrelationCapability);
}
AlgorithmSpec analysisAlgorithmSpec(AnalysisKind kind) {
  const bool quality=kind==AnalysisKind::WellQuality;
  AlgorithmSpec spec;spec.id=quality?"python:well_curve_quality":"python:attribute_correlation";
  spec.displayName=quality?"Well curve quality report":"Selected well numeric attribute correlation";
  spec.engine="python";spec.complete=true;spec.executionShape=AlgorithmExecutionShape::Reduction;
  spec.reproducibility=AlgorithmReproducibility::Tolerance;spec.cancelSupported=true;
  spec.implementationVersion="python314-numpy254:"+analysisTemplateSha256(kind);
  AlgoPort input;input.id="CURVES";input.kind="table";input.hidden=true;
  input.cardinalityMin=quality?1:2;input.cardinalityMax=AnalysisMaxChannels;input.axis="MD";
  input.constraints={{"contract","paleo.python-analysis-input.v1"},{"sourceKind","well_curve_numeric_attributes"},
    {"maxSamples",AnalysisMaxSamples},{"alignment","exact_shared_axis_v1"},{"unitConvention","declared_per_column"}};
  spec.inputs.append(input);
  AlgoPort report;report.id="REPORT";report.kind="file";
  report.constraints={{"fileExtension","json"},{"encoding","utf8"},{"maxBytes",256*1024},
    {"contract",quality?"paleo.well-quality-report.v1":"paleo.attribute-correlation-report.v1"}};
  spec.outputs.append(report);
  for(const auto &id:{"EXECUTION_LOG","STDOUT_LOG","STDERR_LOG"}) {
    AlgoPort log;log.id=QLatin1String(id);log.kind="file";log.required=false;log.hidden=true;
    log.constraints={{"fileExtension","txt"},{"encoding","utf8"},{"diagnostic",true}};spec.outputs.append(log);
  }
  return spec;
}
CapabilitySpec analysisCapabilitySpec(AnalysisKind kind) {
  CapabilitySpec spec;spec.capabilityId=analysisCapabilityId(kind);spec.algorithmId=analysisAlgorithmSpec(kind).id;
  spec.kind=CapabilityKind::Compute;spec.exposure=CapabilityExposure::Callable;
  spec.aliases={kind==AnalysisKind::WellQuality?QLatin1String(WellQualityTool):QLatin1String(CorrelationTool)};
  spec.exposureReason="Registered trusted template; exact selected well numeric windows only; runtime, policy and source checks required";
  spec.executionEffects=host::LocalExecute;spec.publishEffect="optional";spec.cancelSupported=true;
  spec.restartable=true;spec.resumeSupported=false;spec.operatorShapes={"standalone-reduction"};return spec;
}
} // namespace paleo::python
