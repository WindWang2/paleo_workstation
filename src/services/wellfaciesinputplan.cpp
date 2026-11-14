// 层：数据
#include "wellfaciesinputplan_internal.h"
#include "areawellchart.h"
#include "io/areamodel.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <cmath>
#include <algorithm>

namespace paleo::wellfacies {
PreparedInput prepareInput(const InputJob &job,const std::function<bool()> &cancelled) {
  PreparedInput out;out.model=job.model;
  const auto fail=[&](const QString &code,const QString &error=QString()) {
    PreparedInput failed;failed.code=code;failed.error=error.isEmpty()?code:error;return failed;
  };
  const auto wasCancelled=[&]{return cancelled&&cancelled();};
  const auto readiness=modelReadiness(job.model,job.request.modelId,job.request.modelVersion);
  if(!readiness.isEmpty())return fail(readiness);
  if(!job.request.well.valid()||!job.request.well.modelAvailable||!job.request.scope.valid())return fail("well_input_reference_required");
  if(wasCancelled())return fail("cancelled");
  data::DataService service;std::string error;
  if(!service.openProjectReadOnly(area_model::directory(job.projectDir).toStdString(),error))return fail("model_store_unavailable",QString::fromStdString(error));
  data::ModelChangeHead before,after;
  if(!service.readModelChangeHead(before,error)||!before.available||
      before.storeId!=job.request.well.modelStoreIdentity.toStdString()||
      before.generation!=job.request.well.modelGeneration.toStdString()||before.sequence<job.request.well.queryHead)
    return fail("stale_well_ref");
  data::WellPlotOptions options;options.includeLogs=options.includeIntervals=options.includeMarkers=options.includePhotos=false;
  data::WellPlotData well;
  if(!service.readWellPlotData(job.request.well.modelWellId,options,well,error))return fail("well_unavailable");
  const auto mdReference=QString::fromStdString(well.defaultDepthInfo.mdReference);
  // Occurrence depths use the well's declared MD frame. A TVD/TVDSS axis needs an explicit mapping.
  if(!declaredSemanticToken(mdReference))return fail("input_md_reference_unknown");
  out.wellName=QString::fromStdString(well.well.wellName);
  if(out.wellName.isEmpty()||out.wellName.toUtf8().size()>512)return fail("invalid_well_name");
  QString code,unitLabel;
  if(!detail::windows(service,job,out.occurrences,code,&unitLabel))return fail(code);
  if(unitLabel.isEmpty()||unitLabel.size()>128)return fail("unit_label_unavailable");
  WellFaciesInput frozenClasses;QString classError;
  if(!bindWellFaciesClasses(job.model,job.faciesSchemas.value(unitLabel,job.facies),&frozenClasses,&classError))
    return fail("model_class_schema_unavailable",classError);
  if(frozenClasses.facies.size()>4096||QJsonDocument(QJsonArray::fromVariantList(frozenClasses.facies)).toJson(QJsonDocument::Compact).size()>512*1024)
    return fail("model_class_schema_budget_exceeded");
  if(!detail::curveRefs(service,job,out.curves,code,wasCancelled))return fail(code);
  qlonglong lithology=0;QJsonObject lithologyManifest;
  if(!detail::lithologyRef(service,job,lithology,lithologyManifest,code,wasCancelled))return fail(code);
  if(lithologyManifest.value("recordCount").toString().toLongLong()*out.occurrences.size()>100000)
    return fail("lithology_budget_exceeded");
  out.featureSchemaHash=detail::hash(frozenModelJson(job.model));
  auto actualWell=document::wellRefJson(job.request.well);actualWell.remove("model_head");
  AreaWellCurveReadUsage used;QJsonArray windows,curveRefs,segmentManifest;
  for(const auto &curve:out.curves)curveRefs.append(QJsonObject{{"feature",curve.feature},{"unit",curve.unit},
    {"sourceName",curve.sourceName},{"sourceDepthType",curve.sourceDepthType},{"sourceDepthUnit",curve.sourceDepthUnit},{"mdReference",curve.mdReference},
    {"curveId",QString::number(curve.curveId)},{"sourceId",QString::number(curve.sourceId)},
    {"logSetId",QString::number(curve.logSetId)},{"sourceFingerprint",curve.sourceFingerprint}});
  qint64 bodyBytes=0,candidateRows=0;constexpr qint64 maxBodyBytes=32*1024*1024;
  for(const auto &window:out.occurrences) {
    if(wasCancelled())return fail("cancelled");
    AreaWellChartQuery query;query.curveNames=job.model.curves;
    query.curves={window.topMd,window.bottomMd,2000000-used.samples,64LL*1024*1024-used.payloadBytes};
    if(query.curves.maxSamples<=0||query.curves.maxReadBytes<=0)return fail("input_read_budget_exceeded");
    query.requireDeclaredMd=true;query.mdReference=mdReference;query.exactIntervalTrackIds={lithology};
    for(const auto &ref:out.curves)query.exactCurveIds.insert(ref.feature,ref.curveId);
    WellComposite::ComprehensiveWellData chart;AreaWellCurveReadUsage usage;QString detail;
    if(!readAreaWellChart(service,job.request.well.modelWellId,chart,&detail,cancelled,&query,&usage))return fail("well_input_unavailable",detail);
    used.samples+=usage.samples;used.payloadBytes+=usage.payloadBytes;
    if(chart.continuousCurves.size()!=job.model.curves.size())return fail("curve_source_unavailable");
    const auto reference=std::find_if(chart.continuousCurves.begin(),chart.continuousCurves.end(),[&](const auto &curve){
      return curve.name==job.model.curves.front();});
    if(reference==chart.continuousCurves.end())return fail("curve_source_unavailable");
    candidateRows+=reference->depths.size();
    if(candidateRows>131072)return fail("input_row_budget_exceeded");
    // The immutable scheme occurrence is the formation label, not an unrelated default well track.
    chart.formationIntervals.clear();chart.seismicIntervals.clear();chart.seismicSubIntervals.clear();
    WellComposite::FormationInterval formation;
    formation.topDepth=float(window.topMd);formation.bottomDepth=float(window.bottomMd);
    formation.name=unitLabel;
    formation.unitType=job.request.scope.unit.aggregate?QStringLiteral("地震层位"):QStringLiteral("地震小层");
    chart.formationIntervals.append(formation);
    chart.maxDepth=formation.bottomDepth;
    // Honor [top,bottom) for S1-S3/aggregate. S4's endpoint is allowed only where both auxiliary records cover it.
    for(auto &curve:chart.continuousCurves) {
      while(!curve.depths.isEmpty() && curve.depths.last()>=window.bottomMd && job.request.scope.unit.subIndex!=4) {
        curve.depths.removeLast();curve.values.removeLast();
      }
    }
    QVector<WellFaciesInput> segments;QStringList skipped;
    const auto first=prepareWellFaciesInput(chart,job.model,&segments,&skipped,job.request.scope.unit.subIndex==4,
      WellFaciesSourcePolicy::DeclaredSdkValues);
    if(segments.isEmpty())return fail("no_valid_input_segment",first.reason);
    for(const auto &reason:skipped)out.skipped.append(window.key+": "+reason);
    for(auto &input:segments) {
      if(wasCancelled())return fail("cancelled");
      input.facies=frozenClasses.facies;input.modelLabels=frozenClasses.modelLabels;
      const QJsonObject body{{"modelVersionId",job.model.id},{"waitTimeoutSeconds",30},
        {"wells",QJsonArray{QJsonObject{{"wellName",out.wellName},{"rows",input.rows}}}}};
      const auto bytes=QJsonDocument(body).toJson(QJsonDocument::Compact);
      bodyBytes+=bytes.size();
      if(bodyBytes>maxBodyBytes||out.segments.size()>=64)return fail("input_result_budget_exceeded");
      const auto requestHash=detail::hash(QJsonObject{{"model",frozenModelJson(job.model)},
        {"endpointId",job.request.endpointId},
        {"well",actualWell},{"context",document::interpretationContextJson(job.request.context)},
        {"occurrence",window.key},{"bodyHash",detail::hash(bytes)},
        {"facies",QJsonArray::fromVariantList(input.facies)}});
      segmentManifest.append(QJsonObject{{"requestHash",requestHash},{"bodyHash",detail::hash(bytes)},
        {"occurrence",window.key},{"rows",input.rows.size()}});
      out.requestBodies.append(bytes);out.requestHashes.append(requestHash);
      out.segmentOccurrences.append(window.key);out.segments.append(std::move(input));
    }
    windows.append(QJsonObject{{"key",window.key},{"depthType",window.originalDepthType},
      {"identityKind",window.key.startsWith("occ-")?"sdk-occurrence-id":"immutable-snapshot-hash"},
      {"sourceDepthUnit",window.originalDepthUnit},{"topMdMetres",window.topMd},{"bottomMdMetres",window.bottomMd},
      {"derivedDivisionGeometry",window.derivedDivisionGeometry}});
  }
  if(wasCancelled())return fail("cancelled");
  if(!service.readModelChangeHead(after,error)||!after.available||after.storeId!=before.storeId||
      after.generation!=before.generation||after.sequence!=before.sequence)return fail("input_changed_during_read");
  out.sourceManifest={{"contract","paleo.well-facies.input.v2"},{"well",document::wellRefJson(job.request.well)},
    {"context",document::interpretationContextJson(job.request.context)},{"model",frozenModelJson(job.model)},
    {"modelEndpointId",job.request.endpointId},
    {"unit_scope",unitScopeJson(job.request.scope)},{"coverage",job.request.scope.occurrenceKeys.isEmpty()?"all_unit_occurrences":"explicit_occurrences"},
    {"featureSchemaHash",out.featureSchemaHash},{"unitLabel",unitLabel},{"curves",curveRefs},{"lithology",lithologyManifest},
    {"preparation",QJsonObject{{"numericRepresentation","float32"},{"sampleAlignment","first feature axis; bounded linear interpolation"},
      {"valuePolicy","SDK processed quality; null and threshold flags masked; manual finite values retained"},
      {"nullsAndGaps","split without filling; each segment meets model.window"},{"curveRevisionMode","frozen delivered values"}}},
    {"occurrences",windows},{"segments",segmentManifest},{"mdReference",mdReference},
    {"readWatermark",QString::number(before.sequence)},{"skipped",QJsonArray::fromStringList(out.skipped)}};
  auto consumed=out.sourceManifest;consumed.remove("readWatermark");consumed.remove("skipped");
  auto wellIdentity=consumed.value("well").toObject();wellIdentity.remove("model_head");consumed.insert("well",wellIdentity);
  out.inputHash=detail::hash(consumed);out.sourceManifest.insert("inputHash",out.inputHash);
  out.ok=true;return out;
}
} // namespace paleo::wellfacies
