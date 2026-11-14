// 层：数据
// services/algorithmspec_pilots.cpp — 本轮试点算法登记，并入 AlgorithmSpecRegistry。
#include "algorithmspec.h"

#include <QCoreApplication>

namespace {

AlgorithmParamField numberParam(const QString &key, AlgorithmParamField::Type type, const QVariant &def,
                                bool required, bool hasMin, double minV, bool hasMax, double maxV) {
  AlgorithmParamField field;
  field.key = key;
  field.label = key;
  field.type = type;
  field.defaultValue = def;
  field.required = required;
  field.hasMin = hasMin;
  field.minValue = minV;
  field.hasMax = hasMax;
  field.maxValue = maxV;
  return field;
}

AlgorithmParamField textParam(const QString &key, bool required) {
  AlgorithmParamField field;
  field.key = key;
  field.label = key;
  field.type = AlgorithmParamField::String;
  field.required = required;
  return field;
}

AlgoPort filePort(const QString &id, bool required) {
  AlgoPort port;
  port.id = id;
  port.kind = QStringLiteral("file");
  port.required = required;
  port.cardinalityMin = required ? 1 : 0;
  port.cardinalityMax = 1;
  return port;
}

} // namespace

void appendTileInferenceSpec(QVector<AlgorithmSpec> *specs);
void appendAtomicPilotSpecs(QVector<AlgorithmSpec> *specs) {
  if (!specs)
    return;
  appendTileInferenceSpec(specs);

  AlgorithmSpec depth;
  depth.id = QStringLiteral("native:depth_batch_convert");
  depth.displayName = QStringLiteral("Depth batch convert");
  depth.engine = QStringLiteral("native_cpp");
  depth.complete = true;
  depth.contractVersion = 1;
  depth.implementationVersion = QStringLiteral("1");
  depth.paramSchemaVersion = 1;
  depth.executionShape = AlgorithmExecutionShape::Batch;
  depth.reproducibility = AlgorithmReproducibility::Deterministic;
  depth.cancelSupported = true;
  depth.resumeSupported = false;
  {
    AlgoPort samples;
    samples.id = QStringLiteral("SAMPLES");
    samples.kind = QStringLiteral("table");
    samples.required = true;
    samples.unit = QStringLiteral("ms");
    samples.axis = QStringLiteral("time");
    samples.verticalDatum = QStringLiteral("explicit");
    depth.inputs = {samples};
    depth.outputs = {filePort(QStringLiteral("DEPTH"), true)};
    depth.outputs[0].unit = QStringLiteral("m");
    depth.outputs[0].axis = QStringLiteral("TVD");
    depth.outputs[0].constraints = {{"fileExtension","json"},{"encoding","json"}};
  }
  depth.params = {
      textParam(QStringLiteral("TIME_SEMANTIC"), true),
      textParam(QStringLiteral("DEPTH_SEMANTIC"), true),
      textParam(QStringLiteral("EXTRAPOLATION"), true),
      textParam(QStringLiteral("MODEL_REVISION"), true),
      textParam(QStringLiteral("OWT_POLICY"), false),
  };
  specs->append(depth);

  AlgorithmSpec seismic;
  seismic.id = QStringLiteral("native:seismic_attribute_group");
  seismic.displayName = QStringLiteral("Seismic attribute group");
  seismic.engine = QStringLiteral("native_cpp");
  seismic.complete = true;
  seismic.contractVersion = 1;
  seismic.implementationVersion = QStringLiteral("1");
  seismic.paramSchemaVersion = 1;
  seismic.executionShape = AlgorithmExecutionShape::WholeTrace;
  seismic.reproducibility = AlgorithmReproducibility::Deterministic;
  seismic.cancelSupported = true;
  seismic.resumeSupported = false;
  seismic.outputs = {filePort(QStringLiteral("RMS"), false), filePort(QStringLiteral("ENVELOPE"), false),
                     filePort(QStringLiteral("PHASE"), false), filePort(QStringLiteral("COHERENCE"), false)};
  for(auto &port:seismic.outputs)port.constraints={{"fileExtension","f32"},{"encoding","float32le"}};
  {
    AlgoPort volume;
    volume.id = QStringLiteral("VOLUME");
    volume.kind = QStringLiteral("table");
    volume.required = true;
    volume.unit = QStringLiteral("amplitude");
    volume.axis = QStringLiteral("TWT");
    seismic.inputs = {volume};
  }
  AlgorithmParamField attributes;
  attributes.key = QStringLiteral("ATTRIBUTES");
  attributes.label = QStringLiteral("ATTRIBUTES");
  attributes.type = AlgorithmParamField::StringList;
  attributes.required = true;
  attributes.options = {{QStringLiteral("rms"), QStringLiteral("rms")},
                        {QStringLiteral("envelope"), QStringLiteral("envelope")},
                        {QStringLiteral("phase"), QStringLiteral("phase")},
                        {QStringLiteral("coherence"), QStringLiteral("coherence")}};
  seismic.params = {
      attributes,
      numberParam(QStringLiteral("WINDOW_HALF"), AlgorithmParamField::Int, 8, false, true, 0, true, 256),
      numberParam(QStringLiteral("SAMPLE_INTERVAL_MS"), AlgorithmParamField::Double, QVariant(), true, true,
                  0.0001, false, 0),
      numberParam(QStringLiteral("COHERENCE_IL_HALF"), AlgorithmParamField::Int, 1, false, true, 0, true, 8),
      numberParam(QStringLiteral("COHERENCE_XL_HALF"), AlgorithmParamField::Int, 1, false, true, 0, true, 8),
      numberParam(QStringLiteral("COHERENCE_TIME_HALF"), AlgorithmParamField::Int, 2, false, true, 0, true, 64),
      numberParam(QStringLiteral("IL_SPACING"), AlgorithmParamField::Double, 0.0, false, true, 0, false, 0),
      numberParam(QStringLiteral("XL_SPACING"), AlgorithmParamField::Double, 0.0, false, true, 0, false, 0),
  };
  AlgorithmParamField weight = textParam(QStringLiteral("COHERENCE_WEIGHT"), false);
  weight.defaultValue = QStringLiteral("equal");
  seismic.params.append(weight);
  specs->append(seismic);

  AlgorithmSpec facies;
  facies.id = QStringLiteral("paleo:facies_draft_mapping");
  facies.displayName = QStringLiteral("Facies draft and QA");
  facies.engine = QStringLiteral("native_cpp");
  facies.complete = true;
  facies.contractVersion = 1;
  facies.implementationVersion = QStringLiteral("1");
  facies.paramSchemaVersion = 1;
  facies.executionShape = AlgorithmExecutionShape::GlobalNode;
  facies.reproducibility = AlgorithmReproducibility::Deterministic;
  facies.outputs = {filePort(QStringLiteral("OUTPUT"), true), filePort(QStringLiteral("REPORT"), true)};
  specs->append(facies);

  AlgorithmSpec contour;
  contour.id = QStringLiteral("native:factor_contour");
  contour.displayName = QStringLiteral("Fixed-level contours");
  contour.engine = QStringLiteral("native_cpp");
  contour.complete = true;
  contour.contractVersion = 1;
  contour.implementationVersion = QStringLiteral("1");
  contour.paramSchemaVersion = 1;
  contour.executionShape = AlgorithmExecutionShape::GlobalNode;
  contour.reproducibility = AlgorithmReproducibility::Deterministic;
  contour.cancelSupported = true;
  contour.resumeSupported = false;
  AlgoPort raster;
  raster.id = QStringLiteral("RASTER");
  raster.kind = QStringLiteral("raster");
  raster.required = true;
  contour.inputs = {raster};
  AlgoPort lines;
  lines.id = QStringLiteral("LINES");
  lines.kind = QStringLiteral("vector");
  lines.geometryHint = QStringLiteral("line");
  lines.required = true;
  contour.outputs = {lines};
  contour.params = {numberParam(QStringLiteral("CONTOUR_LEVEL"), AlgorithmParamField::Double, QVariant(), true, false, 0,
                                false, 0)};
  specs->append(contour);
}
