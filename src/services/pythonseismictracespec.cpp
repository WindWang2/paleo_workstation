// 层：数据
#include "pythonseismictracespec.h"
#include "pythonseismictrace.h"
#include "pythongeneratedspec.h"
namespace paleo::python {
AlgorithmSpec seismicTraceAlgorithmSpec() {
  auto spec=generatedAlgorithmSpec();spec.id="python:seismic_trace_compute";
  spec.displayName="Generated authenticated seismic trace window report";spec.implementationVersion="python-seismic-trace-report.v1";
  spec.inputs.clear();AlgoPort trace;trace.id="TRACE";trace.kind="table";trace.hidden=true;trace.cardinalityMin=1;trace.cardinalityMax=1;
  trace.axis="time";trace.constraints={{"contract",QLatin1String(SeismicTraceInputContract)},
    {"sourceKind","authenticated_seismic_trace_window"},{"maxSamples",data::SeismicWindowMaxSamples},{"timeUnit","ms"},
    {"unitConvention","explicit_source_sampling"},{"scope","segy_rev1_fixed_big_endian_ieee32_single_exact_trace"}};
  spec.inputs.append(trace);return spec;
}
CapabilitySpec seismicTraceCapabilitySpec() {
  auto spec=generatedCapabilitySpec();spec.capabilityId=QLatin1String(SeismicTraceCapability);spec.algorithmId=seismicTraceAlgorithmSpec().id;
  spec.aliases={QLatin1String(SeismicTraceTool)};
  spec.exposureReason="Exact authenticated source ranges and SDK read cut; explicit time semantics, generated permission and proven isolation required";
  return spec;
}
}
