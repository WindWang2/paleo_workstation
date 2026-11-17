// 层：数据
#include "pythonseismictrace_internal.h"
#include "pythongenerated.h"
#include <QJsonArray>
#include <QCryptographicHash>
#include <cmath>
namespace paleo::python::seismic_trace_detail {
bool digest(const QJsonValue &value) {
  if(!value.isString()||value.toString().size()!=64)return false;
  for(const auto c:value.toString())if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;return true;
}
bool text(const QJsonValue &value,int maximum,bool declared) {
  if(!value.isString())return false;const auto string=value.toString();const auto bytes=string.toUtf8();
  return !string.trimmed().isEmpty()&&bytes.size()<=maximum&&!string.contains(QChar(0))&&QString::fromUtf8(bytes)==string&&
    (!declared||!QStringList{"unknown","unspecified","undefined","?","n/a"}.contains(string.trimmed().toCaseFolded()));
}
bool decimal(const QJsonValue &value,qlonglong &number,bool positive) {
  bool ok=false;if(!value.isString())return false;number=value.toString().toLongLong(&ok);
  return ok&&number>=(positive?1:0)&&QString::number(number)==value.toString();
}
bool keys(const QJsonObject &object,std::initializer_list<const char*> expected) {
  if(object.size()!=qsizetype(expected.size()))return false;for(const auto *key:expected)if(!object.contains(QLatin1String(key)))return false;return true;
}
QString hash(QByteArrayView bytes){return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
}
namespace paleo::python {
QJsonObject seismicTraceSelectionJson(const SeismicTraceSelection &value) {
  return {{"source_ref",document::sourceRefJson(value.source)},{"volume_id",QString::number(value.volumeId)},{"volume_revision",value.volumeRevision},
    {"trace_index",QString::number(value.traceIndex)},{"first_sample",value.firstSample},{"sample_count",value.sampleCount},
    {"amplitude_unit",value.amplitudeUnit},{"time_unit",value.timeUnit},{"time_type",value.timeType},{"time_reference",value.timeReference}};
}
bool seismicTraceSelectionFromJson(const QJsonObject &object,SeismicTraceSelection &out,QString *error) {
  using namespace seismic_trace_detail;out={};const auto fail=[&](const char *code){out={};if(error)*error=QLatin1String(code);return false;};
  if(!keys(object,{"source_ref","volume_id","volume_revision","trace_index","first_sample","sample_count","amplitude_unit","time_unit","time_type","time_reference"}))
    return fail("invalid_python_seismic_selection");
  const auto source=object.value("source_ref").toObject();
  if(!object.value("source_ref").isObject()||!document::sourceRefFromJson(source,out.source)||document::sourceRefJson(out.source)!=source||
      out.source.modelStoreId!="area-model"||out.source.sourceId<1||!text(out.source.documentId,256)||!digest(out.source.fileRevisionId))return fail("python_seismic_source_mismatch");
  if(!decimal(object.value("volume_id"),out.volumeId,true)||!decimal(object.value("trace_index"),out.traceIndex))return fail("python_seismic_trace_required");
  const auto revision=object.value("volume_revision");const auto prefix=QStringLiteral("seismic-window-descriptor.v1:");
  if(!revision.isString()||!revision.toString().startsWith(prefix)||!digest(revision.toString().mid(prefix.size())))return fail("python_seismic_revision_required");
  out.volumeRevision=revision.toString();
  const auto first=object.value("first_sample"),count=object.value("sample_count");
  if(!first.isDouble()||!std::isfinite(first.toDouble())||first.toDouble()!=std::floor(first.toDouble())||first.toDouble()<0||first.toDouble()>65534||
      !count.isDouble()||!std::isfinite(count.toDouble())||count.toDouble()!=std::floor(count.toDouble())||count.toDouble()<1||count.toDouble()>data::SeismicWindowMaxSamples)
    return fail("python_seismic_window_out_of_range");
  out.firstSample=first.toInt();out.sampleCount=count.toInt();
  if(!text(object.value("amplitude_unit"),128,true)||object.value("time_unit")!="ms"||
      (object.value("time_type")!="TWT"&&object.value("time_type")!="OWT")||!text(object.value("time_reference"),256,true))return fail("python_seismic_axis_required");
  out.amplitudeUnit=object.value("amplitude_unit").toString();out.timeUnit="ms";out.timeType=object.value("time_type").toString();out.timeReference=object.value("time_reference").toString();
  if(error)error->clear();return true;
}
namespace {
QJsonObject objectSchema(const QJsonObject &properties) {
  QJsonArray required;for(const auto &key:properties.keys())required.append(key);
  return {{"type","object"},{"properties",properties},{"required",required},{"additionalProperties",false}};
}
QJsonObject selectionSchema() {
  const QJsonObject decimal{{"type","string"},{"pattern","^(0|[1-9][0-9]{0,18})$"}};
  const auto source=objectSchema({{"documentId",QJsonObject{{"type","string"},{"minLength",1},{"maxLength",256}}},{"modelStoreId",QJsonObject{{"const","area-model"}}},
    {"sourceId",QJsonObject{{"type","string"},{"pattern","^[1-9][0-9]{0,18}$"}}},{"fileRevisionId",QJsonObject{{"type","string"},{"pattern","^[0-9a-f]{64}$"}}}});
  return objectSchema({{"source_ref",source},{"volume_id",QJsonObject{{"type","string"},{"pattern","^[1-9][0-9]{0,18}$"}}},
    {"volume_revision",QJsonObject{{"type","string"},{"pattern","^seismic-window-descriptor\\.v1:[0-9a-f]{64}$"}}},{"trace_index",decimal},
    {"first_sample",QJsonObject{{"type","integer"},{"minimum",0},{"maximum",65534}}},{"sample_count",QJsonObject{{"type","integer"},{"minimum",1},{"maximum",8192}}},
    {"amplitude_unit",QJsonObject{{"type","string"},{"minLength",1},{"maxLength",128}}},{"time_unit",QJsonObject{{"const","ms"}}},
    {"time_type",QJsonObject{{"enum",QJsonArray{"TWT","OWT"}}}},{"time_reference",QJsonObject{{"type","string"},{"minLength",1},{"maxLength",256}}}});
}
}
QJsonObject seismicTraceToolRequestSchema() {
  auto schema=generatedToolRequestSchema();auto properties=schema.value("properties").toObject();
  const auto publish=objectSchema({{"source_operation_id",QJsonObject{{"type","string"},{"minLength",1},{"maxLength",256}}}});
  const auto input=selectionSchema();properties["inputs"]=QJsonObject{{"oneOf",QJsonArray{input,publish}}};
  auto parameters=properties.value("parameters").toObject();auto choices=parameters.value("oneOf").toArray();auto coded=choices[0].toObject();
  auto fields=coded.value("properties").toObject();auto code=fields.value("code").toObject();
  code["description"]="UTF-8 Python source <=16KiB. stdin: uint32LE JSON length + paleo.python-seismic-trace-input.v1 metadata + binary: n little-endian float64 time-axis values, n float64 amplitudes, n uint8 null flags (1=null). shape=[n,1], explicit ms TWT/OWT reference and amplitude units. One exact trace/window; no paths, network, subprocesses, inference, installation or full-volume loading. Output exactly paleo.python-output.v1 REPORT paleo.python-generated-report.v1. Host binds authenticated source/ReadCut/code/runtime evidence.";
  fields["code"]=code;coded["properties"]=fields;choices[0]=coded;parameters["oneOf"]=choices;properties["parameters"]=parameters;schema["properties"]=properties;
  auto conditions=schema.value("allOf").toArray();auto phase=conditions[0].toObject();
  for(const auto &key:{QString("then"),QString("else")}) {
    auto branch=phase.value(key).toObject();auto selected=branch.value("properties").toObject();selected["inputs"]=key=="then"?publish:input;
    if(key=="else")selected["parameters"]=coded;branch["properties"]=selected;phase[key]=branch;
  }
  conditions[0]=phase;schema["allOf"]=conditions;return schema;
}
QString validateSeismicTraceToolArguments(const QJsonObject &arguments) {
  using namespace seismic_trace_detail;
  for(auto it=arguments.begin();it!=arguments.end();++it)if(!QStringList{"client_request_id","contract_version","binding_mode","publish_intent","inputs","parameters"}.contains(it.key()))return "invalid_python_seismic_request";
  if(!text(arguments.value("client_request_id"),256)||arguments.value("binding_mode")!="pinned-value"||!arguments.value("parameters").isObject()||!arguments.value("inputs").isObject())return "invalid_python_seismic_request";
  if(arguments.contains("contract_version")&&arguments.value("contract_version")!=1)return "unsupported_contract_version";
  if(arguments.contains("publish_intent")&&!arguments.value("publish_intent").isString())return "unsupported_intent";
  const auto phase=arguments.value("publish_intent").toString("compute-only");
  if(phase=="publish") {
    const auto inputs=arguments.value("inputs").toObject();return keys(inputs,{"source_operation_id"})&&text(inputs.value("source_operation_id"),256)&&
      arguments.value("parameters").toObject().isEmpty()?QString():QStringLiteral("source_operation_required");
  }
  if(phase!="prepare"&&phase!="compute-only")return "unsupported_intent";
  QByteArray code;const auto invalid=generatedSourceFromArguments(arguments,code);if(!invalid.isEmpty())return invalid;
  SeismicTraceSelection selection;QString error;return seismicTraceSelectionFromJson(arguments.value("inputs").toObject(),selection,&error)?QString():error;
}
}
