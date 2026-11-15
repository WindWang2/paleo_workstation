// 层：数据
#include "pythonseismictrace_internal.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QtEndian>
#include <bit>
#include <cmath>
#include <limits>
namespace paleo::python {
QJsonObject seismicTraceReadCutJson(const data::SeismicWindowReadCut &cut) {
  return {{"contract","seismic-window-read-cut.v1"},{"model",QJsonObject{{"storeId",QString::fromStdString(cut.model.storeId)},
    {"generation",QString::fromStdString(cut.model.generation)},{"sequence",QString::number(cut.model.sequence)},{"available",cut.model.available}}},
    {"volumeId",QString::number(cut.volumeId)},{"sourceId",QString::number(cut.sourceId)},{"volumeRevision",QString::fromStdString(cut.volumeRevision)},
    {"boundary","observed_sqlite_read_snapshot_and_authenticated_source_ranges_not_history_lease"}};
}
namespace seismic_trace_detail {
QByteArray payload(const data::SeismicWindow &window,QString &error) {
  error.clear();const auto rows=window.axis.size();const auto fail=[&]{error="python_seismic_value_invalid";return QByteArray{};};
  if(rows<1||rows>data::SeismicWindowMaxSamples||window.amplitudes.size()!=rows||window.nullMask.size()!=rows)return fail();
  QByteArray bytes;bytes.reserve(qsizetype(rows)*17);
  for(size_t i=0;i<rows;++i) {
    const auto expected=window.descriptor.firstTime+(window.firstSample+int(i))*window.descriptor.sampleInterval;
    if(!std::isfinite(window.axis[i])||window.axis[i]!=expected)return fail();
    char raw[8];qToLittleEndian<quint64>(std::bit_cast<quint64>(window.axis[i]),raw);bytes.append(raw,8);
  }
  for(size_t i=0;i<rows;++i) {
    if(!std::isfinite(window.amplitudes[i])||window.nullMask[i]>1||(window.nullMask[i]&&window.amplitudes[i]!=0))return fail();
    char raw[8];qToLittleEndian<quint64>(std::bit_cast<quint64>(window.amplitudes[i]),raw);bytes.append(raw,8);
  }
  bytes.append(reinterpret_cast<const char*>(window.nullMask.data()),qsizetype(rows));return bytes;
}
QJsonObject metadata(const SeismicTraceReadPlan &plan,const data::SeismicWindow &window,const QJsonObject &metrics,const QByteArray &bytes) {
  const auto n=int(window.axis.size());const auto &d=window.descriptor;
  const QJsonObject axis{{"id","time"},{"kind","time"},{"unit",QString::fromStdString(d.timeUnit)},{"type",QString::fromStdString(d.timeType)},
    {"reference",QString::fromStdString(d.timeReference)},{"dtype","<f8"},{"first",window.axis.front()},{"interval",d.sampleInterval},
    {"valueOffset",0},{"valueBytes",n*8}};
  const QJsonObject column{{"id","amplitude"},{"label","Amplitude"},{"unit",QString::fromStdString(d.amplitudeUnit)},{"dtype","<f8"},
    {"valueOffset",n*8},{"valueBytes",n*8},{"nullOffset",n*16},{"nullBytes",n},{"nullRule","nonfinite_ieee32_is_null_with_zero_payload"}};
  const QJsonObject provenance{{"documentId",plan.documentId},{"callerScope",plan.callerScope},
    {"sources",QJsonArray{QJsonObject{{"source",document::sourceRefJson(plan.selection.source)},{"sourceVersionId",plan.source.versionId},
      {"assetId",plan.source.assetId},{"byteSize",QString::number(plan.authenticatedSource.byteSize)}}}},
    {"readCut",seismicTraceReadCutJson(d.cut)},{"selection",seismicTraceSelectionJson(plan.selection)},
    {"declaredSampling",QJsonObject{{"firstTime",d.firstTime},{"sampleInterval",d.sampleInterval},{"totalSamples",d.totalSamples}}},
    {"trace",QJsonObject{{"index",QString::number(window.traceIndex)},{"number",QString::number(window.traceNumber)},
      {"inline",window.inlineNumber},{"crossline",window.crosslineNumber}}},{"axis",axis},{"readMetrics",metrics}};
  return {{"contract",QLatin1String(SeismicTraceInputContract)},{"inputProtocol","paleo.python-analysis-frame.v1"},
    {"analysis","generated_seismic_trace"},{"sourceKind","authenticated_seismic_trace_window"},{"shape",QJsonArray{n,1}},
    {"columns",QJsonArray{column}},{"axis",axis},{"layout",QJsonObject{{"order","axis_f64_values_f64_nullmask_u8"},{"littleEndian",true},{"conversion","ieee32_to_f64_no_unit_conversion"}}},
    {"payload",QJsonObject{{"bytes",bytes.size()},{"sha256",hash(bytes)}}},{"provenance",provenance}};
}
namespace {
bool integer(const QJsonValue &value,int minimum,int maximum,int &out) {
  if(!value.isDouble()||!std::isfinite(value.toDouble())||value.toDouble()!=std::floor(value.toDouble())||
      value.toDouble()<minimum||value.toDouble()>maximum)return false;out=value.toInt();return true;
}
bool finite(const QJsonValue &value,double &number) {
  if(!value.isDouble()||!std::isfinite(value.toDouble()))return false;number=value.toDouble();return true;
}
bool metrics(const QJsonObject &value,int n,qlonglong sourceSize) {
  if(!keys(value,{"metadataTextBytes","sdkRequestedSourceBytes","sdkSourceReadCalls","typedOutputBytes","sdkReservedBytes",
      "hostNumericPreparationReservedBytes","authenticatedFullScans","authenticatedFullHashBytes","authenticatedChunkHashBytes",
      "authenticatedReadBytes","cachedDigestBytes","measurement"})||value.value("measurement")!="sdk_requested_ranges_and_actual_authentication_io_not_process_rss")return false;
  qlonglong metadata=0,requested=0,calls=0,output=0,reserved=0,host=0,scans=0,full=0,chunks=0,read=0,index=0;
  return decimal(value.value("metadataTextBytes"),metadata)&&metadata<=16384&&decimal(value.value("sdkRequestedSourceBytes"),requested)&&requested==640+4*n&&
    decimal(value.value("sdkSourceReadCalls"),calls)&&calls==3&&decimal(value.value("typedOutputBytes"),output)&&output==17*n&&
    decimal(value.value("sdkReservedBytes"),reserved)&&reserved<=2*1024*1024&&decimal(value.value("hostNumericPreparationReservedBytes"),host,true)&&host<=512LL*1024*1024&&
    decimal(value.value("authenticatedFullScans"),scans)&&scans<=1&&decimal(value.value("authenticatedFullHashBytes"),full)&&full==(scans?sourceSize:0)&&
    decimal(value.value("authenticatedChunkHashBytes"),chunks)&&decimal(value.value("authenticatedReadBytes"),read)&&chunks==read&&
    read>=requested&&read>=full&&read<=sourceSize+6*1024*1024&&decimal(value.value("cachedDigestBytes"),index)&&index<=4*1024*1024;
}
}
}
bool validateSeismicTraceInputMetadata(const QJsonObject &metadata) {
  using namespace seismic_trace_detail;
  if(QJsonDocument(metadata).toJson(QJsonDocument::Compact).size()>65536||
      !keys(metadata,{"contract","inputProtocol","analysis","sourceKind","shape","columns","axis","layout","payload","provenance"})||
      metadata.value("contract")!=QLatin1String(SeismicTraceInputContract)||metadata.value("inputProtocol")!="paleo.python-analysis-frame.v1"||
      metadata.value("analysis")!="generated_seismic_trace"||metadata.value("sourceKind")!="authenticated_seismic_trace_window")return false;
  const auto provenance=metadata.value("provenance").toObject();
  if(!keys(provenance,{"documentId","callerScope","sources","readCut","selection","declaredSampling","trace","axis","readMetrics"})||
      !text(provenance.value("documentId"),256)||!text(provenance.value("callerScope"),256)||!provenance.value("sources").isArray()||provenance.value("sources").toArray().size()!=1)return false;
  SeismicTraceSelection selected;if(!seismicTraceSelectionFromJson(provenance.value("selection").toObject(),selected)||selected.source.documentId!=provenance.value("documentId"))return false;
  const auto source=provenance.value("sources").toArray().first().toObject();qlonglong size=0;
  if(!keys(source,{"source","sourceVersionId","assetId","byteSize"})||source.value("source")!=QJsonValue(document::sourceRefJson(selected.source))||
      !text(source.value("sourceVersionId"),256)||!text(source.value("assetId"),256)||!decimal(source.value("byteSize"),size)||size<3844||size>artifacts::AuthenticatedArtifactReader::MaxSeismicSourceBytes)return false;
  const auto cut=provenance.value("readCut").toObject(),model=cut.value("model").toObject();qlonglong sequence=0;
  if(!keys(cut,{"contract","model","volumeId","sourceId","volumeRevision","boundary"})||cut.value("contract")!="seismic-window-read-cut.v1"||
      cut.value("boundary")!="observed_sqlite_read_snapshot_and_authenticated_source_ranges_not_history_lease"||
      !keys(model,{"storeId","generation","sequence","available"})||!text(model.value("storeId"),64)||!text(model.value("generation"),64)||
      !decimal(model.value("sequence"),sequence)||model.value("available")!=true||cut.value("volumeId")!=QString::number(selected.volumeId)||
      cut.value("sourceId")!=QString::number(selected.source.sourceId)||cut.value("volumeRevision")!=selected.volumeRevision)return false;
  const auto shape=metadata.value("shape").toArray();int n=0,c=0;
  if(!metadata.value("shape").isArray()||shape.size()!=2||!integer(shape[0],1,8192,n)||n!=selected.sampleCount||!integer(shape[1],1,1,c))return false;
  const auto sampling=provenance.value("declaredSampling").toObject();double first=0,interval=0;int total=0;
  if(!keys(sampling,{"firstTime","sampleInterval","totalSamples"})||!finite(sampling.value("firstTime"),first)||!finite(sampling.value("sampleInterval"),interval)||interval<=0||
      !integer(sampling.value("totalSamples"),1,65535,total)||selected.firstSample>=total||n>total-selected.firstSample)return false;
  const auto start=first+selected.firstSample*interval;if(!std::isfinite(start)||!std::isfinite(first+(selected.firstSample+n-1)*interval))return false;
  const QJsonObject axis{{"id","time"},{"kind","time"},{"unit",selected.timeUnit},{"type",selected.timeType},{"reference",selected.timeReference},
    {"dtype","<f8"},{"first",start},{"interval",interval},{"valueOffset",0},{"valueBytes",n*8}};
  const QJsonObject column{{"id","amplitude"},{"label","Amplitude"},{"unit",selected.amplitudeUnit},{"dtype","<f8"},{"valueOffset",n*8},
    {"valueBytes",n*8},{"nullOffset",n*16},{"nullBytes",n},{"nullRule","nonfinite_ieee32_is_null_with_zero_payload"}};
  if(metadata.value("axis")!=QJsonValue(axis)||provenance.value("axis")!=QJsonValue(axis)||metadata.value("columns")!=QJsonValue(QJsonArray{column})||
      metadata.value("layout")!=QJsonValue(QJsonObject{{"order","axis_f64_values_f64_nullmask_u8"},{"littleEndian",true},{"conversion","ieee32_to_f64_no_unit_conversion"}}))return false;
  const auto trace=provenance.value("trace").toObject();int inlineNumber=0,crossline=0;bool signedValid=false;
  const auto number=trace.value("number").toString().toLongLong(&signedValid);
  if(!keys(trace,{"index","number","inline","crossline"})||trace.value("index")!=QString::number(selected.traceIndex)||!trace.value("number").isString()||
      !signedValid||QString::number(number)!=trace.value("number").toString()||number<std::numeric_limits<int>::min()||number>std::numeric_limits<int>::max()||
      !integer(trace.value("inline"),std::numeric_limits<int>::min(),std::numeric_limits<int>::max(),inlineNumber)||
      !integer(trace.value("crossline"),std::numeric_limits<int>::min(),std::numeric_limits<int>::max(),crossline))return false;
  const auto bytes=metadata.value("payload").toObject();
  return keys(bytes,{"bytes","sha256"})&&bytes.value("bytes")==17*n&&digest(bytes.value("sha256"))&&
    metrics(provenance.value("readMetrics").toObject(),n,size);
}
}
