// 层：数据
#include "wellfaciesinputplan_internal.h"
#include "areawellmetadata.h"
#include "domain/wellnumeric.h"
#include "io/lasalias.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSet>
#include <cmath>
#include <algorithm>

namespace paleo::wellfacies::detail {
QString hash(const QByteArray &bytes) {
  return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
}
QString hash(const QJsonObject &json) { return hash(QJsonDocument(json).toJson(QJsonDocument::Compact)); }
QString occurrenceKey(const data::WellIntervalOccurrence &occ) {
  if(!occ.occurrenceId.empty())return QString::fromStdString(occ.occurrenceId);
  QJsonArray internal,status;
  for(int i=0;i<3;++i){internal.append(std::isfinite(occ.internal[i])?QJsonValue(occ.internal[i]):QJsonValue());
    status.append(QString::fromStdString(occ.internalStatus[i]));}
  return hash(QJsonObject{{"well",QString::number(occ.wellId)},{"interval",QString::fromStdString(occ.interval.value)},
    {"type",QString::fromStdString(occ.depthType)},{"unit",QString::fromStdString(occ.depthUnit)},
    {"top",occ.top},{"bottom",occ.bottom},{"internal",internal},{"internalStatus",status},
    {"status",QString::fromStdString(occ.status)},{"evidence",QString::fromStdString(occ.evidenceJson)}});
}

bool windows(data::DataService &service,const InputJob &job,QVector<OccurrenceWindow> &out,QString &code,QString *unitLabel) {
  data::StratigraphicScheme scheme; std::string error;
  const auto &unit=job.request.scope.unit;
  if(!service.readStratigraphicScheme(unit.schemeId.toStdString(),unit.revision.toStdString(),scheme,error) ||
      scheme.revision.value!=unit.revision.toStdString()) {code="unit_revision_unavailable";return false;}
  if(unitLabel)*unitLabel=QString::fromStdString(unit.aggregate?data::framework::displayName(scheme,data::IntervalId{unit.intervalId.toStdString()}):
    data::framework::displayName(scheme,data::SubIntervalId{unit.subIntervalId.toStdString()}));
  return windows(scheme,job,out,code);
}
bool windows(const data::StratigraphicScheme &scheme,const InputJob &job,QVector<OccurrenceWindow> &out,QString &code) {
  const auto &unit=job.request.scope.unit;
  if(scheme.id!=unit.schemeId.toStdString()||scheme.revision.value!=unit.revision.toStdString()) {code="unit_revision_unavailable";return false;}
  const auto *parent=data::framework::findInterval(scheme,{unit.intervalId.toStdString()});
  if(!parent||parent->status!="valid") {code="invalid_unit_scope";return false;}
  const auto *child=unit.aggregate?nullptr:data::framework::findSubInterval(scheme,{unit.subIntervalId.toStdString()});
  if(!unit.aggregate && (!child||child->interval!=parent->id||child->index!=unit.subIndex||child->status!="valid")) {
    code="invalid_unit_scope";return false;
  }
  QSet<QString> seen,selected;
  auto one=scheme;one.wellOccurrences.clear();
  for(const auto &occ:scheme.wellOccurrences) {
    if(occ.wellId!=job.request.well.modelWellId||occ.interval!=parent->id)continue;
    const auto key=occurrenceKey(occ);
    if(!job.request.scope.occurrenceKeys.isEmpty()&&!job.request.scope.occurrenceKeys.contains(key))continue;
    if(seen.contains(key)){code="ambiguous_occurrence_identity";return false;} seen.insert(key);
    selected.insert(key);
    if(occ.depthType!="MD"){code="input_depth_mapping_unavailable";return false;}
    const auto scale=wellnumeric::depthScaleToMetres(QString::fromStdString(occ.depthUnit));
    if(!scale){code="input_depth_unit_unknown";return false;}
    if(occ.status!="complete" || !std::isfinite(occ.top)||!std::isfinite(occ.bottom)||occ.top>=occ.bottom) {
      code="unit_occurrence_unresolved";return false;
    }
    double top=occ.top,bottom=occ.bottom;bool derived=false;
    if(child) {
      one.wellOccurrences={occ};
      const auto range=occ.occurrenceId.empty()?data::framework::wellRange(one,occ.wellId,child->id):
        data::framework::wellRangeForOccurrence(scheme,occ.wellId,child->id,occ.occurrenceId);
      if(!range.resolved||range.depthType!=occ.depthType||range.depthUnit!=occ.depthUnit) {
        code="unit_occurrence_unresolved";return false;
      }
      top=range.top;bottom=range.bottom;
      derived=range.provisional;
    }
    if(!std::isfinite(top)||!std::isfinite(bottom)||top>=bottom||out.size()>=32) {code="unit_window_budget_exceeded";return false;}
    out.append({key,QString::fromStdString(occ.depthType),QString::fromStdString(occ.depthUnit),top**scale,bottom**scale,derived});
  }
  if(out.isEmpty()||(!job.request.scope.occurrenceKeys.isEmpty()&&selected.size()!=job.request.scope.occurrenceKeys.size())) {
    code="unit_occurrence_unavailable";return false;
  }
  // Keep actual occurrences separate even when their depth windows touch or overlap.
  std::sort(out.begin(),out.end(),[](const auto &a,const auto &b){return a.key<b.key;});
  return true;
}

bool curveRefs(data::DataService &service,const InputJob &job,QVector<CurveRef> &out,QString &code,const std::function<bool()> &cancelled) {
  std::vector<data::DataEntry> entries;std::string error;
  if(!readBoundedAreaWellMetadata(service,"curves",job.request.well.modelWellId,4096,entries,error,cancelled)) {
    code=QString::fromStdString(error);return false;
  }
  for(auto it=job.request.curveIds.begin();it!=job.request.curveIds.end();++it)
    if(!job.model.curves.contains(it.key())){code="invalid_curve_ref";return false;}
  auto &aliases=LasAliasMap::shared(); QSet<qlonglong> used;
  for(const auto &feature:job.model.curves) {
    if(cancelled&&cancelled()){code="cancelled";return false;}
    const data::DataEntry *chosen=nullptr;
    for(const auto &entry:entries) {
      if(aliases.canonicalCurve(QString::fromStdString(entry.name))!=aliases.canonicalCurve(feature))continue;
      if(job.request.curveIds.contains(feature)&&job.request.curveIds.value(feature)!=entry.id)continue;
      if(chosen){code="ambiguous_curve_source";return false;} chosen=&entry;
    }
    if(!chosen||chosen->wellId!=job.request.well.modelWellId||chosen->sourceId<=0||chosen->parentId<=0||used.contains(chosen->id)) {
      code="curve_source_unavailable";return false;
    }
    used.insert(chosen->id);data::SourceIndexEntry source;
    if(!service.readSourceIndexEntry(chosen->sourceId,source,error)||!source.active){code="curve_source_inactive";return false;}
    data::CurveWindowQuery query;query.curveId=chosen->id;query.sampleCount=0;query.includeAuxiliaryAxes=false;
    data::CurveWindowResult metadata;
    if(!service.readCurveWindow(query,metadata,error)){code="curve_metadata_unavailable";return false;}
    if(metadata.depthInfo.depthType!="MD"){code="input_depth_mapping_unavailable";return false;}
    if(!wellnumeric::depthScaleToMetres(QString::fromStdString(metadata.depthInfo.depthUnit))) {
      code="input_depth_unit_unknown";return false;
    }
    if(!declaredSemanticToken(QString::fromStdString(metadata.depthInfo.mdReference))){code="input_md_reference_unknown";return false;}
    const auto unit=QString::fromStdString(metadata.curve.unit).trimmed();
    if(unit.isEmpty()){code="input_feature_unit_unknown";return false;}
    if(unit!=job.model.curveUnits.value(feature).trimmed()) {
      code="model_feature_unit_mismatch";return false;
    }
    out.append({feature,unit,QString::fromStdString(source.sha256),chosen->id,chosen->sourceId,chosen->parentId,
      QString::fromStdString(metadata.depthInfo.depthType),QString::fromStdString(metadata.depthInfo.depthUnit),
      QString::fromStdString(metadata.depthInfo.mdReference),QString::fromStdString(chosen->name)});
  }
  return true;
}

bool lithologyRef(data::DataService &service,const InputJob &job,qlonglong &id,QJsonObject &manifest,QString &code,const std::function<bool()> &cancelled) {
  std::vector<data::DataEntry> entries;std::string error;
  if(!readBoundedAreaWellMetadata(service,"intervals",job.request.well.modelWellId,128,entries,error,cancelled)) {
    code=QString::fromStdString(error);return false;
  }
  const data::DataEntry *chosen=nullptr;
  for(const auto &entry:entries) {
    if(cancelled&&cancelled()){code="cancelled";return false;}
    if(job.request.lithologyTrackId>0&&entry.id!=job.request.lithologyTrackId)continue;
    data::IntervalTrack track;
    if(!service.readIntervalTrackInfo(entry.id,track,error)){code="lithology_metadata_unavailable";return false;}
    const auto kind=QString::fromStdString(track.trackType),name=QString::fromStdString(track.trackName);
    if(!kind.contains(QStringLiteral("岩性"))&&!name.contains(QStringLiteral("岩性")))continue;
    if(chosen){code="ambiguous_lithology_source";return false;}chosen=&entry;
  }
  if(!chosen||chosen->wellId!=job.request.well.modelWellId||chosen->sourceId<=0||chosen->count>100000) {
    code="lithology_source_unavailable";return false;
  }
  data::SourceIndexEntry source;
  if(!service.readSourceIndexEntry(chosen->sourceId,source,error)||!source.active){code="lithology_source_inactive";return false;}
  data::DataObjectDescriptor descriptor;
  if(!service.describeDataObject(data::DataObjectKind::IntervalTrack,chosen->id,descriptor,error)) {
    code="lithology_metadata_unavailable";return false;
  }
  id=chosen->id;
  manifest={{"trackId",QString::number(id)},{"sourceId",QString::number(chosen->sourceId)},
    {"sourceFingerprint",QString::fromStdString(source.sha256)},
    {"revisionKnown",descriptor.revisionKnown},{"revision",QString::fromStdString(descriptor.revision)},
    {"recordCount",QString::number(chosen->count)}};
  return true;
}
} // namespace paleo::wellfacies::detail
