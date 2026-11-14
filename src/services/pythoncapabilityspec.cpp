// 层：数据
#include "pythoncapabilityspec.h"
#include "hostcallcontext.h"
#include "pythonanalysiscapabilityspec.h"
#include "pythonanalysis.h"

namespace paleo::python {
QString summaryScriptSha256() {return QString::fromLatin1(PALEO_AGENT_PYTHON_SUMMARY_SHA);}
AlgorithmSpec summaryAlgorithmSpec() {
  AlgorithmSpec spec;spec.id=QLatin1String(SummaryAlgorithm);spec.displayName="Trusted Python summary";
  spec.engine="python";spec.complete=true;spec.executionShape=AlgorithmExecutionShape::Reduction;
  spec.reproducibility=AlgorithmReproducibility::Tolerance;spec.cancelSupported=true;
  spec.implementationVersion="python314-numpy254:"+summaryScriptSha256();
  AlgoPort report;report.id="REPORT";report.kind="file";report.constraints={{"fileExtension","json"},{"encoding","utf8"}};
  spec.outputs.append(report);
  for(const auto &id:{"EXECUTION_LOG","STDOUT_LOG","STDERR_LOG"}) {
    AlgoPort log;log.id=QLatin1String(id);log.kind="file";log.required=false;log.hidden=true;
    log.constraints={{"fileExtension","txt"},{"encoding","utf8"},{"diagnostic",true}};spec.outputs.append(log);
  }
  return spec;
}
CapabilitySpec summaryCapabilitySpec() {
  CapabilitySpec spec;spec.capabilityId=QLatin1String(SummaryCapability);spec.algorithmId=QLatin1String(SummaryAlgorithm);
  spec.kind=CapabilityKind::Compute;spec.exposure=CapabilityExposure::Internal;
  spec.exposureReason="Trusted-template operation acceptance only; production analysis adapters follow in PY2";
  spec.executionEffects=host::LocalExecute;spec.publishEffect="optional";spec.cancelSupported=true;
  spec.restartable=true;spec.resumeSupported=false;spec.operatorShapes={"reduction"};return spec;
}
}
void appendPythonAlgorithmSpecs(QVector<AlgorithmSpec> *specs) {
  specs->append(paleo::python::summaryAlgorithmSpec());
  specs->append(paleo::python::analysisAlgorithmSpec(paleo::python::AnalysisKind::WellQuality));
  specs->append(paleo::python::analysisAlgorithmSpec(paleo::python::AnalysisKind::AttributeCorrelation));
}
