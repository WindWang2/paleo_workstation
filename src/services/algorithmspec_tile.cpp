// 层：数据
#include "algorithmspec.h"

void appendTileInferenceSpec(QVector<AlgorithmSpec> *specs) {
  AlgorithmSpec spec;
  spec.id="onnx:tile_classification"; spec.displayName="Tile classification";
  spec.engine="onnx"; spec.complete=true; spec.contractVersion=2; spec.implementationVersion="2";
  spec.paramSchemaVersion=2; spec.executionShape=AlgorithmExecutionShape::FiniteHalo;
  spec.reproducibility=AlgorithmReproducibility::Tolerance; spec.cancelSupported=true; spec.resumeSupported=true;
  AlgoPort source; source.id="RASTER"; source.kind="raster"; spec.inputs={source};
  spec.inputs[0].constraints={{"formats",QStringList{"GTiff"}},{"maxPixels",1024*1024},
    {"maxSourceBytes",512LL*1024*1024},{"maxCompanionFiles",15},{"featureContractVersion",1}};
  for(const auto &id: {"OUTPUT","MASKED","CONFIDENCE"}) {
    AlgoPort output; output.id=QLatin1String(id); output.kind="raster";
    output.constraints.insert("fileExtension","tif"); spec.outputs.append(output);
  }
  spec.outputs[2].constraints.insert("modelOutputSemantics",QStringList{"logits","probabilities"});
  spec.outputs[2].constraints.insert("confidenceDefinition","normalized entropy; binary single-channel uses twice the distance from 0.5");
  const auto integer=[](const QString &key,int value,int min,int max) {
    AlgorithmParamField p; p.key=key;p.label=key;p.type=AlgorithmParamField::Int;p.defaultValue=value;
    p.hasMin=true;p.minValue=min;p.hasMax=true;p.maxValue=max;return p;
  };
  spec.params={integer("TILE_ROWS",128,1,1024),integer("TILE_COLUMNS",128,1,1024),integer("HALO",8,0,64)};
  AlgorithmParamField threshold;threshold.key="LOW_CONFIDENCE";threshold.label=threshold.key;
  threshold.type=AlgorithmParamField::Double;threshold.defaultValue=0.5;
  threshold.hasMin=true;threshold.minValue=0;threshold.hasMax=true;threshold.maxValue=1;
  spec.params.append(threshold);specs->append(spec);
}
