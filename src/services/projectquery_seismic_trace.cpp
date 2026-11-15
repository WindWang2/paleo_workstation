// 层：数据
#include "projectquery_internal.h"
#include "pythonseismictrace.h"
#include "pythonseismictracequery.h"
#include <paleo_data/service.h>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
namespace paleo::query {
namespace {
bool samePath(const QString &a,const QString &b) {
#ifdef Q_OS_WIN
  return a.compare(b,Qt::CaseInsensitive)==0;
#else
  return a==b;
#endif
}
QString modelRoot(const DataCatalog *catalog) {
  const auto project=QFileInfo(catalog->projectDir()).canonicalFilePath();if(project.isEmpty())return {};
  const auto root=QDir(project).filePath("artifacts/metadata/area-model");const QFileInfo directory(root),database(root+"/data.sqlite");
  if(!directory.isDir()||directory.isSymLink()||directory.isJunction()||!samePath(directory.canonicalFilePath(),root)||
      !database.isFile()||database.isSymLink()||database.isJunction()||!samePath(database.canonicalFilePath(),root+"/data.sqlite"))return {};
  return directory.canonicalFilePath();
}
}
QJsonObject ProjectQueryCapability::Impl::seismicTraceDescriptor(QueryState &state,const QJsonObject &arguments,int bytes,bool &more) {
  more=false;document::SourceRef reference;qlonglong volumeId=0;
  const auto invalid=python::seismicTraceDescriptorQueryArguments(arguments,reference,volumeId);if(!invalid.isEmpty())return queryFailure(invalid);
  if(reference.documentId!=state.scope.documentId)return queryFailure("source_document_mismatch");
  const auto owner=catalog;const auto resolver=sourceResolver;const auto root=modelRoot(owner);if(root.isEmpty())return queryFailure("python_seismic_model_unavailable");
  if(!resolver)return queryFailure("source_resolver_unavailable");document::SourceRecord source;
  try{source=resolver(reference);}catch(...){return queryFailure("source_resolver_failed");}
  if(!owner||!owner->isOpen())return queryFailure("document_unavailable");
  if(scope!=state.scope||catalog!=owner||bindingEpoch!=state.bindingEpoch)return queryFailure("cursor_scope_mismatch");
  const auto version=owner->versionById(source.versionId);const auto asset=owner->assetById(source.assetId);
  if(source.ref!=reference||source.versionId.isEmpty()||source.assetId.isEmpty()||version.id!=source.versionId||version.assetId!=source.assetId||asset.id!=source.assetId||
      version.extra.value("areaModelSourceId").toLongLong()!=reference.sourceId||source.fingerprint.kind!=document::FingerprintKind::Sha256||
      source.fingerprint.stored()!=reference.fileRevisionId||document::SourceFingerprint::fromStored(version.sha256).stored()!=reference.fileRevisionId||
      !samePath(QFileInfo(source.modelDirectory).canonicalFilePath(),root))return queryFailure("python_seismic_source_mapping_unavailable");
  data::DataService sdk;std::string detail;if(!sdk.openProjectReadOnly(root.toUtf8().toStdString(),detail))return queryFailure("python_seismic_model_unavailable");
  data::SeismicWindowReadOptions budget;budget.metadata.maxFieldBytes=qMin(4096,bytes);budget.metadata.maxTextBytes=qMin(8192,bytes);
  data::SeismicWindowDescriptor descriptor;data::SeismicWindowReadStats stats;
  if(!sdk.describeSeismicWindow(volumeId,descriptor,detail,budget,&stats))return queryFailure(
    detail.find("Budget")!=std::string::npos?"query_response_budget_exceeded":
      detail=="SeismicWindowAxisUndeclared"?"python_seismic_axis_required":"python_seismic_metadata_unavailable");
  if(stats.requestedSourceBytes||stats.outputBytes||stats.sourceReadCalls||stats.metadataBytesRead<0||stats.metadataBytesRead>budget.metadata.maxTextBytes)
    return queryFailure("python_seismic_metadata_payload_violation");
  if(descriptor.cut.volumeId!=volumeId||descriptor.cut.sourceId!=reference.sourceId||
      (descriptor.registeredSourceFingerprint.size()==64&&QString::fromStdString(descriptor.registeredSourceFingerprint)!=reference.fileRevisionId))
    return queryFailure("python_seismic_source_mismatch");
  const auto cut=python::seismicTraceReadCutJson(descriptor.cut);const auto model=cut.value("model").toObject();
  state.modelToken=QString::fromUtf8(QJsonDocument(QJsonObject{{"store",model.value("storeId")},{"generation",model.value("generation")},
    {"sequence",model.value("sequence")}}).toJson(QJsonDocument::Compact));
  python::SeismicTraceSelection selection;selection.source=reference;selection.volumeId=volumeId;selection.volumeRevision=QString::fromStdString(descriptor.cut.volumeRevision);
  selection.traceIndex=0;selection.sampleCount=qMin(data::SeismicWindowMaxSamples,descriptor.totalSamples);
  selection.amplitudeUnit=QString::fromStdString(descriptor.amplitudeUnit);selection.timeUnit=QString::fromStdString(descriptor.timeUnit);
  selection.timeType=QString::fromStdString(descriptor.timeType);selection.timeReference=QString::fromStdString(descriptor.timeReference);
  return {{"ok",true},{"code","ok"},{"contract","paleo.project.query.page.v1"},{"tool","paleo.query_project"},{"topic","seismic_trace_descriptor"},
    {"metadataOnly",true},{"count",1},{"returnedCount",1},{"numericPayloadBytes",0},{"sourceIntegrity","not_checked_metadata_only"},
    {"input_ref",python::seismicTraceSelectionJson(selection)},{"readCut",cut},{"source_version_id",source.versionId},
    {"descriptor",QJsonObject{{"volume_id",QString::number(volumeId)},{"volume_revision",selection.volumeRevision},{"total_samples",descriptor.totalSamples},
      {"first_time",descriptor.firstTime},{"sample_interval",descriptor.sampleInterval},{"time_unit",selection.timeUnit},{"time_type",selection.timeType},
      {"time_reference",selection.timeReference},{"amplitude_unit",selection.amplitudeUnit},{"supported_scope","segy_rev1_fixed_big_endian_ieee32_exact_file_trace_ordinal"}}},
    {"metadataTextBytes",QString::number(stats.metadataBytesRead)}};
}
}
