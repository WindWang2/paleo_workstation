// 层：数据
#include "wellfaciesinputplan_internal.h"
#include "areawellmetadata.h"
#include "io/areamodel.h"
#include <QJsonArray>
#include <QJsonDocument>

namespace paleo::wellfacies {
QJsonObject inspectInput(const InputInspectionJob &job,const std::function<bool()> &cancelled) {
  const auto fail=[](const QString &code){return QJsonObject{{"ok",false},{"code",code}};};
  if(!job.well.valid()||!job.well.modelAvailable)return fail("well_model_unavailable");
  data::DataService service;std::string error;data::ModelChangeHead before,after;
  if(!service.openProjectReadOnly(area_model::directory(job.projectDir).toStdString(),error)||
      !service.readModelChangeHead(before,error)||!before.available)return fail("model_store_unavailable");
  if(before.storeId!=job.well.modelStoreIdentity.toStdString()||before.generation!=job.well.modelGeneration.toStdString()||
      before.sequence<job.well.queryHead)return fail("stale_well_ref");
  data::WellPlotOptions options;options.includeLogs=options.includeIntervals=options.includeMarkers=options.includePhotos=false;
  data::WellPlotData well;
  if(!service.readWellPlotData(job.well.modelWellId,options,well,error))return fail("well_unavailable");
  QJsonArray curves,tracks;std::vector<data::DataEntry> entries;
  if(!readBoundedAreaWellMetadata(service,"curves",job.well.modelWellId,4096,entries,error,cancelled))return fail(QString::fromStdString(error));
  for(const auto &entry:entries) {
    if(cancelled&&cancelled())return fail("cancelled");
    data::CurveWindowQuery query;query.curveId=entry.id;query.sampleCount=0;query.includeAuxiliaryAxes=false;
    data::CurveWindowResult value;data::SourceIndexEntry source;
    if(!service.readCurveWindow(query,value,error)||!service.readSourceIndexEntry(entry.sourceId,source,error))return fail("curve_metadata_unavailable");
    if(!source.active)continue;
    curves.append(QJsonObject{{"id",QString::number(entry.id)},{"name",QString::fromStdString(entry.name)},
      {"sourceId",QString::number(entry.sourceId)},{"logSetId",QString::number(entry.parentId)},
      {"sourceFingerprint",QString::fromStdString(source.sha256)},{"unit",QString::fromStdString(value.curve.unit)},
      {"depthType",QString::fromStdString(value.depthInfo.depthType)},{"depthUnit",QString::fromStdString(value.depthInfo.depthUnit)},
      {"mdReference",QString::fromStdString(value.depthInfo.mdReference)}});
  }
  entries.clear();
  if(!readBoundedAreaWellMetadata(service,"intervals",job.well.modelWellId,128,entries,error,cancelled))return fail(QString::fromStdString(error));
  int excludedSourceLessIntervalTracks=0;
  for(const auto &entry:entries) {
    if(cancelled&&cancelled())return fail("cancelled");
    if(entry.sourceId<=0){++excludedSourceLessIntervalTracks;continue;}
    data::IntervalTrack value;data::SourceIndexEntry source;data::DataObjectDescriptor descriptor;
    if(!service.readIntervalTrackInfo(entry.id,value,error)||!service.readSourceIndexEntry(entry.sourceId,source,error)||
       !service.describeDataObject(data::DataObjectKind::IntervalTrack,entry.id,descriptor,error))return fail("track_metadata_unavailable");
    if(!source.active)continue;
    tracks.append(QJsonObject{{"id",QString::number(entry.id)},{"name",QString::fromStdString(value.trackName)},
      {"type",QString::fromStdString(value.trackType)},{"sourceId",QString::number(entry.sourceId)},
      {"revisionKnown",descriptor.revisionKnown},{"revision",QString::fromStdString(descriptor.revision)},
      {"sourceFingerprint",QString::fromStdString(source.sha256)},
      {"depthType",QString::fromStdString(value.depthInfo.depthType)},{"depthUnit",QString::fromStdString(value.depthInfo.depthUnit)},
      {"mdReference",QString::fromStdString(value.depthInfo.mdReference)}});
  }
  auto observed=job.well;observed.queryHead=before.sequence;
  QJsonObject result{{"contract","paleo.well-facies.inputs.v2"},{"ok",true},{"well_ref",document::wellRefJson(observed)},
    {"curves",curves},{"intervalTracks",tracks},
    {"coverage",QJsonObject{{"complete",true},{"scope","active_file_backed_curve_and_interval_metadata"},
      {"excludedSourceLessIntervalTracks",excludedSourceLessIntervalTracks}}},{"hasMore",false},
    {"wellMdReference",QString::fromStdString(well.defaultDepthInfo.mdReference)},
    {"inputFrameReady",declaredSemanticToken(QString::fromStdString(well.defaultDepthInfo.mdReference))},
    {"inputFrameCode",declaredSemanticToken(QString::fromStdString(well.defaultDepthInfo.mdReference))?"":"input_md_reference_unknown"},
    {"contextAvailable",false},{"contextCode","unit_scope_unavailable"}};
  if(job.context) {
    auto context=*job.context;context.purpose=document::RelationPurpose::FaciesAnalysis;context.targetVolume.reset();
    data::StratigraphicScheme scheme;
    if(service.readStratigraphicScheme(context.schemeId.toStdString(),context.schemeRevision.toStdString(),scheme,error)) {
      UnitScope scope;scope.unit.schemeId=context.schemeId;scope.unit.revision=context.schemeRevision;
      scope.unit.intervalId=context.intervalId;scope.unit.subIntervalId=context.subIntervalId;scope.unit.aggregate=context.aggregate;
      const auto *sub=context.aggregate?nullptr:data::framework::findSubInterval(scheme,{context.subIntervalId.toStdString()});
      scope.unit.subIndex=sub?sub->index:0;
      if(scope.valid()&&context.documentId==job.well.documentId&&context.modelStoreId==job.well.modelStoreId) {
        InputJob input;input.projectDir=job.projectDir;input.request.well=job.well;input.request.context=context;input.request.scope=scope;
        QString code;QJsonArray windows;QHash<QString,int> counts;
        for(const auto &occ:scheme.wellOccurrences)if(occ.wellId==job.well.modelWellId&&occ.interval.value==context.intervalId.toStdString())
          ++counts[detail::occurrenceKey(occ)];
        auto one=scheme;one.wellOccurrences.clear();
        for(const auto &occ:scheme.wellOccurrences) {
          if(occ.wellId!=job.well.modelWellId||occ.interval.value!=context.intervalId.toStdString())continue;
          if(windows.size()>=4096)return fail("input_metadata_budget_exceeded");
          const auto key=detail::occurrenceKey(occ);input.request.scope.occurrenceKeys={key};
          one.wellOccurrences={occ};QVector<OccurrenceWindow> actual;QString readiness;
          const bool ready=counts.value(key)==1&&detail::windows(one,input,actual,readiness);
          if(counts.value(key)!=1)readiness="ambiguous_occurrence_identity";
          QJsonObject row{{"key",key},{"status",QString::fromStdString(occ.status)},{"sourceDepthType",QString::fromStdString(occ.depthType)},
            {"occurrenceId",QString::fromStdString(occ.occurrenceId)},
            {"identityKind",occ.occurrenceId.empty()?"immutable-snapshot-hash":"sdk-occurrence-id"},
            {"sourceDepthUnit",QString::fromStdString(occ.depthUnit)},{"sourceTop",occ.top},{"sourceBottom",occ.bottom},
            {"ready",ready},{"readinessCode",readiness}};
          if(ready&&!actual.isEmpty()){row.insert("topMdMetres",actual.front().topMd);row.insert("bottomMdMetres",actual.front().bottomMd);
            row.insert("derivedDivisionGeometry",actual.front().derivedDivisionGeometry);}
          windows.append(row);if(!ready)code="unit_occurrence_unresolved";
        }
        if(windows.isEmpty())code="unit_occurrence_unavailable";
        else if(windows.size()>32)code="unit_window_budget_exceeded";
        result.insert("context",document::interpretationContextJson(context));result.insert("unit_scope",unitScopeJson(scope));
        result.insert("contextAvailable",true);result.insert("contextCode",QString());result.insert("defaultScopeReady",code.isEmpty());
        result.insert("defaultScopeCode",code);result.insert("occurrences",windows);
      }
    }
  }
  if(!service.readModelChangeHead(after,error)||before.storeId!=after.storeId||before.generation!=after.generation||before.sequence!=after.sequence)
    return fail("query_snapshot_changed");
  if(QJsonDocument(result).toJson(QJsonDocument::Compact).size()>1024*1024)return fail("input_metadata_budget_exceeded");
  return result;
}
} // namespace paleo::wellfacies
