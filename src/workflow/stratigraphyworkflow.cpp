// 层：功能
#include "stratigraphyworkflow.h"
#include "../io/areamodel.h"
#include "../services/paleotaskservice.h"
#include <paleo_stratigraphy/quick_workflow.h>
#include <paleo_stratigraphy/time_depth_selection.h>
#include <QDateTime>
#include <QUuid>
#include <QPointer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QSaveFile>
#include <QDir>
#include "../domain/arearules.h"
#include <algorithm>
using namespace paleo::data;
using namespace paleo::stratigraphy;
namespace { QString text(const std::string &s) { return QString::fromUtf8(s); }
void require(bool ok, const std::string &error) { if(!ok) throw std::runtime_error(error); }
template<class T> void checkPlan(const T &plan) {
 if (plan.ready()) return;
 std::string errors;
 for (const auto &issue:plan.issues) if(issue.blocking) errors+=issue.message+"\n";
 throw std::runtime_error(errors.empty()?"资料或模板尚未确认":errors);
}
GeometryOptions geometry(DataService &service) {
 GeometryOptions g; CorrelationProjectConventions c; std::string error;
 require(service.readCorrelationProjectConventions(c,error),error);
 g.resolveFromSources=true; g.confirmedDepthUnit=c.depthUnit;
 g.confirmedLogDepthType=c.depthType; g.confirmedDepthType=c.depthType;
 g.confirmedWellTimeUnit=c.timeUnit; g.confirmedHorizonTimeUnit=c.timeUnit;
 g.allowProvisionalAlignment=c.allowProvisionalAlignment; g.policyEvidence=c.evidence;
 g.allowBottomExtrapolation=true; g.maximumBottomExtrapolationM=0;
 g.allowTopExtrapolation=true; g.maximumLogExtrapolationM=100;
 return g;
}
}
struct StratigraphyWorkflow::State {
 PaleoTaskService *tasks; QPointer<PaleoTask> task;
 QString path, project; quint64 generation=0; bool readOnly=false, dirty=false;
 QList<StratigraphyWell> wells; InterpretationDocument document;
 std::vector<InterpretationDocument> history;
 CorrelationInputRequest preferred;
 void remember() { if(document.id.empty())return;if(history.size()>=8)history.erase(history.begin());history.push_back(std::move(document)); }
};
StratigraphyWorkflow::StratigraphyWorkflow(PaleoTaskService *tasks,QObject *parent)
 :QObject(parent),d(std::make_unique<State>()) { d->tasks=tasks; }
StratigraphyWorkflow::~StratigraphyWorkflow() { if(d->task) d->task->requestCancel(); }
void StratigraphyWorkflow::setTaskService(PaleoTaskService *tasks) { d->tasks=tasks; }
void StratigraphyWorkflow::setProject(const QString &project,bool readOnly) {
 ++d->generation; if(d->task) d->task->requestCancel(); d->task=nullptr;
 d->project=project;d->path=project.isEmpty()?QString():paleo::area_model::directory(project);
 d->readOnly=readOnly; d->dirty=false;d->wells.clear(); d->document={};d->history.clear();d->preferred={};
 if(!d->path.isEmpty()) {
  DataService service;std::string error;std::vector<WellSummary> wells;
  const bool opened=readOnly?service.openProjectReadOnly(d->path.toStdString(),error):service.openProject(d->path.toStdString(),error);
  if(opened&&service.listWells(wells,error)) {
   for(const auto &w:wells)d->wells.append({w.id,text(w.name)});
   std::vector<InterpretationSummary> versions;
   if(service.listInterpretations(versions,error)&&!versions.empty())
    service.readInterpretationRevision(versions.back().id,{},d->document,error);
  } else emit statusChanged(text(error));
 }
 emit busyChanged(false);emit changed();
}
QList<StratigraphyWell> StratigraphyWorkflow::wells() const {return d->wells;}
bool StratigraphyWorkflow::canEdit() const {return !d->path.isEmpty()&&!d->readOnly&&!d->task;}
void StratigraphyWorkflow::refreshWells() {
 if(d->task||d->path.isEmpty())return;
 DataService service;std::string error;std::vector<WellSummary> wells;
 if(!service.openProjectReadOnly(d->path.toStdString(),error)||!service.listWells(wells,error)){emit statusChanged(text(error));return;}
 d->wells.clear();for(const auto &well:wells)d->wells.append({well.id,text(well.name)});emit changed();
}
QList<StratigraphyTrack> StratigraphyWorkflow::tracks() const {
 QList<StratigraphyTrack> result;
 for(const auto &w:d->document.wells) {
  const auto append=[&](const InterpretationSeries &s){
   if(s.axisKind!="MD"||s.axis.size()!=s.values.size())return;
   const auto unit=text(s.axisUnit).trimmed().toLower();
   const double scale=unit=="ft"||unit=="feet"?0.3048:1.0;
   if(unit!="m"&&unit!="metre"&&unit!="meter"&&unit!="ft"&&unit!="feet")return;
   StratigraphyTrack track;track.wellId=w.wellId;track.name=text(w.name);track.mnemonic=text(s.name);
   for(size_t i=0;i<s.axis.size();++i){track.depths.append(float(s.axis[i]*scale));track.values.append(i<s.quality.size()&&(s.quality[i]&5)?qQNaN():float(s.values[i]));}
   result.append(std::move(track));
  };append(w.input.log);for(const auto &s:w.series)append(s);
 }
 return result;
}
QList<StratigraphyRow> StratigraphyWorkflow::rows() const {
 QList<StratigraphyRow> result;
 for(const auto &w:d->document.wells) {
  for(const auto &b:w.boundaries) result.append({w.wellId,text(w.input.well.wellName),text(b.id),text(b.name),tr("界面"),text(b.status),b.md,b.md});
  for(const auto &c:w.cycles) {
   const auto top=std::find_if(w.boundaries.begin(),w.boundaries.end(),[&](const auto &b){return b.id==c.topBoundaryId;});
   const auto bottom=std::find_if(w.boundaries.begin(),w.boundaries.end(),[&](const auto &b){return b.id==c.bottomBoundaryId;});
   if(top!=w.boundaries.end()&&bottom!=w.boundaries.end()) result.append({w.wellId,text(w.input.well.wellName),{},text(c.name),QString::number(c.level),text(c.status),top->md,bottom->md});
  }
 }
 return result;
}
void StratigraphyWorkflow::calculate(const QList<qlonglong> &ids,bool subdivide) {
 if(d->task||!d->tasks||d->path.isEmpty()||d->readOnly||ids.isEmpty()) {emit statusChanged(tr("请打开可写工程并选择井，等待当前任务完成"));return;}
 const auto path=d->path.toStdString();const auto generation=d->generation;
 auto result=std::make_shared<InterpretationDocument>(d->document);const auto preferred=d->preferred;
 auto *task=d->tasks->start(tr("地层对比"),[path,ids,subdivide,result,preferred](PaleoTask *task)->QString {
  try {
   DataService service;std::string error;require(service.openProject(path,error),error);
   const auto g=geometry(service);const CancelCheck cancel=[task]{return task->cancelRequested();};
   if(!subdivide) {
    std::vector<long long> selected(ids.begin(),ids.end());
    auto plan=planQuickInputs(service,selected,preferred);checkPlan(plan);
    auto selection=selectTimeDepthCandidates(service,plan.request,g,cancel,result->wells);checkPlan(selection);
    CorrelationInputSnapshot snapshot;require(service.captureCorrelationInputs(selection.request,snapshot,error,cancel),error);
    const bool same=result->wells.size()==selected.size()&&std::all_of(result->wells.begin(),result->wells.end(),[&](const auto &well){return std::find(selected.begin(),selected.end(),well.wellId)!=selected.end();});
    InterpretationDocument doc=same?*result:InterpretationDocument{};
    if(!same)doc.id=QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    doc.name="地层对比";doc.created=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString();
    const bool changed=doc.snapshot.fingerprint!=snapshot.fingerprint;
    doc.snapshot=std::move(snapshot);doc.projectId=doc.snapshot.projectId;
    for(auto &well:doc.wells)for(const auto &input:doc.snapshot.wells)if(well.wellId==input.wellId) {
      well.input=input;if(changed){for(auto &series:well.series)series.status="stale";for(auto &cycle:well.cycles)cycle.status="review";}
    }
    *result=calculateHorizons(doc,g,cancel);
    for(const auto &issue:selection.issues)for(auto &well:result->wells)
     if(!issue.wellId||issue.wellId==well.wellId)well.warnings.push_back(issue.message);
    if(!result->runs.empty()) {
     auto parameters=QJsonDocument::fromJson(QByteArray::fromStdString(result->runs.back().parametersJson)).object();
     parameters["timeDepthSelection"]=QJsonDocument::fromJson(QByteArray::fromStdString(selection.auditJson)).object();
     result->runs.back().parametersJson=QJsonDocument(parameters).toJson(QJsonDocument::Compact).toStdString();
    }
   } else {
    if(result->wells.empty())throw std::runtime_error("请先计算区域界面");
    auto discovery=discoverQuickReferences(service,g,cancel,2.0);
    std::vector<InterpretationTemplate> stored;require(service.listReferenceTemplates(stored,error),error);
    stored.insert(stored.end(),discovery.templates.begin(),discovery.templates.end());
    for(const auto &candidate:stored)
     if(std::none_of(result->templates.begin(),result->templates.end(),[&](const auto &t){return t.id==candidate.id&&t.version==candidate.version;}))result->templates.push_back(candidate);
    auto plan=planQuickTemplates(*result,g,0,{},2.0,discovery.manualReferences);checkPlan(plan);
    for(const auto &reference:plan.manualReferences)for(auto &well:result->wells)
     if(well.wellId==reference.wellId&&well.cycles.empty()&&
        std::any_of(plan.segmentPlans.begin(),plan.segmentPlans.end(),[&](const auto &segment){return segment.referenceWellId==reference.wellId;})&&
        std::none_of(well.boundaries.begin(),well.boundaries.end(),[](const auto &boundary){return boundary.locked||boundary.source=="manual";})) {
      auto computed=well.boundaries;well.boundaries=reference.boundaries;well.cycles=reference.cycles;
      for(auto &boundary:computed){boundary.role="candidate:"+boundary.role;boundary.id+="-reference-candidate";well.boundaries.push_back(boundary);}
     }
    for(const auto &candidate:plan.candidates)
     if(std::none_of(result->templates.begin(),result->templates.end(),[&](const auto &t){return t.id==candidate.id&&t.version==candidate.version;}))result->templates.push_back(candidate);
    auto view=QJsonDocument::fromJson(QByteArray::fromStdString(result->viewStateJson)).object();
    view["uniformTemplatePlan"]=QJsonDocument::fromJson(QByteArray::fromStdString(plan.recommendationJson)).object();
    result->viewStateJson=QJsonDocument(view).toJson(QJsonDocument::Compact).toStdString();
    for(const auto &issue:plan.issues)for(auto &well:result->wells)
     if(!issue.wellId||issue.wellId==well.wellId)well.warnings.push_back(issue.message);
    *result=deriveForSegments(*result,g,cancel);
    *result=propagateTemplates(*result,plan.assignments,g,cancel);
    if(!result->runs.empty()) {
     auto parameters=QJsonDocument::fromJson(QByteArray::fromStdString(result->runs.back().parametersJson)).object();
     parameters["uniformTemplatePlan"]=QJsonDocument::fromJson(QByteArray::fromStdString(plan.recommendationJson)).object();
     result->runs.back().parametersJson=QJsonDocument(parameters).toJson(QJsonDocument::Compact).toStdString();
    }
   }
   return {};
  }catch(const std::exception &e){return QString::fromUtf8(e.what());}
 });d->task=task;emit busyChanged(true);
 connect(task,&PaleoTask::finished,this,[this,task,result,generation] {
  if(generation!=d->generation)return;d->task=nullptr;emit busyChanged(false);
  if(task->state()!=PaleoTask::State::Succeeded){emit statusChanged(task->errorText());return;}
  d->remember();d->document=std::move(*result);d->dirty=true;QStringList diagnostics;
  for(const auto &w:d->document.wells)for(const auto &message:w.warnings)diagnostics.append(text(message));
  emit changed();emit statusChanged(diagnostics.isEmpty()?tr("计算完成，请复核并保存分层版本"):diagnostics.join('\n'));
 });
}
void StratigraphyWorkflow::save() {
 if(d->readOnly||d->task||d->document.wells.empty())return;
 DataService service;std::string error;InterpretationCommitResult result;
 InterpretationCommitOptions options;options.expectedHeadRevision=d->document.revision;
 options.idempotencyKey=QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
 if(!service.openProject(d->path.toStdString(),error)||!service.commitInterpretation(d->document,options,result,error)) {emit statusChanged(text(error));return;}
 d->document.parentRevision=d->document.revision;d->document.revision=result.revision;d->dirty=false;
 emit statusChanged(tr("分层版本已保存到工区数据服务"));emit saved();
}
void StratigraphyWorkflow::activateClassification(qlonglong wellId) {
 if(d->readOnly||d->task||d->dirty||d->document.revision.empty()) {emit statusChanged(tr("请先保存分层版本"));return;}
 const auto it=std::find_if(d->document.wells.begin(),d->document.wells.end(),[&](const auto &well){return well.wellId==wellId;});
 if(it==d->document.wells.end())return;
 auto boundaries=it->boundaries;
 boundaries.erase(std::remove_if(boundaries.begin(),boundaries.end(),[](const auto &b){return !std::isfinite(b.md);}),boundaries.end());
 std::sort(boundaries.begin(),boundaries.end(),[](const auto &a,const auto &b){return a.md<b.md;});
 QStringList names;for(const auto &b:boundaries)
  if(std::isfinite(b.md)&&!b.role.starts_with("candidate:")&&b.status!="stale"&&!names.contains(text(b.name)))names.append(text(b.name));
 if(names.size()<2){emit statusChanged(tr("至少需要两个有效地层界面"));return;}
 const QString path=AreaRules::configFilePath(d->project);QJsonObject configuration;QFile input(path);
 if(input.exists()) {
  if(!input.open(QIODevice::ReadOnly)){emit statusChanged(input.errorString());return;}
  QJsonParseError error;const auto json=QJsonDocument::fromJson(input.readAll(),&error);
  if(error.error!=QJsonParseError::NoError||!json.isObject()){emit statusChanged(tr("工程参数文件无效，未写入"));return;}
  configuration=json.object();input.close();
 }
 configuration["sequence_boundaries"]=QJsonArray::fromStringList(names);
 if(!names.contains(configuration.value("target_horizon").toString()))configuration["target_horizon"]=names.first();
 QSaveFile output(path);if(!output.open(QIODevice::WriteOnly)||output.write(QJsonDocument(configuration).toJson())<0||!output.commit()) {emit statusChanged(output.errorString());return;}
 AreaRules::setProjectDir(d->project);emit classificationChanged(names);
 emit statusChanged(tr("已将所选井的地层界面应用到当前工程，分层版本与原始资料均可追溯"));
}
void StratigraphyWorkflow::moveBoundary(qlonglong well,const QString &boundary,double md) {
 if(d->task||d->readOnly)return;
 try {auto next=d->document;for(auto &w:next.wells)if(w.wellId==well)paleo::stratigraphy::moveBoundary(w,boundary.toStdString(),md);d->remember();d->document=std::move(next);d->dirty=true;emit changed();}
 catch(const std::exception &e){emit statusChanged(QString::fromUtf8(e.what()));}
}
void StratigraphyWorkflow::undo() {
 if(d->task||d->readOnly||d->history.empty())return;
 const auto revision=d->document.revision;const auto identity=d->document.id;
 d->document=std::move(d->history.back());d->history.pop_back();
 if(identity==d->document.id)d->document.revision=revision;d->dirty=true;emit changed();
}
QList<StratigraphyResource> StratigraphyWorkflow::resources(qlonglong well,const QString &kind) const {
 QList<StratigraphyResource> result;DataService service;std::string error;std::vector<DataEntry> entries;
 if(!service.openProjectReadOnly(d->path.toStdString(),error)||!service.listData(kind.toStdString(),well,entries,error))return result;
 for(const auto &entry:entries)result.append({entry.id,text(entry.name)});return result;
}
void StratigraphyWorkflow::chooseResource(qlonglong well,const QString &kind,qlonglong id) {
 if(d->task||d->readOnly)return;
 if(kind=="T70"||kind=="T72"||kind=="T73"||kind=="T80") {
  auto &h=d->preferred.horizons;const auto role=kind.toStdString();
  h.erase(std::remove_if(h.begin(),h.end(),[&](const auto &x){return x.role==role;}),h.end());
  if(id)h.push_back({id,role});return;
 }
 auto &wells=d->preferred.wells;
 auto it=std::find_if(wells.begin(),wells.end(),[&](const auto &w){return w.wellId==well;});
 if(it==wells.end()){CorrelationWellSelection selection;selection.wellId=well;wells.push_back(selection);it=std::prev(wells.end());}
 if(kind=="curves")it->curveId=id;
 else if(kind=="time_depth")it->timeDepthId=id;
 else if(kind=="trajectories")it->trajectoryId=id;
 else if(kind=="manual4")it->intervalTrackId=id;
 else if(kind=="manual3")it->manualLevel3TrackId=id;
 else if(kind=="formation")it->formationTrackId=id;
}
StratigraphyConventions StratigraphyWorkflow::conventions() const {
 DataService service;std::string error;CorrelationProjectConventions value;
 if(service.openProjectReadOnly(d->path.toStdString(),error))service.readCorrelationProjectConventions(value,error);
 return {text(value.depthType),text(value.depthUnit),text(value.timeUnit),text(value.evidence),value.allowProvisionalAlignment};
}
void StratigraphyWorkflow::setConventions(const StratigraphyConventions &value) {
 if(d->readOnly||d->task)return;
 DataService service;std::string error;CorrelationProjectConventions policy;
 policy.depthType=value.depthType.toStdString();policy.depthUnit=value.depthUnit.toStdString();
 policy.timeUnit=value.timeUnit.toStdString();policy.evidence=value.evidence.toStdString();policy.allowProvisionalAlignment=value.provisional;
 if(!service.openProject(d->path.toStdString(),error)||!service.saveCorrelationProjectConventions(policy,error))emit statusChanged(text(error));
 else emit statusChanged(tr("项目约定已保存，原始数值与单位声明保持来源记录"));
}
