// 层：数据
#include "unitonnxcapabilityspec.h"

namespace paleo {
QJsonObject unitOnnxInputFeatureContract() {
  return QJsonObject{{"version", 1}, {"kind", "layer_window_rms"}, {"unit", "amplitude"},
                     {"domain", "TWT"}, {"preprocessing", "raw_float32_v1"}};
}

CapabilitySpec unitOnnxCapabilitySpec() {
  CapabilitySpec spec;
  spec.capabilityId = QLatin1String(kUnitOnnxCapabilityId);
  spec.contractVersion = kUnitOnnxContractVersion;
  spec.kind = CapabilityKind::Compute;
  spec.algorithmId = QLatin1String(kUnitOnnxAlgorithmId);
  spec.algorithmContractVersion = kUnitOnnxContractVersion;
  spec.exposure = CapabilityExposure::Callable;
  spec.exposureReason = QStringLiteral(
      "frozen S1-S4 unit layer-window RMS to pinned-weight local ONNX classification; "
      "prepare materializes the bounded 64x64 input, compute-only keeps a private "
      "operation, publish only commits an already computed source operation");
  spec.aliases = {QLatin1String(kUnitOnnxToolName)};
  spec.publishEffect = QStringLiteral("optional");
  spec.cancelSupported = true;
  spec.resumeSupported = true;
  spec.restartable = true;
  spec.operatorShapes = {QStringLiteral("global")};
  return spec;
}
} // namespace paleo
