// 层：数据
// services/algorithmspec_unitonnx.cpp — I2 的 AlgorithmSpec（参数默认值单一来源）。
// v1 无可调参数：网格/特征/输出合同全部由冻结计划承载，模型、地震来源与
// 单元是宿主解析的输入引用，不是算法参数。
#include "algorithmspec.h"
#include "unitonnxcapabilityspec.h"

void appendUnitOnnxSpec(QVector<AlgorithmSpec> *specs) {
  using namespace paleo;
  AlgorithmSpec spec;
  spec.id = QLatin1String(kUnitOnnxAlgorithmId);
  spec.displayName = QStringLiteral("Unit-window RMS ONNX prediction");
  spec.engine = QStringLiteral("onnx");
  spec.complete = true;
  spec.contractVersion = kUnitOnnxContractVersion;
  spec.implementationVersion = QLatin1String(kUnitOnnxImplementationVersion);
  spec.executionShape = AlgorithmExecutionShape::GlobalNode;
  spec.reproducibility = AlgorithmReproducibility::Tolerance;
  spec.cancelSupported = true;
  spec.resumeSupported = true;
  AlgoPort input;
  input.id = QStringLiteral("UNIT_RMS");
  input.kind = QStringLiteral("raster");
  input.constraints = {
      {"rows", kUnitOnnxGridRows},
      {"columns", kUnitOnnxGridColumns},
      {"featureContractVersion", 1},
      {"featureKind", QStringLiteral("layer_window_rms")},
      {"featureUnit", QStringLiteral("amplitude")},
      {"featureDomain", QStringLiteral("TWT")},
      {"missingPolicy", QStringLiteral("zero-input-validity-mask-reapplied")},
      {"maxWindowValues", 1048576}};
  spec.inputs = {input};
  AlgoPort output;
  output.id = QStringLiteral("OUTPUT");
  output.kind = QStringLiteral("raster");
  output.constraints = {
      {"fileExtension", QStringLiteral("tif")},
      {"modelOutputSemantics", QStringList{QStringLiteral("logits"), QStringLiteral("probabilities")}},
      {"classEncoding", QStringLiteral("declared_channel_index")},
      {"nodata", 255}};
  spec.outputs = {output};
  AlgoPort diagnostics;
  diagnostics.id = QStringLiteral("DIAGNOSTICS");
  diagnostics.kind = QStringLiteral("file");
  diagnostics.required = false;
  diagnostics.hidden = true;
  diagnostics.constraints = {{"fileExtension", QStringLiteral("json")}, {"encoding", QStringLiteral("utf8")},
                             {"diagnostic", true}};
  spec.outputs.append(diagnostics);
  specs->append(spec);
}
