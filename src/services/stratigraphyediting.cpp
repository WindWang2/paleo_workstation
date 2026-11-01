// 层：数据
// Manual editing semantics ported from 地层对比_优选外推统一模板_20261029.zip.
#include "stratigraphyediting.h"
#include <paleo_stratigraphy/core.h>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
using namespace paleo::data;
namespace {
const char* roles[]={"T70","T72","T73","T80"};
const char* segments[]={"E12","E3","E4"};
std::string uid(){return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();}
WellInterpretation& well(InterpretationDocument& d,long long id) {
 for(auto& w:d.wells)if(w.wellId==id)return w;
 throw std::runtime_error("先选择一口已加载的井");
}
InterpretationBoundary& boundary(WellInterpretation& w,const std::string& id) {
 for(auto& b:w.boundaries)if(b.id==id)return b;
 throw std::runtime_error("界面不存在");
}
const InterpretationBoundary* role(const WellInterpretation& w,const std::string& id) {
 for(const auto& b:w.boundaries)if(b.role==id)return &b;
 return nullptr;
}
bool regional(const std::string& r){return r=="T70"||r=="T72"||r=="T73"||r=="T80";}
void audit(InterpretationDocument& d,const std::string& target,const std::string& action,
           const QJsonObject& before={},const QJsonObject& after={}) {
 InterpretationEdit e;e.id=uid();e.targetId=target;e.action=action;
 e.created=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString();
 e.beforeJson=QJsonDocument(before).toJson(QJsonDocument::Compact).toStdString();
 e.afterJson=QJsonDocument(after).toJson(QJsonDocument::Compact).toStdString();d.edits.push_back(e);
}
void staleCdmt(WellInterpretation& w,const std::string& segment) {
 for(auto& s:w.series)if(s.kind=="FISCHER_CDMT"&&s.segmentId==segment)s.status="stale";
}
void rebuild(WellInterpretation& w,const std::string& segment) {
 const int si=segment=="E12"?0:segment=="E3"?1:2;
 const auto* a=role(w,roles[si]);const auto* b=role(w,roles[si+1]);
 if(!a||!b)throw std::runtime_error("该段缺少完整顶底界面");
 std::vector<const InterpretationBoundary*> bs;
 for(const auto& p:w.boundaries)if(p.md>=a->md-1e-6&&p.md<=b->md+1e-6)bs.push_back(&p);
 std::sort(bs.begin(),bs.end(),[](auto x,auto y){return x->md<y->md;});
 std::erase_if(w.cycles,[&](const auto& c){return c.segmentId==segment;});
 for(size_t i=1;i<bs.size();++i) {
  if(bs[i]->md-bs[i-1]->md<1e-6)throw std::runtime_error("段内有重复深度的界面，请先合并");
  InterpretationCycle c;c.id=uid();c.name=segment+"-"+std::to_string(i);c.segmentId=segment;
  c.topBoundaryId=bs[i-1]->id;c.bottomBoundaryId=bs[i]->id;c.source="manual";c.status="review";w.cycles.push_back(c);
 }
}
}
namespace paleo::stratigraphy_edit {
void move(InterpretationDocument& d,long long id,const std::string& target,double md) {
 auto& w=well(d,id);const auto old=boundary(w,target);
 if(std::abs(old.md-md)<1e-8)return;
 paleo::stratigraphy::moveBoundary(w,target,md);
 if(regional(old.role))w.warnings.push_back("段界已修改，请复核相邻段并按需重新计算曲线与旋回");
 audit(d,target,"move_boundary",{{"md",old.md}},{{"md",md},{"locked",true}});
}
void properties(InterpretationDocument& d,long long id,const std::string& target,double md,
                const std::string& name,bool locked) {
 auto& w=well(d,id);if(std::abs(boundary(w,target).md-md)>1e-8)paleo::stratigraphy::moveBoundary(w,target,md);
 auto& b=boundary(w,target);if(!b.role.empty()&&b.role!="cycle")b.name=b.role;else if(!name.empty())b.name=name;
 b.locked=locked;audit(d,target,"edit_boundary",{},{{"md",md},{"name",QString::fromStdString(b.name)},{"locked",locked}});
}
std::string add(InterpretationDocument& d,long long id,double md,const std::string& name,const std::string& requestedRole) {
 auto& w=well(d,id);std::string segment;
 if(!requestedRole.empty()) {
  if(!regional(requestedRole))throw std::runtime_error("未知区域界面类型");
  if(role(w,requestedRole))throw std::runtime_error("该井已有此区域界面，请选择并调整已有界面");
  InterpretationBoundary b;b.id=uid();b.name=b.role=requestedRole;b.md=b.originalMd=md;
  b.source="manual";b.status="confirmed";b.locked=true;w.boundaries.push_back(b);
  audit(d,b.id,"add_regional_boundary");return b.id;
 }
 for(int i=0;i<3;++i){const auto* a=role(w,roles[i]);const auto* b=role(w,roles[i+1]);if(a&&b&&md>a->md&&md<b->md)segment=segments[i];}
 if(segment.empty())throw std::runtime_error("新增界面须位于已确认的内部段中");
 InterpretationBoundary b;b.id=uid();b.name=name.empty()||regional(name)?"人工旋回界面":name;
 b.role="cycle";b.md=b.originalMd=md;b.source="manual";b.locked=true;w.boundaries.push_back(b);
 std::vector<InterpretationCycle> children;bool split=false;
 for(auto& c:w.cycles)if(c.segmentId==segment&&c.level==4) {
  const auto& top=boundary(w,c.topBoundaryId);const auto& bottom=boundary(w,c.bottomBoundaryId);
  if(md>top.md&&md<bottom.md) {
   auto lower=c;lower.id=uid();lower.name=c.name+" 下";lower.topBoundaryId=b.id;
   lower.templateIdentity.clear();lower.templateVersion.clear();lower.source="manual";lower.status="review";
   c.bottomBoundaryId=b.id;c.name+=" 上";c.templateIdentity.clear();c.templateVersion.clear();c.source="manual";c.status="review";
   children.push_back(lower);split=true;break;
  }
 }
 if(split)w.cycles.insert(w.cycles.end(),children.begin(),children.end());
 else if(std::none_of(w.cycles.begin(),w.cycles.end(),[&](const auto& c){return c.segmentId==segment&&c.level==4;}))rebuild(w,segment);
 else throw std::runtime_error("新增深度不在一个完整的四级旋回内部");
 staleCdmt(w,segment);audit(d,b.id,"add_boundary");return b.id;
}
void remove(InterpretationDocument& d,long long id,const std::string& target) {
 auto& w=well(d,id);const auto& b=boundary(w,target);
 if(regional(b.role))throw std::runtime_error("区域界面可调整，不作为旋回界面删除。");
 std::string upper,lower;
 for(const auto& c:w.cycles) {
  if(c.level==3&&(c.topBoundaryId==target||c.bottomBoundaryId==target))throw std::runtime_error("此界面也是三级旋回边界，请拖拽调整以保留父子层级。");
  if(c.level==4&&c.bottomBoundaryId==target)upper=c.id;
  if(c.level==4&&c.topBoundaryId==target)lower=c.id;
 }
 if(upper.empty()||lower.empty())throw std::runtime_error("请选择相邻四级旋回之间的公共界面。");
 auto a=std::find_if(w.cycles.begin(),w.cycles.end(),[&](const auto& c){return c.id==upper;});
 const auto z=std::find_if(w.cycles.begin(),w.cycles.end(),[&](const auto& c){return c.id==lower;});
 if(a->parentId!=z->parentId||a->segmentId!=z->segmentId)throw std::runtime_error("不能跨越三级层序或区域段界合并");
 a->bottomBoundaryId=z->bottomBoundaryId;if(a->direction!=z->direction)a->direction=0;
 a->templateIdentity.clear();a->templateVersion.clear();a->source="manual";a->status="review";const auto segment=a->segmentId;
 w.cycles.erase(z);std::erase_if(w.boundaries,[&](const auto& x){return x.id==target;});
 staleCdmt(w,segment);audit(d,target,"merge_adjacent_cycles");
}
std::string confirm(InterpretationDocument& d,long long id,const std::string& target) {
 auto& w=well(d,id);auto& b=boundary(w,target);auto adopted=target;
 if(b.role.starts_with("candidate:")) {
  const auto r=b.role.substr(10);if(!regional(r))throw std::runtime_error("候选界面角色无效");
  auto candidate=b;auto current=std::find_if(w.boundaries.begin(),w.boundaries.end(),[&](const auto& x){return x.role==r;});
  candidate.role=r;candidate.name=r;candidate.source="manual";candidate.locked=true;candidate.status="confirmed";
  if(current!=w.boundaries.end()) {
   candidate.id=current->id;adopted=candidate.id;*current=candidate;
   std::erase_if(w.boundaries,[&](const auto& x){return x.id==target;});
  }else b=candidate;
  for(auto& s:w.series)s.status="stale";
 }else{b.locked=true;b.status="confirmed";}
 audit(d,adopted,"confirm_or_adopt_boundary");return adopted;
}
void cycle(InterpretationDocument& d,long long id,const std::string& target,const std::string& name,int level,int direction) {
 auto& w=well(d,id);auto it=std::find_if(w.cycles.begin(),w.cycles.end(),[&](const auto& c){return c.id==target;});
 if(it==w.cycles.end())throw std::runtime_error("未选择旋回");
 if(level!=it->level)throw std::runtime_error("层级由父子结构定义，不能只改编号。请按模板重新组织层级。");
 it->direction=direction;if(!name.empty())it->name=name;it->source="manual";it->status="review";audit(d,target,"edit_cycle");
}
void hierarchy(InterpretationDocument& d,long long id,const std::string& segment,const std::vector<Group>& edits) {
 auto& w=well(d,id);std::vector<InterpretationCycle> children;
 for(const auto& c:w.cycles)if(c.segmentId==segment&&c.level==4)children.push_back(c);
 std::sort(children.begin(),children.end(),[&](const auto& a,const auto& b){return boundary(w,a.topBoundaryId).md<boundary(w,b.topBoundaryId).md;});
 if(children.empty()||edits.size()!=children.size())throw std::runtime_error("所选段没有完整的四级旋回组合");
 std::map<int,std::vector<size_t>> groups;std::map<int,int> directions;int previous=0;
 for(size_t i=0;i<children.size();++i) {
  const auto& e=edits[i];if(e.cycleId!=children[i].id)throw std::runtime_error("旋回组合已变化，请重新打开");
  const int n=e.number,dir=e.direction;if(n<previous)throw std::runtime_error("组号必须从小到大、连续排列");previous=n;
  if(n){if(directions.contains(n)&&directions[n]!=dir)throw std::runtime_error("同一三级组的方向必须一致");directions[n]=dir;groups[n].push_back(i);}
 }
 std::vector<InterpretationCycle> oldParents;for(const auto& c:w.cycles)if(c.segmentId==segment&&c.level==3)oldParents.push_back(c);
 std::erase_if(w.cycles,[&](const auto& c){return c.segmentId==segment&&c.level==3;});
 for(auto& c:w.cycles)if(c.segmentId==segment&&c.level==4)c.parentId.clear();
 for(const auto& [n,rows]:groups) {
  for(size_t j=1;j<rows.size();++j)if(children[rows[j-1]].bottomBoundaryId!=children[rows[j]].topBoundaryId)throw std::runtime_error("三级组合不能跨越未连接的四级旋回");
  InterpretationCycle p;p.id=uid();p.name=segment+"-L3-"+std::to_string(n);p.level=3;p.segmentId=segment;
  p.topBoundaryId=children[rows.front()].topBoundaryId;p.bottomBoundaryId=children[rows.back()].bottomBoundaryId;
  for(const auto& old:oldParents)if(old.topBoundaryId==p.topBoundaryId&&old.bottomBoundaryId==p.bottomBoundaryId){p.id=old.id;p.name=old.name;}
  p.direction=directions[n];p.source="manual";p.status="review";
  for(auto row:rows)for(auto& c:w.cycles)if(c.id==children[row].id){c.parentId=p.id;c.status="review";}
  w.cycles.push_back(p);
 }
 staleCdmt(w,segment);audit(d,segment,"rebuild_manual_L3_groups");
}
}
