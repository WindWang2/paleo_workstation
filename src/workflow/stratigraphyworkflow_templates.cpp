// 层：功能
#include "stratigraphyworkflow_state.h"
#include <algorithm>
#include <map>
using namespace paleo::data;
QList<StratigraphyVersion> StratigraphyWorkflow::templates() const {
 QList<StratigraphyVersion> result;DataService service;std::string error;std::vector<InterpretationTemplate> all;
 if(!service.openProjectReadOnly(d->path.toStdString(),error)||!service.listReferenceTemplates(all,error))return result;
 for(const auto& t:all)result.append({QString::fromStdString(t.id+"|"+t.version),QString::fromStdString(t.name+" · "+t.version),{}});return result;
}
QList<StratigraphyAssignment> StratigraphyWorkflow::prepareAssignments(const QString& scope,const QString& selected) const {
 QList<StratigraphyAssignment> result;DataService service;std::string error;std::vector<InterpretationTemplate> all;
 if(selected.isEmpty()||!service.openProjectReadOnly(d->path.toStdString(),error)||!service.listReferenceTemplates(all,error))return result;
 std::map<long long,int> groups;for(const auto& c:d->document.clusterings)if(c.id==d->document.activeClusteringId)for(const auto& m:c.members)groups[m.wellId]=m.group;
 for(const auto& w:d->document.wells)for(const char* segment:{"E12","E3","E4"})if(scope.isEmpty()||scope==QLatin1String(segment)) {
  StratigraphyAssignment row;row.well=w.wellId;row.name=QString::fromStdString(w.name);row.segment=QLatin1String(segment);
  QString recommended,fallback;bool own=false;
  for(const auto& t:all) {
   const QString key=QString::fromStdString(t.id+"|"+t.version);if(key==selected&&t.referenceWellId==w.wellId)own=true;
   if(t.referenceWellId==w.wellId||std::none_of(t.segments.begin(),t.segments.end(),[&](const auto& s){return s.segmentId==segment;}))continue;
   const bool same=groups.contains(w.wellId)&&groups.contains(t.referenceWellId)&&groups[w.wellId]==groups[t.referenceWellId];
   row.choices.append({key,QString::fromStdString(t.name),same});if(same&&recommended.isEmpty())recommended=key;if(key==selected)fallback=key;
  }
  if(!own)row.key=recommended.isEmpty()?fallback:recommended;result.append(row);
 }return result;
}
