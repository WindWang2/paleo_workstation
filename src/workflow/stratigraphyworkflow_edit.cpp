// 层：功能
#include "stratigraphyworkflow_state.h"
#include "../services/stratigraphyediting.h"
#include "stratigraphygeometry.h"
#include <paleo_stratigraphy/core.h>
#include <QDateTime>
#include <QUuid>
#include <paleo_data/service.h>
#include <algorithm>
#include <set>
using namespace paleo::data;
namespace {
void record(InterpretationDocument& d,const char* action) {
 InterpretationEdit e;e.id=QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();e.action=action;
 e.created=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString();d.edits.push_back(e);
}
}
StratigraphyWorkflow::State::Geometry StratigraphyWorkflow::State::geometry() const {
 Geometry result;for(const auto& w:document.wells) {
  Geometry::Well item{w.wellId,w.boundaries,w.cycles,w.warnings,{}};
  for(const auto& s:w.series)item.seriesStates.emplace_back(s.id,s.status);result.wells.push_back(std::move(item));
 }return result;
}
void StratigraphyWorkflow::State::restore(const Geometry& state) {
 for(const auto& item:state.wells)for(auto& w:document.wells)if(w.wellId==item.id) {
  w.boundaries=item.boundaries;w.cycles=item.cycles;w.warnings=item.warnings;
  for(auto& s:w.series)for(const auto& [id,status]:item.seriesStates)if(s.id==id)s.status=status;
 }dirty=true;
}
const InterpretationDocument& StratigraphyWorkflow::document() const{return d->document;}
bool StratigraphyWorkflow::hasUnsavedChanges() const{return d->dirty;}
bool StratigraphyWorkflow::canUndo() const{return canEdit()&&!d->history.empty();}
bool StratigraphyWorkflow::canRedo() const{return canEdit()&&!d->future.empty();}
void StratigraphyWorkflow::cancel(){if(d->task)d->task->requestCancel();}
bool StratigraphyWorkflow::edit(const std::function<void(InterpretationDocument&)>& change) {
 if(!canEdit())return false;
 auto before=d->geometry();const auto editCount=d->document.edits.size();const bool dirty=d->dirty;
 try {
  change(d->document);
  for(const auto& w:d->document.wells){const auto errors=paleo::stratigraphy::validateInterpretation(w);if(!errors.empty())throw std::runtime_error(errors.front());}
  d->history.push_back(std::move(before));if(d->history.size()>100)d->history.erase(d->history.begin());d->future.clear();
  d->dirty=true;emit changed();return true;
 }catch(const std::exception& e){d->restore(before);d->document.edits.resize(editCount);d->dirty=dirty;emit statusChanged(QString::fromUtf8(e.what()));return false;}
}
void StratigraphyWorkflow::undo() {
 if(!canUndo())return;d->future.push_back(d->geometry());const auto state=std::move(d->history.back());d->history.pop_back();
 d->restore(state);record(d->document,"undo");emit changed();emit statusChanged(tr("已撤销上次人工编辑"));
}
void StratigraphyWorkflow::redo() {
 if(!canRedo())return;d->history.push_back(d->geometry());const auto state=std::move(d->future.back());d->future.pop_back();
 d->restore(state);record(d->document,"redo");emit changed();emit statusChanged(tr("已重做人工编辑"));
}
void StratigraphyWorkflow::moveBoundary(qlonglong well,const QString& id,double md) {
 for(const auto& w:d->document.wells)if(w.wellId==well)for(const auto& b:w.boundaries)if(b.id==id.toStdString()&&std::abs(b.md-md)<1e-8)return;
 if(edit([&](auto& doc){paleo::stratigraphy_edit::move(doc,well,id.toStdString(),md);}))emit boundarySelected(well,id);
}
void StratigraphyWorkflow::editBoundary(qlonglong well,const QString& id,double md,const QString& name,bool locked) {
 if(edit([&](auto& doc){paleo::stratigraphy_edit::properties(doc,well,id.toStdString(),md,name.trimmed().toStdString(),locked);}))emit boundarySelected(well,id);
}
void StratigraphyWorkflow::addBoundary(qlonglong well,double md,const QString& name) {
 std::string id;if(edit([&](auto& doc){id=paleo::stratigraphy_edit::add(doc,well,md,name.trimmed().toStdString());}))emit boundarySelected(well,QString::fromStdString(id));
}
void StratigraphyWorkflow::removeBoundary(qlonglong well,const QString& id) {
 if(edit([&](auto& doc){paleo::stratigraphy_edit::remove(doc,well,id.toStdString());}))emit boundarySelected(well,{});
}
void StratigraphyWorkflow::confirmBoundary(qlonglong well,const QString& id) {
 std::string adopted;if(edit([&](auto& doc){adopted=paleo::stratigraphy_edit::confirm(doc,well,id.toStdString());}))emit boundarySelected(well,QString::fromStdString(adopted));
}
void StratigraphyWorkflow::editCycle(qlonglong well,const QString& id,const QString& name,int level,int direction) {
 edit([&](auto& doc){paleo::stratigraphy_edit::cycle(doc,well,id.toStdString(),name.trimmed().toStdString(),level,direction);});
}
void StratigraphyWorkflow::editHierarchy(qlonglong well,const QString& segment,const QList<StratigraphyGroup>& groups) {
 std::vector<paleo::stratigraphy_edit::Group> values;
 for(const auto& g:groups)values.push_back({g.cycle.toStdString(),g.number,g.direction});
 edit([&](auto& doc){paleo::stratigraphy_edit::hierarchy(doc,well,segment.toStdString(),values);});
}
void StratigraphyWorkflow::reorderWells(const QList<qlonglong>& order) {
 if(!canEdit()||order.size()!=qsizetype(d->document.wells.size()))return;
 auto reordered=d->document.wells;std::set<qlonglong> unique;
 for(qsizetype i=0;i<order.size();++i) {
  const auto it=std::find_if(d->document.wells.begin(),d->document.wells.end(),[&](const auto& w){return w.wellId==order[i];});
  if(it==d->document.wells.end()||!unique.insert(order[i]).second)return;reordered[size_t(i)]=*it;
 }d->document.wells=std::move(reordered);d->dirty=true;emit changed();
}
void StratigraphyWorkflow::setProfile(const CorrelationAlgorithmProfile& profile) {
 if(!canEdit())return;
 d->document.profile=profile;d->document.profile.id="custom-"+QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
 for(auto& w:d->document.wells) {
  w.warnings.push_back("算法参数已变化，重新计算前现有曲线为历史结果");
  for(auto& s:w.series)s.status="stale";for(auto& c:w.cycles)if(c.source=="algorithm")c.status="stale";
 }d->history.clear();d->future.clear();d->dirty=true;emit changed();
}
void StratigraphyWorkflow::makeTemplate(qlonglong id,const QString& name,double tolerance) {
 if(!canEdit()||name.trimmed().isEmpty())return;
 try {
  auto next=d->document;auto it=std::find_if(next.wells.begin(),next.wells.end(),[&](const auto& w){return w.wellId==id;});
  if(it==next.wells.end())throw std::runtime_error("请先加载并选择标准井");
  auto ref=*it;
  DataService service;std::string error;if(!service.openProject(d->path.toStdString(),error))throw std::runtime_error(error);
  if(std::none_of(ref.cycles.begin(),ref.cycles.end(),[](const auto& c){return c.source=="manual"&&c.level==4;})) {
   if(ref.input.manualTrack.intervals.empty()||ref.input.formationTrack.intervals.empty())throw std::runtime_error("该井需要人工旋回道和组段道，或先手工建立旋回");
   const auto g=paleo::stratigraphy_native::geometry(service,d->document.viewStateJson);
   ref=paleo::stratigraphy::makeManualInterpretation(it->input,g);
  }
  const auto issues=paleo::stratigraphy::validateInterpretation(ref);if(!issues.empty())throw std::runtime_error(issues.front());
  auto t=paleo::stratigraphy::extractTemplate(ref,QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),"1",tolerance);t.name=name.trimmed().toStdString();
  if(!service.saveReferenceTemplate(t,error))throw std::runtime_error(error);
  d->remember();it->boundaries=ref.boundaries;it->cycles=ref.cycles;next.templates.push_back(t);next.activeTemplateId=t.id;
  d->document=std::move(next);d->dirty=true;emit changed();emit statusChanged(tr("人工标准模板已保存，原人工井端点保持不变"));
 }catch(const std::exception& e){emit statusChanged(QString::fromUtf8(e.what()));}
}
