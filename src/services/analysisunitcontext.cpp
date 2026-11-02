// 层：数据
#include "analysisunitcontext.h"
#include "subintervalfactors.h"
#include "stratigraphicsubintervals.h"
#include "io/areamodel.h"
#include <paleo_data/service.h>
#include <QJsonObject>
#include <cmath>
#include <algorithm>

namespace paleo::strat {
namespace fw=paleo::data::framework;
QString analysisProjectCrs(const QString &project,QString *error){
  paleo::data::DataService ds;paleo::data::ProjectCoordinates coordinates;std::string detail;
  if(!ds.openProjectReadOnly(area_model::directory(project).toStdString(),detail)||!ds.readProjectCoordinates(coordinates,detail)||coordinates.coordinateUnit!="m"){
    if(error)*error=QString::fromStdString(detail.empty()?"分析需要工区米制坐标系":detail);return {};
  }
  return QString::fromStdString(coordinates.coordinateSystem);
}
bool freezeAnalysisUnit(const QString &project,const UnitSelection &requested,UnitSelection &frozen,
                        QString *error,bool allowAggregate){
  frozen={};if(error)error->clear();
  const auto fail=[&](const QString &reason){if(error)*error=reason;return false;};
  if(!requested.isValid()||(!allowAggregate&&requested.aggregate))
    return fail(QStringLiteral("请先选择地震层位单元及具体子层段 S1–S4"));
  paleo::data::DataService ds;std::string detail;paleo::data::StratigraphicScheme scheme;
  if(!ds.openProjectReadOnly(area_model::directory(project).toStdString(),detail)||
     !ds.readStratigraphicScheme(requested.schemeId.toStdString(),requested.revision.toStdString(),scheme,detail))
    return fail(QString::fromStdString(detail));
  const auto *interval=fw::findInterval(scheme,{requested.intervalId.toStdString()});
  if(!interval||interval->status!="valid")return fail(QStringLiteral("所选地震层位单元不存在或顶底关系无效"));
  int index=0;
  if(!requested.aggregate){
    const auto *sub=fw::findSubInterval(scheme,{requested.subIntervalId.toStdString()});
    if(!sub||sub->interval!=interval->id)return fail(QStringLiteral("子层段与所选地震层位单元不一致"));
    index=sub->index;
  }
  frozen=makeSelection(scheme,requested.intervalId,index);
  return frozen.isValid()&&!frozen.revision.isEmpty();
}
bool analysisUnitGeometryCurrent(const QString &project,const UnitSelection &unit,QString *error){
  paleo::data::DataService ds;std::string detail;paleo::data::StratigraphicScheme old,head;
  if(!unit.isValid()||unit.revision.isEmpty()||
     !ds.openProjectReadOnly(area_model::directory(project).toStdString(),detail)||
     !ds.readStratigraphicScheme(unit.schemeId.toStdString(),unit.revision.toStdString(),old,detail)||
     !ds.readStratigraphicScheme(unit.schemeId.toStdString(),{},head,detail)){
    if(error)*error=QString::fromStdString(detail.empty()?"分析层段身份不完整":detail);return false;
  }
  bool checked=false;
  for(const auto *sub:fw::subIntervalsOf(old,{unit.intervalId.toStdString()})){
    if(!unit.aggregate&&sub->id.value!=unit.subIntervalId.toStdString())continue;
    checked=true;
    if(fw::subIntervalFingerprint(old,sub->id)!=fw::subIntervalFingerprint(head,sub->id)){
      if(error)*error=QStringLiteral("分析层段在计算期间已修改，请重新获取或预测");return false;
    }
  }
  return checked;
}
paleo::data::WellSubIntervalRange analysisWellRange(const paleo::data::StratigraphicScheme &scheme,
    const UnitSelection &unit,const QString &entityId,qlonglong modelWellId){
  const bool native=modelWellId>0&&std::any_of(scheme.wellOccurrences.begin(),scheme.wellOccurrences.end(),[&](const auto &o){
    return o.wellId==modelWellId&&o.interval.value==unit.intervalId.toStdString();});
  const auto id=native||entityId.isEmpty()?modelWellId:stableWellKey(entityId);
  if(!unit.aggregate)return fw::wellRange(scheme,id,{unit.subIntervalId.toStdString()});
  paleo::data::WellSubIntervalRange combined;combined.resolved=true;combined.provisional=false;combined.wellId=id;
  for(const auto *sub:fw::subIntervalsOf(scheme,{unit.intervalId.toStdString()})){
    const auto r=fw::wellRange(scheme,id,sub->id);if(!r.resolved)return r;
    if(!std::isfinite(combined.top)||r.top<combined.top)combined.top=r.top;
    if(!std::isfinite(combined.bottom)||r.bottom>combined.bottom)combined.bottom=r.bottom;
    combined.depthType=r.depthType;combined.depthUnit=r.depthUnit;combined.provisional|=r.provisional;
  }
  combined.index=4;combined.resolved=std::isfinite(combined.top)&&combined.bottom>combined.top;return combined;
}
QJsonArray scopeWellRows(const QJsonArray &rows,const paleo::data::WellSubIntervalRange &range){
  QJsonArray scoped;
  if(!range.resolved||range.depthType!="MD"||range.depthUnit!="m")return scoped;
  for(const auto &row:rows){
    const double d=row.toObject().value(QStringLiteral("深度")).toDouble(NAN);
    if(std::isfinite(d)&&d>=range.top&&(d<range.bottom||(range.index==4&&d==range.bottom)))scoped.append(row);
  }
  return scoped;
}
}
