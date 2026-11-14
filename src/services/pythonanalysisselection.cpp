// 层：数据
#include "pythonanalysisselection.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <cmath>

namespace paleo::python {
namespace {
bool keys(const QJsonObject &object,std::initializer_list<const char *> expected) {
  if(object.size()!=qsizetype(expected.size()))return false;
  for(const auto *key:expected)if(!object.contains(QLatin1String(key)))return false;return true;
}
bool text(const QJsonValue &value) {
  return value.isString() && value.toString().size()<=256 && !value.toString().trimmed().isEmpty() &&
    !value.toString().contains(QChar(0)) && value.toString().toUtf8().size()<=256;
}
bool decimal(const QJsonValue &value,qlonglong &number,bool positive=false) {
  bool parsed=false;if(!value.isString())return false;
  number=value.toString().toLongLong(&parsed);
  return parsed && number>=(positive?1:0) && QString::number(number)==value.toString();
}
QJsonObject object(QJsonObject properties) {
  QJsonArray required;for(const auto &name:properties.keys())required.append(name);
  return {{"type","object"},{"properties",properties},{"required",required},{"additionalProperties",false}};
}
QJsonObject string() {return {{"type","string"},{"minLength",1},{"maxLength",256}};}
QJsonObject decimalSchema(bool positive=false) {
  return {{"type","string"},{"pattern",positive?"^[1-9][0-9]{0,18}$":"^(0|[1-9][0-9]{0,18})$"}};
}
QJsonObject sourceSchema() {
  return object({{"documentId",string()},{"modelStoreId",QJsonObject{{"const","area-model"}}},
    {"sourceId",decimalSchema(true)},{"fileRevisionId",QJsonObject{{"type","string"},{"pattern","^[0-9a-f]{64}$"}}}});
}
QJsonObject wellSchema() {
  return object({{"contract_version",QJsonObject{{"const",1}}},{"document_id",string()},{"catalog_entity_id",string()},
    {"model_store_id",QJsonObject{{"const","area-model"}}},{"model_store_identity",string()},{"model_generation",string()},
    {"model_well_id",decimalSchema(true)},{"model_head",decimalSchema()},{"model_available",QJsonObject{{"const",true}}}});
}
QJsonObject selectedInputs(AnalysisKind kind) {
  const auto curve=object({{"source_ref",sourceSchema()},{"source_version_id",string()},{"well_id",decimalSchema(true)},
    {"log_set_id",decimalSchema(true)},{"curve_id",decimalSchema(true)},{"unit",string()},
    {"depth_type",QJsonObject{{"const","MD"}}},{"depth_unit",QJsonObject{{"type","string"},{"enum",QJsonArray{"m","ft","M","FT"}}}},
    {"depth_reference",string()},{"total_samples",decimalSchema()}});
  return object({{"well_ref",wellSchema()},{"curves",QJsonObject{{"type","array"},{"minItems",kind==AnalysisKind::WellQuality?1:2},
    {"maxItems",AnalysisMaxChannels},{"items",curve}}},{"first_sample",decimalSchema()},
    {"sample_count",QJsonObject{{"type","integer"},{"minimum",0},{"maximum",AnalysisMaxSamples}}},
    {"processed",QJsonObject{{"type","boolean"}}},{"alignment",QJsonObject{{"const","exact_shared_axis_v1"}}},
    {"sampling",object({{"method",QJsonObject{{"const","all_selected_window_v1"}}},{"seed",QJsonObject{{"const",0}}}})}});
}
}
bool analysisSelectionFromJson(const QJsonObject &inputs,AnalysisKind kind,AnalysisSelectionRequest &out,QString *error) {
  out={};const auto fail=[&](const QString &code){if(error)*error=code;out={};return false;};
  if(!keys(inputs,{"well_ref","curves","first_sample","sample_count","processed","alignment","sampling"}))return fail("invalid_python_analysis_selection");
  const auto wellJson=inputs.value("well_ref").toObject();
  if(wellJson.size()!=9)return fail("invalid_well_ref");
  for(const auto &key:{"document_id","catalog_entity_id","model_store_id","model_store_identity","model_generation","model_well_id","model_head"})
    if(!text(wellJson.value(QLatin1String(key))))return fail("invalid_well_ref");
  if(!inputs.value("well_ref").isObject() || !document::wellRefFromJson(inputs.value("well_ref").toObject(),out.well) ||
      !out.well.modelAvailable || document::wellRefJson(out.well)!=inputs.value("well_ref").toObject())return fail("invalid_well_ref");
  qlonglong first=0;
  const auto count=inputs.value("sample_count");
  if(!decimal(inputs.value("first_sample"),first) || !count.isDouble() || !std::isfinite(count.toDouble()) ||
      count.toDouble()!=std::floor(count.toDouble()) || count.toDouble()<0 || count.toDouble()>AnalysisMaxSamples ||
      !inputs.value("processed").isBool())return fail("python_analysis_window_out_of_range");
  const auto sampling=inputs.value("sampling").toObject();
  if(inputs.value("alignment")!="exact_shared_axis_v1" || !keys(sampling,{"method","seed"}) ||
      sampling.value("method")!="all_selected_window_v1" || !sampling.value("seed").isDouble() || sampling.value("seed").toDouble()!=0)
    return fail("python_analysis_sampling_unsupported");
  out.alignment="exact_shared_axis_v1";out.sampling="all_selected_window_v1";
  if(!inputs.value("curves").isArray())return fail("python_analysis_selection_invalid");
  const auto curves=inputs.value("curves").toArray();
  if(curves.size()<(kind==AnalysisKind::WellQuality?1:2) || curves.size()>AnalysisMaxChannels)return fail("python_analysis_channel_budget");
  QSet<qlonglong> seen;
  for(const auto &value:curves) {
    const auto ref=value.toObject();AnalysisCurveSelection selected;qlonglong total=0;
    if(!value.isObject() || !keys(ref,{"source_ref","source_version_id","well_id","log_set_id","curve_id","unit","depth_type","depth_unit","depth_reference","total_samples"}) ||
        !text(ref.value("source_version_id")) || !ref.value("source_ref").isObject() ||
        !document::sourceRefFromJson(ref.value("source_ref").toObject(),selected.source.ref) ||
        document::sourceRefJson(selected.source.ref)!=ref.value("source_ref").toObject() ||
        selected.source.ref.documentId!=out.well.documentId || selected.source.ref.modelStoreId!="area-model")return fail("python_analysis_source_mismatch");
    static const QRegularExpression sha("^[0-9a-f]{64}$");
    if(selected.source.ref.fileRevisionId.size()!=64 || !sha.match(selected.source.ref.fileRevisionId).hasMatch() || !decimal(ref.value("well_id"),selected.wellId,true) ||
        !decimal(ref.value("log_set_id"),selected.logSetId,true) || !decimal(ref.value("curve_id"),selected.curveId,true) ||
        !decimal(ref.value("total_samples"),total) || selected.wellId!=out.well.modelWellId || seen.contains(selected.curveId))
      return fail("python_analysis_selection_invalid");
    if(!text(ref.value("unit")) || ref.value("depth_type")!="MD" ||
        !QStringList{"m","ft","M","FT"}.contains(ref.value("depth_unit").toString()) || !text(ref.value("depth_reference")))
      return fail("python_analysis_semantics_required");
    if(first>total || count.toDouble()>double(total-first))return fail("python_analysis_window_out_of_range");
    seen.insert(selected.curveId);selected.sourceVersionId=ref.value("source_version_id").toString();
    selected.unit=ref.value("unit").toString();selected.depthType="MD";selected.depthUnit=ref.value("depth_unit").toString();
    selected.depthReference=ref.value("depth_reference").toString();selected.firstSample=first;selected.sampleCount=count.toInt();
    selected.processed=inputs.value("processed").toBool();out.curves.append(selected);
  }
  if(error)error->clear();return true;
}
QJsonObject analysisToolRequestSchema(AnalysisKind kind) {
  auto schema=object({{"client_request_id",string()},{"binding_mode",QJsonObject{{"const","pinned-value"}}},
    {"parameters",object({})},{"inputs",QJsonObject{{"oneOf",QJsonArray{selectedInputs(kind),object({{"source_operation_id",string()}})}}}}});
  auto properties=schema.value("properties").toObject();properties.insert("contract_version",QJsonObject{{"const",1}});
  properties.insert("publish_intent",QJsonObject{{"type","string"},{"enum",QJsonArray{"prepare","compute-only","publish"}}});
  schema.insert("properties",properties);
  schema.insert("allOf",QJsonArray{QJsonObject{{"if",QJsonObject{{"required",QJsonArray{"publish_intent"}},
    {"properties",QJsonObject{{"publish_intent",QJsonObject{{"const","publish"}}}}}}},
    {"then",QJsonObject{{"properties",QJsonObject{{"inputs",object({{"source_operation_id",string()}})}}}}},
    {"else",QJsonObject{{"properties",QJsonObject{{"inputs",selectedInputs(kind)}}}}}}});return schema;
}
QString validateAnalysisToolArguments(const QJsonObject &args,AnalysisKind kind) {
  const QStringList allowed{"client_request_id","binding_mode","parameters","inputs","contract_version","publish_intent"};
  for(const auto &key:args.keys())if(!allowed.contains(key))return "unknown_python_analysis_field";
  if(!text(args.value("client_request_id")) || args.value("binding_mode")!="pinned-value" ||
      (args.contains("contract_version") && args.value("contract_version")!=QJsonValue(1)) ||
      !args.value("parameters").isObject() || !args.value("parameters").toObject().isEmpty() || !args.value("inputs").isObject())
    return "invalid_python_analysis_request";
  const auto phase=args.value("publish_intent").toString("compute-only");
  if(args.contains("publish_intent") && !args.value("publish_intent").isString())return "unsupported_intent";
  if(!QStringList{"prepare","compute-only","publish"}.contains(phase))return "unsupported_intent";
  const auto inputs=args.value("inputs").toObject();
  if(phase=="publish")return keys(inputs,{"source_operation_id"}) && text(inputs.value("source_operation_id"))?QString():QStringLiteral("source_operation_required");
  AnalysisSelectionRequest selection;QString error;analysisSelectionFromJson(inputs,kind,selection,&error);return error;
}
} // namespace paleo::python
