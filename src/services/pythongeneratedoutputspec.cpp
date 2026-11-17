// 层：数据
#include "pythongeneratedoutputspec.h"
#include "pythonnumerictable.h"
#include "pythonseismictrace.h"
namespace paleo::python {
AlgorithmSpec generatedOutputsAlgorithmSpec() {
  auto spec=tableOutputsAlgorithmSpec();spec.id="python:generated_outputs";
  spec.displayName="Generated report, numeric table and nonspatial image";spec.implementationVersion="python-generated-outputs.v1";
  auto &input=spec.inputs[0];input.id="NUMERIC_WINDOW";input.axis="declared_input_axis";
  input.constraints={{"contracts",QStringList{QLatin1String(NumericTableInputContract),QLatin1String(SeismicTraceInputContract)}},
    {"maxRows",8192},{"maxColumns",16},{"alignment","single_explicit_window"}};
  AlgoPort image;image.id="IMAGE";image.kind="file";
  image.constraints={{"fileExtension","png"},{"encoding","host-png.rgba8.v1"},{"maxBytes",ImageOutputPngBytes},
    {"contract","paleo.image-output.v1"},{"spatial",false},{"maxWidth",1024},{"maxHeight",1024}};
  spec.outputs.append(image);return spec;
}
CapabilitySpec generatedOutputsCapabilitySpec() {
  auto spec=tableOutputsCapabilitySpec();spec.capabilityId=QLatin1String(GeneratedOutputsCapability);
  spec.algorithmId=generatedOutputsAlgorithmSpec().id;spec.aliases={QLatin1String(GeneratedOutputsTool)};
  spec.exposureReason="Explicit numeric input and output declarations; host CSV/PNG encoding and atomic three-member output set";return spec;
}
}
