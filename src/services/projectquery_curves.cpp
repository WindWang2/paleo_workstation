// 层：数据
#include "projectquery_internal.h"
#include "domain/wellref.h"
#include <paleo_data/service.h>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStringDecoder>

namespace paleo::query {
namespace {
bool samePath(const QString &left,const QString &right) {
#ifdef Q_OS_WIN
  return left.compare(right,Qt::CaseInsensitive)==0;
#else
  return left==right;
#endif
}
QString modelRoot(const DataCatalog *catalog) {
  const auto project=QFileInfo(catalog->projectDir()).canonicalFilePath();if(project.isEmpty())return {};
  const auto root=QDir(project).filePath("artifacts/metadata/area-model");const QFileInfo directory(root),database(root+"/data.sqlite");
  if(!directory.isDir()||directory.isSymLink()||directory.isJunction()||!samePath(root,directory.canonicalFilePath())||
      !database.isFile()||database.isSymLink()||database.isJunction()||!samePath(root+"/data.sqlite",database.canonicalFilePath()))return {};
  return directory.canonicalFilePath();
}
QString checkHead(const data::DataService &service,const document::WellRef &ref) {
  data::ModelChangeHead head;std::string detail;
  if(!service.readModelChangeHead(head,detail)||!head.available)return "model_query_unavailable";
  if(QString::fromStdString(head.storeId)!=ref.modelStoreIdentity||QString::fromStdString(head.generation)!=ref.modelGeneration||head.sequence!=ref.queryHead)
    return "model_revision_changed";
  return {};
}
QString token(const document::WellRef &ref) {
  return QString::fromUtf8(QJsonDocument(QJsonObject{{"store",ref.modelStoreIdentity},{"generation",ref.modelGeneration},
    {"sequence",QString::number(ref.queryHead)}}).toJson(QJsonDocument::Compact));
}
bool bounded(const QString &text) {return !text.trimmed().isEmpty()&&text.toUtf8().size()<=256&&!text.contains(QChar(0));}
bool declared(const QString &text) {
  return bounded(text)&&
    !QStringList{"unknown","unspecified","undefined","?","n/a"}.contains(text.toCaseFolded());
}
bool sha256(const QString &value) {
  if(value.size()!=64)return false;for(const auto c:value)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;return true;
}
QString utf8(const std::string &value,bool &valid) {
  QStringDecoder decoder(QStringDecoder::Utf8);const QString result=decoder(QByteArrayView(value.data(),qsizetype(value.size())));
  valid=valid&&!decoder.hasError()&&!result.contains(QChar(0));return result;
}
struct CurveRow {QJsonObject value;QString error;};
CurveRow curveRow(const data::DataService &service,const data::DataEntry &entry,const document::WellRef &well,
    const QString &root,const QPointer<DataCatalog> &catalog,const ProjectQueryCapability::SourceResolver &resolver,qint64 &sourceBytes,qint64 &curveBytes) {
  CurveRow out;bool entryEncoding=true;
  out.value={{"curve_id",QString::number(entry.id)},{"log_set_id",QString::number(entry.parentId)},{"name",utf8(entry.name,entryEncoding)},
    {"unit",QJsonValue::Null},{"depth_type",QJsonValue::Null},{"depth_unit",QJsonValue::Null},{"depth_reference",QJsonValue::Null},
    {"total_samples",entry.count>=0?QJsonValue(QString::number(entry.count)):QJsonValue(QJsonValue::Null)}};
  data::SourceIndexEntry registered;document::SourceRef sourceRef;document::SourceRecord source;std::string detail;
  data::SourceIndexReadOptions sourceOptions;sourceOptions.maxFieldBytes=8192;sourceOptions.maxTextBytes=qMin<qint64>(8192,128*1024-sourceBytes);
  data::SourceIndexReadStats sourceStats;
  const bool sourceRead=entry.sourceId>0&&service.readSourceIndexEntry(entry.sourceId,registered,detail,sourceOptions,&sourceStats);
  if(sourceStats.textBytesRead<0||sourceStats.textBytesRead>sourceOptions.maxTextBytes){out.error="curve_metadata_budget_violation";return out;}
  sourceBytes+=sourceStats.textBytesRead;
  if(!sourceRead||registered.id!=entry.sourceId||!registered.active||!sha256(QString::fromStdString(registered.sha256))){
    out.value.insert("metadataReady",false);out.value.insert("code","curve_source_unavailable");return out;}
  sourceRef={well.documentId,"area-model",entry.sourceId,QString::fromStdString(registered.sha256)};
  data::CurveWindowQuery query;query.curveId=entry.id;query.sampleCount=0;query.includeAuxiliaryAxes=false;
  data::CurveReadOptions options;options.maxSamples=0;options.maxReadBytes=8192;options.maxLegacyBytes=0;
  options.maxMetadataBytes=8192;options.maxMetadataFieldBytes=8192;
  data::CurveWindowResult window;data::CurveReadStats stats;
  const bool read=service.readCurveWindow(query,window,detail,options,&stats);
  if(stats.metadataBytesRead<0||stats.metadataBytesRead>8192||curveBytes>128*1024-stats.metadataBytesRead){out.error="curve_metadata_budget_violation";return out;}
  curveBytes+=stats.metadataBytesRead;
  if(stats.payloadBytesRead||stats.legacyBytesMaterialized||stats.axisBlocksRead||stats.valueBlocksRead||!window.curve.depths.empty()||
      !window.curve.rawValues.empty()||!window.curve.values.empty()||!window.curve.quality.empty()||!window.tvdValues.empty()||!window.tvdssValues.empty()){
    out.error="curve_metadata_payload_violation";return out;}
  if(!read){out.value.insert("metadataReady",false);out.value.insert("code","curve_metadata_unavailable");return out;}
  if(window.curve.curveId!=entry.id||window.totalSamples<0||window.firstSample!=0){out.error="curve_metadata_invalid";return out;}
  for(const auto *value:{&window.curve.name,&window.curve.unit,&window.depthInfo.depthType,&window.depthInfo.depthUnit,&window.depthInfo.mdReference})
    if(value->size()>8192){out.error="query_entry_too_large";return out;}
  bool encoding=true;const auto name=utf8(window.curve.name,encoding),unit=utf8(window.curve.unit,encoding);
  const auto type=utf8(window.depthInfo.depthType,encoding),depthUnit=utf8(window.depthInfo.depthUnit,encoding),reference=utf8(window.depthInfo.mdReference,encoding);
  const bool typeDeclared=window.depthOrigins.depthType==data::CurveDepthOrigin::Declared;
  const bool unitDeclared=window.depthOrigins.depthUnit==data::CurveDepthOrigin::Declared;
  const bool referenceDeclared=window.depthOrigins.mdReference==data::CurveDepthOrigin::Declared;
  out.value={{"curve_id",QString::number(entry.id)},{"log_set_id",QString::number(entry.parentId)},{"name",name},{"unit",unit},
    {"depth_type",typeDeclared?QJsonValue(type):QJsonValue(QJsonValue::Null)},
    {"depth_unit",unitDeclared?QJsonValue(depthUnit):QJsonValue(QJsonValue::Null)},
    {"depth_reference",referenceDeclared?QJsonValue(reference):QJsonValue(QJsonValue::Null)},{"total_samples",QString::number(window.totalSamples)}};
  QString code;
  if(!encoding)code="curve_metadata_encoding_invalid";
  else if(!typeDeclared||!unitDeclared||!referenceDeclared)code="curve_semantics_not_declared";
  else if(!bounded(name)||!declared(unit)||!declared(type)||!declared(depthUnit)||!declared(reference))code="curve_semantics_required";
  if(code.isEmpty()){
    if(!resolver)code="source_resolver_unavailable";
    else {try{source=resolver(sourceRef);}catch(...){code="source_resolver_failed";}
      if(!catalog||!catalog->isOpen()){out.error="document_unavailable";return out;}
      const auto version=catalog->versionById(source.versionId);
      if(code.isEmpty()&&(source.ref!=sourceRef||source.fingerprint.stored()!=sourceRef.fileRevisionId||!bounded(source.versionId)||
          version.id!=source.versionId||(!source.assetId.isEmpty()&&version.assetId!=source.assetId)||
          document::SourceFingerprint::fromStored(version.sha256).stored()!=sourceRef.fileRevisionId||
          !samePath(QFileInfo(source.modelDirectory).canonicalFilePath(),root)))code="curve_source_mapping_unavailable";
    }
  }
  out.value.insert("metadataReady",code.isEmpty());out.value.insert("code",code.isEmpty()?"ok":code);
  if(code.isEmpty())out.value.insert("input_ref",QJsonObject{{"source_ref",document::sourceRefJson(sourceRef)},{"source_version_id",source.versionId},
    {"well_id",QString::number(well.modelWellId)},{"log_set_id",QString::number(entry.parentId)},{"curve_id",QString::number(entry.id)},
    {"unit",unit},{"depth_type",type},{"depth_unit",depthUnit},{"depth_reference",reference},{"total_samples",QString::number(window.totalSamples)}});
  return out;
}
int overhead(QJsonObject result,const QueryState &state) {
  result.insert("curves",QJsonArray{});result.insert("returnedCount",16);result.insert("hasMore",true);result.insert("nextCursor",QString(256,'x'));
  result.insert("truncated","page_available");result.insert("coverage",QJsonObject{{"documentId",state.scope.documentId},{"epoch",QString::number(state.scope.epoch)},
    {"catalogRevision",QString::number(state.revision)},{"mutationSequence",QString::number(state.mutation)},{"modelSnapshot",state.modelToken},
    {"sortVersion","id@1"},{"complete",false}});
  return int(QJsonDocument(result).toJson(QJsonDocument::Compact).size()-2);
}
} // namespace
QJsonObject ProjectQueryCapability::Impl::wellCurves(QueryState &state,const QJsonObject &args,int rows,int bytes,bool &more) {
  for(auto it=args.begin();it!=args.end();++it)if(!QStringList{"topic","well_ref","cursor","page_size","max_bytes"}.contains(it.key()))
    return queryFailure("unsupported_topic_argument");
  document::WellRef well;
  if(!args.value("well_ref").isObject()||!document::wellRefFromJson(args.value("well_ref").toObject(),well)||
      document::wellRefJson(well)!=args.value("well_ref").toObject())return queryFailure("invalid_well_ref");
  if(well.documentId!=scope.documentId)return queryFailure("well_ref_scope_mismatch");
  if(!well.modelAvailable)return queryFailure("well_model_unavailable");
  const auto entity=catalog->storedEntityById(well.catalogEntityId);bool mapped=false;
  const auto mapping=entity.extra.value("areaModelWellId").toString();const auto id=mapping.toLongLong(&mapped);
  if(entity.id!=well.catalogEntityId||entity.entityType!="well"||!mapped||QString::number(id)!=mapping||id!=well.modelWellId)
    return queryFailure("well_model_mapping_mismatch");
  const auto root=modelRoot(catalog);if(root.isEmpty())return queryFailure("model_query_unavailable");
  data::DataService service;std::string detail;
  if(!service.openProjectReadOnly(root.toUtf8().toStdString(),detail))return queryFailure("model_query_unavailable");
  const auto first=checkHead(service,well);if(!first.isEmpty())return queryFailure(first);
  const auto head=token(well);if(!state.modelToken.isEmpty()&&state.modelToken!=head)return queryFailure("model_revision_changed");state.modelToken=head;
  bool numeric=true;const auto after=state.after.isEmpty()?0:state.after.toLongLong(&numeric);
  if(!numeric||after<0||(!state.after.isEmpty()&&QString::number(after)!=state.after))return queryFailure("cursor_invalid");
  std::vector<data::DataEntry> entries;bool tail=false;data::MetadataReadOptions listOptions;listOptions.maxFieldBytes=8192;listOptions.maxTextBytes=qint64(rows)*8192;
  data::MetadataReadStats listStats;
  if(!service.listDataPage("curves",well.modelWellId,after,rows,entries,tail,detail,listOptions,&listStats)||entries.size()>size_t(rows))return queryFailure("model_query_unavailable");
  if(listStats.textBytesRead<0||listStats.textBytesRead>listOptions.maxTextBytes)return queryFailure("curve_metadata_budget_violation");
  QJsonObject result{{"ok",true},{"code","ok"},{"contract","paleo.project.query.page.v1"},{"tool","paleo.query_project"},{"topic","well_curves"},
    {"well_ref",document::wellRefJson(well)},{"totalCount",QJsonValue::Null},{"metadataOnly",true},{"numericPayloadBytes","0"},
    {"sourceMetadataBytes","131072"},{"metadataTextBytes","393216"},{"metadataReadBudgetBytes",QString::number(qint64(rows)*3*8192)}};
  const auto reserved=overhead(result,state);QJsonArray values;qlonglong previous=after;qint64 sourceBytes=0,curveBytes=0;const auto resolver=sourceResolver;const auto owner=catalog;
  for(const auto &entry:entries){
    if(entry.id<=previous||entry.wellId!=well.modelWellId||entry.parentId<=0)return queryFailure("curve_metadata_invalid");previous=entry.id;
    const auto row=curveRow(service,entry,well,root,owner,resolver,sourceBytes,curveBytes);if(!row.error.isEmpty())return queryFailure(row.error);
    if(scope!=state.scope||catalog!=owner||state.bindingEpoch!=bindingEpoch)return queryFailure("cursor_scope_mismatch");
    if(!appendWithinBudget(values,row.value,bytes,reserved)){if(values.isEmpty())return queryFailure("query_entry_too_large");more=true;break;}
    state.after=QString::number(entry.id);
  }
  const auto last=checkHead(service,well);if(!last.isEmpty())return queryFailure(last);
  more=more||tail;result.insert("curves",values);result.insert("returnedCount",values.size());result.insert("sourceMetadataBytes",QString::number(sourceBytes));
  result.insert("metadataTextBytes",QString::number(listStats.textBytesRead+curveBytes+sourceBytes));return result;
}
} // namespace paleo::query
