// 层：功能
#include "tileoperation.h"
#include "ai/onnxpredictionservice.h"
#include "ai/tileinference.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <cmath>
namespace paleo {
namespace {
bool reject(QString *error,const QString &code) { if(error)*error=code;return false; }
bool knownSemantics(const QString &value) {
  return value==QLatin1String("logits") || value==QLatin1String("probabilities");
}
QString schemaHash(const FrozenTilePlan &plan) {
  const QJsonObject schema{{"channelCount",1},{"inputSignature",plan.inputSignature},
    {"outputSemantics",plan.outputSemantics},{"classes",plan.classCount},{"bandUnit",plan.raster.bandUnit},{"missingInput","zero-with-invalid-mask"}};
  auto contract=schema;
  contract.insert("featureContractVersion",plan.featureContractVersion);contract.insert("modelFeatures",plan.modelFeatures);
  contract.insert("sourceFeature",plan.raster.featureContract);
  return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(contract).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
}
bool modelFeature(const QJsonArray &features,int version,QJsonObject &out) {
  if(version!=1 || features.size()!=1 || QJsonDocument(features).toJson(QJsonDocument::Compact).size()>8192)return false;
  auto feature=features[0].toObject();
  if(feature.contains("version") && feature.value("version")!=QJsonValue(version))return false;
  feature.insert("version",version);
  return normalizeRasterFeatureContract(feature,out);
}
bool parametersFor(const QVariantMap &input,QVariantMap &out,QString *error,bool applyDefaults=true) {
  const auto spec=AlgorithmSpecRegistry::specAtVersion("onnx:tile_classification",2);
  for(auto it=input.cbegin();it!=input.cend();++it) if(!spec.paramKeys().contains(it.key()))
    return reject(error,"unknown_tile_parameter");
  for(const auto &field:spec.params) {
    if(!applyDefaults && !input.contains(field.key))return reject(error,"incomplete_frozen_tile_parameters");
    const auto value=input.value(field.key,field.defaultValue);
    const int type=value.metaType().id();
    if(type!=QMetaType::Int && type!=QMetaType::Double && type!=QMetaType::LongLong &&
       type!=QMetaType::UInt && type!=QMetaType::ULongLong)return reject(error,"invalid_tile_parameter_type");
    bool ok=false;const double number=value.toDouble(&ok);
    if(!ok || !std::isfinite(number) || (field.type==AlgorithmParamField::Int && number!=std::floor(number)) ||
       (field.hasMin && number<field.minValue) || (field.hasMax && number>field.maxValue))
      return reject(error,"invalid_tile_parameter");
    out.insert(field.key,field.type==AlgorithmParamField::Int?QVariant(int(number)):QVariant(number));
  }
  return !spec.id.isEmpty();
}
}
QJsonObject FrozenTilePlan::toJson() const {
  return {{"schema","paleo.tile-plan.v2"},{"raster",raster.toJson()},{"model",model},{"modelSha256",modelSha256},
    {"featureSchemaHash",featureSchemaHash},{"inputSignature",inputSignature},{"outputSemantics",outputSemantics},
    {"classes",classCount},{"parameters",QJsonObject::fromVariantMap(parameters)},
    {"featureContractVersion",featureContractVersion},{"modelFeatures",modelFeatures}};
}
bool FrozenTilePlan::fromJson(const QJsonObject &json,FrozenTilePlan &out,QString *error) {
  out={};
  if(json.value("schema")!="paleo.tile-plan.v2" ||
     !FrozenRasterInput::fromJson(json.value("raster").toObject(),out.raster,error))return false;
  out.model=json.value("model").toString();out.modelSha256=json.value("modelSha256").toString();
  out.featureSchemaHash=json.value("featureSchemaHash").toString();out.inputSignature=json.value("inputSignature").toString();
  out.outputSemantics=json.value("outputSemantics").toString();
  out.classCount=json.value("classes").toInt();
  out.featureContractVersion=json.value("featureContractVersion").toInt();out.modelFeatures=json.value("modelFeatures").toArray();
  QJsonObject feature;
  if(out.model.isEmpty() || out.modelSha256.size()!=64 || out.inputSignature.isEmpty() ||
     !modelFeature(out.modelFeatures,out.featureContractVersion,feature) || feature!=out.raster.featureContract ||
     !knownSemantics(out.outputSemantics) ||
     out.classCount<1 || out.classCount>255 || qint64(out.raster.rows)*out.raster.columns*out.classCount>32*1024*1024 ||
     !parametersFor(json.value("parameters").toObject().toVariantMap(),out.parameters,error,false) ||
     schemaHash(out)!=out.featureSchemaHash)return reject(error,"invalid_frozen_tile_plan");
  return true;
}
bool prepareTilePlan(PaleoOnnxService *onnx,const QString &model,const FrozenRasterInput &source,
                     const QVariantMap &parameters,FrozenTilePlan &out,
                     const std::function<bool()> &cancelled,QString *error) {
  out={};
#if !PALEO_HAVE_ORT
  Q_UNUSED(onnx);Q_UNUSED(model);Q_UNUSED(source);Q_UNUSED(parameters);Q_UNUSED(cancelled);
  return reject(error,"ort_unavailable");
#else
  if(cancelled && cancelled())return reject(error,"cancelled");
  if(!onnx)return reject(error,"ort_unavailable");
  if(!freezeRasterInput(source.path,source.versionId,source.sha256,out.raster,cancelled,error,source.catalogFeatureContract))return false;
  if(!parametersFor(parameters,out.parameters,error))return false;
  OnnxModelMeta meta;
  QString loadError;const auto status=onnx->loadModelMeta(model,&meta,&loadError);
  if(status!=OnnxLoadStatus::Ok)return reject(error,onnxPublicLoadCode(status,loadError));
  if(meta.inputShape.size()!=4 || meta.inputShape[0]>1 || meta.inputShape[1]>1)
    return reject(error,"tile_model_signature_incompatible");
  if(meta.inputElemType!=1)return reject(error,"tile_model_input_type_unsupported");
  const auto outputCode=tileOutputContractCode(meta,meta.classes);
  if(!outputCode.isEmpty())return reject(error,outputCode);
  QJsonObject feature;
  if(!modelFeature(meta.features,meta.featureContractVersion,feature))return reject(error,"model_feature_contract_unavailable");
  if(feature!=out.raster.featureContract)return reject(error,"feature_contract_mismatch");
  if(meta.outputSemantics==QLatin1String("classes") || meta.outputSemantics==QLatin1String("class_index"))
    return reject(error,"tile_confidence_output_unavailable");
  if(!knownSemantics(meta.outputSemantics))return reject(error,"tile_model_semantics_unknown");
  if(meta.classes<1 || meta.classes>255 || qint64(out.raster.rows)*out.raster.columns*meta.classes>32*1024*1024)
    return reject(error,"tile_model_output_contract_or_budget_invalid");
  out.model=model;out.modelSha256=meta.sha256;out.inputSignature=meta.inputSignature;
  out.outputSemantics=meta.outputSemantics;out.classCount=meta.classes;
  out.featureContractVersion=meta.featureContractVersion;out.modelFeatures=meta.features;out.featureSchemaHash=schemaHash(out);
  return !(cancelled && cancelled()) || reject(error,"cancelled");
#endif
}
}
