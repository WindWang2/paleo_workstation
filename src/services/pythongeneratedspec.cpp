// 层：数据
#include "pythongeneratedspec.h"
#include "pythongenerated.h"
#include "pythonanalysis.h"
#include "pythonanalysiscapabilityspec.h"
#include "hostcallcontext.h"
namespace paleo::python {
AlgorithmSpec generatedAlgorithmSpec() {
  auto spec=analysisAlgorithmSpec(AnalysisKind::WellQuality);spec.id="python:generated_compute";
  spec.displayName="Generated bounded numerical report";spec.implementationVersion="python-generated-numeric.v1";
  spec.reproducibility=AlgorithmReproducibility::Nondeterministic;
  for(auto &output:spec.outputs)if(output.id=="REPORT")output.constraints["contract"]="paleo.python-generated-report.v1";
  return spec;
}
CapabilitySpec generatedCapabilitySpec() {
  CapabilitySpec spec;spec.capabilityId=QLatin1String(GeneratedCapability);spec.algorithmId=generatedAlgorithmSpec().id;
  spec.kind=CapabilityKind::Compute;spec.exposure=CapabilityExposure::Callable;spec.aliases={QLatin1String(GeneratedTool)};
  spec.exposureReason="Generated numerical report; explicit code/input identities; proven platform isolation mandatory";
  spec.executionEffects=host::LocalExecute;spec.publishEffect="optional";spec.cancelSupported=true;
  spec.restartable=true;spec.resumeSupported=false;spec.operatorShapes={"standalone-reduction"};return spec;
}
}
