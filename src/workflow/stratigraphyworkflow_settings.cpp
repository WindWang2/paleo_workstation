// 层：功能
#include "stratigraphyworkflow_state.h"
#include "stratigraphygeometry.h"
#include <QJsonDocument>
#include <QUuid>
#include <algorithm>
#include <set>
#include <cmath>
using namespace paleo::data;
bool StratigraphyWorkflow::selectionMatches(const QList<qlonglong>& ids) const {
 std::set<qlonglong> loaded,selected(ids.begin(),ids.end());for(const auto& w:d->document.wells)loaded.insert(w.wellId);
 return !loaded.empty()&&loaded==selected;
}
bool StratigraphyWorkflow::canSubdivide(const QList<qlonglong>& ids,const QString& scope) const {
 if(!canEdit()||!selectionMatches(ids))return false;
 const char* roles[]={"T70","T72","T73","T80"};const char* segments[]={"E12","E3","E4"};
 for(const auto& w:d->document.wells)for(int i=0;i<3;++i)if(scope.isEmpty()||scope==QLatin1String(segments[i])) {
  double a=qQNaN(),b=qQNaN();for(const auto& p:w.boundaries){if(p.role==roles[i])a=p.md;if(p.role==roles[i+1])b=p.md;}
  if(std::isfinite(a)&&std::isfinite(b)&&b>a)return true;
 }return false;
}
QList<StratigraphyIssue> StratigraphyWorkflow::inputIssues(const QList<qlonglong>& ids) const {
 QList<StratigraphyIssue> issues;DataService service;std::string error;
 if(!service.openProjectReadOnly(d->path.toStdString(),error)){issues.append({0,{},QString::fromStdString(error),{}});return issues;}
 const auto plan=paleo::stratigraphy::planQuickInputs(service,std::vector<long long>(ids.begin(),ids.end()),d->preferred);
 for(const auto& issue:plan.issues)if(issue.blocking&&issue.field!="timeDepth"&&issue.field!="coordinates"&&issue.field!="trajectory") {
  StratigraphyIssue item;item.well=issue.wellId;item.field=QString::fromStdString(issue.field);item.message=QString::fromStdString(issue.message);
  for(auto id:issue.candidateIds)item.candidates.append(id);issues.append(item);
 }return issues;
}
QJsonObject StratigraphyWorkflow::geometrySettings() const {
 try{DataService service;std::string error;if(!service.openProjectReadOnly(d->path.toStdString(),error))return {};
  return paleo::stratigraphy_native::geometryJson(paleo::stratigraphy_native::geometry(service,d->document.viewStateJson));
 }catch(const std::exception&){return {};}
}
void StratigraphyWorkflow::setGeometrySettings(const QJsonObject& value) {
 if(!canEdit())return;setViewOptions(value);d->dirty=true;emit changed();
}
void StratigraphyWorkflow::setViewOptions(const QJsonObject& value) {
 if(!canEdit())return;auto json=QJsonDocument::fromJson(QByteArray::fromStdString(d->document.viewStateJson)).object();
 for(auto it=value.begin();it!=value.end();++it)json[it.key()]=it.value();d->document.viewStateJson=QJsonDocument(json).toJson(QJsonDocument::Compact).toStdString();
}
InterpretationClustering StratigraphyWorkflow::previewClustering(int count) {
 if(!canEdit())return {};
 try {
  auto inputs=d->document.wells;const auto g=geometrySettings();
  for(auto& w:inputs) {
   const auto basis=w.input.depthInfo.depthType.empty()?g["logDepthType"].toString().toStdString():w.input.depthInfo.depthType;
   const auto unit=w.input.depthInfo.depthUnit.empty()?g["depthUnit"].toString().toStdString():w.input.depthInfo.depthUnit;
   if(basis!="MD"||(unit!="m"&&unit!="ft"))throw std::runtime_error("先确认测井主轴 MD 与深度单位，再进行井群分组");
   w.input.log.axisKind="MD";w.input.log.axisUnit="m";if(unit=="ft")for(auto& v:w.input.log.axis)v*=.3048;
  }
  return paleo::stratigraphy::clusterWells(inputs,count);
 }catch(const std::exception& e){emit statusChanged(QString::fromUtf8(e.what()));return {};}
}
void StratigraphyWorkflow::saveClustering(const InterpretationClustering& value) {
 if(!canEdit()||value.members.empty())return;auto groups=value;groups.id=QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();groups.version="1";groups.name="GR形态分组";
 DataService service;std::string error;if(!service.openProject(d->path.toStdString(),error)||!service.saveClustering(groups,error)){emit statusChanged(QString::fromStdString(error));return;}
 d->document.clusterings.push_back(groups);d->document.activeClusteringId=groups.id;d->dirty=true;emit changed();emit statusChanged(tr("已保存井群分组及人工修订记录"));
}
